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
                .access_cb = NULL,
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
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) return; // уже подключены — не рекламируемся

    char ip[16];
    app_get_ip_string(ip, sizeof(ip));
    float temp = app_get_chip_temp_c();

    // Manufacturer data: company ID 0xFFFF + 4 bytes IP + 4 bytes float temp
    uint8_t mfg[10];
    mfg[0] = 0xFF; mfg[1] = 0xFF;
    uint32_t ip_u32 = 0;
    sscanf(ip, "%hhu.%hhu.%hhu.%hhu",
           &mfg[2], &mfg[3], &mfg[4], &mfg[5]);
    memcpy(&mfg[6], &temp, 4);

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &nus_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    fields.mfg_data = mfg;
    fields.mfg_data_len = sizeof(mfg);

    ble_gap_adv_stop();
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params advp = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x20,
        .itvl_max = 0x40,
    };
    ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &advp, NULL, NULL);
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

void ble_nus_init(void)
{
    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);

    ble_svc_gap_device_name_set("ESP32-NUS-Mock");

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = NULL;

    // Устанавливаем предпочитаемый MTU из конфига
    ble_att_set_preferred_mtu(g_config.ble_mtu);

    nimble_port_freertos_init(host_task);
}