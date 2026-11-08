#include "app.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "storage";
static const char *NVS_NS = "appcfg";
static const char *NVS_KEY = "json";

app_config_t g_config = {
    .wifi_ssid    = "MyWiFi",
    .wifi_pass    = "MyPassword",
    .ble_mtu      = 247,
    .float_value  = 3.14f,
    .int_value    = 42,
    .fw_version   = "1.0.0-mock",
};

void storage_init(void)
{
    // Всё уже инициализировано в main (nvs_flash_init).
    // Оставлено для симметрии.
}

void storage_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "No saved config, using defaults");
        return;
    }

    size_t sz = 0;
    if (nvs_get_str(h, NVS_KEY, NULL, &sz) != ESP_OK || sz == 0) {
        nvs_close(h);
        return;
    }

    char *buf = malloc(sz);
    if (!buf) { nvs_close(h); return; }

    if (nvs_get_str(h, NVS_KEY, buf, &sz) == ESP_OK) {
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            cJSON *it;
            if ((it = cJSON_GetObjectItem(root, "ssid")))  strlcpy(g_config.wifi_ssid, it->valuestring, sizeof(g_config.wifi_ssid));
            if ((it = cJSON_GetObjectItem(root, "pass")))  strlcpy(g_config.wifi_pass, it->valuestring, sizeof(g_config.wifi_pass));
            if ((it = cJSON_GetObjectItem(root, "mtu")))   g_config.ble_mtu = (uint16_t)it->valueint;
            if ((it = cJSON_GetObjectItem(root, "f")))     g_config.float_value = (float)it->valuedouble;
            if ((it = cJSON_GetObjectItem(root, "i")))     g_config.int_value = it->valueint;
            if ((it = cJSON_GetObjectItem(root, "ver")))   strlcpy(g_config.fw_version, it->valuestring, sizeof(g_config.fw_version));
            cJSON_Delete(root);
        }
    }
    free(buf);
    nvs_close(h);
}

void storage_save(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddStringToObject(root, "ssid", g_config.wifi_ssid);
    cJSON_AddStringToObject(root, "pass", g_config.wifi_pass);
    cJSON_AddNumberToObject(root, "mtu", g_config.ble_mtu);
    cJSON_AddNumberToObject(root, "f", g_config.float_value);
    cJSON_AddNumberToObject(root, "i", g_config.int_value);
    cJSON_AddStringToObject(root, "ver", g_config.fw_version);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY, json);
        nvs_commit(h);
        nvs_close(h);
    }
    free(json);
}