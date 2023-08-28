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

#define LCD_PIXMAN_TRANSFORM(v00, v01, v10, v11, WIDTH, HEIGHT) \
	{ \
		{ \
			{ v00, v01, WIDTH * pixman_fixed_1 / 2 }, \
			{ v10, v11, HEIGHT * pixman_fixed_1 / 2 }, \
			{ 0, 0, pixman_fixed_1 } \
		} \
	}

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
	
	pixman_transform_t transform;
	pixman_transform_init_identity(&transform);
	
	if (lcd->v_flip || lcd->h_flip) {
		// Flip
		pixman_transform_t flip;
		pixman_fixed_t trans_x = lcd->h_flip ? pixman_int_to_fixed(-lcd->width) : pixman_int_to_fixed(0);
		pixman_fixed_t trans_y = lcd->v_flip ? pixman_int_to_fixed(-lcd->height) : pixman_int_to_fixed(0);
		pixman_fixed_t scale_x = lcd->h_flip ? pixman_fixed_minus_1 : pixman_fixed_1;
		pixman_fixed_t scale_y = lcd->v_flip ? pixman_fixed_minus_1 : pixman_fixed_1;
		
		pixman_transform_init_translate(&flip, trans_x, trans_y);
		pixman_transform_scale(&flip, NULL, scale_x, scale_y);
		pixman_transform_multiply(&transform, &transform, &flip);
	}
	
	if (lcd->rotation != 0) {
		pixman_fixed_t rotation_cos;
		pixman_fixed_t rotation_sin;
		
		switch (lcd->rotation) {
			case 90:
				rotation_sin = pixman_fixed_1;
				rotation_cos = 0;
			break;
			
			case 180:
				rotation_sin = 0;
				rotation_cos = pixman_fixed_minus_1;
			break;
			
			case 270:
				rotation_sin = pixman_fixed_minus_1;
				rotation_cos = 0;
			break;
			
			default:
				error_report("Invalid LCD rotation: %d\n", lcd->rotation);
				exit(1);
			break;
		}
		
		// Translate to (center, center)
		pixman_transform_t translate_before;
		pixman_transform_init_translate(&translate_before, pixman_int_to_fixed(lcd->width / 2), pixman_int_to_fixed(lcd->height / 2));
		pixman_transform_multiply(&transform, &transform, &translate_before);
		
		// Rotate to angle
		pixman_transform_t rotate;
		pixman_transform_init_rotate(&rotate, rotation_cos, rotation_sin);
		pixman_transform_multiply(&transform, &transform, &rotate);
		
		// Translate to (0, 0)
		pixman_transform_t translate_after;
		pixman_transform_init_translate(&translate_after, pixman_int_to_fixed(-lcd->width / 2), pixman_int_to_fixed(-lcd->height / 2));
		pixman_transform_multiply(&transform, &transform, &translate_after);
	}
	
	if (lcd->v_flip || lcd->h_flip || lcd->rotation != 0) {
		// Apply transform
		pixman_image_set_transform(lcd->surface->image, &transform);
	}
	
	DPRINTF("mode %s, bpp: %d [%dB], buffer: %d\n", pmb887x_lcd_get_mode_name(lcd->mode), lcd->bpp, lcd->byte_pp, lcd->buffer_size);
}

static inline void pmb887x_lcd_incr_y(pmb887x_lcd_t *lcd) {
	if (lcd->mode_y == LCD_ADDR_MODE_DECR) {
		if (lcd->buffer_y == lcd->window_y1) {
			lcd->buffer_y = lcd->window_y2;
		} else {
			lcd->buffer_y--;
		}
	} else {
		if (lcd->buffer_y == lcd->window_y2) {
			lcd->buffer_y = lcd->window_y1;
		} else {
			lcd->buffer_y++;
		}
	}
}

static inline void pmb887x_lcd_incr_x(pmb887x_lcd_t *lcd) {
	if (lcd->mode_x == LCD_ADDR_MODE_DECR) {
		if (lcd->buffer_x == lcd->window_x1) {
			lcd->buffer_x = lcd->window_x2;
			pmb887x_lcd_incr_y(lcd);
		} else {
			lcd->buffer_x--;
		}
	} else {
		if (lcd->buffer_x == lcd->window_x2) {
			lcd->buffer_x = lcd->window_x1;
			pmb887x_lcd_incr_y(lcd);
		} else {
			lcd->buffer_x++;
		}
	}
}

void pmb887x_lcd_put_pixel_byte(pmb887x_lcd_t *lcd, uint8_t byte) {
	uint32_t pixel_index = lcd->buffer_y * lcd->width + lcd->buffer_x;
	lcd->buffer[pixel_index * lcd->byte_pp + lcd->tmp_index] = byte;
	lcd->tmp_index++;
	
	if (lcd->tmp_index == lcd->byte_pp) {
		lcd->tmp_index = 0;
		lcd->invalidate = true;
		pmb887x_lcd_incr_x(lcd);
	}
}

void pmb887x_lcd_set_cd(pmb887x_lcd_t *lcd, bool value) {
	if (lcd->cd != value) {
		lcd->cd = value;
		pmb887x_fifo_reset(&lcd->write_fifo);
	}
}

void pmb887x_lcd_write(pmb887x_lcd_t *lcd, uint32_t value, uint32_t size) {
	pmb887x_lcd_class_t *k = PMB887X_LCD_GET_CLASS(lcd);
	
	if (lcd->write_to_ram && !lcd->cd) {
		k->write_ram(lcd, value, size);
	} else {
		for (int i = 0; i < size; i++) {
			uint8_t b = (value >> (i * 8)) & 0xFF;
			pmb887x_fifo_push(&lcd->write_fifo, b);
		}
		
		while (pmb887x_fifo_count(&lcd->write_fifo) >= k->bus_width) {
			uint32_t bus_value = 0;
			for (int i = 0; i < k->bus_width; i++)
				bus_value |= pmb887x_fifo_pop(&lcd->write_fifo) << ((k->bus_width - i - 1) * 8);
			k->write(lcd, bus_value);
		}
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

static const GraphicHwOps pmb887x_lcd_gfx_ops = {
	.invalidate = pmb887x_lcd_invalidate_display,
	.gfx_update = pmb887x_lcd_update_display
};

void pmb887x_lcd_init(pmb887x_lcd_t *lcd, DeviceState *dev) {
	pmb887x_fifo_init(&lcd->write_fifo, 8);
	
	lcd->console = graphic_console_init(dev, 0, &pmb887x_lcd_gfx_ops, lcd);
	qemu_console_resize(lcd->console, lcd->width, lcd->height);
}

static Property pmb887x_lcd_props[] = {
	DEFINE_PROP_UINT32("width", pmb887x_lcd_t, width, 240),
	DEFINE_PROP_UINT32("height", pmb887x_lcd_t, height, 320),
	DEFINE_PROP_UINT32("rotation", pmb887x_lcd_t, rotation, 0),
	DEFINE_PROP_END_OF_LIST(),
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
