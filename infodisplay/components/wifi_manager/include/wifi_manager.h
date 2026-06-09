#pragma once
#include "esp_err.h"
#include "esp_event.h"
#include <stdbool.h>
#include <stddef.h>

ESP_EVENT_DECLARE_BASE(WIFI_MANAGER_EVENTS);

typedef enum {
    WIFI_MANAGER_CONNECTED    = 0,
    WIFI_MANAGER_DISCONNECTED = 1,
    WIFI_MANAGER_AP_STARTED   = 2,  /* no WiFi credentials — AP setup mode */
} wifi_manager_event_id_t;

typedef struct {
    char   ssid[33];
    int8_t rssi;
} wifi_scan_ap_t;

/* Returns number of networks from the last background scan (0 if not done yet). */
int wifi_manager_copy_scan_results(wifi_scan_ap_t *out, int max_count);

esp_err_t wifi_manager_start(void);
bool      wifi_manager_is_connected(void);
bool      wifi_manager_is_ap_mode(void);
void      wifi_manager_get_ip(char *buf, size_t len);
int8_t    wifi_manager_get_rssi(void);
