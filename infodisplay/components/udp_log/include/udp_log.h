#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t udp_log_init(const char *host, uint16_t port);
void udp_log_stop(void);
