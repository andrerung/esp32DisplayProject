#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "hello_world";

void app_main(void)
{
    int count = 0;
    while (1) {
        ESP_LOGI(TAG, "Hello, World! count=%d", count++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
