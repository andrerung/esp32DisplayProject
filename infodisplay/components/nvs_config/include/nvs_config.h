#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/* NVS key names — max 15 chars (NVS limit) */
#define NVS_KEY_WIFI_SSID        "wifi_ssid"
#define NVS_KEY_WIFI_PASS        "wifi_pass"
#define NVS_KEY_WEATHER_KEY      "weather_key"
#define NVS_KEY_WEATHER_CITY     "weather_city"
#define NVS_KEY_CRYPTO_COINS     "crypto_coins"
#define NVS_KEY_CRYPTO_CURRENCY  "crypto_curr"
#define NVS_KEY_CRYPTO_INTERVAL  "crypto_ivl"
#define NVS_KEY_WEATHER_INTERVAL "weather_ivl"
#define NVS_KEY_LOG_UDP_HOST     "log_udp_host"
#define NVS_KEY_LOG_UDP_PORT     "log_udp_port"
#define NVS_KEY_TIMEZONE         "timezone"

/* Compile-time defaults */
#define NVS_DEFAULT_WEATHER_CITY     "London"
#define NVS_DEFAULT_CRYPTO_COINS     "bitcoin,ethereum,litecoin"
#define NVS_DEFAULT_CRYPTO_CURRENCY  "usd"
#define NVS_DEFAULT_CRYPTO_INTERVAL  60u
#define NVS_DEFAULT_WEATHER_INTERVAL 600u
#define NVS_DEFAULT_LOG_UDP_PORT     5555u
#define NVS_DEFAULT_TIMEZONE         "UTC0"

esp_err_t nvs_config_init(void);
esp_err_t nvs_config_get_str(const char *key, char *out, size_t max_len);
esp_err_t nvs_config_set_str(const char *key, const char *val);
esp_err_t nvs_config_get_u32(const char *key, uint32_t *out, uint32_t default_val);
esp_err_t nvs_config_set_u32(const char *key, uint32_t val);
esp_err_t nvs_config_factory_reset(void);
