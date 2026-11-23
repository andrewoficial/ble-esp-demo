#include "app.h"

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bmp180_web.h"
#include "web_page.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

static const char *TAG = "web";
#define OTA_RECV_CHUNK_SIZE (5U * 1024U)

typedef struct {
    bool active;
    size_t total_bytes;
    size_t written_bytes;
    char stage[32];
    char message[96];
} ota_status_t;

static bool ota_upload_in_progress;
static portMUX_TYPE ota_lock = portMUX_INITIALIZER_UNLOCKED;
static ota_status_t ota_status = { .stage = "Idle" };

static void ota_log(const char *level, const char *format, ...)
{
    char message[192];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    printf("[OTA][%s] %s\r\n", level, message);
    fflush(stdout);
}


static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    const char *body_end = strstr(dashboard_html, "</body>");
    if (body_end == NULL) {
        return httpd_resp_send(req, dashboard_html, HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = httpd_resp_send_chunk(req, dashboard_html,
                                          (ssize_t)(body_end - dashboard_html));
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_send_chunk(req, bmp180_dashboard_script,
                                HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_send_chunk(req, body_end, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char ip[16], json[640];
    app_get_ip_string(ip, sizeof(ip));
    wifi_ap_record_t ap_info = {0};
    bool wifi_connected = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
    int32_t pid_mv = 0;
    bool pid_ready = app_get_pid_millivolts(&pid_mv);
    int32_t pid_offset_mv;
    float pid_multiplier;
    uint32_t pid_avg_points;
    app_get_pid_config(&pid_offset_mv, &pid_multiplier, &pid_avg_points);
    float bmx_temperature_c = 0.0f;
    int32_t bmx_pressure_pa = 0;
    bool bmx_ready = app_get_bmx_reading(&bmx_temperature_c, &bmx_pressure_pa);
    float bmx_temp_multiplier = app_get_bmx_temp_multiplier();
    snprintf(json, sizeof(json),
             "{\"temp\":%.2f,\"ip\":\"%s\",\"fw\":\"%s\",\"ble_clients\":%d,"
             "\"wifi_connected\":%s,\"wifi_rssi\":%d,\"heap_free\":%u,"
             "\"heap_min\":%u,\"heap_largest\":%u,\"html_bytes\":%u,"
             "\"hw\":\"%s\",\"pid_ready\":%s,\"pid_mv\":%ld,\"pid_samples\":%lu,"
             "\"pid_offset_mv\":%ld,\"pid_multiplier\":%.6g,\"pid_avg_points\":%lu,"
             "\"bmx_ready\":%s,\"bmx_temp_c\":%.2f,\"bmx_pressure_pa\":%ld,"
             "\"bmx_temp_multiplier\":%.6g}",
             app_get_chip_temp_c(), ip, app_get_firmware_version(), app_get_ble_client_count(),
             wifi_connected ? "true" : "false", wifi_connected ? ap_info.rssi : 0,
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)(strlen(dashboard_html) + strlen(bmp180_dashboard_script)),
             APP_HARDWARE_VERSION,
             pid_ready ? "true" : "false", (long)pid_mv,
             (unsigned long)app_get_pid_sample_count(), (long)pid_offset_mv,
             (double)pid_multiplier, (unsigned long)pid_avg_points,
             bmx_ready ? "true" : "false", (double)bmx_temperature_c,
             (long)bmx_pressure_pa, (double)bmx_temp_multiplier);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    ota_status_t current;
    portENTER_CRITICAL(&ota_lock);
    current = ota_status;
    portEXIT_CRITICAL(&ota_lock);

    char json[256];
    snprintf(json, sizeof(json),
             "{\"active\":%s,\"total\":%u,\"written\":%u,\"stage\":\"%s\",\"message\":\"%s\"}",
             current.active ? "true" : "false",
             (unsigned)current.total_bytes,
             (unsigned)current.written_bytes,
             current.stage,
             current.message);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t cmd_handler(httpd_req_t *req)
{
    char query[128], cmd[128], resp[512];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_sendstr(req, "ERR no query");
        return ESP_OK;
    }
    if (httpd_query_key_value(query, "c", cmd, sizeof(cmd)) != ESP_OK) {
        httpd_resp_sendstr(req, "ERR no cmd");
        return ESP_OK;
    }
    protocol_execute(cmd, resp, sizeof(resp));
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static bool ota_try_start(size_t total_bytes)
{
    bool started = false;
    portENTER_CRITICAL(&ota_lock);
    if (!ota_upload_in_progress) {
        ota_upload_in_progress = true;
        ota_status.active = true;
        ota_status.total_bytes = total_bytes;
        ota_status.written_bytes = 0;
        strlcpy(ota_status.stage, "Starting", sizeof(ota_status.stage));
        ota_status.message[0] = '\0';
        started = true;
    }
    portEXIT_CRITICAL(&ota_lock);
    return started;
}

static void ota_status_update(const char *stage, size_t written_bytes, const char *message)
{
    portENTER_CRITICAL(&ota_lock);
    if (stage != NULL) {
        strlcpy(ota_status.stage, stage, sizeof(ota_status.stage));
    }
    ota_status.written_bytes = written_bytes;
    if (message != NULL) {
        strlcpy(ota_status.message, message, sizeof(ota_status.message));
    }
    portEXIT_CRITICAL(&ota_lock);
}

static void ota_set_finished(void)
{
    portENTER_CRITICAL(&ota_lock);
    ota_upload_in_progress = false;
    ota_status.active = false;
    portEXIT_CRITICAL(&ota_lock);
}

static void ota_complete_async_request(httpd_req_t *req)
{
    esp_err_t err = httpd_req_async_handler_complete(req);
    if (err != ESP_OK) {
        ota_log("ERROR", "Could not complete asynchronous request: %s", esp_err_to_name(err));
    }
}

static void ota_upload_task(void *arg)
{
    httpd_req_t *req = arg;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t ota_handle = 0;
    bool ota_started = false;
    char *buffer = NULL;
    httpd_err_code_t failure_status = HTTPD_500_INTERNAL_SERVER_ERROR;
    const char *failure_message = NULL;
    esp_err_t err;
    size_t written_bytes = 0;
    unsigned next_log_percent = 5;
    int64_t upload_start_us = esp_timer_get_time();

    if (update_partition == NULL) {
        failure_message = "No inactive OTA partition is available.";
        goto failed;
    }
    if (req->content_len == 0 || req->content_len > update_partition->size) {
        failure_status = HTTPD_413_CONTENT_TOO_LARGE;
        failure_message = "Firmware file is empty or larger than the OTA partition.";
        goto failed;
    }

    ota_status_update("Preparing flash", 0, NULL);
    ota_log("INFO", "Started: %u bytes, target=%s, read block=%u bytes, current FW=%s",
            (unsigned)req->content_len, update_partition->label,
            (unsigned)OTA_RECV_CHUNK_SIZE, esp_app_get_description()->version);

    int64_t erase_start_us = esp_timer_get_time();
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ota_log("ERROR", "esp_ota_begin failed: %s", esp_err_to_name(err));
        failure_message = "Could not start OTA update.";
        goto failed;
    }
    ota_started = true;
    ota_log("INFO", "Partition ready in %lld ms",
            (long long)((esp_timer_get_time() - erase_start_us) / 1000));

    buffer = malloc(OTA_RECV_CHUNK_SIZE);
    if (buffer == NULL) {
        failure_message = "Not enough memory to receive firmware.";
        goto failed;
    }

    size_t remaining = req->content_len;
    TickType_t last_received = xTaskGetTickCount();
    TickType_t last_wait_log = last_received;
    while (remaining > 0) {
        size_t chunk_len = remaining < OTA_RECV_CHUNK_SIZE ? remaining : OTA_RECV_CHUNK_SIZE;
        ota_status_update("Receiving data", written_bytes, NULL);
        int received = httpd_req_recv(req, buffer, chunk_len);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            TickType_t now = xTaskGetTickCount();
            if (now - last_wait_log >= pdMS_TO_TICKS(15000)) {
                ota_log("WARN", "Waiting for browser data: %u/%u bytes received",
                        (unsigned)written_bytes, (unsigned)req->content_len);
                last_wait_log = now;
            }
            if (now - last_received >= pdMS_TO_TICKS(60000)) {
                failure_status = HTTPD_408_REQ_TIMEOUT;
                failure_message = "Timed out while receiving firmware.";
                ota_log("ERROR", "Timed out waiting for upload data");
                goto failed;
            }
            continue;
        }
        if (received <= 0) {
            failure_message = "Firmware upload connection closed unexpectedly.";
            ota_log("ERROR", "Upload connection closed unexpectedly");
            goto failed;
        }

        size_t pending_written = written_bytes + (size_t)received;
        unsigned pending_percent = (unsigned)(pending_written * 100U / req->content_len);
        if (pending_percent >= next_log_percent) {
            ota_log("INFO", "Received %u/%u bytes; writing flash (about %u%%)",
                    (unsigned)pending_written, (unsigned)req->content_len, pending_percent);
        }

        ota_status_update("Writing flash", written_bytes, NULL);
        err = esp_ota_write(ota_handle, buffer, received);
        if (err != ESP_OK) {
            ota_log("ERROR", "Flash write failed: %s", esp_err_to_name(err));
            failure_message = "Writing firmware failed.";
            goto failed;
        }
        remaining -= received;
        written_bytes += received;
        ota_status_update("Receiving data", written_bytes, NULL);
        last_received = xTaskGetTickCount();
        last_wait_log = last_received;

        unsigned progress_percent = (unsigned)(written_bytes * 100U / req->content_len);
        if (progress_percent >= next_log_percent) {
            int64_t elapsed_ms = (esp_timer_get_time() - upload_start_us) / 1000;
            ota_log("INFO", "Flash progress %u%% (%u/%u bytes), %lld ms, free heap=%u",
                    progress_percent, (unsigned)written_bytes, (unsigned)req->content_len,
                    (long long)elapsed_ms, (unsigned)esp_get_free_heap_size());
            while (next_log_percent <= progress_percent) {
                next_log_percent += 5;
            }
        }
    }

    free(buffer);
    buffer = NULL;
    ota_status_update("Validating image", written_bytes, NULL);
    ota_log("INFO", "Upload received completely; validating image");
    err = esp_ota_end(ota_handle);
    ota_started = false;
    if (err != ESP_OK) {
        ota_log("ERROR", "Image validation failed: %s", esp_err_to_name(err));
        failure_status = HTTPD_400_BAD_REQUEST;
        failure_message = "Invalid application firmware image.";
        goto failed;
    }

    esp_app_desc_t new_app_desc;
    err = esp_ota_get_partition_description(update_partition, &new_app_desc);
    if (err != ESP_OK ||
        strcmp(new_app_desc.project_name, esp_app_get_description()->project_name) != 0) {
        ota_log("ERROR", "Uploaded firmware is not for this project");
        failure_status = HTTPD_400_BAD_REQUEST;
        failure_message = "Firmware is for a different project.";
        goto failed;
    }

    ota_log("INFO", "Image verified: project=%s version=%s",
            new_app_desc.project_name, new_app_desc.version);
    ota_status_update("Selecting boot image", written_bytes, NULL);
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ota_log("ERROR", "Selecting boot partition failed: %s", esp_err_to_name(err));
        failure_message = "Could not select new firmware.";
        goto failed;
    }

    ota_log("INFO", "Complete in %lld ms; rebooting into %s",
            (long long)((esp_timer_get_time() - upload_start_us) / 1000),
            new_app_desc.version);
    ota_status_update("Restarting", written_bytes, NULL);
    ota_set_finished();
    httpd_resp_set_type(req, "text/plain");
    err = httpd_resp_sendstr(req, "Firmware installed. Device is restarting.");
    if (err != ESP_OK) {
        ota_log("WARN", "Could not send success response: %s", esp_err_to_name(err));
    }
    ota_complete_async_request(req);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

failed:
    if (buffer != NULL) {
        free(buffer);
    }
    if (ota_started) {
        esp_err_t abort_err = esp_ota_abort(ota_handle);
        if (abort_err != ESP_OK) {
            ota_log("WARN", "esp_ota_abort failed: %s", esp_err_to_name(abort_err));
        }
    }
    ota_status_update("Failed", written_bytes, failure_message != NULL ? failure_message : "Unknown OTA error.");
    ota_log("ERROR", "Failed after %u/%u bytes: %s",
            (unsigned)written_bytes, (unsigned)req->content_len,
            failure_message != NULL ? failure_message : "Unknown OTA error.");
    ota_set_finished();
    if (failure_message != NULL) {
        httpd_resp_send_err(req, failure_status, failure_message);
    }
    ota_complete_async_request(req);
    vTaskDelete(NULL);
}

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    if (g_config.ota_password[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "Configure an OTA password over serial first.");
    }

    size_t password_len = httpd_req_get_hdr_value_len(req, "X-OTA-Password");
    char password[sizeof(g_config.ota_password)];
    if (password_len == 0 || password_len >= sizeof(password) ||
        httpd_req_get_hdr_value_str(req, "X-OTA-Password", password, sizeof(password)) != ESP_OK ||
        strcmp(password, g_config.ota_password) != 0) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid OTA password.");
    }

    if (!ota_try_start(req->content_len)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "An OTA update is already in progress.");
    }

    ota_log("INFO", "Request accepted: %u bytes", (unsigned)req->content_len);

    httpd_req_t *async_req = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) {
        ota_status_update("Failed", 0, "Could not start OTA request.");
        ota_set_finished();
        ota_log("ERROR", "Could not start asynchronous request: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not start OTA request.");
    }

    if (xTaskCreate(ota_upload_task, "ota_upload", 8192, async_req, 5, NULL) != pdPASS) {
        ota_status_update("Failed", 0, "Could not create OTA task.");
        ota_set_finished();
        ota_log("ERROR", "Could not create OTA worker task");
        httpd_resp_send_err(async_req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Could not create OTA task.");
        ota_complete_async_request(async_req);
    }
    return ESP_OK;
}

void web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return;
    }

    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t uri_status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
    httpd_uri_t uri_ota_status = { .uri = "/api/ota/status", .method = HTTP_GET, .handler = ota_status_handler };
    httpd_uri_t uri_cmd = { .uri = "/api/cmd", .method = HTTP_GET, .handler = cmd_handler };
    httpd_uri_t uri_ota = { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_upload_handler };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_ota_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_cmd));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_ota));

    ESP_LOGI(TAG, "HTTP server started on port %d", cfg.server_port);
}