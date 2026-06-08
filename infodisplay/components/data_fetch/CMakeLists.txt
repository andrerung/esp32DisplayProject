idf_component_register(
    SRCS "data_fetch.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_config esp_http_client mbedtls log freertos lwip
)
