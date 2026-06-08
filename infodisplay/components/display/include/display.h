#pragma once

#include <stdint.h>

/* 320x240 landscape, RGB565 */
#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240

/* RGB565 colour helpers */
#define RGB565(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)))
#define COLOR_BLACK   RGB565(0,   0,   0)
#define COLOR_WHITE   RGB565(255, 255, 255)
#define COLOR_RED     RGB565(255, 0,   0)
#define COLOR_GREEN   RGB565(0,   255, 0)
#define COLOR_BLUE    RGB565(0,   0,   255)
#define COLOR_YELLOW  RGB565(255, 255, 0)
#define COLOR_CYAN    RGB565(0,   255, 255)
#define COLOR_GRAY    RGB565(128, 128, 128)

esp_err_t display_init(void);
void display_fill_color(uint16_t color);
void display_fill_rect(int x, int y, int w, int h, uint16_t color);
void display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);
void display_draw_text_large(int x, int y, const char *text, uint16_t fg, uint16_t bg);
