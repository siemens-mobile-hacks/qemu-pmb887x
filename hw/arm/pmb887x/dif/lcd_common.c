#define PMB887X_TRACE_ID		LCD
#define PMB887X_TRACE_PREFIX	"pmb887x-lcd-common"

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "migration/vmstate.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/fifo.h"
#include "hw/arm/pmb887x/dif/lcd_common.h"

#include <math.h>

#define PMB887X_LCD(obj)	OBJECT_CHECK(pmb887x_lcd_t, (obj), TYPE_PMB887X_LCD)

#define LCD_CMD_MAX_PARAMS 64

#define __SWAP_VALUES(a, b)		\
	do {						\
		typeof(a) temp = a;		\
		a = b;					\
		b = temp;				\
	} while(0)

static uint32_t pmb887x_lcd_get_px_index(pmb887x_lcd_t *lcd);
static void pmb887x_lcd_incr_px(pmb887x_lcd_t *lcd);

static void pmb887x_lcd_write_pixel_byte(pmb887x_lcd_t *lcd, uint8_t byte);
static void pmb887x_lcd_write_control_byte(pmb887x_lcd_t *lcd, uint8_t value);

static uint32_t pmb887x_lcd_read_from_fifo(pmb887x_lcd_t *lcd, uint32_t width);
static void pmb887x_lcd_clear_fifo(pmb887x_lcd_t *lcd);

static void pmb887x_lcd_set_mirror(pmb887x_lcd_t *lcd, bool flag);

static const char *pmb887x_lcd_get_mode_name(enum pmb887x_lcd_pixel_mode_t mode) {
	switch (mode) {
		case LCD_MODE_BGR565:	return "BGR565";
		case LCD_MODE_BGR666:	return "BGR666";
		case LCD_MODE_BGR888:	return "BGR888";
		case LCD_MODE_RGB565:	return "RGB565";
		case LCD_MODE_RGB666:	return "RGB666";
		case LCD_MODE_RGB888:	return "RGB888";
		case LCD_MODE_NONE:		return "NONE";
	}
	return "UNKNOWN";
}

void pmb887x_lcd_set_mode(pmb887x_lcd_t *lcd, enum pmb887x_lcd_pixel_mode_t mode) {
	if (lcd->mode == mode)
		return;
	
	if (lcd->surface)
		qemu_free_displaysurface(lcd->surface);
	
	if (lcd->buffer)
		g_free(lcd->buffer);
	
	switch (mode) {
		case LCD_MODE_BGR565:
			lcd->bpp = 16;
			lcd->byte_pp = 2;
			lcd->format = PIXMAN_b5g6r5;
		break;
		
		case LCD_MODE_BGR666:
			lcd->bpp = 18;
			lcd->byte_pp = 3;
			lcd->format = PIXMAN_b8g8r8;
		break;
		
		case LCD_MODE_BGR888:
			lcd->bpp = 24;
			lcd->byte_pp = 3;
			lcd->format = PIXMAN_b8g8r8;
		break;
		
		case LCD_MODE_RGB565:
			lcd->bpp = 16;
			lcd->byte_pp = 2;
			lcd->format = PIXMAN_r5g6b5;
		break;
		
		case LCD_MODE_RGB666:
			lcd->bpp = 18;
			lcd->byte_pp = 3;
			lcd->format = PIXMAN_r8g8b8;
		break;
		
		case LCD_MODE_RGB888:
			lcd->bpp = 24;
			lcd->byte_pp = 3;
			lcd->format = PIXMAN_r8g8b8;
		break;
		
		default:
			error_report("Invalid LCD mode: %d\n", mode);
			exit(1);
		break;
	}
	
	lcd->mode = mode;
	lcd->buffer_size = (lcd->width * lcd->height * lcd->byte_pp);
	lcd->buffer = g_new0(uint8_t, lcd->buffer_size);
	
	uint32_t linesize = (lcd->width * lcd->byte_pp);
	lcd->surface = qemu_create_displaysurface_from(lcd->width, lcd->height, lcd->format, linesize, lcd->buffer);
	dpy_gfx_replace_surface(lcd->console, lcd->surface);
	
	DPRINTF("mode %s, bpp: %d [%dB], buffer: %d\n", pmb887x_lcd_get_mode_name(lcd->mode), lcd->bpp, lcd->byte_pp, lcd->buffer_size);
}

