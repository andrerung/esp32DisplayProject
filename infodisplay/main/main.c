#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "display.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 InfoDisplay — Phase 1 hardware validation");

    ESP_ERROR_CHECK(display_init());

    ESP_LOGI(TAG, "Fill RED");
    display_fill_color(COLOR_RED);
    vTaskDelay(pdMS_TO_TICKS(800));

    ESP_LOGI(TAG, "Fill GREEN");
    display_fill_color(COLOR_GREEN);
    vTaskDelay(pdMS_TO_TICKS(800));

    ESP_LOGI(TAG, "Fill BLUE");
    display_fill_color(COLOR_BLUE);
    vTaskDelay(pdMS_TO_TICKS(800));

    ESP_LOGI(TAG, "Draw text");
    display_fill_color(COLOR_BLACK);
    display_draw_text_large(8,  30, "Hello",                     COLOR_WHITE,  COLOR_BLACK);
    display_draw_text_large(8,  66, "InfoDisplay",               COLOR_WHITE,  COLOR_BLACK);
    display_draw_text      (8, 120, "Phase 1: HW Validation",    COLOR_CYAN,   COLOR_BLACK);
    display_draw_text      (8, 144, "ILI9341 SPI OK",            COLOR_GREEN,  COLOR_BLACK);
    display_draw_text      (8, 168, "240x320 portrait",          COLOR_YELLOW, COLOR_BLACK);

    ESP_LOGI(TAG, "Phase 1 display test complete");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
