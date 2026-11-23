#include "app.h"

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/uart.h"
#include "soc/soc_caps.h"
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "main";
#if SOC_TEMP_SENSOR_SUPPORTED
static temperature_sensor_handle_t tsens;
#endif
static adc_oneshot_unit_handle_t pid_adc_unit;
static adc_cali_handle_t pid_adc_calibration;
static adc_channel_t pid_adc_channel;
static portMUX_TYPE pid_lock = portMUX_INITIALIZER_UNLOCKED;
static bool pid_reading_ready;
static int32_t pid_millivolts;
static uint32_t pid_samples;

static void pid_reset_reading_locked(void)
{
    pid_reading_ready = false;
    pid_millivolts = 0;
    pid_samples = 0;
}

static void pid_sensor_task(void *arg)
{
    uint64_t sample_sum_mv = 0;
    uint32_t sample_count = 0;
    int32_t previous_offset = -1;
    float previous_multiplier = -1.0f;
    uint32_t previous_points = 0;
    esp_err_t previous_read_error = ESP_OK;

    while (true) {
        int32_t offset_mv;
        float multiplier;
        uint32_t avg_points;
        portENTER_CRITICAL(&pid_lock);
        offset_mv = g_config.pid_offset_mv;
        multiplier = g_config.pid_multiplier;
        avg_points = g_config.pid_avg_points;
        portEXIT_CRITICAL(&pid_lock);

        if (offset_mv != previous_offset || multiplier != previous_multiplier ||
            avg_points != previous_points) {
            sample_sum_mv = 0;
            sample_count = 0;
            previous_offset = offset_mv;
            previous_multiplier = multiplier;
            previous_points = avg_points;
            portENTER_CRITICAL(&pid_lock);
            pid_reset_reading_locked();
            portEXIT_CRITICAL(&pid_lock);
        }

        int raw = 0;
        int input_mv = 0;
        esp_err_t err = adc_oneshot_read(pid_adc_unit, pid_adc_channel, &raw);
        if (err == ESP_OK && pid_adc_calibration != NULL) {
            err = adc_cali_raw_to_voltage(pid_adc_calibration, raw, &input_mv);
        } else if (err == ESP_OK) {
            input_mv = (raw * 3300) / 4095;
        }

        if (err != ESP_OK) {
            if (err != previous_read_error) {
                ESP_LOGE(TAG, "PID ADC read failed: %s", esp_err_to_name(err));
                previous_read_error = err;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        previous_read_error = ESP_OK;

        sample_sum_mv += (uint32_t)input_mv;
        sample_count++;

        if (sample_count >= avg_points) {
            double calibrated_mv = ((double)sample_sum_mv / sample_count + offset_mv) * multiplier;
            if (calibrated_mv < 0.0) {
                calibrated_mv = 0.0;
            } else if (calibrated_mv > 999999.0) {
                calibrated_mv = 999999.0;
            }

            portENTER_CRITICAL(&pid_lock);
            if (g_config.pid_offset_mv == offset_mv &&
                g_config.pid_multiplier == multiplier &&
                g_config.pid_avg_points == avg_points) {
                pid_millivolts = (int32_t)lround(calibrated_mv);
                pid_reading_ready = true;
                pid_samples = sample_count;
            } else {
                pid_reset_reading_locked();
            }
            portEXIT_CRITICAL(&pid_lock);
            sample_sum_mv = 0;
            sample_count = 0;
        } else {
            portENTER_CRITICAL(&pid_lock);
            pid_samples = sample_count;
            portEXIT_CRITICAL(&pid_lock);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t app_pid_sensor_init(void)
{
    adc_unit_t unit;
    esp_err_t err = adc_oneshot_io_to_channel(32, &unit, &pid_adc_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO32 is not available as an ADC input: %s", esp_err_to_name(err));
        return err;
    }
    if (unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "PID sensor must use ADC1 to operate while Wi-Fi is active");
        return ESP_ERR_NOT_SUPPORTED;
    }

    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    err = adc_oneshot_new_unit(&unit_config, &pid_adc_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize PID ADC: %s", esp_err_to_name(err));
        return err;
    }

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(pid_adc_unit, pid_adc_channel, &channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure PID ADC: %s", esp_err_to_name(err));
        return err;
    }

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    const adc_cali_line_fitting_config_t calibration_config = {
        .unit_id = unit,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
        .default_vref = 1100,
    };
    err = adc_cali_create_scheme_line_fitting(&calibration_config, &pid_adc_calibration);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PID ADC calibration unavailable; using approximate voltage: %s",
                 esp_err_to_name(err));
        pid_adc_calibration = NULL;
    }
#else
    ESP_LOGW(TAG, "PID ADC calibration scheme unavailable; using approximate voltage");
#endif

    if (xTaskCreate(pid_sensor_task, "pid_sensor", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PID sensor task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "PID sensor initialized on GPIO32 / ADC1");
    return ESP_OK;
}

bool app_get_pid_millivolts(int32_t *millivolts)
{
    if (millivolts == NULL) {
        return false;
    }

    portENTER_CRITICAL(&pid_lock);
    bool ready = pid_reading_ready;
    *millivolts = pid_millivolts;
    portEXIT_CRITICAL(&pid_lock);
    return ready;
}

uint32_t app_get_pid_sample_count(void)
{
    portENTER_CRITICAL(&pid_lock);
    uint32_t samples = pid_samples;
    portEXIT_CRITICAL(&pid_lock);
    return samples;
}

void app_get_pid_config(int32_t *offset_mv, float *multiplier, uint32_t *avg_points)
{
    portENTER_CRITICAL(&pid_lock);
    if (offset_mv != NULL) {
        *offset_mv = g_config.pid_offset_mv;
    }
    if (multiplier != NULL) {
        *multiplier = g_config.pid_multiplier;
    }
    if (avg_points != NULL) {
        *avg_points = g_config.pid_avg_points;
    }
    portEXIT_CRITICAL(&pid_lock);
}

esp_err_t app_pid_update_config(int32_t offset_mv, float multiplier, uint32_t avg_points)
{
    if (offset_mv < 0 || offset_mv > 999999 || !isfinite(multiplier) ||
        multiplier < 0.0f || multiplier > 999999.0f ||
        avg_points < 1 || avg_points > 999999) {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t old_offset;
    float old_multiplier;
    uint32_t old_avg_points;
    portENTER_CRITICAL(&pid_lock);
    old_offset = g_config.pid_offset_mv;
    old_multiplier = g_config.pid_multiplier;
    old_avg_points = g_config.pid_avg_points;
    g_config.pid_offset_mv = offset_mv;
    g_config.pid_multiplier = multiplier;
    g_config.pid_avg_points = avg_points;
    pid_reset_reading_locked();
    portEXIT_CRITICAL(&pid_lock);

    esp_err_t err = storage_save();
    if (err != ESP_OK) {
        portENTER_CRITICAL(&pid_lock);
        g_config.pid_offset_mv = old_offset;
        g_config.pid_multiplier = old_multiplier;
        g_config.pid_avg_points = old_avg_points;
        pid_reset_reading_locked();
        portEXIT_CRITICAL(&pid_lock);
    }
    return err;
}

// ─── Температура ───
float app_get_chip_temp_c(void)
{
#if SOC_TEMP_SENSOR_SUPPORTED
    float temp_c = 0.0f;
    if (tsens) {
        if (temperature_sensor_get_celsius(tsens, &temp_c) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read chip temperature");
        }
    }
    return temp_c;
#else
    return 0.0f;
#endif
}

const char *app_get_firmware_version(void)
{
    return esp_app_get_description()->version;
}

// ─── IP ───
void app_get_ip_string(char *buf, size_t len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(buf, len, "0.0.0.0");
    }
}

// ─── Serial (UART0) ───
// Для ESP32 WROOM это UART через USB-UART мост.
// Если у вас чип с USB Serial/JTAG (C3/S3), замените на usb_serial_jtag_read_bytes.
static void serial_task(void *arg)
{
    const uart_port_t uart = UART_NUM_0;
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_driver_install(uart, 1024, 0, 0, NULL, 0);
    uart_param_config(uart, &cfg);

    char line[256];
    size_t pos = 0;
    uint8_t ch;

    while (1) {
        if (uart_read_bytes(uart, &ch, 1, portMAX_DELAY) == 1) {
            if (ch == '\n' || ch == '\r') {
                if (pos > 0) {
                    line[pos] = '\0';
                    char resp[512];
                    protocol_execute(line, resp, sizeof(resp));
                    uart_write_bytes(uart, resp, strlen(resp));
                    uart_write_bytes(uart, "\r\n", 2);
                    pos = 0;
                }
            } else if (pos < sizeof(line) - 1) {
                line[pos++] = (char)ch;
            }
        }
    }
}

// ─── WiFi STA ───
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retry...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        // IP изменился — обновляем рекламу BLE
        ble_nus_update_advertising();
    }
}

static void wifi_init_sta(void)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, g_config.wifi_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, g_config.wifi_pass, sizeof(wc.sta.password) - 1);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
}

