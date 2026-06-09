#pragma once
#include "esp_err.h"

/* Start the captive-portal HTTP server (AP setup mode) */
esp_err_t http_server_start_portal(void);

/* Start the LAN config server (STA mode) — pre-fills current NVS values */
esp_err_t http_server_start_config(void);

void http_server_stop(void);
