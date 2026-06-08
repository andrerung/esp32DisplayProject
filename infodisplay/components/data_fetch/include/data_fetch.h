#pragma once
#include "esp_err.h"
#include <time.h>
#include <stdbool.h>

typedef struct {
    char  city[32];
    float temp_c;
    char  condition[48];
    time_t last_updated;
    bool  valid;
} weather_t;

#define DATA_FETCH_MAX_COINS 5

typedef struct {
    char   id[32];       /* CoinGecko id, e.g. "bitcoin" */
    char   symbol[8];    /* Display symbol, e.g. "BTC"    */
    double price_usd;
    time_t last_updated;
    bool   valid;
} crypto_t;

esp_err_t data_fetch_init(void);
esp_err_t data_fetch_start(void);
void data_fetch_get_weather(weather_t *out);
int  data_fetch_get_crypto_count(void);
void data_fetch_get_crypto(int idx, crypto_t *out);
