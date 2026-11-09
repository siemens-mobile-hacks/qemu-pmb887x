#pragma once

#include <stdint.h>

enum pmb887x_lcd_pixel_format_t {
	LCD_PIXEL_FORMAT_NONE,
	LCD_PIXEL_FORMAT_RGB565,
	LCD_PIXEL_FORMAT_RGB666_8_8_2,
	LCD_PIXEL_FORMAT_RGB666_2_8_8,
	LCD_PIXEL_FORMAT_RGB666_6_6_6,
	LCD_PIXEL_FORMAT_RGB888,
	LCD_PIXEL_FORMAT_COUNT,
};

typedef struct pmb887x_lcd_format_t pmb887x_lcd_format_t;

struct pmb887x_lcd_format_t {
	uint8_t bytes_per_pixel;
	uint8_t bits_per_pixel;
	uint32_t (*decode)(uint32_t);
	uint32_t (*encode)(uint32_t);
};

const pmb887x_lcd_format_t *pmb887x_lcd_format_get(enum pmb887x_lcd_pixel_format_t format);
