#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "nvs_config.h"
#include "wifi_manager.h"
#include "ota_handler.h"
#include "http_server.h"

static const char *TAG = "http_server";

/* ---- Embedded HTML pages ---- */

/* Part 1: doctype → form open → SSID label.
   The SSID dropdown is injected dynamically between P1 and P2. */
static const char PORTAL_HTML_P1[] =
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
    "input,select{width:100%;padding:9px 11px;background:#181818;border:1px solid #333;"
    "color:#ddd;border-radius:5px;font-size:1em;margin-top:5px;outline:none}"
    "input:focus,select:focus{border-color:#00ccff}"
    "select{-webkit-appearance:none;appearance:none}"
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
    "<label>WiFi Network (SSID)</label>";

/* Part 2: manual SSID fallback + fields up to coins input */
static const char PORTAL_HTML_P2[] =
    "<label>Or enter SSID manually</label>"
    "<input name='ssid' placeholder='Hidden or unlisted network' autocomplete='off'>"
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
    "<label>Crypto Price Currency</label>"
    "<select name='curr'>"
    "<option value='usd' selected>USD - US Dollar ($)</option>"
    "<option value='brl'>BRL - Real Brasileiro (R$)</option>"
    "<option value='eur'>EUR - Euro (&euro;)</option>"
    "</select>";

/* Part 3: submit button + form close */
static const char PORTAL_HTML_P3[] =
    "<button type='submit'>Save &amp; Connect</button>"
    "</form>"
    "</body></html>";

/* Shown after a save that doesn't require a restart (no WiFi change) */
static const char SAVED_LIVE_HTML[] =
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
    "a{color:#00ccff}"
    "</style>"
    "</head>"
    "<body>"
    "<h1>&#10003; Settings Saved</h1>"
    "<p>Changes take effect immediately.<br><br>"
    "Crypto prices update within 60 s.<br><br>"
    "<a href='/'>Back</a></p>"
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

