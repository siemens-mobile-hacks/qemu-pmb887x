/*
 * Sharp LR38826 (LS020B8UD06 and others)
 * */
#define PMB887X_TRACE_ID		LCD
#define PMB887X_TRACE_PREFIX	"lr38826"

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/ssc/lcd_common.h"

#define TYPE_PMB887X_LCD_PANEL	"lr38826"
#define PMB887X_LCD_PANEL(obj)	OBJECT_CHECK(pmb887x_lcd_panel_t, (obj), TYPE_PMB887X_LCD_PANEL)

#define LS020B8UD06_MAX_REGS	0x100

static const uint16_t DEFAULT_REGS[] = {
	[0x001] = 0x0001,
	[0x003] = 0x6830,
};

typedef struct pmb887x_lcd_panel_t pmb887x_lcd_panel_t;

struct pmb887x_lcd_panel_t {
	pmb887x_lcd_t parent;
	uint16_t regs[LS020B8UD06_MAX_REGS];
};

static void lcd_update_state(pmb887x_lcd_t *lcd) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);
	bool am = (priv->regs[0x05] & (1 << 2)) != 0; /* AM */
	pmb887x_lcd_set_addr_mode(
		lcd,
		(am ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL),
		LCD_AC_INC,
		LCD_AC_INC
	);
	pmb887x_lcd_set_mode(lcd, LCD_MODE_RGB565, false, false);
}

static int lcd_on_cmd(pmb887x_lcd_t *lcd, uint32_t cmd) {
	return 1;
}

static void lcd_on_cmd_with_params(pmb887x_lcd_t *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);

	g_assert(params_n == 1);
	g_assert(cmd < LS020B8UD06_MAX_REGS);

	priv->regs[cmd] = params[0];

	DPRINTF("write reg 0x%02X -> 0x%02X\n", cmd, params[0]);

	switch (cmd) {
		case 0x05:
			lcd_update_state(lcd);
			break;

		case 0x06:
			pmb887x_lcd_set_x(lcd, params[0] & 0xFF);
			break;

		case 0x07:
			pmb887x_lcd_set_y(lcd, params[0] & 0xFF);
			break;

		case 0x08:
			pmb887x_lcd_set_window_x1(lcd, params[0] & 0xFF);
			break;

		case 0x09:
			pmb887x_lcd_set_window_x2(lcd, params[0] &0xFF);
			break;

		case 0x0A:
			pmb887x_lcd_set_window_y1(lcd, params[0] & 0xFF);
			break;

		case 0x0B:
			pmb887x_lcd_set_window_y2(lcd, params[0] & 0xFF);
			break;

		default:
			// Nothing
			break;
	}
}

static void lcd_realize(pmb887x_lcd_t *lcd, Error **errp) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);
	memset(priv->regs, 0, sizeof(priv->regs));
	memcpy(priv->regs, DEFAULT_REGS, sizeof(DEFAULT_REGS));
	lcd_update_state(lcd);
}

static void lcd_class_init(ObjectClass *oc, const void *data) {
	pmb887x_lcd_class_t *k = PMB887X_LCD_CLASS(oc);
	k->cmd_width = 1;
	k->param_width = 1;
	k->cd_polarity = true;
	k->direct_data_write = true; // CD=0 - write data, CD=1 - write CMD+ARG
	k->on_cmd = lcd_on_cmd;
	k->on_cmd_with_params = lcd_on_cmd_with_params;
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
