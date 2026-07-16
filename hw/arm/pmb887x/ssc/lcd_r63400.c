/*
 * Renesas R63400
 * */
#define PMB887X_TRACE_ID		LCD
#define PMB887X_TRACE_PREFIX	"r63400"

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/ssc/lcd_common.h"

#define TYPE_PMB887X_LCD_PANEL	"r63400"
#define PMB887X_LCD_PANEL(obj)	OBJECT_CHECK(pmb887x_lcd_panel_t, (obj), TYPE_PMB887X_LCD_PANEL)

#define R63400_MAX_REGS		0x500

// R63400 registers
#define R63400_R_DRIVER_OUTPUT		0x01	// SS (bit8), SM (bit10)
#define R63400_R_ENTRY_MODE			0x03	// AM/ID/BGR/DFM/TRI
#define R63400_R_DISPLAY_CTRL1		0x07	// D0/D1 (display on)
#define R63400_R_GATE_SCAN			0x400	// GS (bit15) gate scan direction
#define R63400_R_GRAM_X				0x200	// GRAM horizontal address
#define R63400_R_GRAM_Y				0x201	// GRAM vertical address
#define R63400_R_GRAM_WRITE			0x202	// write data to GRAM
#define R63400_R_WINDOW_HSTART		0x210	// window horizontal start
#define R63400_R_WINDOW_HEND		0x211	// window horizontal end
#define R63400_R_WINDOW_VSTART		0x212	// window vertical start
#define R63400_R_WINDOW_VEND		0x213	// window vertical end

static const uint16_t DEFAULT_REGS[] = {
	[R63400_R_DRIVER_OUTPUT]	= 0x0100,	/* SS = 1 */
	[R63400_R_ENTRY_MODE]		= 0x0030,	/* AM=0, ID=11 */
};

typedef struct pmb887x_lcd_panel_t pmb887x_lcd_panel_t;

struct pmb887x_lcd_panel_t {
	pmb887x_lcd_t parent;
	uint16_t regs[R63400_MAX_REGS];
};

static void lcd_update_state(pmb887x_lcd_t *lcd) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);

	bool ss = (priv->regs[R63400_R_DRIVER_OUTPUT] & (1 << 8)) != 0;	/* SS */
	bool am = (priv->regs[R63400_R_ENTRY_MODE] & (1 << 3)) != 0;	/* AM */
	bool id0 = (priv->regs[R63400_R_ENTRY_MODE] & (1 << 4)) != 0;	/* ID0 */
	bool id1 = (priv->regs[R63400_R_ENTRY_MODE] & (1 << 5)) != 0;	/* ID1 */
	bool bgr = (priv->regs[R63400_R_ENTRY_MODE] & (1 << 12)) != 0;	/* BGR */
	bool dfm = (priv->regs[R63400_R_ENTRY_MODE] & (1 << 14)) != 0;	/* DFM */
	bool tri = (priv->regs[R63400_R_ENTRY_MODE] & (1 << 15)) != 0;	/* TRI */
	bool gs = (priv->regs[R63400_R_GATE_SCAN] & (1 << 15)) != 0;	/* GS */

	DPRINTF("am=%d, id1=%d, id0=%d, ss=%d, gs=%d, bgr=%d, tri=%d, dfm=%d\n", am, id1, id0, ss, gs, bgr, tri, dfm);

	pmb887x_lcd_set_addr_mode(
		lcd,
		(am ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL),
		(id0 ? LCD_AC_DEC : LCD_AC_INC),
		(id1 ? LCD_AC_INC : LCD_AC_DEC)
	);

	// TRI=1 + DFM=1: 18-bit pixel transferred in 3 cycles; otherwise 16-bit.
	enum pmb887x_lcd_pixel_mode_t new_mode;
	if (tri && dfm) {
		new_mode = bgr ? LCD_MODE_BGR666 : LCD_MODE_RGB666;
	} else {
		new_mode = bgr ? LCD_MODE_BGR565 : LCD_MODE_RGB565;
	}

	pmb887x_lcd_set_mode(lcd, new_mode, ss, gs);
}

static int lcd_on_cmd(pmb887x_lcd_t *lcd, uint32_t cmd) {
	if (cmd == R63400_R_GRAM_WRITE) {
		pmb887x_lcd_set_ram_mode(lcd, true);
		return 0;
	}
	return 1;
}

static void lcd_on_cmd_with_params(pmb887x_lcd_t *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);

	g_assert(params_n == 1);

	uint16_t value = params[0];

	if (cmd >= R63400_MAX_REGS) {
		DPRINTF("ignoring out-of-range reg R%X\n", cmd);
		return;
	}
	priv->regs[cmd] = value;

	DPRINTF("write R%02X -> 0x%04X\n", cmd, value);

	switch (cmd) {
		case R63400_R_DRIVER_OUTPUT:
		case R63400_R_ENTRY_MODE:
		case R63400_R_DISPLAY_CTRL1:
		case R63400_R_GATE_SCAN:
			lcd_update_state(lcd);
			break;

		case R63400_R_GRAM_X:
			pmb887x_lcd_set_x(lcd, value);
			break;

		case R63400_R_GRAM_Y:
			pmb887x_lcd_set_y(lcd, value);
			break;

		case R63400_R_WINDOW_HSTART:
			pmb887x_lcd_set_window_x1(lcd, value);
			break;

		case R63400_R_WINDOW_HEND:
			pmb887x_lcd_set_window_x2(lcd, value);
			break;

		case R63400_R_WINDOW_VSTART:
			pmb887x_lcd_set_window_y1(lcd, value);
			break;

		case R63400_R_WINDOW_VEND:
			pmb887x_lcd_set_window_y2(lcd, value);
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
	k->cmd_width = 2;
	k->param_width = 2;
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
