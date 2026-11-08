#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_BLE_NAME_MAX_LEN 17
#define APP_OTA_PASSWORD_MAX_LEN 63

// ─── Конфигурация, которая сохраняется в NVS в виде JSON ───
typedef struct {
    char    wifi_ssid[64];
    char    wifi_pass[64];
    char    ota_password[APP_OTA_PASSWORD_MAX_LEN + 1];
    char    device_name[APP_BLE_NAME_MAX_LEN + 1];
    uint16_t ble_mtu;          // предпочитаемый MTU для NimBLE
    float   float_value;       // произвольный float
    int32_t int_value;         // произвольный int
} app_config_t;

extern app_config_t g_config;

// storage.c
void storage_init(void);
void storage_load(void);
esp_err_t storage_save(void);
esp_err_t storage_set_ota_password(const char *password);

// protocol.c
// Единая точка выполнения команд для BLE / HTTP / Serial.
// Возвращает ответ в response (не более max_len байт).
void protocol_execute(const char *cmd, char *response, size_t max_len);

// Температура чипа (реализация в main.c, чтобы не плодить файлы)
float app_get_chip_temp_c(void);
const char *app_get_firmware_version(void);
void  app_get_ip_string(char *buf, size_t len);
esp_err_t app_wifi_apply_config(void);
int   app_get_ble_client_count(void);

// ble_nus.c
esp_err_t ble_nus_init(void);
esp_err_t ble_nus_set_name(const char *name);
void ble_nus_update_advertising(void);
void ble_nus_get_advertising_description(char *buf, size_t len);
void ble_nus_get_service_description(char *buf, size_t len);

// web_server.c
void web_server_start(void);