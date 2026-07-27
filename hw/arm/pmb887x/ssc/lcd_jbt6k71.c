/*
 * Toshiba JBT6K71
 * */
#define PMB887X_TRACE_ID		LCD
#define PMB887X_TRACE_PREFIX	"jbt6k71"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_JBT6K71

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/arm/pmb887x/gen/peripheral/JBT6K71.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/ssc/lcd_common.h"

#define TYPE_PMB887X_LCD_PANEL	"jbt6k71"
#define PMB887X_LCD_PANEL(obj)	OBJECT_CHECK(pmb887x_lcd_panel_t, (obj), TYPE_PMB887X_LCD_PANEL)

#define JBT6K71_MAX_BPP		18
#define JBT6K71_BUS_WIDTH	2
#define JBT6K71_MAX_REGS	0x800
#define JBT6K71_DEVICE_CODE	0x7114

/* Indexed by [TRI][DFM1][DFM0]; unspecified combinations are invalid. */
static const enum pmb887x_lcd_pixel_format_t JBT6K71_PIXEL_FORMATS[2][2][2] = {
	[0][0][0] = LCD_PIXEL_FORMAT_RGB565,
	[1][0][1] = LCD_PIXEL_FORMAT_RGB666_8_8_2,
	[1][1][0] = LCD_PIXEL_FORMAT_RGB666_2_8_8,
	[1][1][1] = LCD_PIXEL_FORMAT_RGB666_6_6_6,
};

static const uint16_t DEFAULT_REGS[] = {
	[JBT6K71_DRIVER_OUTPUT_CONTROL] = 0x27,
	[JBT6K71_ENTRY_MODE] = 0x30,
	[JBT6K71_DISPLAY_CONTROL] = 0x800,
};

typedef struct pmb887x_lcd_panel_t pmb887x_lcd_panel_t;

struct pmb887x_lcd_panel_t {
	pmb887x_lcd_t parent;
	uint16_t regs[JBT6K71_MAX_REGS];
};

static void lcd_update_state(pmb887x_lcd_t *lcd) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);
	uint16_t entry_mode = priv->regs[JBT6K71_ENTRY_MODE];
	uint32_t id = (entry_mode & JBT6K71_ENTRY_MODE_ID) >> JBT6K71_ENTRY_MODE_ID_SHIFT;
	uint32_t dfm = (entry_mode & JBT6K71_ENTRY_MODE_DFM) >> JBT6K71_ENTRY_MODE_DFM_SHIFT;
	bool ss = (priv->regs[JBT6K71_DRIVER_OUTPUT_CONTROL] & JBT6K71_DRIVER_OUTPUT_CONTROL_SS) != 0;
	bool am = (entry_mode & JBT6K71_ENTRY_MODE_AM) != 0;
	bool id0 = (id & BIT(0)) != 0;
	bool id1 = (id & BIT(1)) != 0;
	bool bgr = (entry_mode & JBT6K71_ENTRY_MODE_BGR) != 0;
	bool dfm0 = (dfm & BIT(0)) != 0;
	bool dfm1 = (dfm & BIT(1)) != 0;
	bool tri = (entry_mode & JBT6K71_ENTRY_MODE_TRI) != 0;
	bool ud = (priv->regs[JBT6K71_DISPLAY_CONTROL] & JBT6K71_DISPLAY_CONTROL_UD) != 0;
	
	DPRINTF("am=%d, id1=%d, id0=%d, ss=%d, ud=%d\n", am, id1, id0, ss, ud);
	
	pmb887x_lcd_set_addr_mode(
		lcd,
		(am ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL),
		(id0 ? LCD_AC_INC : LCD_AC_DEC),
		(id1 ? LCD_AC_INC : LCD_AC_DEC)
	);
	
	enum pmb887x_lcd_pixel_format_t pixel_format = JBT6K71_PIXEL_FORMATS[tri][dfm1][dfm0];
	if (pixel_format == LCD_PIXEL_FORMAT_NONE) {
		EPRINTF("invalid config: dfm0=%d, dfm1=%d, tri=%d", dfm0, dfm1, tri);
		return;
	}

	pmb887x_lcd_set_pixel_format(lcd, pixel_format);
	pmb887x_lcd_set_output_bgr(lcd, bgr);
	pmb887x_lcd_set_transform(lcd, ss, !ud);
}

static int lcd_on_cmd(pmb887x_lcd_t *lcd, uint32_t cmd) {
	if (cmd == JBT6K71_GRAM_DATA) {
		pmb887x_lcd_set_ram_mode(lcd, true);
		return 0;
	}
	return 1;
}

