#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"

#include "display.h"
#include "nvs_config.h"
#include "udp_log.h"
#include "wifi_manager.h"
#include "data_fetch.h"
#include "ota_handler.h"
#include "dns_server.h"
#include "http_server.h"

static const char *TAG = "main";

/* ---- boot-phase vprintf hook — mirrors log output to the TFT scroll area ----
 * Chains through the previously installed hook (serial vprintf or udp_vprintf).
 * Stops adding to the display once s_boot_log_active is cleared on WiFi connect.
 * Security: only processes what ESP_LOG emits — credentials are never logged.    */

static vprintf_like_t  s_prev_vprintf;
static volatile bool   s_boot_log_active;

static void strip_ansi(const char *src, char *dst, int maxlen)
{
    int j = 0;
    for (int i = 0; src[i] && j < maxlen - 1; i++) {
        if (src[i] == '\x1b' && src[i + 1] == '[') {
            i += 2;
            while (src[i] && src[i] != 'm') i++;
            continue;
        }
        if (src[i] != '\n' && src[i] != '\r')
            dst[j++] = src[i];
    }
    dst[j] = '\0';
}

static int boot_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int n = s_prev_vprintf(fmt, args);  /* serial / UDP log unchanged */
    if (s_boot_log_active) {
        /* Static buffers — avoids adding stack pressure to the calling task
           (WiFi driver, event loop, etc. all have limited stack). The s_busy
           guard skips display on a re-entrant call; a missed line is harmless. */
        static char s_raw[128];
        static char s_clean[48];
        static volatile bool s_busy;
        if (!s_busy) {
            s_busy = true;
            vsnprintf(s_raw, sizeof(s_raw), fmt, copy);
            strip_ansi(s_raw, s_clean, sizeof(s_clean));
            /* "I (12345) tag: msg" → skip timestamp, show "tag: msg" */
            const char *p = strstr(s_clean, ") ");
            p = p ? p + 2 : s_clean;
            display_log_add(p);
            s_busy = false;
        }
    }
    va_end(copy);
    return n;
}

/* ---- price formatter: "$67,234" / "R$321,456" / "E67,234" ---- */
static void format_price(const crypto_t *c, char *buf, size_t len)
{
    double price = c->price;
    /* E = euro fallback (font only covers ASCII 0x20-0x7E, no € glyph) */
    const char *sym = strcmp(c->currency, "brl") == 0 ? "R$" :
                      strcmp(c->currency, "eur") == 0 ? "E"  : "$";
    if (price >= 1000000.0) {
        long p = (long)price;
        snprintf(buf, len, "%s%ld,%03ld,%03ld",
                 sym, p / 1000000L, (p / 1000L) % 1000L, p % 1000L);
    } else if (price >= 1000.0) {
        long p = (long)price;
        snprintf(buf, len, "%s%ld,%03ld", sym, p / 1000L, p % 1000L);
    } else if (price >= 0.01) {
        snprintf(buf, len, "%s%.2f", sym, price);
    } else {
        snprintf(buf, len, "%s%.4f", sym, price);
    }
}

/* ---- draw the static layout skeleton ---- */
static void draw_layout(void)
{
    display_fill_color(COLOR_BLACK);
    display_draw_hline(TILE_SEP1_Y, COLOR_DARK_GRAY);
    display_draw_hline(TILE_SEP2_Y, COLOR_DARK_GRAY);
    display_draw_hline(TILE_SEP3_Y, COLOR_DARK_GRAY);
}

