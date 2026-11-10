/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/lcd/lcd.h"
#include "regs.h"

#define TYPE_LH15KAH2 "lh15kah2"

static const LCDGeometry LH15KAH2_GEOMETRY = {
    .width = 130,
    .height = 130,
    .gram_width = 132,
    .gram_height = 132,
    .gram_x = 1,
    .gram_y = 0,
};

static int lh15kah2_on_cmd(LCDState *_lcd, uint32_t _cmd) {
    return 1;
}

static void lh15kah2_on_cmd_with_params(LCDState *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
    g_assert(params_n == 1);

    DEBUG_IO_WRITE(&lcd->dbg, cmd, 1, params[0]);

    switch (cmd) {
        case LH15KAH2_X_START:
            lcd_set_x(lcd, params[0]);
            lcd_set_window_x1(lcd, params[0]);
            break;

        case LH15KAH2_Y_START:
            lcd_set_y(lcd, params[0]);
            lcd_set_window_y1(lcd, params[0]);
            break;

        case LH15KAH2_X_END:
            lcd_set_window_x2(lcd, params[0]);
            break;

        case LH15KAH2_Y_END:
            lcd_set_window_y2(lcd, params[0]);
            break;
    }
}

static void lh15kah2_realize(LCDState *lcd, Error **_errp) {
    lcd_set_addr_mode(lcd, LCD_AM_HORIZONTAL, LCD_AC_INC, LCD_AC_INC);
    lcd_set_pixel_format(lcd, LCD_PIXEL_FORMAT_RGB565);
    lcd_set_output_bgr(lcd, false);
    lcd_set_transform(lcd, false, false);
}

static void lh15kah2_class_init(ObjectClass *oc, const void *_data) {
    LCDClass *k = LCD_CLASS(oc);
    k->geometry = &LH15KAH2_GEOMETRY;
    k->debug_regs = &lh15kah2_debug_regs;
    k->cmd_width = 1;
    k->param_width = 1;
    k->cd_polarity = true;
    k->direct_data_write = true;
    k->on_cmd = lh15kah2_on_cmd;
    k->on_cmd_with_params = lh15kah2_on_cmd_with_params;
    k->realize = lh15kah2_realize;
}

static const TypeInfo lh15kah2_info = {
    .name           = TYPE_LH15KAH2,
    .parent         = TYPE_LCD,
    .instance_size  = sizeof(LCDState),
    .class_init     = lh15kah2_class_init,
};

static void lh15kah2_register_types(void) {
    type_register_static(&lh15kah2_info);
}
type_init(lh15kah2_register_types)
