/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Epson L2F50333T (OLED, 132x176) */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/lcd/lcd.h"

#define TYPE_L2F50333T "l2f50333t"
OBJECT_DECLARE_SIMPLE_TYPE(L2F50333TState, L2F50333T)

#define L2F50333T_MAX_BPP       18
#define L2F50333T_MAX_REGS      256

static const uint16_t DEFAULT_REGS[][128] = {
    [0x36] = { 0x00 }, // MADCTL
    [0x3A] = { 0x05 }, // COLMOD
};

struct L2F50333TState {
    LCDState parent;
    uint16_t regs[L2F50333T_MAX_REGS][128];
};

static void l2f50333t_update_state(LCDState *lcd) {
    L2F50333TState *priv = L2F50333T(lcd);

    bool invert_xy = (priv->regs[0x36][0] & (1 << 0)) != 0;
    bool bgr = (priv->regs[0x36][0] & (1 << 3)) != 0;
    bool mv = (priv->regs[0x36][0] & (1 << 5)) != 0;
    bool mx = (priv->regs[0x36][0] & (1 << 6)) != 0; /* 1 - decrement, 0 - increment */
    bool my = (priv->regs[0x36][0] & (1 << 7)) != 0; /* 1 - decrement, 0 - increment */

    uint8_t colmod = (priv->regs[0x3A][0] & 7);

    LCDPixelFormat pixel_format;
    switch (colmod) {
        case 0x05:
            pixel_format = LCD_PIXEL_FORMAT_RGB565;
            break;
        case 0x06:
            pixel_format = LCD_PIXEL_FORMAT_RGB666_6_6_6;
            break;
        default:
            DEBUG_GUEST_ERROR(&lcd->dbg, "invalid color mode: 0x%02X, fallback to 565\n", colmod);
            pixel_format = LCD_PIXEL_FORMAT_RGB565;
            break;
    }

    lcd_set_addr_mode(
        lcd,
        (mv ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL),
        (mx ? LCD_AC_DEC : LCD_AC_INC),
        (my ? LCD_AC_DEC : LCD_AC_INC)
    );

    DEBUG_LOG(&lcd->dbg, "mv=%d, mx=%d, my=%d, invert_xy=%d, bgr=%d\n", mv, mx, my, invert_xy, bgr);
    lcd_set_pixel_format(lcd, pixel_format);
    lcd_set_output_bgr(lcd, bgr);
    lcd_set_transform(lcd, invert_xy, invert_xy);
}

static int l2f50333t_on_cmd(LCDState *lcd, uint32_t cmd) {
    if (cmd == 0x2C) {
        lcd_set_ram_mode(lcd, true);
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

static void l2f50333t_on_cmd_with_params(LCDState *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
    L2F50333TState *priv = L2F50333T(lcd);

    g_assert(cmd < L2F50333T_MAX_REGS);

    DEBUG_LOG(&lcd->dbg, "write reg 0x%02X:\n", cmd);
    for (int i = 0; i < params_n; i++) {
        priv->regs[cmd][i] = params[i];
        DEBUG_LOG(&lcd->dbg, "  param[%d]: 0x%02X\n", i, params[i]);
    }

    switch (cmd) {
        case 0x36:
        case 0x3A:
            l2f50333t_update_state(lcd);
            break;

        case 0x2A:
            lcd_set_x(lcd, (params[0] << 8) | params[1]);
            lcd_set_window_x1(lcd, (params[0] << 8) | params[1]);
            lcd_set_window_x2(lcd, (params[2] << 8) | params[3]);
            break;

        case 0x2B:
            lcd_set_y(lcd, (params[0] << 8) | params[1]);
            lcd_set_window_y1(lcd, (params[0] << 8) | params[1]);
            lcd_set_window_y2(lcd, (params[2] << 8) | params[3]);
            break;
    }
}

static void l2f50333t_realize(LCDState *lcd, Error **_errp) {
    L2F50333TState *priv = L2F50333T(lcd);
    memset(priv->regs, 0, sizeof(priv->regs));
    memcpy(priv->regs, DEFAULT_REGS, sizeof(DEFAULT_REGS));
    l2f50333t_update_state(lcd);
}

static void l2f50333t_class_init(ObjectClass *oc, const void *_data) {
    LCDClass *k = LCD_CLASS(oc);
    k->cmd_width = 1;
    k->param_width = 1;
    k->on_cmd = l2f50333t_on_cmd;
    k->on_cmd_with_params = l2f50333t_on_cmd_with_params;
    k->realize = l2f50333t_realize;
}

static const TypeInfo l2f50333t_info = {
    .name           = TYPE_L2F50333T,
    .parent         = TYPE_LCD,
    .instance_size  = sizeof(L2F50333TState),
    .class_init     = l2f50333t_class_init,
};

static void l2f50333t_register_types(void) {
    type_register_static(&l2f50333t_info);
}
type_init(l2f50333t_register_types)
