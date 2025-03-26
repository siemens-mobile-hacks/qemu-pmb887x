/*
 * Philips HX5050A (WIP)
 * */
#define PMB887X_TRACE_ID		LCD
#define PMB887X_TRACE_PREFIX	"pmb887x-lcd-hx5050a"

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/dif/lcd_common.h"

#define TYPE_PMB887X_LCD_HX5050A	"pmb887x-lcd-hx5050a"
#define PMB887X_LCD_HX5050A(obj)	OBJECT_CHECK(pmb887x_lcd_hx5050a_t, (obj), TYPE_PMB887X_LCD_HX5050A)

#define HX5050A_MAX_BPP		18
#define HX5050A_MAX_REGS	0x100

static const uint16_t DEFAULT_REGS[] = { 0 };

typedef struct {
	pmb887x_lcd_t parent;
	uint16_t regs[HX5050A_MAX_REGS];
} pmb887x_lcd_hx5050a_t;

static void lcd_update_state(pmb887x_lcd_t *lcd) {
	pmb887x_lcd_hx5050a_t *priv = PMB887X_LCD_HX5050A(lcd);
	
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
	if (cmd == 0x2c) {
		pmb887x_lcd_set_ram_mode(lcd, true);
		return 0;
	}
	return 1;
}

static void lcd_on_cmd_with_params(pmb887x_lcd_t *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
	pmb887x_lcd_hx5050a_t *priv = PMB887X_LCD_HX5050A(lcd);
	
	g_assert(params_n == 1);
	g_assert(cmd < HX5050A_MAX_REGS);
	
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
	}
}

static void lcd_realize(DeviceState *dev, Error **errp) {
	pmb887x_lcd_t *lcd = PMB887X_LCD(dev);
	pmb887x_lcd_hx5050a_t *priv = PMB887X_LCD_HX5050A(dev);
	
	memset(priv->regs, 0, sizeof(priv->regs));
	memcpy(priv->regs, DEFAULT_REGS, sizeof(DEFAULT_REGS));
	
	pmb887x_lcd_init(lcd, dev);
	lcd_update_state(lcd);
}

static void lcd_class_init(ObjectClass *oc, void *data) {
	DeviceClass *dc = DEVICE_CLASS(oc);
	dc->realize = lcd_realize;
	
	pmb887x_lcd_class_t *k = PMB887X_LCD_CLASS(oc);
	k->cmd_width = 1;
	k->param_width = 1;
	k->on_cmd = lcd_on_cmd;
	k->on_cmd_with_params = lcd_on_cmd_with_params;
}

static const TypeInfo lcd_info = {
	.name			= TYPE_PMB887X_LCD_HX5050A,
	.parent			= TYPE_PMB887X_LCD,
	.instance_size	= sizeof(pmb887x_lcd_hx5050a_t),
	.class_init		= lcd_class_init,
};

static void lcd_register_types(void) {
	type_register_static(&lcd_info);
}
type_init(lcd_register_types)
