#include "display_demo.h"

#include "app.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <stdio.h>

#define LCD_HOST SPI2_HOST
#define LCD_H_RES 480
#define LCD_V_RES 320
#define LCD_BUFFER_LINES 24
#define LCD_PIN_SCLK GPIO_NUM_18
#define LCD_PIN_MOSI GPIO_NUM_23
#define LCD_PIN_MISO GPIO_NUM_NC
#define LCD_PIN_CS GPIO_NUM_27
#define LCD_PIN_DC GPIO_NUM_16
#define LCD_PIN_RST GPIO_NUM_17

static const char *TAG = "display";
static esp_lcd_panel_handle_t panel;
static lv_display_t *display;
static lv_obj_t *screens[3];
static lv_obj_t *page_indicators[3];
static lv_obj_t *temperature_arc;
static lv_obj_t *temperature_value;
static lv_obj_t *pressure_arc;
static lv_obj_t *pressure_value;
static lv_obj_t *pid_arc;
static lv_obj_t *pid_value;
static lv_obj_t *ip_value;
static lv_obj_t *ble_value;
static lv_obj_t *heap_value;
static lv_obj_t *uptime_value;
static uint8_t active_page;

static uint32_t display_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool display_flush_complete(esp_lcd_panel_io_handle_t io,
                                   esp_lcd_panel_io_event_data_t *event,
                                   void *user_ctx)
{
    lv_display_t **display_ref = user_ctx;
    if (*display_ref != NULL) {
        lv_display_flush_ready(*display_ref);
    }
    return false;
}

static void display_flush(lv_display_t *lv_display, const lv_area_t *area,
                          uint8_t *pixels)
{
    lv_draw_sw_rgb565_swap(pixels,
                           (uint32_t)(area->x2 - area->x1 + 1) *
                           (uint32_t)(area->y2 - area->y1 + 1));
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                                              area->x2 + 1, area->y2 + 1,
                                              pixels);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD flush failed: %s", esp_err_to_name(err));
        lv_display_flush_ready(lv_display);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y,
                            int width, int height, int font_size,
                            uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_font(label, font_size >= 20 ? &lv_font_montserrat_20 :
                                                       &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

static void style_screen(lv_obj_t *screen, const char *title, uint8_t page)
{
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101827), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *heading = make_label(screen, title, 20, 12, 340, 30, 20, 0xF1F5F9);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_LEFT, 0);
    page_indicators[page] = make_label(screen, "DEMO 1/3", 365, 16, 95, 22, 14, 0x94A3B8);
}

static lv_obj_t *make_arc(lv_obj_t *parent, int x, uint32_t color)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, 150, 150);
    lv_obj_set_pos(arc, x, 72);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_value(arc, 0);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x263449), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(arc, 0, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    return arc;
}

static void create_screens(void)
{
    screens[0] = lv_obj_create(NULL);
    style_screen(screens[0], "SENSOR OVERVIEW", 0);
    temperature_arc = make_arc(screens[0], 55, 0x38BDF8);
    pressure_arc = make_arc(screens[0], 275, 0xA78BFA);
    temperature_value = make_label(screens[0], "--.- C", 75, 126, 110, 30, 20, 0xF8FAFC);
    pressure_value = make_label(screens[0], "---- hPa", 295, 126, 110, 30, 20, 0xF8FAFC);
    make_label(screens[0], "TEMPERATURE", 60, 238, 140, 24, 14, 0x94A3B8);
    make_label(screens[0], "PRESSURE", 280, 238, 140, 24, 14, 0x94A3B8);
    make_label(screens[0], "BMP180  |  GPIO21/22  |  1 second updates",
               20, 285, 440, 20, 14, 0x64748B);

    screens[1] = lv_obj_create(NULL);
    style_screen(screens[1], "PID SENSOR", 1);
    pid_arc = make_arc(screens[1], 40, 0x34D399);
    lv_obj_set_pos(pid_arc, 45, 80);
    lv_obj_set_size(pid_arc, 175, 175);
    pid_value = make_label(screens[1], "---- mV", 70, 145, 125, 30, 20, 0xF8FAFC);
    lv_obj_t *pid_info = make_label(screens[1],
                                    "GPIO32 / ADC1\nCalibrated reading",
                                    245, 112, 210, 60, 20, 0xCBD5E1);
    lv_obj_set_style_text_align(pid_info, LV_TEXT_ALIGN_LEFT, 0);
    make_label(screens[1], "Live sensor demo", 245, 194, 210, 28, 14, 0x64748B);

    screens[2] = lv_obj_create(NULL);
    style_screen(screens[2], "DEVICE STATUS", 2);
    ip_value = make_label(screens[2], "IP: ---.---.---.---", 28, 82, 420, 32, 20, 0x38BDF8);
    ble_value = make_label(screens[2], "BLE clients: -", 28, 132, 205, 30, 20, 0xF8FAFC);
    heap_value = make_label(screens[2], "Free heap: ----- KB", 248, 132, 205, 30, 20, 0xF8FAFC);
    uptime_value = make_label(screens[2], "Uptime: -- s", 28, 190, 420, 30, 20, 0xCBD5E1);
    make_label(screens[2], "ESP32-WROOM  |  AUTO DEMO", 20, 285, 440, 20, 14, 0x64748B);
}

