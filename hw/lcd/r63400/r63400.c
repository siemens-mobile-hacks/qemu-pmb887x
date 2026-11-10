/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Renesas R63400 */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/lcd/lcd.h"

#define TYPE_R63400 "r63400"
OBJECT_DECLARE_SIMPLE_TYPE(R63400State, R63400)

#define R63400_MAX_REGS     0x500

// R63400 registers
#define R63400_R_DRIVER_OUTPUT      0x01    // SS (bit8), SM (bit10)
#define R63400_R_ENTRY_MODE         0x03    // AM/ID/BGR/DFM/TRI
#define R63400_R_DISPLAY_CTRL1      0x07    // D0/D1 (display on)
#define R63400_R_GATE_SCAN          0x400   // GS (bit15) gate scan direction
#define R63400_R_GRAM_X             0x200   // GRAM horizontal address
#define R63400_R_GRAM_Y             0x201   // GRAM vertical address
#define R63400_R_GRAM_WRITE         0x202   // write data to GRAM
#define R63400_R_WINDOW_HSTART      0x210   // window horizontal start
#define R63400_R_WINDOW_HEND        0x211   // window horizontal end
#define R63400_R_WINDOW_VSTART      0x212   // window vertical start
#define R63400_R_WINDOW_VEND        0x213   // window vertical end

static const uint16_t DEFAULT_REGS[] = {
    [R63400_R_DRIVER_OUTPUT]    = 0x0100,   /* SS = 1 */
    [R63400_R_ENTRY_MODE]       = 0x0030,   /* AM=0, ID=11 */
};

struct R63400State {
    LCDState parent;
    uint16_t regs[R63400_MAX_REGS];
};

static void r63400_update_state(LCDState *lcd) {
    R63400State *priv = R63400(lcd);

    bool ss = (priv->regs[R63400_R_DRIVER_OUTPUT] & (1 << 8)) != 0; /* SS */
    bool am = (priv->regs[R63400_R_ENTRY_MODE] & (1 << 3)) != 0;    /* AM */
    bool id0 = (priv->regs[R63400_R_ENTRY_MODE] & (1 << 4)) != 0;   /* ID0 */
    bool id1 = (priv->regs[R63400_R_ENTRY_MODE] & (1 << 5)) != 0;   /* ID1 */
    bool bgr = (priv->regs[R63400_R_ENTRY_MODE] & (1 << 12)) != 0;  /* BGR */
    bool dfm = (priv->regs[R63400_R_ENTRY_MODE] & (1 << 14)) != 0;  /* DFM */
    bool tri = (priv->regs[R63400_R_ENTRY_MODE] & (1 << 15)) != 0;  /* TRI */
    bool gs = (priv->regs[R63400_R_GATE_SCAN] & (1 << 15)) != 0;    /* GS */

    DEBUG_LOG(&lcd->dbg, "am=%d, id1=%d, id0=%d, ss=%d, gs=%d, bgr=%d, tri=%d, dfm=%d\n",
        am, id1, id0, ss, gs, bgr, tri, dfm);

    lcd_set_addr_mode(
        lcd,
        (am ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL),
        (id0 ? LCD_AC_INC : LCD_AC_DEC),
        (id1 ? LCD_AC_INC : LCD_AC_DEC)
    );

    LCDPixelFormat pixel_format = tri && dfm ?
        LCD_PIXEL_FORMAT_RGB666_6_6_6 : LCD_PIXEL_FORMAT_RGB565;
    lcd_set_pixel_format(lcd, pixel_format);
    lcd_set_output_bgr(lcd, bgr);
    lcd_set_transform(lcd, false, false);
}

static int r63400_on_cmd(LCDState *lcd, uint32_t cmd) {
    if (cmd == R63400_R_GRAM_WRITE) {
        lcd_set_ram_mode(lcd, true);
        return 0;
    }
    return 1;
}

static void r63400_on_cmd_with_params(LCDState *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
    R63400State *priv = R63400(lcd);

    g_assert(params_n == 1);

    uint16_t value = params[0];

    if (cmd >= R63400_MAX_REGS) {
        DEBUG_LOG(&lcd->dbg, "ignoring out-of-range reg R%X\n", cmd);
        return;
    }
    priv->regs[cmd] = value;

    DEBUG_LOG(&lcd->dbg, "write R%02X -> 0x%04X\n", cmd, value);

    switch (cmd) {
        case R63400_R_DRIVER_OUTPUT:
        case R63400_R_ENTRY_MODE:
        case R63400_R_DISPLAY_CTRL1:
        case R63400_R_GATE_SCAN:
            r63400_update_state(lcd);
            break;

        case R63400_R_GRAM_X:
            lcd_set_x(lcd, value);
            break;

        case R63400_R_GRAM_Y:
            lcd_set_y(lcd, value);
            break;

        case R63400_R_WINDOW_HSTART:
            lcd_set_window_x1(lcd, value);
            break;

        case R63400_R_WINDOW_HEND:
            lcd_set_window_x2(lcd, value);
            break;

        case R63400_R_WINDOW_VSTART:
            lcd_set_window_y1(lcd, value);
            break;

        case R63400_R_WINDOW_VEND:
            lcd_set_window_y2(lcd, value);
            break;

        default:
            // Nothing
            break;
    }
}

static void r63400_reset(LCDState *lcd) {
    R63400State *priv = R63400(lcd);
    memset(priv->regs, 0, sizeof(priv->regs));
    memcpy(priv->regs, DEFAULT_REGS, sizeof(DEFAULT_REGS));
    priv->regs[R63400_R_WINDOW_HEND] = lcd->gram_width - 1;
    priv->regs[R63400_R_WINDOW_VEND] = lcd->gram_height - 1;

    r63400_update_state(lcd);
    lcd_set_x(lcd, priv->regs[R63400_R_GRAM_X]);
    lcd_set_y(lcd, priv->regs[R63400_R_GRAM_Y]);
    lcd_set_window_x1(lcd, priv->regs[R63400_R_WINDOW_HSTART]);
    lcd_set_window_x2(lcd, priv->regs[R63400_R_WINDOW_HEND]);
    lcd_set_window_y1(lcd, priv->regs[R63400_R_WINDOW_VSTART]);
    lcd_set_window_y2(lcd, priv->regs[R63400_R_WINDOW_VEND]);
}

static void r63400_realize(LCDState *lcd, Error **_errp) {
    R63400State *priv = R63400(lcd);
    memset(priv->regs, 0, sizeof(priv->regs));
    memcpy(priv->regs, DEFAULT_REGS, sizeof(DEFAULT_REGS));
    r63400_update_state(lcd);
}

static void r63400_class_init(ObjectClass *oc, const void *_data) {
    LCDClass *k = LCD_CLASS(oc);
    k->cmd_width = 2;
    k->param_width = 2;
    k->on_cmd = r63400_on_cmd;
    k->on_cmd_with_params = r63400_on_cmd_with_params;
    k->reset = r63400_reset;
    k->realize = r63400_realize;
}

static const TypeInfo r63400_info = {
    .name           = TYPE_R63400,
    .parent         = TYPE_LCD,
    .instance_size  = sizeof(R63400State),
    .class_init     = r63400_class_init,
};

static void r63400_register_types(void) {
    type_register_static(&r63400_info);
}
type_init(r63400_register_types)
