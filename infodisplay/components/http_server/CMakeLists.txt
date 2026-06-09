idf_component_register(
    SRCS "http_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server nvs_config wifi_manager log freertos esp_timer esp_wifi ota_handler app_update
)
