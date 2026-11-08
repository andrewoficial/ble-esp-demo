#include "app.h"

#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ble_nus";

static uint8_t own_addr_type;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static int client_count = 0;

int app_get_ble_client_count(void) { return client_count; }

static int gap_event(struct ble_gap_event *event, void *arg);
static esp_err_t update_advertising(void);

// ─── NUS UUIDs ───
// Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
// RX (write): 6E400002-...
// TX (notify): 6E400003-...
static const ble_uuid128_t nus_svc_uuid =
    BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e);
static const ble_uuid128_t nus_rx_uuid =
    BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e);
static const ble_uuid128_t nus_tx_uuid =
    BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e);

static uint16_t tx_val_handle;

static void build_manufacturer_data(uint8_t mfg[10])
{
    char ip[16];
    app_get_ip_string(ip, sizeof(ip));
    float temp = app_get_chip_temp_c();

    mfg[0] = 0xFF;
    mfg[1] = 0xFF;
    sscanf(ip, "%hhu.%hhu.%hhu.%hhu", &mfg[2], &mfg[3], &mfg[4], &mfg[5]);
    memcpy(&mfg[6], &temp, sizeof(temp));
}

void ble_nus_get_advertising_description(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }

    uint8_t mfg[10];
    build_manufacturer_data(mfg);

    int written = snprintf(buf, len,
                           "ADV flags=0x06 conn=undirected disc=general int=20-40ms "
                           "svc=6E400001-B5A3-F393-E0A9-E50E24DCCA9E; "
                           "RSP name=[%s] mfg_raw=",
                           g_config.device_name);
    if (written < 0 || (size_t)written >= len) {
        buf[len - 1] = '\0';
        return;
    }

    size_t pos = (size_t)written;
    for (size_t i = 0; i < sizeof(mfg) && pos < len; i++) {
        written = snprintf(buf + pos, len - pos, "%s%02X", i == 0 ? "" : " ", mfg[i]);
        if (written < 0 || (size_t)written >= len - pos) {
            buf[len - 1] = '\0';
            return;
        }
        pos += (size_t)written;
    }
}

void ble_nus_get_service_description(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }

    snprintf(buf, len,
             "GATT primary=6E400001-B5A3-F393-E0A9-E50E24DCCA9E; "
             "RX=6E400002-B5A3-F393-E0A9-E50E24DCCA9E WRITE,WRITE_NO_RSP; "
             "TX=6E400003-B5A3-F393-E0A9-E50E24DCCA9E NOTIFY");
}

// ─── GATT access ───
static int nus_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    char cmd[256];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len >= sizeof(cmd)) len = sizeof(cmd) - 1;
    ble_hs_mbuf_to_flat(ctxt->om, cmd, len, NULL);
    cmd[len] = '\0';

    char resp[512];
    protocol_execute(cmd, resp, sizeof(resp));

    // Отправляем ответ через TX notify
    struct os_mbuf *om = ble_hs_mbuf_from_flat(resp, strlen(resp));
    if (om) ble_gatts_notify_custom(conn_handle, tx_val_handle, om);

    return 0;
}

static int nus_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &nus_rx_uuid.u,
                .access_cb = nus_rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &nus_tx_uuid.u,
                .access_cb = nus_tx_access,
                .val_handle = &tx_val_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};

// ─── Реклама ───
void ble_nus_update_advertising(void)
{
    esp_err_t err = update_advertising();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update advertising: %s", esp_err_to_name(err));
    }
}

static esp_err_t update_advertising(void)
{
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) return ESP_OK;

    // Manufacturer data: company ID 0xFFFF + 4 bytes IP + 4 bytes float temp
    uint8_t mfg[10];
    build_manufacturer_data(mfg);

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &nus_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    ble_gap_adv_stop();
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return ESP_FAIL;
    }

    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.mfg_data = mfg;
    rsp_fields.mfg_data_len = sizeof(mfg);
    rsp_fields.name = (const uint8_t *)g_config.device_name;
    rsp_fields.name_len = strlen(g_config.device_name);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed: %d", rc);
        return ESP_FAIL;
    }

    struct ble_gap_adv_params advp = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x20,
        .itvl_max = 0x40,
    };
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &advp, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ─── GAP event ───
static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            conn_handle = event->connect.conn_handle;
            client_count++;
            ble_gattc_exchange_mtu(conn_handle, NULL, NULL);
        } else {
            ble_nus_update_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (client_count > 0) client_count--;
        ble_nus_update_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_nus_update_advertising();
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

// ─── Host task ───
static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    ble_nus_update_advertising();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_nus_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(g_config.device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed: %d", rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = NULL;

    // Устанавливаем предпочитаемый MTU из конфига
    rc = ble_att_set_preferred_mtu(g_config.ble_mtu);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_att_set_preferred_mtu failed: %d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

esp_err_t ble_nus_set_name(const char *name)
{
    int rc = ble_svc_gap_device_name_set(name);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed: %d", rc);
        return ESP_FAIL;
    }
    return update_advertising();
}