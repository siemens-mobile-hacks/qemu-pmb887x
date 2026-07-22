/*
 * Toshiba JBT6K71
 * */
#define PMB887X_TRACE_ID		LCD
#define PMB887X_TRACE_PREFIX	"jbt6k71"

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
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
	[0x001]	= 0x27, /* Driver output control setting */
	[0x003]	= 0x30, /* Entry mode  */
	[0x100] = 0x800, /* Display control */
};

typedef struct pmb887x_lcd_panel_t pmb887x_lcd_panel_t;

struct pmb887x_lcd_panel_t {
	pmb887x_lcd_t parent;
	uint16_t regs[JBT6K71_MAX_REGS];
};

static void lcd_update_state(pmb887x_lcd_t *lcd) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);
	
	bool ss = (priv->regs[0x001] & (1 << 8)) != 0; /* SS */
	bool am = (priv->regs[0x003] & (1 << 3)) != 0; /* AM */
	bool id0 = (priv->regs[0x003] & (1 << 4)) != 0; /* ID0 */
	bool id1 = (priv->regs[0x003] & (1 << 5)) != 0; /* ID1 */
	bool bgr = (priv->regs[0x003] & (1 << 12)) != 0; /* BGR */
	bool dfm0 = (priv->regs[0x003] & (1 << 13)) != 0; /* DFM0 */
	bool dfm1 = (priv->regs[0x003] & (1 << 14)) != 0; /* DFM1 */
	bool tri = (priv->regs[0x003] & (1 << 15)) != 0; /* TRI */
	bool ud = (priv->regs[0x100] & (1 << 11)) != 0; /* UD */
	
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
	if (cmd == 0x202) {
		pmb887x_lcd_set_ram_mode(lcd, true);
		return 0;
	}
	return 1;
}

static void lcd_on_cmd_with_params(pmb887x_lcd_t *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);
	
	g_assert(params_n == 1);
	g_assert(cmd < JBT6K71_MAX_REGS);
	
	priv->regs[cmd] = params[0];
	
	DPRINTF("write reg 0x%04X -> 0x%04X\n", cmd, params[0]);
	
	switch (cmd) {
		case 0x001:
		case 0x003:
		case 0x100:
			lcd_update_state(lcd);
			break;
		
		case 0x200:
			pmb887x_lcd_set_x(lcd, params[0]);
			break;
		
		case 0x201:
			pmb887x_lcd_set_y(lcd, params[0]);
			break;
		
		case 0x406:
			pmb887x_lcd_set_window_x1(lcd, params[0]);
			break;
		
		case 0x407:
			pmb887x_lcd_set_window_x2(lcd, params[0]);
			break;
		
		case 0x408:
			pmb887x_lcd_set_window_y1(lcd, params[0]);
			break;
		
		case 0x409:
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
	if (lcd->current_cmd == 0) {
		value = JBT6K71_DEVICE_CODE;
	} else if (lcd->current_cmd < JBT6K71_MAX_REGS) {
		value = priv->regs[lcd->current_cmd];
	}
	uint32_t shift = (1 - index % JBT6K71_BUS_WIDTH) * 8;
	return (value >> shift) & 0xFF;
}

static void lcd_reset(pmb887x_lcd_t *lcd) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);
	memset(priv->regs, 0, sizeof(priv->regs));
	memcpy(priv->regs, DEFAULT_REGS, sizeof(DEFAULT_REGS));
	priv->regs[0x407] = lcd->width - 1;
	priv->regs[0x409] = lcd->height - 1;

	lcd_update_state(lcd);
	pmb887x_lcd_set_x(lcd, priv->regs[0x200]);
	pmb887x_lcd_set_y(lcd, priv->regs[0x201]);
	pmb887x_lcd_set_window_x1(lcd, priv->regs[0x406]);
	pmb887x_lcd_set_window_x2(lcd, priv->regs[0x407]);
	pmb887x_lcd_set_window_y1(lcd, priv->regs[0x408]);
	pmb887x_lcd_set_window_y2(lcd, priv->regs[0x409]);
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