static uint32_t pmb887x_lcd_get_px_index(pmb887x_lcd_t *lcd) {
	if (lcd->ac_x == LCD_AC_INC && lcd->ac_y == LCD_AC_INC) {
		return lcd->buffer_y * lcd->width + lcd->buffer_x;
	} else if (lcd->ac_x == LCD_AC_DEC && lcd->ac_y == LCD_AC_INC) {
		return lcd->buffer_y * lcd->width + (lcd->width - lcd->buffer_x - 1);
	} else if (lcd->ac_x == LCD_AC_INC && lcd->ac_y == LCD_AC_DEC) {
		return (lcd->height - lcd->buffer_y - 1) * lcd->width + lcd->buffer_x;
	} else {
		return (lcd->height - lcd->buffer_y - 1) * lcd->width + (lcd->width - lcd->buffer_x - 1);
	}
}

static void pmb887x_lcd_incr_px(pmb887x_lcd_t *lcd) {
	if (lcd->am == LCD_AM_VERTICAL) {
		lcd->buffer_y++;
		
		if (lcd->buffer_y > lcd->window_y2) {
			lcd->buffer_y = lcd->window_y1;
			lcd->buffer_x++;
			
			if (lcd->buffer_x > lcd->window_x2)
				lcd->buffer_x = lcd->window_x1;
		}
	} else {
		lcd->buffer_x++;
		
		if (lcd->buffer_x > lcd->window_x2) {
			lcd->buffer_x = lcd->window_x1;
			lcd->buffer_y++;
			
			if (lcd->buffer_y > lcd->window_y2)
				lcd->buffer_y = lcd->window_y1;
		}
	}
}

static void pmb887x_lcd_write_pixel_byte(pmb887x_lcd_t *lcd, uint8_t byte) {
	uint32_t pixel_index = pmb887x_lcd_get_px_index(lcd);
	lcd->buffer[pixel_index * lcd->byte_pp + lcd->tmp_index] = byte;
	lcd->tmp_index++;
	
	if (lcd->tmp_index == lcd->byte_pp) {
		lcd->tmp_index = 0;
		lcd->invalidate = true;
		pmb887x_lcd_incr_px(lcd);
	}
}

static void pmb887x_lcd_write_control_byte(pmb887x_lcd_t *lcd, uint8_t value) {
	pmb887x_lcd_class_t *k = PMB887X_LCD_GET_CLASS(lcd);
	
	
	DPRINTF("pmb887x_lcd_write_control_byte %d / %02X\n", lcd->cd, value);
	
	if (lcd->cd) {
		if (lcd->wr_state != LCD_WR_STATE_CMD) {
			if (lcd->wr_state == LCD_WR_STATE_PARAM)
				DPRINTF("CMD %04X ignored, too few params.\n", lcd->current_cmd);
			pmb887x_lcd_clear_fifo(lcd);
			lcd->wr_state = LCD_WR_STATE_CMD;
		}
		
		pmb887x_fifo8_push(&lcd->fifo, value);
		
		if (pmb887x_fifo_count(&lcd->fifo) >= k->cmd_width) {
			uint32_t cmd = pmb887x_lcd_read_from_fifo(lcd, k->cmd_width);
			lcd->wr_state = LCD_WR_STATE_PARAM;
			lcd->current_cmd = cmd;
			DPRINTF("CMD: %04X\n", lcd->current_cmd);
			lcd->current_cmd_params = k->on_cmd(lcd, cmd);
			g_assert(lcd->current_cmd_params <= LCD_CMD_MAX_PARAMS);
			
			if (lcd->current_cmd_params == 0 && lcd->wr_state == LCD_WR_STATE_PARAM)
				lcd->wr_state = LCD_WR_STATE_NONE;
		}
	} else {
		if (lcd->wr_state == LCD_WR_STATE_CMD) {
			pmb887x_lcd_clear_fifo(lcd);
			lcd->wr_state = LCD_WR_STATE_NONE;
		} else if (lcd->wr_state == LCD_WR_STATE_NONE) {
			// Unexpected data
			DPRINTF("Unexpected PARAM: %02X (no command)\n", value);
		} else {
			pmb887x_fifo8_push(&lcd->fifo, value);
			
			if (pmb887x_fifo_count(&lcd->fifo) >= (k->param_width * lcd->current_cmd_params)) {
				uint32_t tmp_params[LCD_CMD_MAX_PARAMS];
				uint32_t tmp_params_n = 0;
				while (pmb887x_fifo_count(&lcd->fifo) >= k->param_width) {
					tmp_params[tmp_params_n++] = pmb887x_lcd_read_from_fifo(lcd, k->param_width);
					// DPRINTF("PARAM: %04X\n", tmp_params[tmp_params_n - 1]);
				}
				
				k->on_cmd_with_params(lcd, lcd->current_cmd, tmp_params, tmp_params_n);
				
				lcd->current_cmd = 0;
				lcd->current_cmd_params = 0;
				lcd->wr_state = LCD_WR_STATE_NONE;
			}
		}
	}
}

