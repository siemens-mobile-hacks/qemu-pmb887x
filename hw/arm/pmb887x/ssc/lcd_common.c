/*
 * Generic serial display
 */
#define PMB887X_TRACE_ID		LCD
#define PMB887X_TRACE_PREFIX	"pmb887x-lcd-common"

#include <math.h>
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/hw.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/ssc/lcd_common.h"

#define LCD_CMD_MAX_PARAMS 64

static void lcd_incr_px(pmb887x_lcd_t *lcd);

static void lcd_write_control_byte(pmb887x_lcd_t *lcd, uint8_t value);

static uint32_t lcd_read_from_fifo(pmb887x_lcd_t *lcd, uint32_t width);
static void lcd_clear_fifo(pmb887x_lcd_t *lcd);

static const char *lcd_get_mode_name(enum pmb887x_lcd_pixel_mode_t mode) {
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

void pmb887x_lcd_set_mode(pmb887x_lcd_t *lcd, enum pmb887x_lcd_pixel_mode_t mode, bool flip_h_pins, bool flip_v_pins) {
	uint8_t *old_shadow_buffer = lcd->shadow_buffer;
	uint8_t *old_buffer = lcd->buffer;

	uint32_t new_rotation = lcd->default_rotation;
	bool new_flip_horizontal = lcd->default_flip_horizontal;
	bool new_flip_vertical = lcd->default_flip_vertical;

	// Apply HW ping remapping
	if (flip_h_pins)
		new_flip_horizontal = !new_flip_horizontal;
	if (flip_v_pins)
		new_flip_vertical = !new_flip_vertical;

	// 180deg + flip_v + flip_h == 0deg
	if (new_rotation == 180 && new_flip_horizontal && new_flip_vertical) {
		new_rotation = 0;
		new_flip_horizontal = false;
		new_flip_vertical = false;
	}

	bool is_changed = (
		lcd->surface == NULL ||
		lcd->mode != mode ||
		lcd->rotation != new_rotation ||
		lcd->flip_horizontal != new_flip_horizontal ||
		lcd->flip_vertical != new_flip_vertical
	);

	if (!is_changed)
		return;

	lcd->mode = mode;
	lcd->rotation = new_rotation;
	lcd->flip_horizontal = new_flip_horizontal;
	lcd->flip_vertical = new_flip_vertical;

	switch (mode) {
		case LCD_MODE_BGR565:
			lcd->bpp = 16;
			lcd->byte_pp = 2;
			lcd->format = PIXMAN_b5g6r5;
			lcd->byte_mask = 0xFF;
			lcd->byte_fill = 0x00;
			break;
		
		case LCD_MODE_BGR666:
			lcd->bpp = 18;
			lcd->byte_pp = 3;
			lcd->format = PIXMAN_b8g8r8;
			lcd->byte_mask = 0xFC;
			lcd->byte_fill = 0x03;
			break;
		
		case LCD_MODE_BGR888:
			lcd->bpp = 24;
			lcd->byte_pp = 3;
			lcd->format = PIXMAN_b8g8r8;
			lcd->byte_mask = 0xFF;
			lcd->byte_fill = 0x00;
			break;
		
		case LCD_MODE_RGB565:
			lcd->bpp = 16;
			lcd->byte_pp = 2;
			lcd->format = PIXMAN_r5g6b5;
			lcd->byte_mask = 0xFF;
			lcd->byte_fill = 0x00;
			break;
		
		case LCD_MODE_RGB666:
			lcd->bpp = 18;
			lcd->byte_pp = 3;
			lcd->format = PIXMAN_r8g8b8;
			lcd->byte_mask = 0xFC;
			lcd->byte_fill = 0x03;
			break;
		
		case LCD_MODE_RGB888:
			lcd->bpp = 24;
			lcd->byte_pp = 3;
			lcd->format = PIXMAN_r8g8b8;
			lcd->byte_mask = 0xFF;
			lcd->byte_fill = 0x00;
			break;
		
		default:
			hw_error("Invalid LCD mode: %d\n", mode);
	}

	lcd->mode = mode;
	lcd->buffer_size = (lcd->width * lcd->height * lcd->byte_pp);
	lcd->buffer = g_new0(uint8_t, lcd->buffer_size);
	
	uint32_t linesize = (lcd->width * lcd->byte_pp);
	lcd->surface = qemu_create_displaysurface_from(lcd->width, lcd->height, lcd->format, linesize, lcd->buffer);

	bool need_transform = lcd->rotation != 0 || lcd->flip_horizontal || lcd->flip_vertical;
	if (need_transform) {
		lcd->shadow_buffer = g_new0(uint8_t, lcd->buffer_size);
		if (lcd->rotation == 90 || lcd->rotation == 270) {
			uint32_t shadow_linesize = (lcd->height * lcd->byte_pp);
			lcd->shadow_surface = qemu_create_displaysurface_from(lcd->height, lcd->width, lcd->format, shadow_linesize, lcd->shadow_buffer);
		} else {
			uint32_t shadow_linesize = (lcd->width * lcd->byte_pp);
			lcd->shadow_surface = qemu_create_displaysurface_from(lcd->width, lcd->height, lcd->format, shadow_linesize, lcd->shadow_buffer);
		}
		dpy_gfx_replace_surface(lcd->console, lcd->shadow_surface);
	} else {
		lcd->shadow_buffer = NULL;
		lcd->shadow_surface = NULL;
		dpy_gfx_replace_surface(lcd->console, lcd->surface);
	}

	if (old_buffer)
		g_free(old_buffer);

	if (old_shadow_buffer)
		g_free(old_shadow_buffer);

	if (need_transform) {
		pixman_transform_t transform;
		pixman_transform_init_identity(&transform);
		pixman_fixed_t fw = pixman_int_to_fixed(surface_width(lcd->surface));
		pixman_fixed_t fh = pixman_int_to_fixed(surface_height(lcd->surface));

		if (lcd->flip_horizontal || lcd->flip_vertical) {
			if (lcd->rotation == 90 || lcd->rotation == 270) {
				const pixman_fixed_t sx = pixman_int_to_fixed(lcd->flip_vertical ? -1 : 1);
				const pixman_fixed_t sy = pixman_int_to_fixed(lcd->flip_horizontal ? -1 : 1);
				pixman_transform_scale(&transform, NULL, sx, sy);
				pixman_transform_translate(
					&transform,
					NULL,
					lcd->flip_vertical ? fh : 0,
					lcd->flip_horizontal ? fw : 0
				);
			} else {
				const pixman_fixed_t sx = pixman_int_to_fixed(lcd->flip_horizontal ? -1 : 1);
				const pixman_fixed_t sy = pixman_int_to_fixed(lcd->flip_vertical ? -1 : 1);
				pixman_transform_scale(&transform, NULL, sx, sy);
				pixman_transform_translate(
					&transform,
					NULL,
					lcd->flip_horizontal ? fw : 0,
					lcd->flip_vertical ? fh : 0
				);
			}
		}

		switch (lcd->rotation) {
			case 90:
				pixman_transform_rotate(&transform, NULL, 0, -pixman_fixed_1);
				pixman_transform_translate(&transform, NULL, 0, fh);
				break;
			case 180:
				pixman_transform_rotate(&transform, NULL, -pixman_fixed_1, 0);
				pixman_transform_translate(&transform, NULL, fw, fh);
				break;
			case 270:
				pixman_transform_rotate(&transform, NULL, 0, pixman_fixed_1);
				pixman_transform_translate(&transform, NULL, fw, 0);
				break;
		}

		pixman_image_set_transform(lcd->surface->image, &transform);
		pixman_image_set_filter(lcd->surface->image, PIXMAN_FILTER_NEAREST, NULL, 0);
	}

	DPRINTF("mode %s, bpp: %d [%dB], buffer: %d\n", lcd_get_mode_name(lcd->mode), lcd->bpp, lcd->byte_pp, lcd->buffer_size);
}

static inline bool lcd_incr_ac_x(pmb887x_lcd_t *lcd) {
	if (lcd->ac_x == LCD_AC_INC) {
		lcd->buffer_x++;
		if (lcd->buffer_x > lcd->window.x2) {
			lcd->buffer_x = lcd->window.x1;
			return true;
		}
	} else {
		lcd->buffer_x--;
		if (lcd->buffer_x < lcd->window.x1) {
			lcd->buffer_x = lcd->window.x2;
			return true;
		}
	}
	return false;
}

static inline bool lcd_incr_ac_y(pmb887x_lcd_t *lcd) {
	if (lcd->ac_y == LCD_AC_INC) {
		lcd->buffer_y++;
		if (lcd->buffer_y > lcd->window.y2) {
			lcd->buffer_y = lcd->window.y1;
			return true;
		}
	} else {
		lcd->buffer_y--;
		if (lcd->buffer_y < lcd->window.y1) {
			lcd->buffer_y = lcd->window.y2;
			return true;
		}
	}
	return false;
}

static inline void lcd_incr_px(pmb887x_lcd_t *lcd) {
	if (lcd->am == LCD_AM_VERTICAL) {
		if (lcd_incr_ac_y(lcd))
			lcd_incr_ac_x(lcd);
	} else {
		if (lcd_incr_ac_x(lcd))
			lcd_incr_ac_y(lcd);
	}

	lcd->dirty.x1 = MIN(lcd->dirty.x1, lcd->buffer_x);
	lcd->dirty.y1 = MIN(lcd->dirty.y1, lcd->buffer_y);
	lcd->dirty.x2 = MAX(lcd->dirty.x2, lcd->buffer_x);
	lcd->dirty.y2 = MAX(lcd->dirty.y2, lcd->buffer_y);
}

static void lcd_write_control_byte(pmb887x_lcd_t *lcd, uint8_t value) {
	if (lcd->cd) {
		if (lcd->wr_state != LCD_WR_STATE_CMD) {
			if (lcd->wr_state == LCD_WR_STATE_PARAM)
				DPRINTF("CMD %04X ignored, too few params.\n", lcd->current_cmd);
			lcd_clear_fifo(lcd);
			lcd->wr_state = LCD_WR_STATE_CMD;
		}
		
		pmb887x_fifo8_push(&lcd->fifo, value);
		
		if (pmb887x_fifo_count(&lcd->fifo) >= lcd->k->cmd_width) {
			uint32_t cmd = lcd_read_from_fifo(lcd, lcd->k->cmd_width);
			lcd->wr_state = LCD_WR_STATE_PARAM;
			lcd->current_cmd = cmd;
			DPRINTF("CMD: %04X\n", lcd->current_cmd);
			lcd->current_cmd_params = lcd->k->on_cmd(lcd, cmd);
			g_assert(lcd->current_cmd_params <= LCD_CMD_MAX_PARAMS);
			
			if (lcd->current_cmd_params == 0 && lcd->wr_state == LCD_WR_STATE_PARAM)
				lcd->wr_state = LCD_WR_STATE_NONE;
		}
	} else {
		if (lcd->wr_state == LCD_WR_STATE_CMD) {
			lcd_clear_fifo(lcd);
			lcd->wr_state = LCD_WR_STATE_NONE;
		} else if (lcd->wr_state == LCD_WR_STATE_NONE) {
			// Unexpected data
			DPRINTF("Unexpected PARAM: %02X (no command)\n", value);
		} else {
			pmb887x_fifo8_push(&lcd->fifo, value);
			
			if (pmb887x_fifo_count(&lcd->fifo) >= (lcd->k->param_width * lcd->current_cmd_params)) {
				uint32_t tmp_params[LCD_CMD_MAX_PARAMS];
				uint32_t tmp_params_n = 0;
				while (pmb887x_fifo_count(&lcd->fifo) >= lcd->k->param_width) {
					tmp_params[tmp_params_n++] = lcd_read_from_fifo(lcd, lcd->k->param_width);
					// DPRINTF("PARAM: %04X\n", tmp_params[tmp_params_n - 1]);
				}
				
				lcd->k->on_cmd_with_params(lcd, lcd->current_cmd, tmp_params, tmp_params_n);
				
				lcd->current_cmd = 0;
				lcd->current_cmd_params = 0;
				lcd->wr_state = LCD_WR_STATE_NONE;
			}
		}
	}
}

static uint32_t lcd_read_from_fifo(pmb887x_lcd_t *lcd, uint32_t width) {
	uint32_t value = 0;
	for (int i = 0; i < width; i++)
		value |= pmb887x_fifo8_pop(&lcd->fifo) << ((width - i - 1) * 8);
	return value;
}

static void lcd_clear_fifo(pmb887x_lcd_t *lcd) {
	if (pmb887x_fifo_count(&lcd->fifo) > 0) {
		g_autoptr(GString) buffer = g_string_new("");
		while (pmb887x_fifo_count(&lcd->fifo) > 0) {
			uint8_t byte = pmb887x_fifo8_pop(&lcd->fifo);
			g_string_append_printf(buffer, " %02X", byte);
		}
		DPRINTF("Ignored bytes:%s\n", buffer->str);
	}
}

static inline void lcd_transform_rect(pmb887x_lcd_t *lcd, pmb887x_lcd_rect_t *r) {
	uint32_t rect_w = r->x2 - r->x1 + 1;
	uint32_t rect_h = r->y2 - r->y1 + 1;
	uint32_t x = r->x1;
	uint32_t y = r->y1;

	if (lcd->flip_horizontal)
		x = lcd->width - x - rect_w;
	if (lcd->flip_vertical)
		y = lcd->height - y - rect_h;

	uint32_t out_x = x, out_y = y, out_w = rect_w, out_h = rect_h;
	switch (lcd->rotation) {
		case 0:
			out_x = x;
			out_y = y;
			break;
		case 90:
			out_x = lcd->height - y - rect_h;
			out_y = x;
			out_w = rect_h;
			out_h = rect_w;
			break;
		case 180:
			out_x = lcd->width - x - rect_w;
			out_y = lcd->height - y - rect_h;
			break;
		case 270:
			out_x = y;
			out_y = lcd->width - x - rect_w;
			out_w = rect_h;
			out_h = rect_w;
			break;
	}

	r->x1 = out_x;
	r->y1 = out_y;
	r->x2 = out_x + out_w - 1;
	r->y2 = out_y + out_h - 1;
}

static inline pmb887x_lcd_rect_t lcd_get_dirty_region(pmb887x_lcd_t *lcd) {
	pmb887x_lcd_rect_t dirty_reset = {
		.x1 = lcd->width  - 1,
		.y1 = lcd->height - 1,
		.x2 = 0,
		.y2 = 0
	};
	pmb887x_lcd_rect_t dirty = lcd->dirty;
	lcd->dirty = dirty_reset;
	return dirty;
}

static void lcd_update_display(void *opaque) {
	pmb887x_lcd_t *lcd = opaque;
	
	if (!lcd->surface)
		return;
	
	pmb887x_lcd_rect_t dirty = lcd_get_dirty_region(lcd);
	if (dirty.x1 > dirty.x2 || dirty.y1 > dirty.y2)
		return;

	if (lcd->shadow_surface) {
		lcd_transform_rect(lcd, &dirty);
		pixman_image_composite32(
			PIXMAN_OP_SRC,
			lcd->surface->image, /* src */
			NULL /* mask */,
			lcd->shadow_surface->image, /* dest */
			dirty.x1, dirty.y1, /* src_x, src_y */
			0, 0, /* mask_x, mask_y */
			dirty.x1, dirty.y1, /* dest_x, dest_y */
			dirty.x2 - dirty.x1 + 1, /* width */
			dirty.y2 - dirty.y1 + 1 /* height */
		);
	}

	dpy_gfx_update(lcd->console, dirty.x1, dirty.y1, dirty.x2 - dirty.x1 + 1, dirty.y2 - dirty.y1 + 1);
}

static void lcd_invalidate_display(void *opaque) {
	pmb887x_lcd_t *lcd = opaque;
	lcd->dirty.x1 = 0;
	lcd->dirty.y1 = 0;
	lcd->dirty.x2 = lcd->width - 1;
	lcd->dirty.y2 = lcd->height - 1;
}

void pmb887x_lcd_write(pmb887x_lcd_t *lcd, uint32_t value, uint32_t size) {

}

void pmb887x_lcd_set_addr_mode(pmb887x_lcd_t *lcd, enum pmb887x_lcd_am_t am, enum pmb887x_lcd_ac_t ac_x, enum pmb887x_lcd_ac_t ac_y) {
	DPRINTF("[addr] am=%s, ac_x=%s, ac_y=%s [%d x %d]\n",
		(am == LCD_AM_VERTICAL ? "vert" : "horiz"),
		(ac_x == LCD_AC_INC ? "inc" : "dec"),
		(ac_y == LCD_AC_INC ? "inc" : "dec"),
		lcd->width,
		lcd->height
	);
	
	lcd->am = am;
	lcd->ac_x = ac_x;
	lcd->ac_y = ac_y;
}

void pmb887x_lcd_set_ram_mode(pmb887x_lcd_t *lcd, bool flag) {
	DPRINTF("write mode: %s\n", flag ? "ram" : "cmd");
	lcd->wr_state = flag ? LCD_WR_STATE_RAM : LCD_WR_STATE_NONE;
	lcd->tmp_index = 0;
	lcd_clear_fifo(lcd);
}

static uint32_t lcd_transfer(SSIPeripheral *dev, uint32_t data) {
	pmb887x_lcd_t *lcd = PMB887X_LCD(dev);

	if (lcd->wr_state == LCD_WR_STATE_RAM) {
		uint32_t index = lcd->buffer_y * lcd->width + lcd->buffer_x;
		lcd->buffer[index * lcd->byte_pp + (lcd->byte_pp - lcd->tmp_index - 1)] = data | lcd->byte_fill;
		lcd->tmp_index++;

		if (lcd->tmp_index == lcd->byte_pp) {
			lcd->tmp_index = 0;
			lcd_incr_px(lcd);
		}
	} else {
		lcd_write_control_byte(lcd, data);
	}

	return 0; // TODO: handle RD, WR
}

static const GraphicHwOps pmb887x_lcd_gfx_ops = {
	.invalidate = lcd_invalidate_display,
	.gfx_update = lcd_update_display
};

static const Property lcd_props[] = {
	DEFINE_PROP_UINT32("width", pmb887x_lcd_t, width, 240),
	DEFINE_PROP_UINT32("height", pmb887x_lcd_t, height, 320),
	DEFINE_PROP_UINT32("rotation", pmb887x_lcd_t, default_rotation, 0),
	DEFINE_PROP_BOOL("flip_horizontal", pmb887x_lcd_t, default_flip_horizontal, false),
	DEFINE_PROP_BOOL("flip_vertical", pmb887x_lcd_t, default_flip_vertical, false),
};

static void lcd_handle_cd(void *opaque, int n, int level) {
	pmb887x_lcd_t *lcd = PMB887X_LCD(opaque);
	pmb887x_lcd_set_cd(lcd, level != 0);
	if (lcd->cd && lcd->wr_state == LCD_WR_STATE_RAM)
		pmb887x_lcd_set_ram_mode(lcd, false);
}

static void lcd_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	qdev_init_gpio_in_named(DEVICE(dev), lcd_handle_cd, "CD_IN", 1);
}

