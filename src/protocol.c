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
//   GETWIFISSID         → получить сохранённый SSID
//   GETWIFIPASS         → получить сохранённый пароль
//   set_mtu <n>         → сохранить предпочитаемый MTU
//   GETIP               → получить IP-адрес Wi-Fi
//   GETADVDESC          → получить данные advertising и scan response
//   GETADVSVC           → получить описание GATT-сервиса и характеристик
//   SETNAME<name>       → задать и сохранить имя BLE (до 17 байт)
//   LOGOFF              → отключить runtime-логи ESP-IDF
//   LOGON               → включить runtime-логи ESP-IDF
//   get_ble_clients     → количество подключённых BLE-клиентов
//   temp                → температура чипа
//   set_float <f>       → сохранить float
//   get_float           → прочитать float
//   set_int <i>         → сохранить int
//   get_int             → прочитать int
//
void protocol_execute(const char *cmd, char *response, size_t max_len)
{
    if (!cmd || !response || max_len == 0) return;

    char tmp[256];
    strlcpy(tmp, cmd, sizeof(tmp));
    tmp[strcspn(tmp, "\r\n")] = '\0';

    if (strcmp(tmp, "LOGOFF") == 0) {
        esp_log_level_set("*", ESP_LOG_NONE);
        snprintf(response, max_len, "OK logging off");
        return;
    }

    if (strcmp(tmp, "LOGON") == 0) {
        esp_log_level_set("*", ESP_LOG_INFO);
        snprintf(response, max_len, "OK logging on");
        return;
    }

    if (strcmp(tmp, "GETIP") == 0) {
        char ip[16];
        app_get_ip_string(ip, sizeof(ip));
        if (strcmp(ip, "0.0.0.0") == 0) {
            snprintf(response, max_len, "ERR WiFi not connected");
        } else {
            snprintf(response, max_len, "IP: %s", ip);
        }
        return;
    }

    if (strcmp(tmp, "GETADVDESC") == 0) {
        ble_nus_get_advertising_description(response, max_len);
        return;
    }

    if (strcmp(tmp, "GETADVSVC") == 0) {
        ble_nus_get_service_description(response, max_len);
        return;
    }

    if (strcmp(tmp, "GETWIFISSID") == 0) {
        snprintf(response, max_len, "SSID: [%s]", g_config.wifi_ssid);
        return;
    }

    if (strcmp(tmp, "GETWIFIPASS") == 0) {
        snprintf(response, max_len, "PASS: [%s]", g_config.wifi_pass);
        return;
    }

    if (strncmp(tmp, "SETNAME", 7) == 0) {
        const char *name = tmp + 7;
        while (*name == ' ') {
            name++;
        }

        size_t name_len = strlen(name);
        if (name_len == 0 || name_len > APP_BLE_NAME_MAX_LEN) {
            snprintf(response, max_len, "ERR name must be 1..%d bytes", APP_BLE_NAME_MAX_LEN);
            return;
        }

        char old_name[sizeof(g_config.device_name)];
        strlcpy(old_name, g_config.device_name, sizeof(old_name));
        strlcpy(g_config.device_name, name, sizeof(g_config.device_name));

        esp_err_t err = ble_nus_set_name(g_config.device_name);
        if (err != ESP_OK) {
            strlcpy(g_config.device_name, old_name, sizeof(g_config.device_name));
            ble_nus_set_name(old_name);
            snprintf(response, max_len, "ERR failed to update BLE name");
            return;
        }

        err = storage_save();
        if (err != ESP_OK) {
            strlcpy(g_config.device_name, old_name, sizeof(g_config.device_name));
            ble_nus_set_name(old_name);
            snprintf(response, max_len, "ERR failed to save BLE name");
            return;
        }

        snprintf(response, max_len, "OK name=%s", g_config.device_name);
        return;
    }

    if (strcmp(tmp, "ver") == 0) {
        snprintf(response, max_len, "FW: %s", app_get_firmware_version());
        return;
    }

    if (strncmp(tmp, "set_wifi_ssid ", 14) == 0) {
        const char *ssid = tmp + 14;
        size_t ssid_len = strlen(ssid);
        if (ssid_len == 0 || ssid_len > 32) {
            snprintf(response, max_len, "ERR SSID must be 1..32 bytes");
            return;
        }

        char old_ssid[sizeof(g_config.wifi_ssid)];
        strlcpy(old_ssid, g_config.wifi_ssid, sizeof(old_ssid));
        strlcpy(g_config.wifi_ssid, ssid, sizeof(g_config.wifi_ssid));
        esp_err_t err = storage_save();
        if (err != ESP_OK) {
            strlcpy(g_config.wifi_ssid, old_ssid, sizeof(g_config.wifi_ssid));
            snprintf(response, max_len, "ERR failed to save SSID");
            return;
        }

        err = app_wifi_apply_config();
        if (err != ESP_OK) {
            snprintf(response, max_len, "ERR SSID saved, WiFi reconnect failed: %s",
                     esp_err_to_name(err));
            return;
        }

        snprintf(response, max_len, "OK ssid=[%s], reconnecting", g_config.wifi_ssid);
        return;
    }

    if (strncmp(tmp, "set_wifi_pass ", 14) == 0) {
        const char *password = tmp + 14;
        size_t password_len = strlen(password);
        if (password_len >= sizeof(g_config.wifi_pass)) {
            snprintf(response, max_len, "ERR password must be at most 63 bytes");
            return;
        }

        char old_password[sizeof(g_config.wifi_pass)];
        strlcpy(old_password, g_config.wifi_pass, sizeof(old_password));
        strlcpy(g_config.wifi_pass, password, sizeof(g_config.wifi_pass));
        esp_err_t err = storage_save();
        if (err != ESP_OK) {
            strlcpy(g_config.wifi_pass, old_password, sizeof(g_config.wifi_pass));
            snprintf(response, max_len, "ERR failed to save password");
            return;
        }

        err = app_wifi_apply_config();
        if (err != ESP_OK) {
            snprintf(response, max_len, "ERR password saved, WiFi reconnect failed: %s",
                     esp_err_to_name(err));
            return;
        }

        snprintf(response, max_len, "OK password saved, reconnecting");
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

    snprintf(response, max_len, "ERR unknown command");
}