void pmb887x_lcd_write(pmb887x_lcd_t *lcd, uint32_t value, uint32_t size) {
	if (lcd->cd && lcd->wr_state == LCD_WR_STATE_RAM)
		pmb887x_lcd_set_ram_mode(lcd, false);
	
	for (uint32_t i = 0; i < size; i++) {
		if (lcd->wr_state == LCD_WR_STATE_RAM) {
			pmb887x_lcd_write_pixel_byte(lcd, (value >> (i * 8)) & 0xFF);
		} else {
			pmb887x_lcd_write_control_byte(lcd, (value >> (i * 8)) & 0xFF);
		}
	}
}

static void pmb887x_lcd_set_mirror(pmb887x_lcd_t *lcd, bool flag) {
	if (lcd->mirror_xy != flag) {
		__SWAP_VALUES(lcd->width, lcd->height);
		__SWAP_VALUES(lcd->buffer_x, lcd->buffer_y);
		__SWAP_VALUES(lcd->window_x1, lcd->window_y1);
		__SWAP_VALUES(lcd->window_x2, lcd->window_y2);
		lcd->mirror_xy = flag;
	}
}

static uint32_t pmb887x_lcd_read_from_fifo(pmb887x_lcd_t *lcd, uint32_t width) {
	uint32_t value = 0;
	for (int i = 0; i < width; i++)
		value |= pmb887x_fifo8_pop(&lcd->fifo) << ((width - i - 1) * 8);
	return value;
}

static void pmb887x_lcd_clear_fifo(pmb887x_lcd_t *lcd) {
	if (pmb887x_fifo_count(&lcd->fifo) > 0) {
		g_autoptr(GString) buffer = g_string_new("");
		while (pmb887x_fifo_count(&lcd->fifo) > 0) {
			uint8_t byte = pmb887x_fifo8_pop(&lcd->fifo);
			g_string_append_printf(buffer, " %02X", byte);
		}
		DPRINTF("Ignored bytes:%s\n", buffer->str);
	}
}

static void pmb887x_lcd_update_display(void *opaque) {
	pmb887x_lcd_t *lcd = (pmb887x_lcd_t *) opaque;
	
	if (!lcd->invalidate || !lcd->surface)
		return;
	
	dpy_gfx_update(lcd->console, 0, 0, lcd->width, lcd->height);
	
	// TODO: partial update
	lcd->invalidate = false;
}

static void pmb887x_lcd_invalidate_display(void *opaque) {
	pmb887x_lcd_t *lcd = (pmb887x_lcd_t *) opaque;
	lcd->invalidate = true;
}

