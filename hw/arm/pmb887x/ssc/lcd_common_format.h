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

/* inline for lcd_run_rows(), which decodes a burst without the indirect
 * call through decode_pixel */
static inline uint32_t pmb887x_lcd_rgb565_decode(uint32_t value) {
	uint32_t red = (value >> 11) & 0x1F;
	uint32_t green = (value >> 5) & 0x3F;
	uint32_t blue = value & 0x1F;
	return ((red << 3) | (red >> 2)) << 16 |
		((green << 2) | (green >> 4)) << 8 |
		(blue << 3) | (blue >> 2);
}

const pmb887x_lcd_format_t *pmb887x_lcd_format_get(enum pmb887x_lcd_pixel_format_t format);