/* ---- splash shown before WiFi connects ---- */
static void draw_splash(void)
{
    display_fill_color(COLOR_BLACK);

    if (wifi_manager_is_ap_mode()) {
        /* AP setup mode: "Setup Mode" = 10 chars × 16px = 160px → x=40 */
        display_draw_text_large(40, 60, "Setup Mode", COLOR_CYAN, COLOR_BLACK);
        display_draw_text_centered(104, "Join WiFi network:", COLOR_GRAY,   COLOR_BLACK);
        display_draw_text_centered(122, "InfoDisplay-Config", COLOR_WHITE,  COLOR_BLACK);
        display_draw_text_centered(122 + 36, "Then open browser:", COLOR_GRAY,   COLOR_BLACK);
        display_draw_text_centered(122 + 54, "192.168.4.1",        COLOR_YELLOW, COLOR_BLACK);
    } else {
        /* Compact header — log lines fill the rest of the screen */
        display_draw_text_centered(1, "InfoDisplay", COLOR_CYAN, COLOR_BLACK);
        display_fill_rect(0, 17, DISPLAY_WIDTH, 1, COLOR_DARK_GRAY);
    }
}

/* ---- UI update task (1 Hz) ---- */
static void ui_task(void *arg)
{
    draw_splash();

    char      prev_time[16]  = {0};
    char      prev_date[32]  = {0};
    weather_t prev_weather   = {0};
    crypto_t  prev_crypto[3] = {0};
    bool      layout_active  = false;
    bool      prev_wifi      = false;
    int       status_tick    = 0;

    while (1) {
        /* In AP mode there is no STA connection — keep showing the setup splash */
        if (wifi_manager_is_ap_mode()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        bool wifi = wifi_manager_is_connected();

        /* Transition: WiFi just connected — clear splash, draw layout */
        if (wifi && !layout_active) {
            layout_active = true;
            draw_layout();
            prev_time[0] = '\0';
            prev_date[0] = '\0';
            memset(&prev_weather, 0, sizeof(prev_weather));
            memset(prev_crypto,   0, sizeof(prev_crypto));
            prev_wifi    = false;
            status_tick  = 5;
        }

        if (!layout_active) {
            display_log_render();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* ---- Time ---- */
        time_t now;
        struct tm t;
        time(&now);
        localtime_r(&now, &t);

        char ts[16], ds[32];
        strftime(ts, sizeof(ts), "%H:%M:%S", &t);
        strftime(ds, sizeof(ds), "%a %d %b %Y", &t);

        if (strcmp(ts, prev_time) != 0) {
            strlcpy(prev_time, ts, sizeof(prev_time));
            display_fill_rect(0, TILE_TIME_Y, DISPLAY_WIDTH, TILE_TIME_H, COLOR_BLACK);
            display_draw_text_large(56, TILE_TIME_Y + 8, ts, COLOR_WHITE, COLOR_BLACK);
        }

        if (strcmp(ds, prev_date) != 0) {
            strlcpy(prev_date, ds, sizeof(prev_date));
            display_fill_rect(0, TILE_DATE_Y, DISPLAY_WIDTH, TILE_DATE_H, COLOR_BLACK);
            display_draw_text_centered(TILE_DATE_Y, ds, COLOR_LIGHT_GRAY, COLOR_BLACK);
        }

        /* ---- Weather ---- */
        weather_t w = {0};
        data_fetch_get_weather(&w);
        bool weather_changed =
            w.valid != prev_weather.valid ||
            w.temp_c != prev_weather.temp_c ||
            strcmp(w.city,      prev_weather.city)      != 0 ||
            strcmp(w.condition, prev_weather.condition) != 0;

        if (weather_changed) {
            prev_weather = w;
            display_fill_rect(0, TILE_WEATHER_Y, DISPLAY_WIDTH, TILE_WEATHER_H, COLOR_BLACK);
            if (w.valid) {
                char line1[32];
                snprintf(line1, sizeof(line1), "%.16s  %.0fC", w.city, w.temp_c);
                display_draw_text(8, TILE_WEATHER_Y + 12, line1,       COLOR_CYAN,   COLOR_BLACK);
                display_draw_text(8, TILE_WEATHER_Y + 34, w.condition, COLOR_YELLOW, COLOR_BLACK);
            } else {
                display_draw_text(8, TILE_WEATHER_Y + 20, "Fetching...", COLOR_GRAY, COLOR_BLACK);
            }
        }

        /* ---- Crypto ---- */
        static const int tile_y[3] = {TILE_CRYPTO0_Y, TILE_CRYPTO1_Y, TILE_CRYPTO2_Y};
        int n = data_fetch_get_crypto_count();
        for (int i = 0; i < 3; i++) {
            crypto_t c = {0};
            if (i < n) data_fetch_get_crypto(i, &c);
            bool changed =
                c.valid != prev_crypto[i].valid ||
                c.price != prev_crypto[i].price ||
                strcmp(c.symbol,   prev_crypto[i].symbol)   != 0 ||
                strcmp(c.currency, prev_crypto[i].currency) != 0;
            if (changed) {
                prev_crypto[i] = c;
                int y = tile_y[i];
                display_fill_rect(0, y, DISPLAY_WIDTH, TILE_CRYPTO_H, COLOR_BLACK);
                if (c.valid) {
                    char price[16];
                    format_price(&c, price, sizeof(price));
                    display_draw_text(8, y + 4,  c.symbol, COLOR_GRAY,  COLOR_BLACK);
                    display_draw_text_large(8, y + 22, price, COLOR_WHITE, COLOR_BLACK);
                } else if (n == 0 && i == 0) {
                    display_draw_text(8, y + 20, "Fetching...", COLOR_GRAY, COLOR_BLACK);
                }
            }
        }

        /* ---- Status (every 5 s) ---- */
        bool wifi_changed = (wifi != prev_wifi);
        if (wifi_changed || ++status_tick >= 5) {
            status_tick = 0;
            prev_wifi   = wifi;
            display_fill_rect(0, TILE_STATUS_Y, DISPLAY_WIDTH, TILE_STATUS_H, COLOR_BLACK);
            if (wifi) {
                char status[48];
                snprintf(status, sizeof(status), "WiFi %d dBm", wifi_manager_get_rssi());
                display_draw_text(8, TILE_STATUS_Y, status, COLOR_DARK_GRAY, COLOR_BLACK);
            } else {
                display_draw_text(8, TILE_STATUS_Y, "No WiFi", COLOR_RED, COLOR_BLACK);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---- Event handlers ---- */

static void on_wifi_connected(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    ESP_LOGI(TAG, "WiFi connected — starting data fetch");
    s_boot_log_active = false;  /* normal UI takes over; stop log overlay */
    data_fetch_start();
    http_server_start_config();
}

static void on_ap_started(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    ESP_LOGI(TAG, "AP mode active — starting captive portal");
    dns_server_start();
    http_server_start_portal();
}

/* ---- app entry point ---- */
void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 InfoDisplay booting");

    ESP_ERROR_CHECK(nvs_config_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Optional UDP log forwarding */
    char udp_host[32] = {0};
    uint32_t udp_port = 0;
    nvs_config_get_str(NVS_KEY_LOG_UDP_HOST, udp_host, sizeof(udp_host));
    nvs_config_get_u32(NVS_KEY_LOG_UDP_PORT, &udp_port, NVS_DEFAULT_LOG_UDP_PORT);
    if (udp_host[0] != '\0') {
        if (udp_log_init(udp_host, (uint16_t)udp_port) == ESP_OK) {
            ESP_LOGI(TAG, "UDP log → %s:%" PRIu32, udp_host, udp_port);
        }
    }

    ESP_ERROR_CHECK(display_init());
    s_boot_log_active = true;
    s_prev_vprintf = esp_log_set_vprintf(boot_vprintf);
    ESP_ERROR_CHECK(data_fetch_init());

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_MANAGER_EVENTS, WIFI_MANAGER_CONNECTED,  on_wifi_connected, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_MANAGER_EVENTS, WIFI_MANAGER_AP_STARTED, on_ap_started, NULL));

    wifi_manager_start();

    xTaskCreate(ui_task, "ui", 8192, NULL, 3, NULL);

    /* Mark firmware valid after 30 s stable run (OTA rollback guard) */
    vTaskDelay(pdMS_TO_TICKS(30000));
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "App marked valid");

    while (1) vTaskDelay(pdMS_TO_TICKS(60000));
}