static void html_escape(const char *in, char *out, size_t len)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j < len - 1; i++) {
        const char *ent = NULL;
        size_t elen = 0;
        if      (in[i] == '&')  { ent = "&amp;";  elen = 5; }
        else if (in[i] == '"')  { ent = "&quot;"; elen = 6; }
        else if (in[i] == '<')  { ent = "&lt;";   elen = 4; }
        else if (in[i] == '>')  { ent = "&gt;";   elen = 4; }
        else if (in[i] == '\'') { ent = "&#39;";  elen = 5; }
        if (ent) {
            if (j + elen >= len) break;
            memcpy(out + j, ent, elen);
            j += elen;
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = '\0';
}

static void json_escape(const char *in, char *out, size_t len)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j < len - 1; i++) {
        if (in[i] == '"' || in[i] == '\\') {
            if (j + 2 >= len) break;
            out[j++] = '\\';
            out[j++] = in[i];
        } else if ((unsigned char)in[i] >= 0x20) {
            out[j++] = in[i];
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

/* ---- Timezone table (POSIX strings validated against this list) ---- */
/* POSIX TZ strings with full DST rules where applicable.
   Entries without a DST component are for regions that do not observe DST.
   newlib localtime_r() evaluates these rules against the current epoch each
   call, so UTC offset adjusts automatically at every DST transition. */
static const struct { const char *posix; const char *label; } s_timezones[] = {
    /* ---- Fixed UTC ---- */
    {"UTC0",                             "UTC+0 (no DST)"},
    /* ---- Americas ---- */
    {"NST3:30NDT,M3.2.0/0:01,M11.1.0",  "Canada Newfoundland (UTC-3:30/-2:30, DST)"},
    {"AST4ADT,M3.2.0,M11.1.0",          "Canada Atlantic (UTC-4/-3, DST Mar-Nov)"},
    {"EST5EDT,M3.2.0,M11.1.0",          "US / Canada Eastern (UTC-5/-4, DST Mar-Nov)"},
    {"CST6CDT,M3.2.0,M11.1.0",          "US / Canada Central (UTC-6/-5, DST Mar-Nov)"},
    {"MST7MDT,M3.2.0,M11.1.0",          "US / Canada Mountain (UTC-7/-6, DST Mar-Nov)"},
    {"PST8PDT,M3.2.0,M11.1.0",          "US / Canada Pacific (UTC-8/-7, DST Mar-Nov)"},
    {"AKST9AKDT,M3.2.0,M11.1.0",        "US Alaska (UTC-9/-8, DST Mar-Nov)"},
    {"HST10",                            "US Hawaii (UTC-10, no DST)"},
    {"ART3",                             "Argentina (UTC-3, no DST)"},
    {"BRT3",                             "Brazil Brasilia (UTC-3, no DST)"},
    {"AMT4",                             "Brazil Amazon (UTC-4, no DST)"},
    {"FNT2",                             "Brazil Noronha (UTC-2, no DST)"},
    {"CLT3",                             "Chile (UTC-3, no DST since 2022)"},
    /* ---- Europe ---- */
    {"GMT0BST,M3.5.0/1,M10.5.0",        "UK / Ireland (UTC+0/+1, DST Mar-Oct)"},
    {"WET0WEST,M3.5.0/1,M10.5.0",       "Portugal / Canary Is. (UTC+0/+1, DST Mar-Oct)"},
    {"CET-1CEST,M3.5.0,M10.5.0/3",      "Europe Central (UTC+1/+2, DST Mar-Oct)"},
    {"EET-2EEST,M3.5.0/3,M10.5.0/4",    "Europe Eastern (UTC+2/+3, DST Mar-Oct)"},
    {"MSK-3",                            "Russia Moscow (UTC+3, no DST)"},
    /* ---- Africa / Middle East ---- */
    {"WAT-1",                            "West Africa (UTC+1, no DST)"},
    {"CAT-2",                            "Central / South Africa (UTC+2, no DST)"},
    {"EAT-3",                            "East Africa (UTC+3, no DST)"},
    {"GST-4",                            "Gulf States (UTC+4, no DST)"},
    /* ---- Asia / Pacific ---- */
    {"PKT-5",                            "Pakistan (UTC+5, no DST)"},
    {"IST-5:30",                         "India (UTC+5:30, no DST)"},
    {"ICT-7",                            "SE Asia - Bangkok (UTC+7, no DST)"},
    {"CST-8",                            "China / Singapore (UTC+8, no DST)"},
    {"JST-9",                            "Japan / Korea (UTC+9, no DST)"},
    {"AEST-10AEDT,M10.1.0,M4.1.0/3",    "Australia East (UTC+10/+11, DST Oct-Apr)"},
    {"ACST-9:30ACDT,M10.1.0,M4.1.0/3",  "Australia Central (UTC+9:30/+10:30, DST Oct-Apr)"},
    {"AWST-8",                           "Australia West (UTC+8, no DST)"},
    {"NZST-12NZDT,M9.5.0,M4.1.0/3",     "New Zealand (UTC+12/+13, DST Sep-Apr)"},
};
#define TZ_COUNT ((int)(sizeof(s_timezones)/sizeof(s_timezones[0])))

static bool tz_valid(const char *tz)
{
    for (int i = 0; i < TZ_COUNT; i++)
        if (strcmp(tz, s_timezones[i].posix) == 0) return true;
    return false;
}

/* ---- Restart timer ---- */

static esp_timer_handle_t s_restart_timer = NULL;
static void restart_cb(void *arg)
{
    /* Proper deauth before restart — prevents the AP from holding the
       SAE/WPA3 session open and rate-limiting subsequent re-auth attempts. */
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_restart();
}

/* ---- HTTP handlers ---- */

static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    httpd_resp_sendstr_chunk(req, PORTAL_HTML_P1);

    /* SSID dropdown from background WiFi scan */
    wifi_scan_ap_t aps[20];
    int n = wifi_manager_copy_scan_results(aps, 20);

    httpd_resp_sendstr_chunk(req,
        "<select name='ssid_sel'>"
        "<option value=''>-- Select your network --</option>");
    for (int i = 0; i < n; i++) {
        char esc[200];
        html_escape(aps[i].ssid, esc, sizeof(esc));
        char opt[512];
        snprintf(opt, sizeof(opt),
                 "<option value=\"%s\">%s (%d dBm)</option>",
                 esc, esc, (int)aps[i].rssi);
        httpd_resp_sendstr_chunk(req, opt);
    }
    if (n == 0) {
        httpd_resp_sendstr_chunk(req,
            "<option value='' disabled>"
            "Scanning... reload page to refresh</option>");
    }
    httpd_resp_sendstr_chunk(req, "</select>");

    httpd_resp_sendstr_chunk(req, PORTAL_HTML_P2);

    /* Timezone dropdown — inject with current NVS value pre-selected */
    {
        char tz_nvs[64] = {0};
        nvs_config_get_str(NVS_KEY_TIMEZONE, tz_nvs, sizeof(tz_nvs));
        if (tz_nvs[0] == '\0') strlcpy(tz_nvs, NVS_DEFAULT_TIMEZONE, sizeof(tz_nvs));
        httpd_resp_sendstr_chunk(req, "<label>Timezone</label><select name='tz'>");
        for (int i = 0; i < TZ_COUNT; i++) {
            char opt[160];
            snprintf(opt, sizeof(opt), "<option value='%s'%s>%s</option>",
                     s_timezones[i].posix,
                     strcmp(tz_nvs, s_timezones[i].posix) == 0 ? " selected" : "",
                     s_timezones[i].label);
            httpd_resp_sendstr_chunk(req, opt);
        }
        httpd_resp_sendstr_chunk(req, "</select>");
    }

    httpd_resp_sendstr_chunk(req, PORTAL_HTML_P3);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t handler_save(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 768) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    char body[769] = {0};
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (ret <= 0) return ESP_FAIL;
        received += ret;
    }
    body[received] = '\0';

    char ssid[64]     = {0};
    char ssid_sel[64] = {0};
    char pass[64]     = {0};
    char city[32]     = {0};
    char wkey[64]     = {0};
    char coins[128]   = {0};
    char curr[4]      = {0};
    char tz[64]       = {0};
    get_param(body, "ssid",     ssid,     sizeof(ssid));
    get_param(body, "ssid_sel", ssid_sel, sizeof(ssid_sel));
    get_param(body, "pass",     pass,     sizeof(pass));
    get_param(body, "city",     city,     sizeof(city));
    get_param(body, "wkey",     wkey,     sizeof(wkey));
    get_param(body, "coins",    coins,    sizeof(coins));
    get_param(body, "curr",     curr,     sizeof(curr));
    get_param(body, "tz",       tz,       sizeof(tz));

    /* Manual text input takes priority; dropdown value is the fallback */
    if (ssid[0] == '\0') strlcpy(ssid, ssid_sel, sizeof(ssid));

    if (ssid[0] == '\0') {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    /* Save credentials — never log pass or wkey (FR-7.4 / NFR-3.1) */
    nvs_config_set_str(NVS_KEY_WIFI_SSID, ssid);
    /* Only overwrite the password when a value was entered — a blank field
       (browsers don't refill password inputs) must not wipe a stored password. */
    if (pass[0]) nvs_config_set_str(NVS_KEY_WIFI_PASS, pass);
    if (city[0])  nvs_config_set_str(NVS_KEY_WEATHER_CITY, city);
    if (wkey[0])  nvs_config_set_str(NVS_KEY_WEATHER_KEY,  wkey);
    if (coins[0]) nvs_config_set_str(NVS_KEY_CRYPTO_COINS, coins);
    if (strcmp(curr, "usd") == 0 || strcmp(curr, "brl") == 0 || strcmp(curr, "eur") == 0)
        nvs_config_set_str(NVS_KEY_CRYPTO_CURRENCY, curr);
    if (tz_valid(tz)) nvs_config_set_str(NVS_KEY_TIMEZONE, tz);
    ESP_LOGI(TAG, "Config saved: SSID=\"%s\" city=\"%s\" curr=%s tz=%s", ssid, city, curr, tz);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, SAVED_HTML, HTTPD_RESP_USE_STRLEN);

    esp_timer_start_once(s_restart_timer, 2000000ULL);
    return ESP_OK;
}

