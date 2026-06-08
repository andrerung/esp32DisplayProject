#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "ota_handler.h"

static const char *TAG = "ota_handler";
static bool s_running;

static void ota_task(void *arg)
{
    char *url = (char *)arg;
    ESP_LOGI(TAG, "OTA start: %s", url);

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 30000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA success — restarting");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    }

    free(url);
    s_running = false;
    vTaskDelete(NULL);
}

esp_err_t ota_handler_start(const char *url)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    char *url_copy = strdup(url);
    if (!url_copy) return ESP_ERR_NO_MEM;
    s_running = true;
    if (xTaskCreate(ota_task, "ota", 8192, url_copy, 5, NULL) != pdPASS) {
        free(url_copy);
        s_running = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool ota_handler_is_running(void) { return s_running; }
