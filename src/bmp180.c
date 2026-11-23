#include "app.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

#define BMP180_ADDRESS 0x77

static const char *TAG = "bmp180";
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t bmp180_device;
static portMUX_TYPE bmx_lock = portMUX_INITIALIZER_UNLOCKED;
static bool reading_ready;
static float raw_temperature_c;
static int32_t pressure_pa;

typedef struct {
    int16_t ac1;
    int16_t ac2;
    int16_t ac3;
    uint16_t ac4;
    uint16_t ac5;
    uint16_t ac6;
    int16_t b1;
    int16_t b2;
    int16_t mb;
    int16_t mc;
    int16_t md;
} bmp180_calibration_t;

static bmp180_calibration_t calibration;

static esp_err_t read_registers(uint8_t reg, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(bmp180_device, &reg, 1, data, length, 100);
}

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    const uint8_t command[] = { reg, value };
    return i2c_master_transmit(bmp180_device, command, sizeof(command), 100);
}

static uint16_t read_u16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static esp_err_t read_calibration(void)
{
    uint8_t data[22];
    esp_err_t err = read_registers(0xAA, data, sizeof(data));
    if (err != ESP_OK) {
        return err;
    }

    calibration.ac1 = (int16_t)read_u16(&data[0]);
    calibration.ac2 = (int16_t)read_u16(&data[2]);
    calibration.ac3 = (int16_t)read_u16(&data[4]);
    calibration.ac4 = read_u16(&data[6]);
    calibration.ac5 = read_u16(&data[8]);
    calibration.ac6 = read_u16(&data[10]);
    calibration.b1 = (int16_t)read_u16(&data[12]);
    calibration.b2 = (int16_t)read_u16(&data[14]);
    calibration.mb = (int16_t)read_u16(&data[16]);
    calibration.mc = (int16_t)read_u16(&data[18]);
    calibration.md = (int16_t)read_u16(&data[20]);

    if (calibration.ac4 == 0 || calibration.ac5 == 0 || calibration.ac6 == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t read_measurement(float *temperature_c, int32_t *pressure,
                                 const char **failure_reason,
                                 uint16_t *uncompensated_temp_out,
                                 int32_t *uncompensated_pressure_out)
{
    uint8_t data[3];
    *failure_reason = "temperature conversion";
    *uncompensated_temp_out = 0;
    *uncompensated_pressure_out = 0;
    esp_err_t err = write_register(0xF4, 0x2E);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    err = read_registers(0xF6, data, 2);
    if (err != ESP_OK) {
        return err;
    }

    const int32_t uncompensated_temp = read_u16(data);
    *uncompensated_temp_out = (uint16_t)uncompensated_temp;
    const int64_t x1 = ((int64_t)(uncompensated_temp - calibration.ac6) * calibration.ac5) >> 15;
    const int64_t temp_divisor = x1 + calibration.md;
    if (temp_divisor == 0) {
        *failure_reason = "temperature divisor is zero";
        return ESP_ERR_INVALID_RESPONSE;
    }
    const int64_t x2 = ((int64_t)calibration.mc * 2048) / temp_divisor;
    const int64_t b5 = x1 + x2;
    const int64_t temperature_tenths = (b5 + 8) >> 4;

    *failure_reason = "pressure conversion";
    err = write_register(0xF4, 0x34);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    err = read_registers(0xF6, data, sizeof(data));
    if (err != ESP_OK) {
        return err;
    }
    const int32_t uncompensated_pressure =
        (((int32_t)data[0] << 16) | ((int32_t)data[1] << 8) | data[2]) >> 8;
    *uncompensated_pressure_out = uncompensated_pressure;

    const int64_t b6 = b5 - 4000;
    const int64_t b6_squared = (b6 * b6) >> 12;
    const int64_t pressure_x1 = ((int64_t)calibration.b2 * b6_squared) >> 11;
    const int64_t pressure_x2 = ((int64_t)calibration.ac2 * b6) >> 11;
    const int64_t pressure_x3 = pressure_x1 + pressure_x2;
    const int64_t b3 = ((((int64_t)calibration.ac1 * 4 + pressure_x3) + 2) >> 2);

    const int64_t x1_pressure = ((int64_t)calibration.ac3 * b6) >> 13;
    const int64_t x2_pressure = ((int64_t)calibration.b1 * b6_squared) >> 16;
    const int64_t x3_pressure = (x1_pressure + x2_pressure + 2) >> 2;
    const int64_t b4_term = x3_pressure + 32768;
    const int64_t pressure_delta = uncompensated_pressure - b3;
    if (b4_term <= 0 || pressure_delta < 0) {
        *failure_reason = "invalid pressure compensation input";
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint64_t b4 = ((uint64_t)calibration.ac4 * (uint64_t)b4_term) >> 15;
    if (b4 == 0) {
        *failure_reason = "pressure compensation divisor is zero";
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint64_t b7 = (uint64_t)pressure_delta * 50000;
    int64_t compensated_pressure = b7 < 0x80000000ULL
        ? (int64_t)((b7 * 2) / b4)
        : (int64_t)((b7 / b4) * 2);
    const int64_t final_x1 = ((compensated_pressure >> 8) *
                              (compensated_pressure >> 8) * 3038) >> 16;
    const int64_t final_x2 = (-7357 * compensated_pressure) >> 16;
    compensated_pressure += (final_x1 + final_x2 + 3791) >> 4;
    if (temperature_tenths < -400 || temperature_tenths > 850 ||
        compensated_pressure < 30000 || compensated_pressure > 120000) {
        *failure_reason = "compensated result outside sensor range";
        return ESP_ERR_INVALID_RESPONSE;
    }

    *failure_reason = NULL;
    *temperature_c = (float)temperature_tenths / 10.0f;
    *pressure = (int32_t)compensated_pressure;
    return ESP_OK;
}

static void bmp180_task(void *arg)
{
    esp_err_t previous_error = ESP_OK;
    while (true) {
        float temperature_c;
        int32_t pressure;
        const char *failure_reason;
        uint16_t uncompensated_temp;
        int32_t uncompensated_pressure;
        esp_err_t err = read_measurement(&temperature_c, &pressure, &failure_reason,
                                         &uncompensated_temp, &uncompensated_pressure);
        if (err == ESP_OK) {
            portENTER_CRITICAL(&bmx_lock);
            raw_temperature_c = temperature_c;
            pressure_pa = pressure;
            reading_ready = true;
            portEXIT_CRITICAL(&bmx_lock);
            if (previous_error != ESP_OK) {
                ESP_LOGI(TAG, "BMP180 measurements resumed");
            }
            previous_error = ESP_OK;
        } else {
            portENTER_CRITICAL(&bmx_lock);
            reading_ready = false;
            portEXIT_CRITICAL(&bmx_lock);
            if (err != previous_error) {
                if (err == ESP_ERR_INVALID_RESPONSE) {
                    ESP_LOGE(TAG, "BMP180 measurement rejected (%s): UT=%u UP=%ld",
                             failure_reason, (unsigned)uncompensated_temp,
                             (long)uncompensated_pressure);
                } else {
                    ESP_LOGE(TAG, "BMP180 measurement failed: %s",
                             esp_err_to_name(err));
                }
                previous_error = err;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t app_bmx_sensor_init(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_21,
        .scl_io_num = GPIO_NUM_22,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C on SDA GPIO21 / SCL GPIO22: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = i2c_master_probe(i2c_bus, BMP180_ADDRESS, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BMP180 not found at I2C address 0x77: %s", esp_err_to_name(err));
        return err;
    }
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BMP180_ADDRESS,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(i2c_bus, &device_config, &bmp180_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add BMP180 to I2C bus: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t chip_id = 0;
    err = read_registers(0xD0, &chip_id, 1);
    if (err != ESP_OK || chip_id != 0x55) {
        ESP_LOGE(TAG, "I2C device at 0x77 is not a BMP180 (chip ID 0x%02x)", chip_id);
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }
    err = read_calibration();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read BMP180 calibration data: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(bmp180_task, "bmp180", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create BMP180 measurement task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "BMP180 initialized on I2C address 0x77 (SDA GPIO21 / SCL GPIO22)");
    ESP_LOGI(TAG, "BMP180 measures temperature and pressure; humidity is not supported");
    return ESP_OK;
}

bool app_get_bmx_reading(float *temperature_c, int32_t *pressure)
{
    portENTER_CRITICAL(&bmx_lock);
    const bool ready = reading_ready;
    if (temperature_c != NULL) {
        *temperature_c = raw_temperature_c * g_config.bmx_temp_multiplier;
    }
    if (pressure != NULL) {
        *pressure = pressure_pa;
    }
    portEXIT_CRITICAL(&bmx_lock);
    return ready;
}

float app_get_bmx_temp_multiplier(void)
{
    portENTER_CRITICAL(&bmx_lock);
    const float multiplier = g_config.bmx_temp_multiplier;
    portEXIT_CRITICAL(&bmx_lock);
    return multiplier;
}

esp_err_t app_bmx_update_temp_multiplier(float multiplier)
{
    if (!isfinite(multiplier) || multiplier < 0.0f || multiplier > 999999.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&bmx_lock);
    const float old_multiplier = g_config.bmx_temp_multiplier;
    g_config.bmx_temp_multiplier = multiplier;
    portEXIT_CRITICAL(&bmx_lock);

    esp_err_t err = storage_save();
    if (err != ESP_OK) {
        portENTER_CRITICAL(&bmx_lock);
        g_config.bmx_temp_multiplier = old_multiplier;
        portEXIT_CRITICAL(&bmx_lock);
    }
    return err;
}