static void lcd_on_cmd_with_params(pmb887x_lcd_t *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);
	
	g_assert(params_n == 1);
	g_assert(cmd < JBT6K71_MAX_REGS);

	IO_DUMP_WRITE(cmd, 2, params[0]);
	priv->regs[cmd] = params[0];
	
	switch (cmd) {
		case JBT6K71_DRIVER_OUTPUT_CONTROL:
		case JBT6K71_ENTRY_MODE:
		case JBT6K71_DISPLAY_CONTROL:
			lcd_update_state(lcd);
			break;
		
		case JBT6K71_RAM_ADDRESS_LOW:
			pmb887x_lcd_set_x(lcd, params[0]);
			break;
		
		case JBT6K71_RAM_ADDRESS_HIGH:
			pmb887x_lcd_set_y(lcd, params[0]);
			break;
		
		case JBT6K71_HORIZONTAL_RAM_START:
			pmb887x_lcd_set_window_x1(lcd, params[0]);
			break;
		
		case JBT6K71_HORIZONTAL_RAM_END:
			pmb887x_lcd_set_window_x2(lcd, params[0]);
			break;
		
		case JBT6K71_VERTICAL_RAM_START:
			pmb887x_lcd_set_window_y1(lcd, params[0]);
			break;
		
		case JBT6K71_VERTICAL_RAM_END:
			pmb887x_lcd_set_window_y2(lcd, params[0]);
			break;

		default:
			// Nothing
			break;
	}
}

static uint32_t lcd_on_read(pmb887x_lcd_t *lcd, uint32_t index) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);
	uint16_t value = 0;
	uint32_t shift = (1 - index % JBT6K71_BUS_WIDTH) * 8;

	if (lcd->current_cmd == JBT6K71_OSCILLATION) {
		value = JBT6K71_DEVICE_CODE;
	} else if (lcd->current_cmd < JBT6K71_MAX_REGS) {
		value = priv->regs[lcd->current_cmd];
	}
	if (index % JBT6K71_BUS_WIDTH == 0)
		IO_DUMP_READ(lcd->current_cmd, 2, value);
	return (value >> shift) & 0xFF;
}

static void lcd_reset(pmb887x_lcd_t *lcd) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);
	memset(priv->regs, 0, sizeof(priv->regs));
	memcpy(priv->regs, DEFAULT_REGS, sizeof(DEFAULT_REGS));
	priv->regs[JBT6K71_HORIZONTAL_RAM_END] = lcd->width - 1;
	priv->regs[JBT6K71_VERTICAL_RAM_END] = lcd->height - 1;

	lcd_update_state(lcd);
	pmb887x_lcd_set_x(lcd, priv->regs[JBT6K71_RAM_ADDRESS_LOW]);
	pmb887x_lcd_set_y(lcd, priv->regs[JBT6K71_RAM_ADDRESS_HIGH]);
	pmb887x_lcd_set_window_x1(lcd, priv->regs[JBT6K71_HORIZONTAL_RAM_START]);
	pmb887x_lcd_set_window_x2(lcd, priv->regs[JBT6K71_HORIZONTAL_RAM_END]);
	pmb887x_lcd_set_window_y1(lcd, priv->regs[JBT6K71_VERTICAL_RAM_START]);
	pmb887x_lcd_set_window_y2(lcd, priv->regs[JBT6K71_VERTICAL_RAM_END]);
}

static void lcd_realize(pmb887x_lcd_t *lcd, Error **errp) {
	lcd_reset(lcd);
}

static void lcd_class_init(ObjectClass *oc, const void *data) {
	pmb887x_lcd_class_t *k = PMB887X_LCD_CLASS(oc);
	k->cmd_width = 2;
	k->param_width = 2;
	k->gram_read_dummy_pixels = 1;
	k->on_cmd = lcd_on_cmd;
	k->on_cmd_with_params = lcd_on_cmd_with_params;
	k->on_read = lcd_on_read;
	k->reset = lcd_reset;
	k->realize = lcd_realize;
}

static const TypeInfo lcd_info = {
	.name			= TYPE_PMB887X_LCD_PANEL,
	.parent			= TYPE_PMB887X_LCD,
	.instance_size	= sizeof(pmb887x_lcd_panel_t),
	.class_init		= lcd_class_init,
};

static void lcd_register_types(void) {
	type_register_static(&lcd_info);
}
type_init(lcd_register_types)
