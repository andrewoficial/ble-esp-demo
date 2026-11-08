#include "app.h"

#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ─── Единый обработчик команд для BLE, HTTP и Serial ───
// Все команды — простой текст. Ответ тоже текст.
//
// Поддерживаемые команды:
//   ver                 → версия прошивки
//   set_wifi_ssid <s>   → сохранить SSID
//   set_wifi_pass <s>   → сохранить пароль
//   set_mtu <n>         → сохранить предпочитаемый MTU
//   get_ble_clients     → количество подключённых BLE-клиентов
//   temp                → температура чипа
//   set_float <f>       → сохранить float
//   get_float           → прочитать float
//   set_int <i>         → сохранить int
//   get_int             → прочитать int
//
// ─── План OTA (пока не реализован) ───
// 1. ota_begin <size> <version> <chunks> <crc32>
// 2. ota_data <chunk_idx> <chunk_crc32> <hex_data>
//    → ответ "OK <chunk_idx>" или "ERR <chunk_idx>", клиент повторяет пакет.
// 3. ota_end → проверка CRC всего файла, запись в раздел OTA, перезагрузка.
// Пока все OTA-команды отвечают "OTA not implemented (mock)".

void protocol_execute(const char *cmd, char *response, size_t max_len)
{
    if (!cmd || !response || max_len == 0) return;

    char tmp[256];
    strlcpy(tmp, cmd, sizeof(tmp));
    tmp[strcspn(tmp, "\r\n")] = '\0';

    if (strcmp(tmp, "ver") == 0) {
        snprintf(response, max_len, "FW: %s", g_config.fw_version);
        return;
    }

    if (strncmp(tmp, "set_wifi_ssid ", 14) == 0) {
        strlcpy(g_config.wifi_ssid, tmp + 14, sizeof(g_config.wifi_ssid));
        storage_save();
        snprintf(response, max_len, "OK ssid=%s", g_config.wifi_ssid);
        return;
    }

    if (strncmp(tmp, "set_wifi_pass ", 14) == 0) {
        strlcpy(g_config.wifi_pass, tmp + 14, sizeof(g_config.wifi_pass));
        storage_save();
        snprintf(response, max_len, "OK pass changed");
        return;
    }

    if (strncmp(tmp, "set_mtu ", 8) == 0) {
        int v = atoi(tmp + 8);
        if (v >= 23 && v <= 517) {
            g_config.ble_mtu = (uint16_t)v;
            storage_save();
            snprintf(response, max_len, "OK mtu=%u", g_config.ble_mtu);
        } else {
            snprintf(response, max_len, "ERR mtu range 23..517");
        }
        return;
    }

    if (strcmp(tmp, "get_ble_clients") == 0) {
        snprintf(response, max_len, "BLE clients: %d", app_get_ble_client_count());
        return;
    }

    if (strcmp(tmp, "temp") == 0) {
        snprintf(response, max_len, "Temp: %.2f C", app_get_chip_temp_c());
        return;
    }

    if (strncmp(tmp, "set_float ", 10) == 0) {
        g_config.float_value = strtof(tmp + 10, NULL);
        storage_save();
        snprintf(response, max_len, "OK float=%.4f", g_config.float_value);
        return;
    }

    if (strcmp(tmp, "get_float") == 0) {
        snprintf(response, max_len, "Float: %.4f", g_config.float_value);
        return;
    }

    if (strncmp(tmp, "set_int ", 8) == 0) {
        g_config.int_value = (int32_t)atoi(tmp + 8);
        storage_save();
        snprintf(response, max_len, "OK int=%ld", (long)g_config.int_value);
        return;
    }

    if (strcmp(tmp, "get_int") == 0) {
        snprintf(response, max_len, "Int: %ld", (long)g_config.int_value);
        return;
    }

    // Заглушки OTA
    if (strncmp(tmp, "ota_", 4) == 0) {
        snprintf(response, max_len, "OTA not implemented (mock)");
        return;
    }

    snprintf(response, max_len, "ERR unknown command");
}