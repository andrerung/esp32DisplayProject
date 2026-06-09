#pragma once

#include <stdint.h>
#include "esp_err.h"

/* 240x320 portrait, RGB565 */
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  320

/* RGB565 colour helpers */
#define RGB565(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)))
#define COLOR_BLACK       RGB565(0,   0,   0)
#define COLOR_WHITE       RGB565(255, 255, 255)
#define COLOR_RED         RGB565(255, 0,   0)
#define COLOR_GREEN       RGB565(0,   255, 0)
#define COLOR_BLUE        RGB565(0,   0,   255)
#define COLOR_YELLOW      RGB565(255, 255, 0)
#define COLOR_CYAN        RGB565(0,   255, 255)
#define COLOR_GRAY        RGB565(128, 128, 128)
#define COLOR_DARK_GRAY   RGB565(40,  40,  40)
#define COLOR_LIGHT_GRAY  RGB565(180, 180, 180)
#define COLOR_ORANGE      RGB565(255, 165, 0)

/* ---- UI tile layout (portrait 240×320) ----
 *
 *  y=  0  h=48  TIME    (large 2× clock)
 *  y= 48  h=16  DATE    (small date line)
 *  y= 64  h= 2  SEP1
 *  y= 66  h=60  WEATHER (2 small lines)
 *  y=126  h= 2  SEP2
 *  y=128  h=58  CRYPTO0
 *  y=186  h=58  CRYPTO1
 *  y=244  h=58  CRYPTO2
 *  y=302  h= 2  SEP3
 *  y=304  h=16  STATUS
 */
#define TILE_TIME_Y      0
#define TILE_TIME_H     48
#define TILE_DATE_Y     48
#define TILE_DATE_H     16
#define TILE_SEP1_Y     64
#define TILE_WEATHER_Y  66
#define TILE_WEATHER_H  60
#define TILE_SEP2_Y    126
#define TILE_CRYPTO0_Y 128
#define TILE_CRYPTO_H   58
#define TILE_CRYPTO1_Y 186
#define TILE_CRYPTO2_Y 244
#define TILE_SEP3_Y    302
#define TILE_STATUS_Y  304
#define TILE_STATUS_H   16

/* Phase 1 primitives */
esp_err_t display_init(void);
void display_fill_color(uint16_t color);
void display_fill_rect(int x, int y, int w, int h, uint16_t color);
void display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);
void display_draw_text_large(int x, int y, const char *text, uint16_t fg, uint16_t bg);

/* Phase 2 helpers */
void display_draw_hline(int y, uint16_t color);
void display_draw_text_centered(int y, const char *text, uint16_t fg, uint16_t bg);

/* Boot-phase scrolling log overlay */
void display_log_add(const char *msg);    /* thread-safe; call from vprintf hook */
void display_log_render(void);            /* call from UI task during splash phase */
