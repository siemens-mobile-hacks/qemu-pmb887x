/* SPDX-License-Identifier: GPL-2.0-or-later */
/* R61505/ILI9320-compatible panels */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/lcd/lcd.h"

#define TYPE_R61505 "r61505"
OBJECT_DECLARE_SIMPLE_TYPE(R61505State, R61505)

#define ILI9320_MAX_REGS    0x100

// ILI9320 registers
#define ILI9320_R_DRIVER_OUTPUT     0x01    // SS (bit8), SM (bit10)
#define ILI9320_R_ENTRY_MODE        0x03    // AM/ID/BGR/DFM/TRI
#define ILI9320_R_DISPLAY_CTRL1     0x07    // D0/D1 (display on), BASEE
#define ILI9320_R_GRAM_X            0x20    // GRAM horizontal address
#define ILI9320_R_GRAM_Y            0x21    // GRAM vertical address
#define ILI9320_R_GRAM_WRITE        0x22    // write data to GRAM
#define ILI9320_R_WINDOW_HSTART     0x50    // window horizontal start
#define ILI9320_R_WINDOW_HEND       0x51    // window horizontal end
#define ILI9320_R_WINDOW_VSTART     0x52    // window vertical start
#define ILI9320_R_WINDOW_VEND       0x53    // window vertical end
#define ILI9320_R_GATE_SCAN         0x60    // GS (bit15) gate scan direction

static const uint16_t DEFAULT_REGS[] = {
    [ILI9320_R_DRIVER_OUTPUT]   = 0x0100,   /* SS = 1 */
    [ILI9320_R_ENTRY_MODE]      = 0x1030,   /* BGR, AM=0, ID=11 */
};

struct R61505State {
    LCDState parent;
    uint16_t regs[ILI9320_MAX_REGS];
};

static void r61505_update_state(LCDState *lcd) {
    R61505State *priv = R61505(lcd);

    bool ss = (priv->regs[ILI9320_R_DRIVER_OUTPUT] & (1 << 8)) != 0;    /* SS */
    bool am = (priv->regs[ILI9320_R_ENTRY_MODE] & (1 << 3)) != 0;   /* AM */
    bool id0 = (priv->regs[ILI9320_R_ENTRY_MODE] & (1 << 4)) != 0;  /* ID0 */
    bool id1 = (priv->regs[ILI9320_R_ENTRY_MODE] & (1 << 5)) != 0;  /* ID1 */
    bool bgr = (priv->regs[ILI9320_R_ENTRY_MODE] & (1 << 12)) != 0; /* BGR */
    bool dfm = (priv->regs[ILI9320_R_ENTRY_MODE] & (1 << 14)) != 0; /* DFM */
    bool tri = (priv->regs[ILI9320_R_ENTRY_MODE] & (1 << 15)) != 0; /* TRI */
    bool gs = (priv->regs[ILI9320_R_GATE_SCAN] & (1 << 15)) != 0;   /* GS */

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

static int r61505_on_cmd(LCDState *lcd, uint32_t cmd) {
    DEBUG_LOG(&lcd->dbg, "write R%02X\n", cmd);

    if (cmd == ILI9320_R_GRAM_WRITE) {
        lcd_set_ram_mode(lcd, true);
        return 0;
    }
    return 1;
}

static void r61505_on_cmd_with_params(LCDState *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
    R61505State *priv = R61505(lcd);

    g_assert(params_n == 1);

    uint16_t value = params[0];

    if (cmd >= ILI9320_MAX_REGS) {
        DEBUG_LOG(&lcd->dbg, "ignoring out-of-range reg R%X\n", cmd);
        return;
    }
    priv->regs[cmd] = value;

    DEBUG_LOG(&lcd->dbg, "write R%02X -> 0x%04X\n", cmd, value);

    switch (cmd) {
        case ILI9320_R_DRIVER_OUTPUT:
        case ILI9320_R_ENTRY_MODE:
        case ILI9320_R_DISPLAY_CTRL1:
        case ILI9320_R_GATE_SCAN:
            r61505_update_state(lcd);
            break;

        case ILI9320_R_GRAM_X:
            lcd_set_x(lcd, value);
            break;

        case ILI9320_R_GRAM_Y:
            lcd_set_y(lcd, value);
            break;

        case ILI9320_R_WINDOW_HSTART:
            lcd_set_window_x1(lcd, value);
            break;

        case ILI9320_R_WINDOW_HEND:
            lcd_set_window_x2(lcd, value);
            break;

        case ILI9320_R_WINDOW_VSTART:
            lcd_set_window_y1(lcd, value);
            break;

        case ILI9320_R_WINDOW_VEND:
            lcd_set_window_y2(lcd, value);
            break;

        default:
            // Nothing
            break;
    }
}

static void r61505_reset(LCDState *lcd) {
    R61505State *priv = R61505(lcd);
    memset(priv->regs, 0, sizeof(priv->regs));
    memcpy(priv->regs, DEFAULT_REGS, sizeof(DEFAULT_REGS));
    priv->regs[ILI9320_R_WINDOW_HEND] = lcd->gram_width - 1;
    priv->regs[ILI9320_R_WINDOW_VEND] = lcd->gram_height - 1;

    r61505_update_state(lcd);
    lcd_set_x(lcd, priv->regs[ILI9320_R_GRAM_X]);
    lcd_set_y(lcd, priv->regs[ILI9320_R_GRAM_Y]);
    lcd_set_window_x1(lcd, priv->regs[ILI9320_R_WINDOW_HSTART]);
    lcd_set_window_x2(lcd, priv->regs[ILI9320_R_WINDOW_HEND]);
    lcd_set_window_y1(lcd, priv->regs[ILI9320_R_WINDOW_VSTART]);
    lcd_set_window_y2(lcd, priv->regs[ILI9320_R_WINDOW_VEND]);
}

static void r61505_realize(LCDState *lcd, Error **_errp) {
    R61505State *priv = R61505(lcd);
    memset(priv->regs, 0, sizeof(priv->regs));
    memcpy(priv->regs, DEFAULT_REGS, sizeof(DEFAULT_REGS));
    r61505_update_state(lcd);
}

static void r61505_class_init(ObjectClass *oc, const void *_data) {
    LCDClass *k = LCD_CLASS(oc);
    k->cmd_width = 2;
    k->param_width = 2;
    k->on_cmd = r61505_on_cmd;
    k->on_cmd_with_params = r61505_on_cmd_with_params;
    k->reset = r61505_reset;
    k->realize = r61505_realize;
}

static const TypeInfo r61505_info = {
    .name           = TYPE_R61505,
    .parent         = TYPE_LCD,
    .instance_size  = sizeof(R61505State),
    .class_init     = r61505_class_init,
};

static void r61505_register_types(void) {
    type_register_static(&r61505_info);
}
type_init(r61505_register_types)
