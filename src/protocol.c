#include "app.h"

#include "esp_log.h"
#include <errno.h>
#include <math.h>
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
//   GETPID              → получить измеренное значение PID в мВ
//   SETOFSETPID<n>      → задать смещение PID в мВ
//   SETAMPPID<n>        → задать коэффициент PID
//   SETAVGPIONTSPID<n>  → задать количество точек усреднения
//   GETBMXTERM          → температура BMP180 в °C
//   GETBMXPRESS         → давление BMP180 в Pa
//   GETBMXTERMAMP       → получить множитель температуры BMP180
//   SETBMXTERMAMP<n>    → задать множитель температуры BMP180
//   GETBMXHYDM          → влажность (BMP180 не поддерживает)
//   SETBMXHYDMAMP<n>    → настройка влажности (BMP180 не поддерживает)
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
static const char *setting_value(const char *command, size_t prefix_len)
{
    const char *value = command + prefix_len;
    while (*value == ' ' || *value == '\t' || *value == '=') {
        value++;
    }
    return value;
}

static bool parse_integer_setting(const char *text, int32_t *value)
{
    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (text == end || errno == ERANGE || parsed < 0 || parsed > 999999) {
        return false;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    *value = (int32_t)parsed;
    return true;
}

static bool parse_multiplier_setting(const char *text, float *value)
{
    errno = 0;
    char *end = NULL;
    float parsed = strtof(text, &end);
    if (text == end || errno == ERANGE || !isfinite(parsed) ||
        parsed < 0.0f || parsed > 999999.0f) {
        return false;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    *value = parsed;
    return true;
}

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

    if (strcmp(tmp, "GETPID") == 0) {
        int32_t millivolts;
        if (app_get_pid_millivolts(&millivolts)) {
            snprintf(response, max_len, "PID: [%ld] mV", (long)millivolts);
        } else {
            snprintf(response, max_len, "PID: [not ready]");
        }
        return;
    }

    if (strcmp(tmp, "GETBMXTERM") == 0) {
        float temperature_c;
        if (app_get_bmx_reading(&temperature_c, NULL)) {
            snprintf(response, max_len, "BMXTERM: [%.2f] C", (double)temperature_c);
        } else {
            snprintf(response, max_len, "BMXTERM: [not ready]");
        }
        return;
    }

    if (strcmp(tmp, "GETBMXPRESS") == 0) {
        int32_t pressure_pa;
        if (app_get_bmx_reading(NULL, &pressure_pa)) {
            snprintf(response, max_len, "BMXPRESS: [%ld] Pa", (long)pressure_pa);
        } else {
            snprintf(response, max_len, "BMXPRESS: [not ready]");
        }
        return;
    }

    if (strcmp(tmp, "GETBMXHYDM") == 0 || strcmp(tmp, "GETBMXHYDMAMP") == 0) {
        snprintf(response, max_len, "ERR humidity is not supported by BMP180");
        return;
    }

    if (strcmp(tmp, "GETBMXTERMAMP") == 0) {
        snprintf(response, max_len, "BMXTERMAMP: [%.6g]",
                 (double)app_get_bmx_temp_multiplier());
        return;
    }

    if (strncmp(tmp, "SETBMXHYDMAMP", 13) == 0) {
        snprintf(response, max_len, "ERR humidity is not supported by BMP180");
        return;
    }

    if (strncmp(tmp, "SETBMXTERMAMP", 13) == 0) {
        float multiplier;
        if (!parse_multiplier_setting(setting_value(tmp, 13), &multiplier)) {
            snprintf(response, max_len, "ERR multiplier range 0..999999");
            return;
        }
        esp_err_t err = app_bmx_update_temp_multiplier(multiplier);
        if (err != ESP_OK) {
            snprintf(response, max_len, "ERR failed to save BMP180 temperature multiplier: %s",
                     esp_err_to_name(err));
            return;
        }
        snprintf(response, max_len, "OK bmx_temp_amp=%.6g", (double)multiplier);
        return;
    }

    if (strcmp(tmp, "GETOFFSETPID") == 0) {
        int32_t offset_mv;
        app_get_pid_config(&offset_mv, NULL, NULL);
        snprintf(response, max_len, "OFFSETPID: [%ld] mV", (long)offset_mv);
        return;
    }

    if (strcmp(tmp, "GETAMPPID") == 0) {
        float multiplier;
        app_get_pid_config(NULL, &multiplier, NULL);
        snprintf(response, max_len, "AMPPID: [%.6g]", (double)multiplier);
        return;
    }

    if (strcmp(tmp, "GETAVGPIONTSPID") == 0) {
        uint32_t avg_points;
        app_get_pid_config(NULL, NULL, &avg_points);
        snprintf(response, max_len, "AVGPIONTSPID: [%lu]", (unsigned long)avg_points);
        return;
    }

    if (strncmp(tmp, "SETOFSETPID", 11) == 0) {
        int32_t offset_mv;
        float multiplier;
        uint32_t avg_points;
        if (!parse_integer_setting(setting_value(tmp, 11), &offset_mv)) {
            snprintf(response, max_len, "ERR offset range 0..999999 mV");
            return;
        }
        app_get_pid_config(NULL, &multiplier, &avg_points);
        esp_err_t err = app_pid_update_config(offset_mv, multiplier, avg_points);
        if (err != ESP_OK) {
            snprintf(response, max_len, "ERR failed to save PID offset: %s", esp_err_to_name(err));
            return;
        }
        snprintf(response, max_len, "OK offset_pid_mv=%ld", (long)offset_mv);
        return;
    }

    if (strncmp(tmp, "SETAMPPID", 9) == 0) {
        int32_t offset_mv;
        float multiplier;
        uint32_t avg_points;
        if (!parse_multiplier_setting(setting_value(tmp, 9), &multiplier)) {
            snprintf(response, max_len, "ERR multiplier range 0..999999");
            return;
        }
        app_get_pid_config(&offset_mv, NULL, &avg_points);
        esp_err_t err = app_pid_update_config(offset_mv, multiplier, avg_points);
        if (err != ESP_OK) {
            snprintf(response, max_len, "ERR failed to save PID multiplier: %s", esp_err_to_name(err));
            return;
        }
        snprintf(response, max_len, "OK amp_pid=%.6g", (double)multiplier);
        return;
    }

    if (strncmp(tmp, "SETAVGPIONTSPID", 15) == 0) {
        int32_t parsed_points;
        int32_t offset_mv;
        float multiplier;
        uint32_t avg_points;
        if (!parse_integer_setting(setting_value(tmp, 15), &parsed_points) || parsed_points == 0) {
            snprintf(response, max_len, "ERR averaging points range 1..999999");
            return;
        }
        app_get_pid_config(&offset_mv, &multiplier, &avg_points);
        avg_points = (uint32_t)parsed_points;
        esp_err_t err = app_pid_update_config(offset_mv, multiplier, avg_points);
        if (err != ESP_OK) {
            snprintf(response, max_len, "ERR failed to save PID averaging points: %s", esp_err_to_name(err));
            return;
        }
        snprintf(response, max_len, "OK avg_points_pid=%lu", (unsigned long)avg_points);
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