void pmb887x_lcd_set_addr_mode(pmb887x_lcd_t *lcd, enum pmb887x_lcd_am_t am, enum pmb887x_lcd_ac_t ac_x, enum pmb887x_lcd_ac_t ac_y) {
	DPRINTF("[addr] am=%s, ac_x=%s, ac_y=%s [%d x %d]\n",
		(am == LCD_AM_VERTICAL ? "vert" : "horiz"),
		(ac_x == LCD_AC_INC ? "inc" : "dec"),
		(ac_y == LCD_AC_INC ? "inc" : "dec"),
		lcd->width,
		lcd->height
	);
	
	if (lcd->flip_horizontal)
		ac_x = (ac_x == LCD_AC_INC ? LCD_AC_DEC : LCD_AC_INC);
	
	if (lcd->flip_vertical)
		ac_y = (ac_y == LCD_AC_INC ? LCD_AC_DEC : LCD_AC_INC);
	
	if (lcd->rotation == 90) {
		if (ac_x == ac_y) {
			ac_x = (ac_x == LCD_AC_INC ? LCD_AC_DEC : LCD_AC_INC);
		} else {
			ac_y = (ac_y == LCD_AC_INC ? LCD_AC_DEC : LCD_AC_INC);
		}
		am = (am == LCD_AM_HORIZONTAL ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL);
		pmb887x_lcd_set_mirror(lcd, true);
	} else if (lcd->rotation == 180) {
		ac_x = (ac_x == LCD_AC_INC ? LCD_AC_DEC : LCD_AC_INC);
		ac_y = (ac_y == LCD_AC_INC ? LCD_AC_DEC : LCD_AC_INC);
		pmb887x_lcd_set_mirror(lcd, false);
	} else if (lcd->rotation == 270) {
		if (ac_x == ac_y) {
			ac_y = (ac_y == LCD_AC_INC ? LCD_AC_DEC : LCD_AC_INC);
		} else {
			ac_x = (ac_x == LCD_AC_INC ? LCD_AC_DEC : LCD_AC_INC);
		}
		am = (am == LCD_AM_HORIZONTAL ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL);
		pmb887x_lcd_set_mirror(lcd, true);
	} else {
		pmb887x_lcd_set_mirror(lcd, false);
	}
	
	lcd->am = am;
	lcd->ac_x = ac_x;
	lcd->ac_y = ac_y;
	
	if (lcd->rotation != 0 || lcd->flip_horizontal || lcd->flip_vertical) {
		DPRINTF("[addr] am=%s, ac_x=%s, ac_y=%s [%d x %d] (%d rotation, hflip:%d, vflip:%d)\n",
			(am == LCD_AM_VERTICAL ? "vert" : "horiz"),
			(ac_x == LCD_AC_INC ? "inc" : "dec"),
			(ac_y == LCD_AC_INC ? "inc" : "dec"),
			lcd->width,
			lcd->height,
			lcd->rotation,
			lcd->flip_horizontal,
			lcd->flip_vertical
		);
	}
}

void pmb887x_lcd_set_cd(pmb887x_lcd_t *lcd, bool value) {
	lcd->cd = value;
}

void pmb887x_lcd_set_ram_mode(pmb887x_lcd_t *lcd, bool flag) {
	DPRINTF("write mode: %s\n", flag ? "ram" : "cmd");
	lcd->wr_state = flag ? LCD_WR_STATE_RAM : LCD_WR_STATE_NONE;
	lcd->tmp_index = 0;
	if (!flag)
		lcd->invalidate = true;
	pmb887x_lcd_clear_fifo(lcd);
}

static const GraphicHwOps pmb887x_lcd_gfx_ops = {
	.invalidate = pmb887x_lcd_invalidate_display,
	.gfx_update = pmb887x_lcd_update_display
};

void pmb887x_lcd_init(pmb887x_lcd_t *lcd, DeviceState *dev) {
	pmb887x_fifo8_init(&lcd->fifo, 64);
	
	lcd->console = graphic_console_init(dev, 0, &pmb887x_lcd_gfx_ops, lcd);
	qemu_console_resize(lcd->console, lcd->width, lcd->height);
	
	lcd->phys_width = lcd->width;
	lcd->phys_height = lcd->height;
	
	pmb887x_lcd_set_window_x2(lcd, lcd->width - 1);
	pmb887x_lcd_set_window_y2(lcd, lcd->height - 1);
}

static const Property pmb887x_lcd_props[] = {
	DEFINE_PROP_UINT32("width", pmb887x_lcd_t, width, 240),
	DEFINE_PROP_UINT32("height", pmb887x_lcd_t, height, 320),
	DEFINE_PROP_UINT32("rotation", pmb887x_lcd_t, rotation, 0),
	DEFINE_PROP_BOOL("flip_horizontal", pmb887x_lcd_t, flip_horizontal, false),
	DEFINE_PROP_BOOL("flip_vertical", pmb887x_lcd_t, flip_vertical, false),
};

static void pmb887x_lcd_class_init(ObjectClass *oc, void *data) {
	DeviceClass *dc = DEVICE_CLASS(oc);
	device_class_set_props(dc, pmb887x_lcd_props);
}

static const TypeInfo pmb887x_lcd_type_info = {
	.name			= TYPE_PMB887X_LCD,
	.parent			= TYPE_DEVICE,
	.instance_size 	= sizeof(pmb887x_lcd_t),
	.abstract		= true,
	.class_size		= sizeof(pmb887x_lcd_class_t),
	.class_init		= pmb887x_lcd_class_init,
};

static void pmb887x_lcd_register_types(void) {
	type_register_static(&pmb887x_lcd_type_info);
}

type_init(pmb887x_lcd_register_types)
