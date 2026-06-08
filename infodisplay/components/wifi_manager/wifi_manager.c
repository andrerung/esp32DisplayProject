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
static char s_ip[16];
static int  s_retry;
static esp_timer_handle_t s_reconnect_timer;

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
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        esp_event_post(WIFI_MANAGER_EVENTS, WIFI_MANAGER_DISCONNECTED, NULL, 0, 0);
        int delay = backoff_s(s_retry++);
        ESP_LOGW(TAG, "Disconnected — retry in %d s", delay);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)delay * 1000000ULL);

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        s_retry = 0;
        ESP_LOGI(TAG, "IP: %s", s_ip);
        esp_event_post(WIFI_MANAGER_EVENTS, WIFI_MANAGER_CONNECTED, NULL, 0, 0);
    }
}

esp_err_t wifi_manager_start(void)
{
    char ssid[64] = {0}, pass[64] = {0};
    nvs_config_get_str(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid));
    nvs_config_get_str(NVS_KEY_WIFI_PASS, pass, sizeof(pass));

    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "No WiFi SSID configured — skipping");
        return ESP_ERR_INVALID_STATE;
    }

    esp_timer_create_args_t ta = {
        .callback = reconnect_cb,
        .name     = "wm_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_reconnect_timer));

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t wcfg = {};
    strlcpy((char *)wcfg.sta.ssid,     ssid, sizeof(wcfg.sta.ssid));
    strlcpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start()); /* STA_START fires → connect */
    ESP_LOGI(TAG, "Connecting to \"%s\"...", ssid);
    return ESP_OK;
}

bool wifi_manager_is_connected(void) { return s_connected; }

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
