/*
 * Solomon L2F50333T
 * */
#define PMB887X_TRACE_ID		LCD
#define PMB887X_TRACE_PREFIX	"l2f50333t"

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/ssc/lcd_common.h"

#define TYPE_PMB887X_LCD_PANEL	"l2f50333t"
#define PMB887X_LCD_PANEL(obj)	OBJECT_CHECK(pmb887x_lcd_panel_t, (obj), TYPE_PMB887X_LCD_PANEL)

#define L2F50333T_MAX_BPP		18
#define L2F50333T_MAX_REGS		256

static const uint16_t DEFAULT_REGS[][128] = {
	[0x36] = { 0x00 }, // MADCTL
	[0x3A] = { 0x05 }, // COLMOD
};

typedef struct pmb887x_lcd_panel_t pmb887x_lcd_panel_t;

struct pmb887x_lcd_panel_t {
	pmb887x_lcd_t parent;
	uint16_t regs[L2F50333T_MAX_REGS][128];
};

static void lcd_update_state(pmb887x_lcd_t *lcd) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);

	bool invert_xy = (priv->regs[0x36][0] & (1 << 0)) != 0;
	bool bgr = (priv->regs[0x36][0] & (1 << 3)) != 0;
	bool mv = (priv->regs[0x36][0] & (1 << 5)) != 0;
	bool mx = (priv->regs[0x36][0] & (1 << 6)) != 0; /* 1 - decrement, 0 - increment */
	bool my = (priv->regs[0x36][0] & (1 << 7)) != 0; /* 1 - decrement, 0 - increment */

	uint8_t colmod = (priv->regs[0x3A][0] & 7);

	enum pmb887x_lcd_pixel_mode_t new_mode;
	switch (colmod) {
		case 0x05:
			new_mode = bgr ? LCD_MODE_BGR565 : LCD_MODE_RGB565;
			break;
		case 0x06:
			new_mode = bgr ? LCD_MODE_BGR666 : LCD_MODE_RGB666;
			break;
		default:
			EPRINTF("Invalid color mode: 0x%02X, fallback to 565", colmod);
			new_mode = bgr ? LCD_MODE_BGR565 : LCD_MODE_RGB565;
			break;
	}

	pmb887x_lcd_set_addr_mode(
		lcd,
		(mv ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL),
		(mx ? LCD_AC_DEC : LCD_AC_INC),
		(my ? LCD_AC_DEC : LCD_AC_INC)
	);

	DPRINTF("mv=%d, mx=%d, my=%d, invert_xy=%d, bgr=%d\n", mv, mx, my, invert_xy, bgr);
	pmb887x_lcd_set_mode(lcd, new_mode, invert_xy, invert_xy);
}

static uint32_t lcd_on_cmd(pmb887x_lcd_t *lcd, uint32_t cmd) {
	if (cmd == 0x2C) {
		pmb887x_lcd_set_ram_mode(lcd, true);
		return 0;
	}

	switch (cmd) {
		case 0xB0: // DISCTL
			return 15;
		case 0xB1: // GCPSETx
		case 0xB2: // GCPSETx
		case 0xBA: // GCPSETx
		case 0xBF: // GCPSETx
			return 60;
		case 0x2D: // RGBSET
			return 128;
		case 0x2A: // CASET
		case 0x2B: // PASET
			return 4;
		case 0x36: // MADCTL
		case 0x26: // GAMSET
		case 0xC2: // IFMOD
		case 0x3A: // COLMOD
		case 0xBE: // VOLCNT
			return 1;
	}

	return 0;
}

static void lcd_on_cmd_with_params(pmb887x_lcd_t *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);

	g_assert(cmd < L2F50333T_MAX_REGS);

	DPRINTF("write reg 0x%02X:\n", cmd);
	for (int i = 0; i < params_n; i++) {
		priv->regs[cmd][i] = params[i];
		DPRINTF("  param[%d]: 0x%02X\n", i, params[i]);
	}

	switch (cmd) {
		case 0x36:
		case 0x3A:
			lcd_update_state(lcd);
			break;

		case 0x2A:
			pmb887x_lcd_set_x(lcd, (params[0] << 8) | params[1]);
			pmb887x_lcd_set_window_x1(lcd, (params[0] << 8) | params[1]);
			pmb887x_lcd_set_window_x2(lcd, (params[2] << 8) | params[3]);
			break;

		case 0x2B:
			pmb887x_lcd_set_y(lcd, (params[0] << 8) | params[1]);
			pmb887x_lcd_set_window_y1(lcd, (params[0] << 8) | params[1]);
			pmb887x_lcd_set_window_y2(lcd, (params[2] << 8) | params[3]);
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
