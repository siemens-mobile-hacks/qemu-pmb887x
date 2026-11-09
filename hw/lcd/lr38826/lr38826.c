/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Sharp LR38826 (LS020B8UD06 and others) */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/lcd/lcd.h"

#define TYPE_LR38826 "lr38826"
OBJECT_DECLARE_SIMPLE_TYPE(LR38826State, LR38826)

#define LS020B8UD06_MAX_REGS    0x100

static const uint16_t DEFAULT_REGS[] = {
    [0x001] = 0x0001,
    [0x003] = 0x6830,
};

struct LR38826State {
    LCDState parent;
    uint16_t regs[LS020B8UD06_MAX_REGS];
};

static void lr38826_update_state(LCDState *lcd) {
    LR38826State *priv = LR38826(lcd);
    bool am = (priv->regs[0x05] & (1 << 2)) != 0; /* AM */
    lcd_set_addr_mode(
        lcd,
        (am ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL),
        LCD_AC_INC,
        LCD_AC_INC
    );
    lcd_set_pixel_format(lcd, LCD_PIXEL_FORMAT_RGB565);
    lcd_set_output_bgr(lcd, false);
    lcd_set_transform(lcd, false, false);
}

static int lr38826_on_cmd(LCDState *lcd, uint32_t cmd) {
    return 1;
}

static void lr38826_on_cmd_with_params(LCDState *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
    LR38826State *priv = LR38826(lcd);

    g_assert(params_n == 1);
    g_assert(cmd < LS020B8UD06_MAX_REGS);

    priv->regs[cmd] = params[0];

    DEBUG_LOG(&lcd->dbg, "write reg 0x%02X -> 0x%02X\n", cmd, params[0]);

    switch (cmd) {
        case 0x05:
            lr38826_update_state(lcd);
            break;

        case 0x06:
            lcd_set_x(lcd, params[0] & 0xFF);
            break;

        case 0x07:
            lcd_set_y(lcd, params[0] & 0xFF);
            break;

        case 0x08:
            lcd_set_window_x1(lcd, params[0] & 0xFF);
            break;

        case 0x09:
            lcd_set_window_x2(lcd, params[0] & 0xFF);
            break;

        case 0x0A:
            lcd_set_window_y1(lcd, params[0] & 0xFF);
            break;

        case 0x0B:
            lcd_set_window_y2(lcd, params[0] & 0xFF);
            break;

        default:
            // Nothing
            break;
    }
}

static void lr38826_realize(LCDState *lcd, Error **_errp) {
    LR38826State *priv = LR38826(lcd);
    memset(priv->regs, 0, sizeof(priv->regs));
    memcpy(priv->regs, DEFAULT_REGS, sizeof(DEFAULT_REGS));
    lr38826_update_state(lcd);
}

static void lr38826_class_init(ObjectClass *oc, const void *_data) {
    LCDClass *k = LCD_CLASS(oc);
    k->cmd_width = 1;
    k->param_width = 1;
    k->cd_polarity = true;
    k->direct_data_write = true; // CD=0 - write data, CD=1 - write CMD+ARG
    k->on_cmd = lr38826_on_cmd;
    k->on_cmd_with_params = lr38826_on_cmd_with_params;
    k->realize = lr38826_realize;
}

static const TypeInfo lr38826_info = {
    .name           = TYPE_LR38826,
    .parent         = TYPE_LCD,
    .instance_size  = sizeof(LR38826State),
    .class_init     = lr38826_class_init,
};

static void lr38826_register_types(void) {
    type_register_static(&lr38826_info);
}
type_init(lr38826_register_types)