/* Catch-all: redirect every unknown URL to the config page.
   iOS requires a non-empty body to detect the portal (per ESP-IDF captive_portal example). */
static esp_err_t handler_404(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, "Redirecting to InfoDisplay setup",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---- LAN config server (STA mode) ---- */

/* Chunks for dynamic config page: A → ip → B → ssid → C → city → D → coins → E */
static const char CFG_HTML_A[] =
    "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>InfoDisplay Config</title><style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:#0a0a0a;color:#ddd;font-family:-apple-system,BlinkMacSystemFont,sans-serif;"
    "padding:20px;max-width:440px;margin:auto}"
    "h1{color:#00ccff;margin:20px 0 4px;font-size:1.4em}"
    ".sub{color:#555;font-size:.85em;margin-bottom:4px}"
    ".ip{color:#00ff88;font-size:.82em;margin-bottom:18px}"
    "label{display:block;color:#777;font-size:.75em;text-transform:uppercase;"
    "letter-spacing:.05em;margin-top:14px}"
    "input,select{width:100%;padding:9px 11px;background:#181818;border:1px solid #333;"
    "color:#ddd;border-radius:5px;font-size:1em;margin-top:5px;outline:none}"
    "input:focus,select:focus{border-color:#00ccff}"
    "select{-webkit-appearance:none;appearance:none}"
    ".hint{color:#444;font-size:.72em;margin-top:3px}"
    "button{width:100%;padding:12px;background:#00ccff;color:#000;border:none;"
    "border-radius:5px;font-size:1em;font-weight:700;cursor:pointer;margin-top:24px}"
    "button:active{background:#009ecc}"
    "</style></head><body>"
    "<h1>&#9881; InfoDisplay Config</h1>"
    "<p class='sub'>Device IP:</p>";

static const char CFG_HTML_B[] =
    "<form method='POST' action='/save'>"
    "<label>WiFi Network (SSID)</label>";

static const char CFG_HTML_C[] =
    "<label>WiFi Password</label>"
    "<input name='pass' type='password' placeholder='Leave blank to keep current'"
    " autocomplete='current-password'>"
    "<label>Weather City</label>";

static const char CFG_HTML_D[] =
    "<label>OpenWeatherMap API Key</label>"
    "<input name='wkey' placeholder='Leave blank to keep current' autocomplete='off'>"
    "<p class='hint'>Get a free key at openweathermap.org &rarr; API keys</p>"
    "<label>Crypto Coins (CoinGecko IDs, comma-separated)</label>";

static const char CFG_HTML_E[] =
    "<button type='submit'>Save &amp; Restart</button>"
    "</form></body></html>";

static esp_err_t handler_cfg_get(httpd_req_t *req)
{
    char ssid[64]   = {0};
    char city[32]   = {0};
    char coins[128] = {0};
    char curr[4]    = {0};
    char ip[16]     = {0};
    nvs_config_get_str(NVS_KEY_WIFI_SSID,       ssid,  sizeof(ssid));
    nvs_config_get_str(NVS_KEY_WEATHER_CITY,    city,  sizeof(city));
    nvs_config_get_str(NVS_KEY_CRYPTO_COINS,    coins, sizeof(coins));
    nvs_config_get_str(NVS_KEY_CRYPTO_CURRENCY, curr,  sizeof(curr));
    if (curr[0] == '\0') strlcpy(curr, NVS_DEFAULT_CRYPTO_CURRENCY, sizeof(curr));
    wifi_manager_get_ip(ip, sizeof(ip));

    char tmp[512];
    char esc[200];

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, CFG_HTML_A);
    snprintf(tmp, sizeof(tmp), "<p class='ip'>%s</p>", ip);
    httpd_resp_sendstr_chunk(req, tmp);
    httpd_resp_sendstr_chunk(req, CFG_HTML_B);
    html_escape(ssid,  esc, sizeof(esc));
    snprintf(tmp, sizeof(tmp), "<input name='ssid' value=\"%s\">", esc);
    httpd_resp_sendstr_chunk(req, tmp);
    httpd_resp_sendstr_chunk(req, CFG_HTML_C);
    html_escape(city,  esc, sizeof(esc));
    snprintf(tmp, sizeof(tmp), "<input name='city' value=\"%s\">", esc);
    httpd_resp_sendstr_chunk(req, tmp);
    httpd_resp_sendstr_chunk(req, CFG_HTML_D);
    html_escape(coins, esc, sizeof(esc));
    snprintf(tmp, sizeof(tmp), "<input name='coins' value=\"%s\">", esc);
    httpd_resp_sendstr_chunk(req, tmp);
    /* Currency dropdown — mark current value as selected */
    snprintf(tmp, sizeof(tmp),
        "<label>Crypto Price Currency</label>"
        "<select name='curr'>"
        "<option value='usd'%s>USD - US Dollar ($)</option>"
        "<option value='brl'%s>BRL - Real Brasileiro (R$)</option>"
        "<option value='eur'%s>EUR - Euro (&euro;)</option>"
        "</select>",
        strcmp(curr, "usd") == 0 ? " selected" : "",
        strcmp(curr, "brl") == 0 ? " selected" : "",
        strcmp(curr, "eur") == 0 ? " selected" : "");
    httpd_resp_sendstr_chunk(req, tmp);

    /* Timezone dropdown */
    {
        char tz_nvs[64] = {0};
        nvs_config_get_str(NVS_KEY_TIMEZONE, tz_nvs, sizeof(tz_nvs));
        if (tz_nvs[0] == '\0') strlcpy(tz_nvs, NVS_DEFAULT_TIMEZONE, sizeof(tz_nvs));
        httpd_resp_sendstr_chunk(req, "<label>Timezone</label><select name='tz'>");
        for (int i = 0; i < TZ_COUNT; i++) {
            char opt[160];
            snprintf(opt, sizeof(opt), "<option value='%s'%s>%s</option>",
                     s_timezones[i].posix,
                     strcmp(tz_nvs, s_timezones[i].posix) == 0 ? " selected" : "",
                     s_timezones[i].label);
            httpd_resp_sendstr_chunk(req, opt);
        }
        httpd_resp_sendstr_chunk(req, "</select>");
    }

    httpd_resp_sendstr_chunk(req, CFG_HTML_E);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t handler_cfg_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 768) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    char body[769] = {0};
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (ret <= 0) return ESP_FAIL;
        received += ret;
    }
    body[received] = '\0';

    char ssid[64]   = {0};
    char pass[64]   = {0};
    char city[32]   = {0};
    char wkey[64]   = {0};
    char coins[128] = {0};
    char curr[4]    = {0};
    char tz[64]     = {0};
    get_param(body, "ssid",  ssid,  sizeof(ssid));
    get_param(body, "pass",  pass,  sizeof(pass));
    get_param(body, "city",  city,  sizeof(city));
    get_param(body, "wkey",  wkey,  sizeof(wkey));
    get_param(body, "coins", coins, sizeof(coins));
    get_param(body, "curr",  curr,  sizeof(curr));
    get_param(body, "tz",    tz,    sizeof(tz));

    /* Only write fields that were actually filled in — preserve the rest */
    if (ssid[0])  nvs_config_set_str(NVS_KEY_WIFI_SSID,    ssid);
    if (pass[0])  nvs_config_set_str(NVS_KEY_WIFI_PASS,    pass);
    if (city[0])  nvs_config_set_str(NVS_KEY_WEATHER_CITY, city);
    if (wkey[0])  nvs_config_set_str(NVS_KEY_WEATHER_KEY,  wkey);
    if (coins[0]) nvs_config_set_str(NVS_KEY_CRYPTO_COINS, coins);
    if (strcmp(curr, "usd") == 0 || strcmp(curr, "brl") == 0 || strcmp(curr, "eur") == 0)
        nvs_config_set_str(NVS_KEY_CRYPTO_CURRENCY, curr);
    if (tz_valid(tz)) {
        nvs_config_set_str(NVS_KEY_TIMEZONE, tz);
        /* Apply timezone immediately — no restart needed */
        setenv("TZ", tz, 1);
        tzset();
    }
    ESP_LOGI(TAG, "Config updated: SSID=%s city=%s curr=%s tz=%s",
             ssid[0] ? ssid : "(unchanged)", city[0] ? city : "(unchanged)", curr, tz);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* Restart only when WiFi credentials actually changed.
     * The SSID field is pre-filled in the form, so it is always present in
     * the POST body even when the user didn't touch it — compare against the
     * stored value to detect a real change. Password field is left blank by
     * default, so any non-empty submission means the user changed it. */
    char stored_ssid[64] = {0};
    nvs_config_get_str(NVS_KEY_WIFI_SSID, stored_ssid, sizeof(stored_ssid));
    bool wifi_changed = (pass[0] != '\0') ||
                        (ssid[0] != '\0' && strcmp(ssid, stored_ssid) != 0);
    if (wifi_changed) {
        httpd_resp_send(req, SAVED_HTML, HTTPD_RESP_USE_STRLEN);
        esp_timer_start_once(s_restart_timer, 2000000ULL);
    } else {
        httpd_resp_send(req, SAVED_LIVE_HTML, HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

/* ---- GET /status ---- */
static esp_err_t handler_status(httpd_req_t *req)
{
    char ip[16]     = {0};
    char ssid[64]   = {0};
    char city[32]   = {0};
    char coins[128] = {0};
    char curr[4]    = {0};
    wifi_manager_get_ip(ip, sizeof(ip));
    nvs_config_get_str(NVS_KEY_WIFI_SSID,       ssid,  sizeof(ssid));
    nvs_config_get_str(NVS_KEY_WEATHER_CITY,    city,  sizeof(city));
    nvs_config_get_str(NVS_KEY_CRYPTO_COINS,    coins, sizeof(coins));
    nvs_config_get_str(NVS_KEY_CRYPTO_CURRENCY, curr,  sizeof(curr));
    if (curr[0] == '\0') strlcpy(curr, NVS_DEFAULT_CRYPTO_CURRENCY, sizeof(curr));

    int8_t  rssi     = wifi_manager_get_rssi();
    int64_t uptime_s = esp_timer_get_time() / 1000000LL;

    char version[32] = "unknown";
    const esp_partition_t *part = esp_ota_get_running_partition();
    esp_app_desc_t desc = {};
    if (part && esp_ota_get_partition_description(part, &desc) == ESP_OK)
        strlcpy(version, desc.version, sizeof(version));

    char tz[64] = {0};
    nvs_config_get_str(NVS_KEY_TIMEZONE, tz, sizeof(tz));
    if (tz[0] == '\0') strlcpy(tz, NVS_DEFAULT_TIMEZONE, sizeof(tz));

    char j_ssid[128]  = {0};
    char j_city[64]   = {0};
    char j_coins[256] = {0};
    char j_tz[80]     = {0};
    json_escape(ssid,  j_ssid,  sizeof(j_ssid));
    json_escape(city,  j_city,  sizeof(j_city));
    json_escape(coins, j_coins, sizeof(j_coins));
    json_escape(tz,    j_tz,    sizeof(j_tz));

    char buf[896];
    snprintf(buf, sizeof(buf),
        "{\"ip\":\"%s\",\"connected\":%s,\"rssi\":%d,\"uptime_s\":%lld,"
        "\"ssid\":\"%s\",\"city\":\"%s\",\"coins\":\"%s\",\"currency\":\"%s\","
        "\"timezone\":\"%s\","
        "\"ota_running\":%s,\"firmware\":\"%s\"}",
        ip,
        wifi_manager_is_connected() ? "true" : "false",
        (int)rssi,
        (long long)uptime_s,
        j_ssid, j_city, j_coins, curr,
        j_tz,
        ota_handler_is_running() ? "true" : "false",
        version);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---- POST /ota  (body: url=http://host/firmware.bin) ---- */
static esp_err_t handler_ota(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 512) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Missing or oversized body\"}",
                        HTTPD_RESP_USE_STRLEN);
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

    char url[256] = {0};
    get_param(body, "url", url, sizeof(url));
    if (url[0] == '\0' ||
        (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"url must start with http:// or https://\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    esp_err_t err = ota_handler_start(url);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"OTA already running\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Failed to start OTA task\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA triggered via /ota: %s", url);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"message\":\"OTA started — device will restart on success\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---- POST /reset ---- */
static esp_err_t handler_reset(httpd_req_t *req)
{
    /* Drain any body the client sent (keep-alive hygiene) */
    char discard[64];
    while (httpd_req_recv(req, discard, sizeof(discard)) > 0) {}

    nvs_config_factory_reset();
    ESP_LOGW(TAG, "Factory reset via /reset");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req,
        "{\"ok\":true,\"message\":\"Factory reset complete — restarting\"}",
        HTTPD_RESP_USE_STRLEN);
    esp_timer_start_once(s_restart_timer, 2000000ULL);
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

static httpd_handle_t s_config_server = NULL;

esp_err_t http_server_start_config(void)
{
    if (s_config_server) return ESP_OK;

    if (!s_restart_timer) {
        esp_timer_create_args_t ta = { .callback = restart_cb, .name = "cfg_rst" };
        ESP_ERROR_CHECK(esp_timer_create(&ta, &s_restart_timer));
    }

    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 6;
    if (httpd_start(&s_config_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start config server");
        return ESP_FAIL;
    }

    static const httpd_uri_t u_get    = { .uri = "/",       .method = HTTP_GET,  .handler = handler_cfg_get  };
    static const httpd_uri_t u_post   = { .uri = "/save",   .method = HTTP_POST, .handler = handler_cfg_post };
    static const httpd_uri_t u_status = { .uri = "/status", .method = HTTP_GET,  .handler = handler_status   };
    static const httpd_uri_t u_ota    = { .uri = "/ota",    .method = HTTP_POST, .handler = handler_ota      };
    static const httpd_uri_t u_reset  = { .uri = "/reset",  .method = HTTP_POST, .handler = handler_reset    };
    httpd_register_uri_handler(s_config_server, &u_get);
    httpd_register_uri_handler(s_config_server, &u_post);
    httpd_register_uri_handler(s_config_server, &u_status);
    httpd_register_uri_handler(s_config_server, &u_ota);
    httpd_register_uri_handler(s_config_server, &u_reset);

    char ip[16] = {0};
    wifi_manager_get_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "Config server: http://%s/", ip);
    return ESP_OK;
}

void http_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    if (s_config_server) {
        httpd_stop(s_config_server);
        s_config_server = NULL;
    }
}
