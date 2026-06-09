#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_config.h"
#include "wifi_manager.h"

ESP_EVENT_DEFINE_BASE(WIFI_MANAGER_EVENTS);
static const char *TAG = "wifi_manager";

static bool s_connected;
static bool s_ap_mode;
static char s_ip[16];
static int  s_retry;
static esp_timer_handle_t s_reconnect_timer;

#define SCAN_MAX_APS 20
static wifi_scan_ap_t s_scan_aps[SCAN_MAX_APS];
static int            s_scan_count = 0;

static int backoff_s(int retry)
{
    int d = 1 << (retry < 6 ? retry : 6); /* 1,2,4,8,16,32,64 → cap 60 */
    return d > 60 ? 60 : d;
}

static void reconnect_cb(void *arg)
{
    ESP_LOGI(TAG, "Reconnecting...");
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (s_ap_mode) {
        if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
            ESP_LOGI(TAG, "AP started — signaling captive portal");
            esp_event_post(WIFI_MANAGER_EVENTS, WIFI_MANAGER_AP_STARTED,
                           NULL, 0, 0);
            esp_err_t se = esp_wifi_scan_start(NULL, false); /* async */
            if (se != ESP_OK)
                ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(se));
        } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
            uint16_t count = SCAN_MAX_APS;
            wifi_ap_record_t recs[SCAN_MAX_APS];
            if (esp_wifi_scan_get_ap_records(&count, recs) == ESP_OK) {
                s_scan_count = 0;
                for (uint16_t i = 0; i < count && s_scan_count < SCAN_MAX_APS; i++) {
                    if (recs[i].ssid[0] == '\0') continue;
                    bool dup = false;
                    for (int j = 0; j < s_scan_count; j++) {
                        if (strcmp(s_scan_aps[j].ssid,
                                   (char *)recs[i].ssid) == 0) {
                            dup = true; break;
                        }
                    }
                    if (!dup) {
                        strlcpy(s_scan_aps[s_scan_count].ssid,
                                (char *)recs[i].ssid,
                                sizeof(s_scan_aps[0].ssid));
                        s_scan_aps[s_scan_count].rssi = recs[i].rssi;
                        s_scan_count++;
                    }
                }
                ESP_LOGI(TAG, "WiFi scan: %d networks found", s_scan_count);
            }
        }
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        esp_event_post(WIFI_MANAGER_EVENTS, WIFI_MANAGER_DISCONNECTED, NULL, 0, 0);
        int delay = backoff_s(s_retry++);
        ESP_LOGW(TAG, "Disconnected — retry in %d s", delay);
        if (s_reconnect_timer) {
            esp_timer_start_once(s_reconnect_timer, (uint64_t)delay * 1000000ULL);
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        s_retry = 0;
        ESP_LOGI(TAG, "IP: %s", s_ip);
        esp_event_post(WIFI_MANAGER_EVENTS, WIFI_MANAGER_CONNECTED, NULL, 0, 0);
    }
}

static void start_sta_mode(const char *ssid, const char *pass)
{
    esp_timer_create_args_t ta = {
        .callback = reconnect_cb,
        .name     = "wm_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_reconnect_timer));

    esp_netif_create_default_wifi_sta();
    wifi_config_t wcfg = {};
    strlcpy((char *)wcfg.sta.ssid,     ssid, sizeof(wcfg.sta.ssid));
    strlcpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password));
    /* WPA2+WPA3 mixed mode: prefer SAE (WPA3) when the AP supports it.
       UniFi-UDM advertises WPA3-transitional; WPA2-only connections get a GTK
       re-key timeout at 10 s because PMF-protected group-key EAPOL frames are
       required by the AP but not properly handled without SAE negotiation.
       WPA3-SAE automatically enables PMF (pmf:1), which resolves both issues. */
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
    wcfg.sta.pmf_cfg.capable    = true;
    wcfg.sta.pmf_cfg.required   = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start()); /* STA_START fires → connect */
    /* Disable power save so DHCP offers and GTK re-key frames are never missed. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "Connecting to \"%s\"...", ssid);
}

static void start_ap_mode(void)
{
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta(); /* APSTA mode needed to scan while AP is up */

    /* Fresh SSID — iOS caches "InfoDisplay-Setup" as WPA2-secured from
       earlier attempts and hides/refuses it now that it is open (security
       downgrade / evil-twin protection). A name iOS has never seen joins
       cleanly. */
    static const char AP_SSID[] = "InfoDisplay-Config";

    /* Open AP (no password) — removes the WPA2 4-way handshake, which was
       causing iOS to loop on join. Minimal config matching the proven
       ESP-IDF captive_portal example. */
    wifi_config_t ap_cfg = {};
    strlcpy((char *)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len       = (uint8_t)strlen(AP_SSID);
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    /* Drop 802.11n: the client kept re-associating every ~0.5s against the
       SoftAP's 11n and never completed DHCP. 11b/g is slower but stable for
       a config portal. Not fatal if it fails — log and continue. */
    esp_err_t pe = esp_wifi_set_protocol(WIFI_IF_AP,
                       WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
    if (pe != ESP_OK) ESP_LOGW(TAG, "set_protocol: %s", esp_err_to_name(pe));

    s_ap_mode = true;
    ESP_LOGI(TAG, "AP mode configured: SSID=\"%s\" (open, 11bg)", AP_SSID);
    ESP_ERROR_CHECK(esp_wifi_start()); /* fires WIFI_EVENT_AP_START async */

    /* 34 × 0.25 dBm = 8.5 dBm — captive portal phone is close; curbs current
       spikes that brown out the radio on USB-powered boards.
       Must be called after esp_wifi_start(). */
    esp_err_t te = esp_wifi_set_max_tx_power(34);
    if (te != ESP_OK) ESP_LOGW(TAG, "set_max_tx_power: %s", esp_err_to_name(te));
}

esp_err_t wifi_manager_start(void)
{
    char ssid[64] = {0};
    nvs_config_get_str(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    if (ssid[0] == '\0') {
        start_ap_mode();
    } else {
        char pass[64] = {0};
        nvs_config_get_str(NVS_KEY_WIFI_PASS, pass, sizeof(pass));
        start_sta_mode(ssid, pass);
    }
    return ESP_OK;
}

int wifi_manager_copy_scan_results(wifi_scan_ap_t *out, int max_count)
{
    int n = s_scan_count < max_count ? s_scan_count : max_count;
    memcpy(out, s_scan_aps, (size_t)n * sizeof(wifi_scan_ap_t));
    return n;
}

bool wifi_manager_is_connected(void) { return s_connected; }
bool wifi_manager_is_ap_mode(void)   { return s_ap_mode;   }

void wifi_manager_get_ip(char *buf, size_t len)
{
    strlcpy(buf, s_connected ? s_ip : "0.0.0.0", len);
}

int8_t wifi_manager_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}