esp_err_t app_wifi_apply_config(void)
{
    wifi_config_t wc = {0};
    size_t ssid_len = strlen(g_config.wifi_ssid);
    size_t pass_len = strlen(g_config.wifi_pass);

    if (ssid_len == 0 || ssid_len > sizeof(wc.sta.ssid) ||
        pass_len >= sizeof(wc.sta.password)) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(wc.sta.ssid, g_config.wifi_ssid, ssid_len);
    memcpy(wc.sta.password, g_config.wifi_pass, pass_len);

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        return err;
    }

    return esp_wifi_connect();
}

// ─── Точка входа ───
void app_main(void)
{
    ESP_LOGI(TAG, "Mock device starting...");

    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    storage_init();
    storage_load();

#if SOC_TEMP_SENSOR_SUPPORTED
    temperature_sensor_config_t tsens_cfg = {
        .range_min = -10,
        .range_max = 80,
    };
    ESP_ERROR_CHECK(temperature_sensor_install(&tsens_cfg, &tsens));
    ESP_ERROR_CHECK(temperature_sensor_enable(tsens));
#endif

    esp_netif_init();
    esp_event_loop_create_default();

    wifi_init_sta();

    err = app_bmx_sensor_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BMP180 sensor unavailable: %s", esp_err_to_name(err));
    }

    err = app_pid_sensor_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PID sensor unavailable: %s", esp_err_to_name(err));
    }

    ble_nus_init();          // NimBLE + NUS + реклама
    web_server_start();      // HTTP-клиентская страничка

    xTaskCreate(serial_task, "serial", 4096, NULL, 5, NULL);

    // Периодическое обновление рекламы (температура меняется)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ble_nus_update_advertising();
    }
}