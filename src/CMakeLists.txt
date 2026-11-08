idf_component_register(
    SRCS "main.c" "storage.c" "protocol.c" "ble_nus.c" "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES 
        log          # <-- Добавлено для исправления ошибки с esp_log.h
        nvs_flash 
        json 
        bt 
        esp_http_server 
        driver 
        esp_wifi
)