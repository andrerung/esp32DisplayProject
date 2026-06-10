#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs_config.h"
#include "data_fetch.h"

static const char *TAG = "data_fetch";

/* ---- shared state ---- */
static SemaphoreHandle_t s_mutex;
static weather_t s_weather;
static crypto_t  s_cryptos[DATA_FETCH_MAX_COINS];
static int       s_crypto_count;

/* ---- HTTP response accumulator ---- */
#define HTTP_BUF_SIZE 4096
static char s_http_buf[HTTP_BUF_SIZE];
static int  s_http_len;

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int space = HTTP_BUF_SIZE - 1 - s_http_len;
        int copy  = evt->data_len < space ? evt->data_len : space;
        memcpy(s_http_buf + s_http_len, evt->data, copy);
        s_http_len += copy;
    } else if (evt->event_id == HTTP_EVENT_ON_FINISH ||
               evt->event_id == HTTP_EVENT_DISCONNECTED) {
        s_http_buf[s_http_len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t http_get(const char *url)
{
    s_http_len = 0;
    s_http_buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = http_evt,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .buffer_size       = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(c);
    int status    = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP status %d for %s", status, url);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* ---- minimal JSON helpers ---- */

/* Find "key": value → parse double into *val */
static bool jnum(const char *hay, const char *key, double *val)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(hay, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    char *end;
    double d = strtod(p, &end);
    if (end == p) return false;
    *val = d;
    return true;
}

/* Find "key": "string value" → copy into out */
static bool jstr(const char *hay, const char *key, char *out, size_t len)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(hay, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < len - 1) out[i++] = *p++;
    out[i] = '\0';
    return (i > 0);
}

/* ---- coin symbol lookup ---- */
static const char *coin_symbol(const char *id)
{
    static const struct { const char *id; const char *sym; } map[] = {
        {"bitcoin",      "BTC"},  {"ethereum",    "ETH"},
        {"litecoin",     "LTC"},  {"binancecoin", "BNB"},
        {"solana",       "SOL"},  {"dogecoin",    "DOGE"},
        {"ripple",       "XRP"},  {"cardano",     "ADA"},
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        if (strcmp(id, map[i].id) == 0) return map[i].sym;
    }
    static char sym[8];
    int n = 0;
    while (id[n] && n < 4) { sym[n] = (char)toupper((unsigned char)id[n]); n++; }
    sym[n] = '\0';
    return sym;
}

/* ---- weather ---- */
static void fetch_weather(void)
{
    char city[32] = {0}, api_key[64] = {0};
    nvs_config_get_str(NVS_KEY_WEATHER_CITY, city, sizeof(city));
    nvs_config_get_str(NVS_KEY_WEATHER_KEY,  api_key, sizeof(api_key));
    if (city[0]    == '\0') strlcpy(city,    NVS_DEFAULT_WEATHER_CITY, sizeof(city));
    if (api_key[0] == '\0') { ESP_LOGW(TAG, "No weather API key"); return; }

    char url[256];
    snprintf(url, sizeof(url),
        "https://api.openweathermap.org/data/2.5/weather?q=%s&appid=%s&units=metric",
        city, api_key);

    if (http_get(url) != ESP_OK) return;

    weather_t w = {};

    /* city name */
    if (!jstr(s_http_buf, "name", w.city, sizeof(w.city)))
        strlcpy(w.city, city, sizeof(w.city));

    /* temperature — nested inside "main":{} */
    const char *main_p = strstr(s_http_buf, "\"main\":");
    if (main_p) {
        double t = 0, tmin = 0, tmax = 0;
        if (jnum(main_p, "temp",     &t))    w.temp_c   = (float)t;
        if (jnum(main_p, "temp_min", &tmin)) w.temp_min = (float)tmin;
        if (jnum(main_p, "temp_max", &tmax)) w.temp_max = (float)tmax;
    }

    /* description — first element of "weather":[] */
    const char *warr = strstr(s_http_buf, "\"weather\":[");
    if (warr) {
        const char *obj = strchr(warr, '{');
        if (obj) {
            jstr(obj, "description", w.condition, sizeof(w.condition));
            if (w.condition[0])
                w.condition[0] = (char)toupper((unsigned char)w.condition[0]);
        }
    }

    w.valid        = true;
    w.last_updated = time(NULL);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_weather = w;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Weather: %s %.1fC %s", w.city, w.temp_c, w.condition);
}

/* ---- crypto ---- */
static void fetch_crypto(void)
{
    char coins_str[128] = {0};
    nvs_config_get_str(NVS_KEY_CRYPTO_COINS, coins_str, sizeof(coins_str));
    if (coins_str[0] == '\0')
        strlcpy(coins_str, NVS_DEFAULT_CRYPTO_COINS, sizeof(coins_str));

    char currency[4] = {0};
    nvs_config_get_str(NVS_KEY_CRYPTO_CURRENCY, currency, sizeof(currency));
    if (currency[0] == '\0') strlcpy(currency, NVS_DEFAULT_CRYPTO_CURRENCY, sizeof(currency));
    /* Validate — prevents invalid values from entering the URL or JSON key */
    if (strcmp(currency, "usd") != 0 && strcmp(currency, "brl") != 0 &&
        strcmp(currency, "eur") != 0)
        strlcpy(currency, NVS_DEFAULT_CRYPTO_CURRENCY, sizeof(currency));

    char url[256];
    snprintf(url, sizeof(url),
        "https://api.coingecko.com/api/v3/simple/price?ids=%s&vs_currencies=%s",
        coins_str, currency);

    if (http_get(url) != ESP_OK) return;

    /* Parse: {"bitcoin":{"usd":67234.5},...} or {"bitcoin":{"brl":321456},...} */
    char work[128];
    strlcpy(work, coins_str, sizeof(work));

    crypto_t tmp[DATA_FETCH_MAX_COINS] = {};
    int count = 0;
    char *tok = strtok(work, ",");
    while (tok && count < DATA_FETCH_MAX_COINS) {
        while (*tok == ' ') tok++;
        /* Build pattern  "bitcoin": to find that object */
        char pat[64];
        snprintf(pat, sizeof(pat), "\"%s\":", tok);
        const char *p = strstr(s_http_buf, pat);
        if (p) {
            p += strlen(pat);
            double price = 0;
            if (jnum(p, currency, &price)) {
                strlcpy(tmp[count].id,       tok,              sizeof(tmp[count].id));
                strlcpy(tmp[count].symbol,   coin_symbol(tok), sizeof(tmp[count].symbol));
                strlcpy(tmp[count].currency, currency,         sizeof(tmp[count].currency));
                tmp[count].price        = price;
                tmp[count].valid        = true;
                tmp[count].last_updated = time(NULL);
                count++;
            }
        }
        tok = strtok(NULL, ",");
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < count; i++) {
        tmp[i].trend = 0;
        for (int j = 0; j < s_crypto_count; j++) {
            if (strcmp(tmp[i].id, s_cryptos[j].id) == 0 && s_cryptos[j].valid) {
                if      (tmp[i].price > s_cryptos[j].price) tmp[i].trend =  1;
                else if (tmp[i].price < s_cryptos[j].price) tmp[i].trend = -1;
                break;
            }
        }
    }
    memcpy(s_cryptos, tmp, sizeof(crypto_t) * count);
    s_crypto_count = count;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Crypto: %d coins updated", count);
}

/* ---- fetch task ---- */
static TaskHandle_t s_task_handle;
static bool s_sntp_started;

static void fetch_task(void *arg)
{
    if (!s_sntp_started) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        s_sntp_started = true;
        int waited = 0;
        while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && waited++ < 15) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGI(TAG, "SNTP %s after %d s", (waited < 15) ? "synced" : "timeout", waited);
    }

    fetch_weather();
    fetch_crypto();

    TickType_t last_weather = xTaskGetTickCount();
    TickType_t last_crypto  = xTaskGetTickCount();

    while (1) {
        uint32_t w_ivl = 0, c_ivl = 0;
        nvs_config_get_u32(NVS_KEY_WEATHER_INTERVAL, &w_ivl, NVS_DEFAULT_WEATHER_INTERVAL);
        nvs_config_get_u32(NVS_KEY_CRYPTO_INTERVAL,  &c_ivl, NVS_DEFAULT_CRYPTO_INTERVAL);

        TickType_t now = xTaskGetTickCount();
        if ((TickType_t)(now - last_weather) >= pdMS_TO_TICKS((uint32_t)w_ivl * 1000u)) {
            fetch_weather();
            last_weather = xTaskGetTickCount();
        }
        if ((TickType_t)(now - last_crypto) >= pdMS_TO_TICKS((uint32_t)c_ivl * 1000u)) {
            fetch_crypto();
            last_crypto = xTaskGetTickCount();
        }

        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

/* ---- public API ---- */

esp_err_t data_fetch_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    memset(&s_weather, 0, sizeof(s_weather));
    memset(s_cryptos,  0, sizeof(s_cryptos));
    s_crypto_count = 0;
    return ESP_OK;
}

esp_err_t data_fetch_start(void)
{
    if (s_task_handle) return ESP_OK;
    if (xTaskCreate(fetch_task, "data_fetch", 12288, NULL, 3, &s_task_handle) != pdPASS) {
        s_task_handle = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void data_fetch_get_weather(weather_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_weather;
    xSemaphoreGive(s_mutex);
}

int data_fetch_get_crypto_count(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_crypto_count;
    xSemaphoreGive(s_mutex);
    return n;
}

void data_fetch_get_crypto(int idx, crypto_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (idx >= 0 && idx < s_crypto_count) *out = s_cryptos[idx];
    else memset(out, 0, sizeof(*out));
    xSemaphoreGive(s_mutex);
}
