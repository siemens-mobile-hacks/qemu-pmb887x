#define PMB887X_TRACE_ID		LCD
#define PMB887X_TRACE_PREFIX	"lh15kah2"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_LH15KAH2

#include "qemu/osdep.h"
#include "hw/arm/pmb887x/gen/peripheral/LH15KAH2.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/ssc/lcd_common.h"

#define TYPE_PMB887X_LCD_PANEL	"lh15kah2"

static const pmb887x_lcd_geometry_t LH15KAH2_GEOMETRY = {
	.width = 130,
	.height = 130,
	.gram_width = 132,
	.gram_height = 132,
	.gram_x = 1,
	.gram_y = 0,
};

static int lcd_on_cmd(pmb887x_lcd_t *_lcd, uint32_t _cmd) {
	return 1;
}

static void lcd_on_cmd_with_params(pmb887x_lcd_t *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
	g_assert(params_n == 1);

	IO_DUMP_WRITE(cmd, 1, params[0]);

	switch (cmd) {
		case LH15KAH2_X_START:
			pmb887x_lcd_set_x(lcd, params[0]);
			pmb887x_lcd_set_window_x1(lcd, params[0]);
			break;

		case LH15KAH2_Y_START:
			pmb887x_lcd_set_y(lcd, params[0]);
			pmb887x_lcd_set_window_y1(lcd, params[0]);
			break;

		case LH15KAH2_X_END:
			pmb887x_lcd_set_window_x2(lcd, params[0]);
			break;

		case LH15KAH2_Y_END:
			pmb887x_lcd_set_window_y2(lcd, params[0]);
			break;
	}
}

static void lcd_realize(pmb887x_lcd_t *lcd, Error **_errp) {
	pmb887x_lcd_set_addr_mode(lcd, LCD_AM_HORIZONTAL, LCD_AC_INC, LCD_AC_INC);
	pmb887x_lcd_set_pixel_format(lcd, LCD_PIXEL_FORMAT_RGB565);
	pmb887x_lcd_set_output_bgr(lcd, false);
	pmb887x_lcd_set_transform(lcd, false, false);
}

static void lcd_class_init(ObjectClass *oc, const void *data) {
	pmb887x_lcd_class_t *k = PMB887X_LCD_CLASS(oc);
	k->geometry = &LH15KAH2_GEOMETRY;
	k->cmd_width = 1;
	k->param_width = 1;
	k->cd_polarity = true;
	k->direct_data_write = true;
	k->on_cmd = lcd_on_cmd;
	k->on_cmd_with_params = lcd_on_cmd_with_params;
	k->realize = lcd_realize;
}

static const TypeInfo lcd_info = {
	.name			= TYPE_PMB887X_LCD_PANEL,
	.parent			= TYPE_PMB887X_LCD,
	.instance_size	= sizeof(pmb887x_lcd_t),
	.class_init		= lcd_class_init,
};

static void lcd_register_types(void) {
	type_register_static(&lcd_info);
}
type_init(lcd_register_types)
