#include "qemu/osdep.h"
#include "hw/arm/pmb887x/ssc/lcd_common_format.h"

static uint32_t lcd_format_rgb565_decode(uint32_t value) {
	uint32_t red = (value >> 11) & 0x1F;
	uint32_t green = (value >> 5) & 0x3F;
	uint32_t blue = value & 0x1F;
	return ((red << 3) | (red >> 2)) << 16 |
		((green << 2) | (green >> 4)) << 8 |
		(blue << 3) | (blue >> 2);
}

static uint32_t lcd_format_rgb565_encode(uint32_t value) {
	uint32_t red = (value >> 19) & 0x1F;
	uint32_t green = (value >> 10) & 0x3F;
	uint32_t blue = (value >> 3) & 0x1F;
	return red << 11 | green << 5 | blue;
}

static uint32_t lcd_format_rgb666_expand(uint32_t red, uint32_t green, uint32_t blue) {
	return ((red << 2) | (red >> 4)) << 16 |
		((green << 2) | (green >> 4)) << 8 |
		(blue << 2) | (blue >> 4);
}

static uint32_t lcd_format_rgb666_8_8_2_decode(uint32_t value) {
	return lcd_format_rgb666_expand((value >> 18) & 0x3F, (value >> 12) & 0x3F, (value >> 6) & 0x3F);
}

static uint32_t lcd_format_rgb666_8_8_2_encode(uint32_t value) {
	return ((value >> 18) & 0x3F) << 18 | ((value >> 10) & 0x3F) << 12 | ((value >> 2) & 0x3F) << 6;
}

static uint32_t lcd_format_rgb666_2_8_8_decode(uint32_t value) {
	return lcd_format_rgb666_expand((value >> 12) & 0x3F, (value >> 6) & 0x3F, value & 0x3F);
}

static uint32_t lcd_format_rgb666_2_8_8_encode(uint32_t value) {
	uint32_t red = (value >> 18) & 0x3F;
	uint32_t green = (value >> 10) & 0x3F;
	uint32_t blue = (value >> 2) & 0x3F;
	return red << 12 | green << 6 | blue;
}

static uint32_t lcd_format_rgb666_6_6_6_decode(uint32_t value) {
	return lcd_format_rgb666_expand((value >> 18) & 0x3F, (value >> 10) & 0x3F, (value >> 2) & 0x3F);
}

static uint32_t lcd_format_rgb666_6_6_6_encode(uint32_t value) {
	return ((value >> 18) & 0x3F) << 18 | ((value >> 10) & 0x3F) << 10 | ((value >> 2) & 0x3F) << 2;
}

static uint32_t lcd_format_rgb888_decode(uint32_t value) {
	return value & 0xFFFFFF;
}

static uint32_t lcd_format_rgb888_encode(uint32_t value) {
	return value & 0xFFFFFF;
}

static const pmb887x_lcd_format_t LCD_PIXEL_FORMATS[LCD_PIXEL_FORMAT_COUNT] = {
	[LCD_PIXEL_FORMAT_RGB565] = { 2, 16, lcd_format_rgb565_decode, lcd_format_rgb565_encode },
	[LCD_PIXEL_FORMAT_RGB666_8_8_2] = { 3, 18, lcd_format_rgb666_8_8_2_decode, lcd_format_rgb666_8_8_2_encode },
	[LCD_PIXEL_FORMAT_RGB666_2_8_8] = { 3, 18, lcd_format_rgb666_2_8_8_decode, lcd_format_rgb666_2_8_8_encode },
	[LCD_PIXEL_FORMAT_RGB666_6_6_6] = { 3, 18, lcd_format_rgb666_6_6_6_decode, lcd_format_rgb666_6_6_6_encode },
	[LCD_PIXEL_FORMAT_RGB888] = { 3, 24, lcd_format_rgb888_decode, lcd_format_rgb888_encode },
};

const pmb887x_lcd_format_t *pmb887x_lcd_format_get(enum pmb887x_lcd_pixel_format_t format) {
	if (format <= LCD_PIXEL_FORMAT_NONE || format >= LCD_PIXEL_FORMAT_COUNT)
		return NULL;
	return &LCD_PIXEL_FORMATS[format];
}
