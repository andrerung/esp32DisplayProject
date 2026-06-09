#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_config.h"
#include "http_server.h"

static const char *TAG = "http_server";

/* ---- Embedded HTML pages ---- */

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html lang='en'>"
    "<head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>InfoDisplay Setup</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:#0a0a0a;color:#ddd;font-family:-apple-system,BlinkMacSystemFont,sans-serif;"
    "padding:20px;max-width:440px;margin:auto}"
    "h1{color:#00ccff;margin:20px 0 4px;font-size:1.4em}"
    ".sub{color:#555;font-size:.85em;margin-bottom:20px}"
    "label{display:block;color:#777;font-size:.75em;text-transform:uppercase;"
    "letter-spacing:.05em;margin-top:14px}"
    "input{width:100%;padding:9px 11px;background:#181818;border:1px solid #333;"
    "color:#ddd;border-radius:5px;font-size:1em;margin-top:5px;outline:none}"
    "input:focus{border-color:#00ccff}"
    ".hint{color:#444;font-size:.72em;margin-top:3px}"
    "button{width:100%;padding:12px;background:#00ccff;color:#000;border:none;"
    "border-radius:5px;font-size:1em;font-weight:700;cursor:pointer;margin-top:24px}"
    "button:active{background:#009ecc}"
    "</style>"
    "</head>"
    "<body>"
    "<h1>&#9881; InfoDisplay Setup</h1>"
    "<p class='sub'>Connect this device to your WiFi network</p>"
    "<form method='POST' action='/save'>"
    "<label>WiFi Network (SSID)</label>"
    "<input name='ssid' placeholder='Network name' required autocomplete='off'>"
    "<label>WiFi Password</label>"
    "<input name='pass' type='password' placeholder='Leave blank for open network'"
    " autocomplete='current-password'>"
    "<label>Weather City</label>"
    "<input name='city' value='London' placeholder='e.g. London,GB'>"
    "<label>OpenWeatherMap API Key</label>"
    "<input name='wkey' placeholder='Free key at openweathermap.org' autocomplete='off'>"
    "<p class='hint'>Get a free API key at openweathermap.org &rarr; API keys</p>"
    "<label>Crypto Coins (CoinGecko IDs, comma-separated)</label>"
    "<input name='coins' value='bitcoin,ethereum,litecoin'>"
    "<button type='submit'>Save &amp; Connect</button>"
    "</form>"
    "</body></html>";

static const char SAVED_HTML[] =
    "<!DOCTYPE html><html>"
    "<head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Saved</title>"
    "<style>"
    "body{background:#0a0a0a;color:#ddd;font-family:-apple-system,sans-serif;"
    "text-align:center;padding:60px 20px}"
    "h1{color:#00ff88;font-size:1.8em;margin-bottom:16px}"
    "p{color:#666}"
    "</style>"
    "</head>"
    "<body>"
    "<h1>&#10003; Settings Saved</h1>"
    "<p>Connecting to your WiFi network&hellip;<br><br>You can close this page.</p>"
    "</body></html>";

/* ---- URL decode + form parsing ---- */

static void url_decode(const char *in, char *out, size_t len)
{
    size_t i = 0, j = 0;
    while (in[i] && j < len - 1) {
        if (in[i] == '%' &&
            isxdigit((unsigned char)in[i + 1]) &&
            isxdigit((unsigned char)in[i + 2])) {
            char hex[3] = {in[i + 1], in[i + 2], '\0'};
            out[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (in[i] == '+') {
            out[j++] = ' ';
            i++;
        } else {
            out[j++] = in[i++];
        }
    }
    out[j] = '\0';
}

/* Extract key=value from URL-encoded body, URL-decode into out */
static bool get_param(const char *body, const char *key, char *out, size_t len)
{
    char search[48];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) return false;
    p += strlen(search);
    char raw[256] = {0};
    size_t i = 0;
    while (*p && *p != '&' && i < sizeof(raw) - 1) raw[i++] = *p++;
    raw[i] = '\0';
    url_decode(raw, out, len);
    return true;
}

/* ---- Restart timer ---- */

static esp_timer_handle_t s_restart_timer = NULL;
static void restart_cb(void *arg) { esp_restart(); }

/* ---- HTTP handlers ---- */

static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handler_save(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 512) {
        /* Redirect back to form on bad request */
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    char body[513] = {0};
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (ret <= 0) return ESP_FAIL;
        received += ret;
    }
    body[received] = '\0';

    char ssid[64]  = {0};
    char pass[64]  = {0};
    char city[32]  = {0};
    char wkey[64]  = {0};
    char coins[128]= {0};
    get_param(body, "ssid",  ssid,  sizeof(ssid));
    get_param(body, "pass",  pass,  sizeof(pass));
    get_param(body, "city",  city,  sizeof(city));
    get_param(body, "wkey",  wkey,  sizeof(wkey));
    get_param(body, "coins", coins, sizeof(coins));

    if (ssid[0] == '\0') {
        /* No SSID — redirect back */
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    /* Save credentials — never log pass or wkey (FR-7.4 / NFR-3.1) */
    nvs_config_set_str(NVS_KEY_WIFI_SSID, ssid);
    nvs_config_set_str(NVS_KEY_WIFI_PASS, pass);
    if (city[0])  nvs_config_set_str(NVS_KEY_WEATHER_CITY, city);
    if (wkey[0])  nvs_config_set_str(NVS_KEY_WEATHER_KEY,  wkey);
    if (coins[0]) nvs_config_set_str(NVS_KEY_CRYPTO_COINS, coins);
    ESP_LOGI(TAG, "Config saved: SSID=\"%s\" city=\"%s\"", ssid, city);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, SAVED_HTML, HTTPD_RESP_USE_STRLEN);

    /* Restart in 2 s — gives the response time to reach the browser */
    esp_timer_start_once(s_restart_timer, 2000000ULL);
    return ESP_OK;
}

/* Catch-all: redirect every unknown URL to the config page.
   This triggers captive-portal detection on iOS, Android, and Windows. */
static esp_err_t handler_404(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---- Public API ---- */

static httpd_handle_t s_server = NULL;

esp_err_t http_server_start_portal(void)
{
    if (s_server) return ESP_OK;

    esp_timer_create_args_t ta = {
        .callback = restart_cb,
        .name     = "portal_rst",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_restart_timer));

    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 6;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    static const httpd_uri_t u_root = {
        .uri = "/", .method = HTTP_GET, .handler = handler_root
    };
    static const httpd_uri_t u_save = {
        .uri = "/save", .method = HTTP_POST, .handler = handler_save
    };
    httpd_register_uri_handler(s_server, &u_root);
    httpd_register_uri_handler(s_server, &u_save);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handler_404);

    ESP_LOGI(TAG, "Captive portal HTTP server started on port 80");
    return ESP_OK;
}

void http_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