static void update_screen(lv_timer_t *timer)
{
    float temperature_c;
    int32_t pressure_pa;
    if (app_get_bmx_reading(&temperature_c, &pressure_pa)) {
        const int temperature_gauge = (int)((temperature_c + 10.0f) * 100.0f / 60.0f);
        const int pressure_hpa = pressure_pa / 100;
        const int pressure_gauge = pressure_hpa - 900;
        lv_arc_set_value(temperature_arc,
                         temperature_gauge < 0 ? 0 : temperature_gauge > 100 ? 100 : temperature_gauge);
        lv_arc_set_value(pressure_arc,
                         pressure_gauge < 0 ? 0 : pressure_gauge > 200 ? 100 : pressure_gauge / 2);
        lv_label_set_text_fmt(temperature_value, "%.1f C", (double)temperature_c);
        lv_label_set_text_fmt(pressure_value, "%d hPa", pressure_hpa);
    } else {
        lv_label_set_text(temperature_value, "WAIT");
        lv_label_set_text(pressure_value, "WAIT");
    }

    int32_t pid_mv;
    if (app_get_pid_millivolts(&pid_mv)) {
        const int pid_gauge = pid_mv > 3500 ? 100 : (int)(pid_mv * 100 / 3500);
        lv_arc_set_value(pid_arc, pid_gauge);
        lv_label_set_text_fmt(pid_value, "%ld mV", (long)pid_mv);
    } else {
        lv_label_set_text(pid_value, "WAIT");
    }

    char ip[16];
    app_get_ip_string(ip, sizeof(ip));
    lv_label_set_text_fmt(ip_value, "IP: %s", ip);
    lv_label_set_text_fmt(ble_value, "BLE clients: %d", app_get_ble_client_count());
    lv_label_set_text_fmt(heap_value, "Free heap: %u KB",
                          (unsigned)(esp_get_free_heap_size() / 1024));
    lv_label_set_text_fmt(uptime_value, "Uptime: %llu s",
                          (unsigned long long)(esp_timer_get_time() / 1000000));
}

static void change_page(lv_timer_t *timer)
{
    active_page = (active_page + 1) % 3;
    lv_screen_load(screens[active_page]);
    for (uint8_t i = 0; i < 3; i++) {
        lv_label_set_text_fmt(page_indicators[i], "DEMO %u/3", (unsigned)(i + 1));
    }
}

static void lvgl_task(void *arg)
{
    while (true) {
        uint32_t next_ms = lv_timer_handler();
        if (next_ms < 10) {
            next_ms = 10;
        } else if (next_ms > 100) {
            next_ms = 100;
        }
        vTaskDelay(pdMS_TO_TICKS(next_ms));
    }
}

esp_err_t app_display_demo_init(void)
{
    const spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = LCD_PIN_MISO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_BUFFER_LINES * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LCD SPI bus: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = display_flush_complete,
        .user_ctx = &display,
    };
    esp_lcd_panel_io_handle_t io;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LCD SPI IO: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7796(io, &panel_config, &panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7796 panel: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_reset(panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_swap_xy(panel, true);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_mirror(panel, true, false);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(panel, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ST7796 panel: %s", esp_err_to_name(err));
        return err;
    }

    lv_init();
    lv_tick_set_cb(display_tick_ms);
    display = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (display == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, display_flush);
    lv_display_set_user_data(display, panel);

    const size_t buffer_size = LCD_H_RES * LCD_BUFFER_LINES * sizeof(uint16_t);
    void *buffer1 = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buffer2 = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buffer1 == NULL || buffer2 == NULL) {
        heap_caps_free(buffer1);
        heap_caps_free(buffer2);
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers (%u bytes each)",
                 (unsigned)buffer_size);
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(display, buffer1, buffer2, buffer_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    create_screens();
    lv_screen_load(screens[0]);
    lv_timer_create(update_screen, 1000, NULL);
    lv_timer_create(change_page, 7000, NULL);

    if (xTaskCreate(lvgl_task, "display_ui", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LVGL task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ST7796/LVGL demo ready: 480x320, 3 auto-changing pages");
    ESP_LOGI(TAG, "LVGL draw buffers use %u bytes total",
             (unsigned)(buffer_size * 2));
    return ESP_OK;
}
