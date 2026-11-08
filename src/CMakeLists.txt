# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

idf_component_register(
	SRCS "main.c" "storage.c" "protocol.c" "ble_nus.c" "web_server.c"
	INCLUDE_DIRS "."
	REQUIRES
		app_update
		bt
		esp_app_format
		esp_driver_tsens
		esp_driver_uart
		esp_event
		esp_http_server
		esp_netif
		esp_wifi
		nvs_flash
)
