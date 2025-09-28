/*
 * Solomon SSD1286
 * */
#define PMB887X_TRACE_ID		LCD
#define PMB887X_TRACE_PREFIX	"ssd1286"

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/ssc/lcd_common.h"

#define TYPE_PMB887X_LCD_SSD1286	"ssd1286"
#define PMB887X_LCD_SSD1286(obj)	OBJECT_CHECK(pmb887x_lcd_ssd1286_t, (obj), TYPE_PMB887X_LCD_SSD1286)

#define SSD1286_MAX_BPP		18
#define SSD1286_MAX_REGS	0x100

static const uint16_t DEFAULT_REGS[] = { 0 };

typedef struct pmb887x_lcd_ssd1286_t pmb887x_lcd_ssd1286_t;

struct pmb887x_lcd_ssd1286_t {
	pmb887x_lcd_t parent;
	uint16_t regs[SSD1286_MAX_REGS];
};

static void lcd_update_state(pmb887x_lcd_t *lcd) {
	pmb887x_lcd_ssd1286_t *priv = PMB887X_LCD_SSD1286(lcd);
	
	bool ss = (priv->regs[0x001] & (1 << 8)) != 0; /* SS */
	bool sm = (priv->regs[0x001] & (1 << 10)) != 0; /* SM */
	
	bool am = (priv->regs[0x003] & (1 << 3)) != 0; /* AM */
	bool id0 = (priv->regs[0x003] & (1 << 4)) != 0; /* ID0 */
	bool id1 = (priv->regs[0x003] & (1 << 5)) != 0; /* ID1 */
	bool bgr = (priv->regs[0x001] & (1 << 11)) != 0; /* BGR */
	
	DPRINTF("am=%d, id1=%d, id0=%d, ss=%d, sm=%d\n", am, id1, id0, ss, sm);
	
	pmb887x_lcd_set_addr_mode(
		lcd,
		(am ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL),
		(id0 ? LCD_AC_INC : LCD_AC_DEC),
		(id1 ? LCD_AC_INC : LCD_AC_DEC)
	);
	
	enum pmb887x_lcd_pixel_mode_t new_mode = bgr ? LCD_MODE_BGR565 : LCD_MODE_RGB565;
	pmb887x_lcd_set_mode(lcd, new_mode);
}

static uint32_t lcd_on_cmd(pmb887x_lcd_t *lcd, uint32_t cmd) {
	if (cmd == 0x22) {
		pmb887x_lcd_set_ram_mode(lcd, true);
		return 0;
	}
	return 1;
}

static void lcd_on_cmd_with_params(pmb887x_lcd_t *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
	pmb887x_lcd_ssd1286_t *priv = PMB887X_LCD_SSD1286(lcd);
	
	g_assert(params_n == 1);
	g_assert(cmd < SSD1286_MAX_REGS);
	
	priv->regs[cmd] = params[0];
	
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
	pmb887x_lcd_ssd1286_t *priv = PMB887X_LCD_SSD1286(lcd);
	memset(priv->regs, 0, sizeof(priv->regs));
	memcpy(priv->regs, DEFAULT_REGS, sizeof(DEFAULT_REGS));
	lcd_update_state(lcd);
}

static void lcd_class_init(ObjectClass *oc, void *data) {
	pmb887x_lcd_class_t *k = PMB887X_LCD_CLASS(oc);
	k->cmd_width = 1;
	k->param_width = 2;
	k->on_cmd = lcd_on_cmd;
	k->on_cmd_with_params = lcd_on_cmd_with_params;
	k->realize = lcd_realize;
}

static const TypeInfo lcd_info = {
	.name			= TYPE_PMB887X_LCD_SSD1286,
	.parent			= TYPE_PMB887X_LCD,
	.instance_size	= sizeof(pmb887x_lcd_ssd1286_t),
	.class_init		= lcd_class_init,
};

static void lcd_register_types(void) {
	type_register_static(&lcd_info);
}
type_init(lcd_register_types)
