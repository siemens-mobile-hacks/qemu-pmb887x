#pragma once

#include "hw/qdev-core.h"
#include "qom/object.h"
#include "hw/ssi/ssi.h"
#include "ui/console.h"
#include "hw/arm/pmb887x/fifo.h"

#define TYPE_PMB887X_LCD	"pmb887x-lcd"
OBJECT_DECLARE_TYPE(pmb887x_lcd_t, pmb887x_lcd_class_t, PMB887X_LCD);

#define LCD_DATA_IS_CMD (1 << 8)

enum pmb887x_lcd_wr_state_t {
	LCD_WR_STATE_NONE,
	LCD_WR_STATE_CMD,
	LCD_WR_STATE_PARAM,
	LCD_WR_STATE_RAM
};

enum pmb887x_lcd_ac_t {
	LCD_AC_DEC,
	LCD_AC_INC,
};

enum pmb887x_lcd_am_t {
	LCD_AM_HORIZONTAL,
	LCD_AM_VERTICAL,
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
	SSIPeripheral parent;
	pmb887x_lcd_class_t *k;

	pmb887x_fifo8_t fifo;
	bool cd;

	uint32_t rotation;
	uint32_t width;
	uint32_t height;
	uint32_t phys_width;
	uint32_t phys_height;
	bool flip_horizontal;
	bool flip_vertical;

	uint8_t bpp;
	uint8_t byte_pp;
	enum pmb887x_lcd_pixel_mode_t mode;
	pixman_format_code_t format;

	bool mirror_xy;

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

	enum pmb887x_lcd_wr_state_t wr_state;
	uint32_t current_cmd;
	uint32_t current_cmd_params;

	enum pmb887x_lcd_ac_t ac_x;
	enum pmb887x_lcd_ac_t ac_y;
	enum pmb887x_lcd_am_t am;

	DisplaySurface *surface;
	QemuConsole *console;

	uint32_t bus_width;

	bool invalidate;
};

struct pmb887x_lcd_class_t {
	SSIPeripheralClass parent_class;
	uint16_t cmd_width;
	uint16_t param_width;
	uint32_t write_ram_cmd;
	uint32_t (*on_cmd)(pmb887x_lcd_t *, uint32_t);
	void (*on_cmd_with_params)(pmb887x_lcd_t *, uint32_t, const uint32_t *, uint32_t);
	void (*realize)(pmb887x_lcd_t *, Error **errp);
};

void pmb887x_lcd_init(pmb887x_lcd_t *lcd, DeviceState *dev);
void pmb887x_lcd_write(pmb887x_lcd_t *lcd, uint32_t value, uint32_t size);
void pmb887x_lcd_set_mode(pmb887x_lcd_t *lcd, enum pmb887x_lcd_pixel_mode_t mode);
void pmb887x_lcd_set_cd(pmb887x_lcd_t *lcd, bool value);
void pmb887x_lcd_set_ram_mode(pmb887x_lcd_t *lcd, bool flag);
void pmb887x_lcd_set_addr_mode(pmb887x_lcd_t *lcd, enum pmb887x_lcd_am_t mode, enum pmb887x_lcd_ac_t ac_x, enum pmb887x_lcd_ac_t ac_y);

void pmb887x_lcd_set_window_y1(pmb887x_lcd_t *lcd, uint32_t value);
void pmb887x_lcd_set_window_y2(pmb887x_lcd_t *lcd, uint32_t value);
void pmb887x_lcd_set_window_x1(pmb887x_lcd_t *lcd, uint32_t value);
void pmb887x_lcd_set_window_x2(pmb887x_lcd_t *lcd, uint32_t value);
void pmb887x_lcd_set_x(pmb887x_lcd_t *lcd, uint32_t value);
void pmb887x_lcd_set_y(pmb887x_lcd_t *lcd, uint32_t value);

bool pmb887x_lcd_get_cd(pmb887x_lcd_t *lcd);
