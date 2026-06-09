idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES display nvs_config udp_log wifi_manager data_fetch ota_handler
             dns_server http_server
             esp_event esp_netif log freertos app_update
)
