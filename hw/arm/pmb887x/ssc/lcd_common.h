#pragma once

#include "hw/core/qdev.h"
#include "qapi-builtin-types.h"
#include "qom/object.h"
#include "hw/ssi/ssi.h"
#include "ui/console.h"
#include "hw/arm/pmb887x/fifo.h"
#include "hw/arm/pmb887x/ssc/lcd_common_format.h"

#define TYPE_PMB887X_LCD	"pmb887x-lcd"
OBJECT_DECLARE_TYPE(pmb887x_lcd_t, pmb887x_lcd_class_t, PMB887X_LCD);

#define LCD_DATA_IS_CMD (1 << 8)

typedef struct pmb887x_lcd_rect_t pmb887x_lcd_rect_t;

enum pmb887x_lcd_wr_state_t {
	LCD_WR_STATE_NONE,
	LCD_WR_STATE_CMD,
	LCD_WR_STATE_PARAM,
	LCD_WR_STATE_RAM,
	LCD_WR_STATE_IGNORE,
};

enum pmb887x_lcd_ac_t {
	LCD_AC_DEC,
	LCD_AC_INC,
};

enum pmb887x_lcd_am_t {
	LCD_AM_HORIZONTAL,
	LCD_AM_VERTICAL,
};

struct pmb887x_lcd_rect_t {
	int x1;
	int y1;
	int x2;
	int y2;
};

struct pmb887x_lcd_t {
	SSIPeripheral parent;
	pmb887x_lcd_class_t *k;

	pmb887x_fifo8_t fifo;
	bool cd;
	bool read_active;
	bool reset_active;

	uint32_t width;
	uint32_t height;

	uint32_t rotation;
	bool flip_horizontal;
	bool flip_vertical;

	uint32_t default_rotation;
	bool default_flip_horizontal;
	bool default_flip_vertical;
	bool bgr_filter;

	uint8_t bpp;
	uint8_t byte_pp;
	enum pmb887x_lcd_pixel_format_t pixel_format;
	uint32_t (*decode_pixel)(uint32_t);
	uint32_t (*encode_pixel)(uint32_t);
	bool output_bgr;

	bool mirror_xy;

	uint32_t tmp_pixel;
	uint32_t tmp_index;
	uint32_t gram_read_dummy_bytes;

	uint32_t *gram; /* 0x00RRGGBB */

	int buffer_x;
	int buffer_y;

	pmb887x_lcd_rect_t window;
	pmb887x_lcd_rect_t dirty;

	enum pmb887x_lcd_wr_state_t wr_state;
	uint32_t current_cmd;
	int current_cmd_params;

	enum pmb887x_lcd_ac_t ac_x;
	enum pmb887x_lcd_ac_t ac_y;
	enum pmb887x_lcd_am_t am;

	uint8_t *shadow_buffer;
	DisplaySurface *shadow_surface;

	DisplaySurface *surface;
	QemuConsole *console;

	bool invalidate;
};

struct pmb887x_lcd_class_t {
	SSIPeripheralClass parent_class;
	uint16_t cmd_width;
	uint16_t param_width;
	uint8_t gram_read_dummy_pixels;
	bool direct_data_write;
	bool cd_polarity;
	int (*on_cmd)(pmb887x_lcd_t *, uint32_t);
	void (*on_cmd_with_params)(pmb887x_lcd_t *, uint32_t, const uint32_t *, uint32_t);
	uint32_t (*on_read)(pmb887x_lcd_t *, uint32_t);
	void (*reset)(pmb887x_lcd_t *);
	void (*realize)(pmb887x_lcd_t *, Error **errp);
};

void pmb887x_lcd_set_pixel_format(pmb887x_lcd_t *lcd, enum pmb887x_lcd_pixel_format_t format);
void pmb887x_lcd_set_output_bgr(pmb887x_lcd_t *lcd, bool enabled);
void pmb887x_lcd_set_transform(pmb887x_lcd_t *lcd, bool flip_h_pins, bool flip_v_pins);
void pmb887x_lcd_set_ram_mode(pmb887x_lcd_t *lcd, bool flag);
void pmb887x_lcd_set_addr_mode(pmb887x_lcd_t *lcd, enum pmb887x_lcd_am_t am, enum pmb887x_lcd_ac_t ac_x, enum pmb887x_lcd_ac_t ac_y);

static inline bool pmb887x_lcd_get_cd(pmb887x_lcd_t *lcd) {
	return lcd->cd;
}

static inline void pmb887x_lcd_set_cd(pmb887x_lcd_t *lcd, bool value) {
	lcd->cd = value;
}

static inline void pmb887x_lcd_set_window_y1(pmb887x_lcd_t *lcd, uint32_t value) {
	lcd->window.y1 = MIN(value, lcd->height - 1);
}

static inline void pmb887x_lcd_set_window_y2(pmb887x_lcd_t *lcd, uint32_t value) {
	lcd->window.y2 = MIN(value, lcd->height - 1);
}

static inline void pmb887x_lcd_set_window_x1(pmb887x_lcd_t *lcd, uint32_t value) {
	lcd->window.x1 = MIN(value, lcd->width - 1);
}

static inline void pmb887x_lcd_set_window_x2(pmb887x_lcd_t *lcd, uint32_t value) {
	lcd->window.x2 = MIN(value, lcd->width - 1);
}

static inline void pmb887x_lcd_set_x(pmb887x_lcd_t *lcd, uint32_t value) {
	lcd->buffer_x = MIN(value, lcd->width - 1);
}

static inline void pmb887x_lcd_set_y(pmb887x_lcd_t *lcd, uint32_t value) {
	lcd->buffer_y = MIN(value, lcd->height - 1);
}
