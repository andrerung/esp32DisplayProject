#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t ota_handler_start(const char *url);
bool      ota_handler_is_running(void);
