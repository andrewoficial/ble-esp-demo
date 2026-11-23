#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_BLE_NAME_MAX_LEN 17
#define APP_OTA_PASSWORD_MAX_LEN 63
#define APP_HARDWARE_VERSION "0.1.1"

// ─── Конфигурация, которая сохраняется в NVS ───
typedef struct {
    char    wifi_ssid[64];
    char    wifi_pass[64];
    char    ota_password[APP_OTA_PASSWORD_MAX_LEN + 1];
    char    device_name[APP_BLE_NAME_MAX_LEN + 1];
    uint16_t ble_mtu;          // предпочитаемый MTU для NimBLE
    float   float_value;       // произвольный float
    int32_t int_value;         // произвольный int
    int32_t pid_offset_mv;
    float   pid_multiplier;
    uint32_t pid_avg_points;
    float bmx_temp_multiplier;
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

// PID analog sensor
esp_err_t app_pid_sensor_init(void);
bool app_get_pid_millivolts(int32_t *millivolts);
uint32_t app_get_pid_sample_count(void);
void app_get_pid_config(int32_t *offset_mv, float *multiplier, uint32_t *avg_points);
esp_err_t app_pid_update_config(int32_t offset_mv, float multiplier, uint32_t avg_points);

// BMP180 sensor
esp_err_t app_bmx_sensor_init(void);
bool app_get_bmx_reading(float *temperature_c, int32_t *pressure_pa);
float app_get_bmx_temp_multiplier(void);
esp_err_t app_bmx_update_temp_multiplier(float multiplier);

// ble_nus.c
esp_err_t ble_nus_init(void);
esp_err_t ble_nus_set_name(const char *name);
void ble_nus_update_advertising(void);
void ble_nus_get_advertising_description(char *buf, size_t len);
void ble_nus_get_service_description(char *buf, size_t len);

// web_server.c
void web_server_start(void);