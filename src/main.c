#include "app.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/temp_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "main";

// ─── Температура ───
float app_get_chip_temp_c(void)
{
#if CONFIG_IDF_TARGET_ESP32
    // На классическом ESP32 используем legacy-датчик.
    // temperature_sens_read() возвращает °F — переводим в °C.
    float tf = temperature_sens_read();
    return (tf - 32.0f) * 5.0f / 9.0f;
#else
    // Для C3/S3 и др. — новый API (при необходимости).
    // Здесь оставлено для совместимости.
    return 0.0f;
#endif
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

    // Temperature sensor (legacy API для ESP32)
    temperature_sensor_handle_t tsens = NULL;
    temperature_sensor_config_t tsens_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    temperature_sensor_install(&tsens_cfg, &tsens);
    temperature_sensor_enable(tsens);

    esp_netif_init();
    esp_event_loop_create_default();

    wifi_init_sta();

    ble_nus_init();          // NimBLE + NUS + реклама
    web_server_start();      // HTTP-клиентская страничка

    xTaskCreate(serial_task, "serial", 4096, NULL, 5, NULL);

    // Периодическое обновление рекламы (температура меняется)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ble_nus_update_advertising();
    }
}