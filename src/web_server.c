#include "app.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "web";

// ─── Минималистичная HTML-страница (клиент) ───
static const char *html_page =
"<!DOCTYPE html><html><head><meta charset='utf-8'><title>ESP32 Mock</title>"
"<style>body{font-family:monospace;margin:2em;}button{margin:2px;}</style></head><body>"
"<h2>ESP32 NUS Mock</h2>"
"<p>Temp: <b id='temp'>--</b> °C</p>"
"<p>IP: <b id='ip'>--</b></p>"
"<p>FW: <b id='fw'>--</b></p>"
"<p>BLE clients: <b id='ble'>--</b></p>"
"<hr>"
"<button onclick=\"cmd('temp')\">Temp</button>"
"<button onclick=\"cmd('ver')\">Version</button>"
"<button onclick=\"cmd('get_ble_clients')\">BLE clients</button>"
"<button onclick=\"cmd('ota_begin 0 0 0 0')\">OTA (mock)</button>"
"<pre id='out'></pre>"
"<script>"
"async function cmd(c){"
"  let r=await fetch('/api/cmd?c='+encodeURIComponent(c));"
"  document.getElementById('out').textContent=await r.text();"
"}"
"async function refresh(){"
"  let r=await fetch('/api/status'); let j=await r.json();"
"  document.getElementById('temp').textContent=j.temp.toFixed(1);"
"  document.getElementById('ip').textContent=j.ip;"
"  document.getElementById('fw').textContent=j.fw;"
"  document.getElementById('ble').textContent=j.ble_clients;"
"}"
"setInterval(refresh,3000); refresh();"
"</script></body></html>";

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char ip[16], json[256];
    app_get_ip_string(ip, sizeof(ip));
    snprintf(json, sizeof(json),
             "{\"temp\":%.2f,\"ip\":\"%s\",\"fw\":\"%s\",\"ble_clients\":%d}",
             app_get_chip_temp_c(), ip, g_config.fw_version, app_get_ble_client_count());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
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

void web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return;
    }

    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t uri_status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
    httpd_uri_t uri_cmd = { .uri = "/api/cmd", .method = HTTP_GET, .handler = cmd_handler };

    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_cmd);

    ESP_LOGI(TAG, "HTTP server started on port %d", cfg.server_port);
}