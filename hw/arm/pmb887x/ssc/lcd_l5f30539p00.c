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

static const uint16_t DEFAULT_REGS[][L2F50333T_MAX_REGS] = {
	[0x001] = { 0x0001 },
	[0x003] = { 0x6830 },
};

typedef struct pmb887x_lcd_panel_t pmb887x_lcd_panel_t;

struct pmb887x_lcd_panel_t {
	pmb887x_lcd_t parent;
	uint16_t regs[L2F50333T_MAX_REGS][128];
};

static void lcd_update_state(pmb887x_lcd_t *lcd) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);

	#if 0
	bool ss = (priv->regs[0x001] & (1 << 8)) != 0; /* SS */
	bool sm = (priv->regs[0x001] & (1 << 10)) != 0; /* SM */

	bool am = (priv->regs[0x003] & (1 << 3)) != 0; /* AM */
	bool id0 = (priv->regs[0x003] & (1 << 4)) != 0; /* ID0 */
	bool id1 = (priv->regs[0x003] & (1 << 5)) != 0; /* ID1 */
	bool bgr = (priv->regs[0x001] & (1 << 11)) != 0; /* BGR */

	bool dfm0 = (priv->regs[0x003] & (1 << 13)) != 0; /* DFM0 */
	bool dfm1 = (priv->regs[0x003] & (1 << 14)) != 0; /* DFM1 */

	DPRINTF("am=%d, id1=%d, id0=%d, ss=%d, sm=%d, dfm0=%d, dfm1=%d, bgr=%d\n", am, id1, id0, ss, sm, dfm0, dfm1, bgr);

	pmb887x_lcd_set_addr_mode(
		lcd,
		(am ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL),
		(id0 ? LCD_AC_INC : LCD_AC_DEC),
		(id1 ? LCD_AC_INC : LCD_AC_DEC)
		);

		if (dfm0 == 0 && dfm1 == 1) {
			enum pmb887x_lcd_pixel_mode_t new_mode = bgr ? LCD_MODE_BGR666 : LCD_MODE_RGB666;
			pmb887x_lcd_set_mode(lcd, new_mode, false, false);
} else {
	enum pmb887x_lcd_pixel_mode_t new_mode = bgr ? LCD_MODE_BGR565 : LCD_MODE_RGB565;
	pmb887x_lcd_set_mode(lcd, new_mode, false, false);
}
#endif
}

static uint32_t lcd_on_cmd(pmb887x_lcd_t *lcd, uint32_t cmd) {
	if (cmd == 0x2C) {
		pmb887x_lcd_set_ram_mode(lcd, true);
		return 0;
	}

	switch (cmd) {
		case 0x26: // GAMSET
		case 0x35: // TEON
		case 0x3A: // COLMOD
		case 0xB8: // MADDEF
		case 0xC2: // IFMOD
			return 1;

		case 0x37: // VSCRSADD
			return 2;

		case 0x2A: // CASET
		case 0x2B: // PASET
		case 0x30: // PTLAR
			return 4;

		case 0xC7: // PPWRCTL
			return 9;

		case 0xC6: // PWRCTL
			return 17;

		case 0xB0: // DISCTL
			return 19;

		case 0xF0 ... 0xFD: // GCSETxx
			return 32;

		case 0x2D: // COLORSET
			return 128;
	}

	return 0;
}

static void lcd_on_cmd_with_params(pmb887x_lcd_t *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);

	g_assert(cmd < L2F50333T_MAX_REGS);

	for (int i = 0; i < params_n; i++)
		priv->regs[cmd][i] = params[i];

	DPRINTF("write reg 0x%04X -> 0x%04X\n", cmd, params[0]);

	switch (cmd) {
		case 0x01:
		case 0x03:
			lcd_update_state(lcd);
			break;

		case 0x21:
			pmb887x_lcd_set_x(lcd, params[0] & 0xFF);
			pmb887x_lcd_set_y(lcd, params[0] >> 8);
			break;

		case 0x44:
			pmb887x_lcd_set_window_x1(lcd, params[0] & 0xFF);
			pmb887x_lcd_set_window_x2(lcd, params[0] >> 8);
			break;

		case 0x45:
			pmb887x_lcd_set_window_y1(lcd, params[0] & 0xFF);
			pmb887x_lcd_set_window_y2(lcd, params[0] >> 8);
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