static void lcd_realize(SSIPeripheral *d, Error **errp) {
	pmb887x_lcd_t *lcd = PMB887X_LCD(d);
	lcd->k = PMB887X_LCD_GET_CLASS(d);

	pmb887x_fifo8_init(&lcd->fifo, 64);

	lcd->console = graphic_console_init(DEVICE(d), 0, &pmb887x_lcd_gfx_ops, lcd);

	lcd->rotation = lcd->default_rotation;
	lcd->flip_horizontal = lcd->default_flip_horizontal;
	lcd->flip_vertical = lcd->default_flip_vertical;

	if (lcd->rotation != 0 && lcd->rotation != 90 && lcd->rotation != 180 && lcd->rotation != 270)
		hw_error("Invalid rotation: %d", lcd->rotation);

	lcd->dirty.x1 = lcd->width - 1;
	lcd->dirty.y1 = lcd->height - 1;
	lcd->dirty.x2 = 0;
	lcd->dirty.y2 = 0;

	pmb887x_lcd_set_window_x1(lcd, 0);
	pmb887x_lcd_set_window_y1(lcd, 0);
	pmb887x_lcd_set_window_x2(lcd, lcd->width - 1);
	pmb887x_lcd_set_window_y2(lcd, lcd->height - 1);

	if (lcd->k->realize)
		lcd->k->realize(lcd, errp);
}

static void lcd_class_init(ObjectClass *klass, void *data) {
	SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, lcd_props);
	k->realize = lcd_realize;
	k->transfer = lcd_transfer;
	k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo lcd_type_info = {
	.name			= TYPE_PMB887X_LCD,
	.parent			= TYPE_SSI_PERIPHERAL,
	.instance_init	= lcd_init,
	.instance_size 	= sizeof(pmb887x_lcd_t),
	.abstract		= true,
	.class_size		= sizeof(pmb887x_lcd_class_t),
	.class_init		= lcd_class_init,
};

static void lcd_register_types(void) {
	type_register_static(&lcd_type_info);
}

type_init(lcd_register_types)
