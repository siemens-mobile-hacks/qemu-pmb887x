/*
 * Solomon SSD1286
 * */
#define PMB887X_TRACE_ID		LCD
#define PMB887X_TRACE_PREFIX	"ssd1286"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_SSD1286

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/arm/pmb887x/gen/peripheral/SSD1286.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/ssc/lcd_common.h"

#define TYPE_PMB887X_LCD_PANEL	"ssd1286"
#define PMB887X_LCD_PANEL(obj)	OBJECT_CHECK(pmb887x_lcd_panel_t, (obj), TYPE_PMB887X_LCD_PANEL)

#define SSD1286_MAX_BPP		18
#define SSD1286_MAX_REGS	0x100

static const uint16_t DEFAULT_REGS[] = {
	[SSD1286_DRIVER_OUTPUT_CONTROL] = 0x0001,
	[SSD1286_ENTRY_MODE] = 0x6830,
};

typedef struct pmb887x_lcd_panel_t pmb887x_lcd_panel_t;

struct pmb887x_lcd_panel_t {
	pmb887x_lcd_t parent;
	uint16_t regs[SSD1286_MAX_REGS];
};

static void lcd_update_state(pmb887x_lcd_t *lcd) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);
	uint16_t driver_output_control = priv->regs[SSD1286_DRIVER_OUTPUT_CONTROL];
	uint16_t entry_mode = priv->regs[SSD1286_ENTRY_MODE];
	uint32_t id = (entry_mode & SSD1286_ENTRY_MODE_ID) >> SSD1286_ENTRY_MODE_ID_SHIFT;
	uint32_t dfm = (entry_mode & SSD1286_ENTRY_MODE_DFM) >> SSD1286_ENTRY_MODE_DFM_SHIFT;
	bool ss = (driver_output_control & SSD1286_DRIVER_OUTPUT_CONTROL_RL) != 0;
	bool sm = (driver_output_control & SSD1286_DRIVER_OUTPUT_CONTROL_SM) != 0;
	bool am = (entry_mode & SSD1286_ENTRY_MODE_AM) != 0;
	bool id0 = (id & BIT(0)) != 0;
	bool id1 = (id & BIT(1)) != 0;
	bool bgr = (driver_output_control & SSD1286_DRIVER_OUTPUT_CONTROL_BGR) != 0;
	bool dfm0 = (dfm & BIT(0)) != 0;
	bool dfm1 = (dfm & BIT(1)) != 0;
	
	DPRINTF("am=%d, id1=%d, id0=%d, ss=%d, sm=%d, dfm0=%d, dfm1=%d, bgr=%d\n", am, id1, id0, ss, sm, dfm0, dfm1, bgr);
	
	pmb887x_lcd_set_addr_mode(
		lcd,
		(am ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL),
		(id0 ? LCD_AC_INC : LCD_AC_DEC),
		(id1 ? LCD_AC_INC : LCD_AC_DEC)
	);
	
	enum pmb887x_lcd_pixel_format_t pixel_format = dfm0 == 0 && dfm1 == 1 ?
		LCD_PIXEL_FORMAT_RGB666_6_6_6 : LCD_PIXEL_FORMAT_RGB565;
	pmb887x_lcd_set_pixel_format(lcd, pixel_format);
	pmb887x_lcd_set_output_bgr(lcd, bgr);
	pmb887x_lcd_set_transform(lcd, false, false);
}

static int lcd_on_cmd(pmb887x_lcd_t *lcd, uint32_t cmd) {
	if (cmd == SSD1286_GRAM_DATA) {
		pmb887x_lcd_set_ram_mode(lcd, true);
		return 0;
	}
	return 1;
}

static void lcd_on_cmd_with_params(pmb887x_lcd_t *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
	pmb887x_lcd_panel_t *priv = PMB887X_LCD_PANEL(lcd);
	
	g_assert(params_n == 1);
	g_assert(cmd < SSD1286_MAX_REGS);

	IO_DUMP_WRITE(cmd, 2, params[0]);
	priv->regs[cmd] = params[0];
	
	switch (cmd) {
		case SSD1286_DRIVER_OUTPUT_CONTROL:
		case SSD1286_ENTRY_MODE:
			lcd_update_state(lcd);
			break;
		
		case SSD1286_RAM_ADDRESS:
			pmb887x_lcd_set_x(lcd, params[0] & 0xFF);
			pmb887x_lcd_set_y(lcd, params[0] >> 8);
			break;
		
		case SSD1286_HORIZONTAL_RAM_ADDRESS:
			pmb887x_lcd_set_window_x1(lcd, (params[0] & SSD1286_HORIZONTAL_RAM_ADDRESS_HSA) >>
				SSD1286_HORIZONTAL_RAM_ADDRESS_HSA_SHIFT);
			pmb887x_lcd_set_window_x2(lcd, (params[0] & SSD1286_HORIZONTAL_RAM_ADDRESS_HEA) >>
				SSD1286_HORIZONTAL_RAM_ADDRESS_HEA_SHIFT);
			break;
		
		case SSD1286_VERTICAL_RAM_ADDRESS:
			pmb887x_lcd_set_window_y1(lcd, (params[0] & SSD1286_VERTICAL_RAM_ADDRESS_VSA) >>
				SSD1286_VERTICAL_RAM_ADDRESS_VSA_SHIFT);
			pmb887x_lcd_set_window_y2(lcd, (params[0] & SSD1286_VERTICAL_RAM_ADDRESS_VEA) >>
				SSD1286_VERTICAL_RAM_ADDRESS_VEA_SHIFT);
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
