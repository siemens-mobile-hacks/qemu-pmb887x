/*
 * Toshiba JBT6K71
 * */
#define PMB887X_TRACE_ID		LCD
#define PMB887X_TRACE_PREFIX	"pmb887x-lcd-jbt6k71"

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/dif/lcd_common.h"

#define TYPE_PMB887X_LCD_JBT6K71	"pmb887x-lcd-jbt6k71"
#define PMB887X_LCD_JBT6K71(obj)	OBJECT_CHECK(pmb887x_lcd_jbt6k71_t, (obj), TYPE_PMB887X_LCD_JBT6K71)

#define JBT6K71_MAX_BPP		18
#define JBT6K71_BUS_WIDTH	2
#define JBT6K71_MAX_REGS	0x800

static const uint16_t DEFAULT_REGS[] = {
	[0x001]	= 0x27, /* Driver output control setting */
	[0x003]	= 0x30, /* Entry mode  */
};

typedef struct {
	pmb887x_lcd_t parent;
	int current_reg;
	uint16_t regs[JBT6K71_MAX_REGS];
} pmb887x_lcd_jbt6k71_t;

static void lcd_update_state(pmb887x_lcd_t *lcd) {
	pmb887x_lcd_jbt6k71_t *priv = PMB887X_LCD_JBT6K71(lcd);
	
	bool shift_select = (priv->regs[0x001] & (1 << 8)) != 0; /* SS */
	bool am = (priv->regs[0x003] & (1 << 3)) != 0; /* AM */
	bool id0 = (priv->regs[0x003] & (1 << 4)) != 0; /* ID0 */
	bool id1 = (priv->regs[0x003] & (1 << 5)) != 0; /* ID1 */
	bool bgr = (priv->regs[0x003] & (1 << 12)) != 0; /* BGR */
	bool dfm0 = (priv->regs[0x003] & (1 << 13)) != 0; /* DFM0 */
	bool dfm1 = (priv->regs[0x003] & (1 << 14)) != 0; /* DFM1 */
	bool tri = (priv->regs[0x003] & (1 << 15)) != 0; /* TRI */
	
	#ifdef WIN32
	id0 = 1;
	id1 = 1;
	#endif
	
	pmb887x_lcd_set_addr_mode_x(lcd, id0 ? LCD_ADDR_MODE_INCR : LCD_ADDR_MODE_DECR);
	pmb887x_lcd_set_addr_mode_y(lcd, id1 ? LCD_ADDR_MODE_INCR : LCD_ADDR_MODE_DECR);
	pmb887x_lcd_set_vflip(lcd, !shift_select);
	
	// DPRINTF("id0=%d, id1=%d, bgr=%d, dfm0=%d, dfm1=%d, tri=%d\n", id0, id1, bgr, dfm0, dfm1, tri);
	
	enum pmb887x_lcd_pixel_mode_t new_mode;
	if (tri && dfm0 && dfm1) {
		new_mode = bgr ? LCD_MODE_BGR666 : LCD_MODE_RGB666;
	} else if (!tri && !dfm0 && !dfm1) {
		new_mode = bgr ? LCD_MODE_BGR565 : LCD_MODE_RGB565;
	} else {
		error_report("[pmb887x-lcd-jbt6k71]: invalid config: dfm0=%d, dfm1=%d, tri=%d", dfm0, dfm1, tri);
		exit(1);
	}
	
	pmb887x_lcd_set_mode(lcd, new_mode);
}

static void lcd_write_ram(pmb887x_lcd_t *lcd, uint32_t value, uint32_t size) {
	for (int i = 0; i < size; i++)
		pmb887x_lcd_put_pixel_byte(lcd, (value >> (i * 8)) & 0xFF);
}

static void lcd_write_reg(pmb887x_lcd_t *lcd, uint16_t reg, uint16_t value) {
	pmb887x_lcd_jbt6k71_t *priv = PMB887X_LCD_JBT6K71(lcd);
	priv->regs[reg] = value;
	
	switch (reg) {
		case 0x001:
		case 0x003:
			lcd_update_state(lcd);
		break;
		
		case 0x200:
			pmb887x_lcd_set_x(lcd, value);
		break;
		
		case 0x201:
			pmb887x_lcd_set_y(lcd, value);
		break;
		
		case 0x406:
			pmb887x_lcd_set_window_x1(lcd, value);
		break;
		
		case 0x407:
			pmb887x_lcd_set_window_x2(lcd, value);
		break;
		
		case 0x408:
			pmb887x_lcd_set_window_y1(lcd, value);
		break;
		
		case 0x409:
			pmb887x_lcd_set_window_y2(lcd, value);
		break;
	}
}

static void lcd_write(pmb887x_lcd_t *lcd, uint32_t value) {
	pmb887x_lcd_jbt6k71_t *priv = PMB887X_LCD_JBT6K71(lcd);
	
	if (pmb887x_lcd_get_cd(lcd)) {
		if (value == 0x202) {
			priv->current_reg = -1;
			pmb887x_lcd_set_ram_mode(lcd, true);
		} else {
			priv->current_reg = value;
			pmb887x_lcd_set_ram_mode(lcd, false);
		}
	} else if (priv->current_reg != -1) {
		lcd_write_reg(lcd, priv->current_reg, value);
		priv->current_reg = -1;
	} else if (value) {
		DPRINTF("Unexpected data write: %08X\n", value);
	}
}

static void lcd_realize(DeviceState *dev, Error **errp) {
	pmb887x_lcd_t *lcd = PMB887X_LCD(dev);
	pmb887x_lcd_jbt6k71_t *priv = PMB887X_LCD_JBT6K71(dev);
	
	memset(priv->regs, 0, sizeof(priv->regs));
	memcpy(priv->regs, DEFAULT_REGS, sizeof(DEFAULT_REGS));
	priv->current_reg = -1;
	
	pmb887x_lcd_init(lcd, dev);
}

static Property lcd_properties[] = {
	DEFINE_PROP_END_OF_LIST(),
};

static void lcd_class_init(ObjectClass *oc, void *data) {
	DeviceClass *dc = DEVICE_CLASS(oc);
	device_class_set_props(dc, lcd_properties);
	dc->realize = lcd_realize;
	
	pmb887x_lcd_class_t *k = PMB887X_LCD_CLASS(oc);
	k->bus_width = JBT6K71_BUS_WIDTH;
	k->write = lcd_write;
	k->write_ram = lcd_write_ram;
}

static const TypeInfo lcd_info = {
	.name			= TYPE_PMB887X_LCD_JBT6K71,
	.parent			= TYPE_PMB887X_LCD,
	.instance_size	= sizeof(pmb887x_lcd_jbt6k71_t),
	.class_init		= lcd_class_init,
};

static void lcd_register_types(void) {
	type_register_static(&lcd_info);
}
type_init(lcd_register_types)
