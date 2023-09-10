#pragma once

#include "hw/qdev-core.h"
#include "qom/object.h"
#include "ui/pixel_ops.h"
#include "ui/console.h"
#include "hw/arm/pmb887x/fifo.h"

#define TYPE_PMB887X_LCD	"pmb887x-lcd"
OBJECT_DECLARE_TYPE(pmb887x_lcd_t, pmb887x_lcd_class_t, PMB887X_LCD)

enum pmb887x_lcd_addr_mode_t {
	LCD_ADDR_MODE_INCR,
	LCD_ADDR_MODE_DECR
};

enum pmb887x_lcd_pixel_mode_t {
	LCD_MODE_NONE = 0,
	
	/* BGR */
	LCD_MODE_BGR565, // 16bit
	LCD_MODE_BGR666, // 18bit
	LCD_MODE_BGR888, // 24bit
	
	/*  RGB */
	LCD_MODE_RGB565, // 16bit
	LCD_MODE_RGB666, // 18bit
	LCD_MODE_RGB888, // 24bit
};

struct pmb887x_lcd_t {
	DeviceState qdev;
	pmb887x_fifo8_t write_fifo;
	bool cd;
	bool write_to_ram;
	bool v_flip;
	bool h_flip;
	
	uint32_t rotation;
	uint32_t width;
	uint32_t height;
	uint8_t bpp;
	uint8_t byte_pp;
	enum pmb887x_lcd_pixel_mode_t mode;
	pixman_format_code_t format;
	
	uint32_t tmp_pixel;
	uint32_t tmp_index;
	
	uint8_t *buffer;
	uint32_t buffer_size;
	uint32_t buffer_index;
	
	uint32_t buffer_x;
	uint32_t buffer_y;
	
	uint32_t window_x1;
	uint32_t window_x2;
	uint32_t window_y1;
	uint32_t window_y2;
	
	enum pmb887x_lcd_addr_mode_t mode_x;
	enum pmb887x_lcd_addr_mode_t mode_y;
	
	DisplaySurface *surface;
    QemuConsole *console;
	
	bool invalidate;
};

struct pmb887x_lcd_class_t {
	DeviceClass parent_class;
	uint16_t bus_width;
	bool write_to_ram;
	void (*write)(pmb887x_lcd_t *lcd, uint32_t value);
	void (*write_ram)(pmb887x_lcd_t *lcd, uint32_t value, uint32_t size);
};

void pmb887x_lcd_init(pmb887x_lcd_t *lcd, DeviceState *dev);
void pmb887x_lcd_write(pmb887x_lcd_t *lcd, uint32_t value, uint32_t size);
void pmb887x_lcd_set_cd(pmb887x_lcd_t *lcd, bool value);
void pmb887x_lcd_put_pixel_byte(pmb887x_lcd_t *lcd, uint8_t byte);
void pmb887x_lcd_set_mode(pmb887x_lcd_t *lcd, enum pmb887x_lcd_pixel_mode_t mode);

static inline void pmb887x_lcd_set_vflip(pmb887x_lcd_t *lcd, bool flag) {
	lcd->v_flip = flag;
}

static inline void pmb887x_lcd_set_hflip(pmb887x_lcd_t *lcd, bool flag) {
	lcd->h_flip = flag;
}

static inline void pmb887x_lcd_set_addr_mode_x(pmb887x_lcd_t *lcd, enum pmb887x_lcd_addr_mode_t mode) {
	lcd->mode_x = mode;
}

static inline void pmb887x_lcd_set_addr_mode_y(pmb887x_lcd_t *lcd, enum pmb887x_lcd_addr_mode_t mode) {
	lcd->mode_y = mode;
}

static inline void pmb887x_lcd_set_window_y1(pmb887x_lcd_t *lcd, uint32_t value) {
	lcd->window_y1 = value;
}

static inline void pmb887x_lcd_set_window_y2(pmb887x_lcd_t *lcd, uint32_t value) {
	lcd->window_y2 = value;
}

static inline void pmb887x_lcd_set_window_x1(pmb887x_lcd_t *lcd, uint32_t value) {
	lcd->window_x1 = value;
}

static inline void pmb887x_lcd_set_window_x2(pmb887x_lcd_t *lcd, uint32_t value) {
	lcd->window_x2 = value;
}

static inline void pmb887x_lcd_set_x(pmb887x_lcd_t *lcd, uint32_t value) {
	lcd->buffer_x = value;
}

static inline void pmb887x_lcd_set_y(pmb887x_lcd_t *lcd, uint32_t value) {
	lcd->buffer_y = value;
}

static inline bool pmb887x_lcd_get_cd(pmb887x_lcd_t *lcd) {
	return lcd->cd;
}

static inline void pmb887x_lcd_set_ram_mode(pmb887x_lcd_t *lcd, bool flag) {
	lcd->write_to_ram = flag;
	lcd->tmp_index = 0;
	if (!flag)
		lcd->invalidate = true;
	pmb887x_fifo_reset(&lcd->write_fifo);
}
