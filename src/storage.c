#include "app.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "storage";
static const char *NVS_NS = "appcfg";

app_config_t g_config = {
    .wifi_ssid    = "MyWiFi",
    .wifi_pass    = "MyPassword",
    .ota_password = "",
    .device_name  = "ESP32-NUS-Mock",
    .ble_mtu      = 247,
    .float_value  = 3.14f,
    .int_value    = 42,
};

void storage_init(void)
{
    // Всё уже инициализировано в main (nvs_flash_init).
    // Оставлено для симметрии.
}

static void load_string(nvs_handle_t handle, const char *key, char *value, size_t capacity)
{
    size_t size = capacity;
    esp_err_t err = nvs_get_str(handle, key, value, &size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to load %s: %s", key, esp_err_to_name(err));
    }
}

static bool save_result(esp_err_t err, const char *key)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save %s: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

void storage_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved config, using defaults");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open config: %s", esp_err_to_name(err));
        return;
    }

    load_string(h, "ssid", g_config.wifi_ssid, sizeof(g_config.wifi_ssid));
    load_string(h, "pass", g_config.wifi_pass, sizeof(g_config.wifi_pass));
    load_string(h, "otapass", g_config.ota_password, sizeof(g_config.ota_password));
    load_string(h, "name", g_config.device_name, sizeof(g_config.device_name));

    err = nvs_get_u16(h, "mtu", &g_config.ble_mtu);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to load mtu: %s", esp_err_to_name(err));
    }
    err = nvs_get_i32(h, "ival", &g_config.int_value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to load ival: %s", esp_err_to_name(err));
    }
    size_t float_size = sizeof(g_config.float_value);
    err = nvs_get_blob(h, "fval", &g_config.float_value, &float_size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to load fval: %s", esp_err_to_name(err));
    }
    nvs_close(h);
}

esp_err_t storage_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open config for saving: %s", esp_err_to_name(err));
        return err;
    }

    if (    !save_result(nvs_set_str(h, "ssid", g_config.wifi_ssid), "ssid") ||
    !save_result(nvs_set_str(h, "pass", g_config.wifi_pass), "pass") ||
    !save_result(nvs_set_str(h, "otapass", g_config.ota_password), "otapass") ||
    !save_result(nvs_set_str(h, "name", g_config.device_name), "name") ||
    !save_result(nvs_set_u16(h, "mtu", g_config.ble_mtu), "mtu") ||
        !save_result(nvs_set_i32(h, "ival", g_config.int_value), "ival") ||
        !save_result(nvs_set_blob(h, "fval", &g_config.float_value, sizeof(g_config.float_value)), "fval")) {
        nvs_close(h);
        return ESP_FAIL;
    }

    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit config: %s", esp_err_to_name(err));
    }
    nvs_close(h);
    return err;
}

esp_err_t storage_set_ota_password(const char *password)
{
    if (password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t password_len = strlen(password);
    if (password_len < 8 || password_len > APP_OTA_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    char previous[sizeof(g_config.ota_password)];
    strlcpy(previous, g_config.ota_password, sizeof(previous));
    strlcpy(g_config.ota_password, password, sizeof(g_config.ota_password));

    esp_err_t err = storage_save();
    if (err != ESP_OK) {
        strlcpy(g_config.ota_password, previous, sizeof(g_config.ota_password));
    }
    return err;
}