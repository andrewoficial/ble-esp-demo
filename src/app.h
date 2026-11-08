#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ─── Конфигурация, которая сохраняется в NVS в виде JSON ───
typedef struct {
    char    wifi_ssid[64];
    char    wifi_pass[64];
    uint16_t ble_mtu;          // предпочитаемый MTU для NimBLE
    float   float_value;       // произвольный float
    int32_t int_value;         // произвольный int
    char    fw_version[16];    // версия прошивки
} app_config_t;

extern app_config_t g_config;

// storage.c
void storage_init(void);
void storage_load(void);
void storage_save(void);

// protocol.c
// Единая точка выполнения команд для BLE / HTTP / Serial.
// Возвращает ответ в response (не более max_len байт).
void protocol_execute(const char *cmd, char *response, size_t max_len);

// Температура чипа (реализация в main.c, чтобы не плодить файлы)
float app_get_chip_temp_c(void);
void  app_get_ip_string(char *buf, size_t len);
int   app_get_ble_client_count(void);

// ble_nus.c
void ble_nus_init(void);
void ble_nus_update_advertising(void);

// web_server.c
void web_server_start(void);