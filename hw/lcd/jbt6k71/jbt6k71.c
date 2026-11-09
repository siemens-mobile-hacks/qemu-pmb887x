/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Toshiba JBT6K71 */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/lcd/lcd.h"
#include "regs.h"

#define TYPE_JBT6K71 "jbt6k71"
OBJECT_DECLARE_SIMPLE_TYPE(JBT6K71State, JBT6K71)

#define JBT6K71_MAX_BPP     18
#define JBT6K71_BUS_WIDTH   2
#define JBT6K71_MAX_REGS    0x800
#define JBT6K71_DEVICE_CODE 0x7114

/* Indexed by [TRI][DFM1][DFM0]; unspecified combinations are invalid. */
static const LCDPixelFormat JBT6K71_PIXEL_FORMATS[2][2][2] = {
    [0][0][0] = LCD_PIXEL_FORMAT_RGB565,
    [1][0][1] = LCD_PIXEL_FORMAT_RGB666_8_8_2,
    [1][1][0] = LCD_PIXEL_FORMAT_RGB666_2_8_8,
    [1][1][1] = LCD_PIXEL_FORMAT_RGB666_6_6_6,
};

static const uint16_t DEFAULT_REGS[] = {
    [JBT6K71_DRIVER_OUTPUT_CONTROL] = 0x27,
    [JBT6K71_ENTRY_MODE] = 0x30,
    [JBT6K71_DISPLAY_CONTROL] = 0x800,
};

struct JBT6K71State {
    LCDState parent;
    uint16_t regs[JBT6K71_MAX_REGS];
};

static void jbt6k71_update_state(LCDState *lcd) {
    JBT6K71State *priv = JBT6K71(lcd);
    uint16_t entry_mode = priv->regs[JBT6K71_ENTRY_MODE];
    uint32_t id = (entry_mode & JBT6K71_ENTRY_MODE_ID) >> JBT6K71_ENTRY_MODE_ID_SHIFT;
    uint32_t dfm = (entry_mode & JBT6K71_ENTRY_MODE_DFM) >> JBT6K71_ENTRY_MODE_DFM_SHIFT;
    bool ss = (priv->regs[JBT6K71_DRIVER_OUTPUT_CONTROL] & JBT6K71_DRIVER_OUTPUT_CONTROL_SS) != 0;
    bool am = (entry_mode & JBT6K71_ENTRY_MODE_AM) != 0;
    bool id0 = (id & BIT(0)) != 0;
    bool id1 = (id & BIT(1)) != 0;
    bool bgr = (entry_mode & JBT6K71_ENTRY_MODE_BGR) != 0;
    bool dfm0 = (dfm & BIT(0)) != 0;
    bool dfm1 = (dfm & BIT(1)) != 0;
    bool tri = (entry_mode & JBT6K71_ENTRY_MODE_TRI) != 0;
    bool ud = (priv->regs[JBT6K71_DISPLAY_CONTROL] & JBT6K71_DISPLAY_CONTROL_UD) != 0;

    DEBUG_LOG(&lcd->dbg, "am=%d, id1=%d, id0=%d, ss=%d, ud=%d\n", am, id1, id0, ss, ud);

    /* Validate before touching anything: a partially applied state (new address mode
     * with the old pixel format and transform) sticks until the next write to one of
     * these registers. */
    LCDPixelFormat pixel_format = JBT6K71_PIXEL_FORMATS[tri][dfm1][dfm0];
    if (pixel_format == LCD_PIXEL_FORMAT_NONE) {
        DEBUG_GUEST_ERROR(&lcd->dbg, "invalid config: dfm0=%d, dfm1=%d, tri=%d\n", dfm0, dfm1, tri);
        return;
    }

    lcd_set_addr_mode(
        lcd,
        (am ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL),
        (id0 ? LCD_AC_INC : LCD_AC_DEC),
        (id1 ? LCD_AC_INC : LCD_AC_DEC)
    );

    lcd_set_pixel_format(lcd, pixel_format);
    lcd_set_output_bgr(lcd, bgr);
    lcd_set_transform(lcd, ss, !ud);
}

static int jbt6k71_on_cmd(LCDState *lcd, uint32_t cmd) {
    if (cmd == JBT6K71_GRAM_DATA) {
        lcd_set_ram_mode(lcd, true);
        return 0;
    }
    return 1;
}

static void jbt6k71_on_cmd_with_params(LCDState *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
    JBT6K71State *priv = JBT6K71(lcd);

    g_assert(params_n == 1);

    /* cmd comes straight off the bus, so an out of range index is a firmware or bus
     * desync, not an emulator invariant. Drop it like jbt6k71_on_read() does. */
    if (cmd >= JBT6K71_MAX_REGS) {
        DEBUG_GUEST_ERROR(&lcd->dbg, "write to unknown reg %04X = %04X\n", cmd, params[0]);
        return;
    }

    DEBUG_IO_WRITE(&lcd->dbg, cmd, 2, params[0]);
    priv->regs[cmd] = params[0];

    switch (cmd) {
        case JBT6K71_DRIVER_OUTPUT_CONTROL:
        case JBT6K71_ENTRY_MODE:
        case JBT6K71_DISPLAY_CONTROL:
            jbt6k71_update_state(lcd);
            break;

        case JBT6K71_RAM_ADDRESS_LOW:
            lcd_set_x(lcd, params[0]);
            break;

        case JBT6K71_RAM_ADDRESS_HIGH:
            lcd_set_y(lcd, params[0]);
            break;

        case JBT6K71_HORIZONTAL_RAM_START:
            lcd_set_window_x1(lcd, params[0]);
            break;

        case JBT6K71_HORIZONTAL_RAM_END:
            lcd_set_window_x2(lcd, params[0]);
            break;

        case JBT6K71_VERTICAL_RAM_START:
            lcd_set_window_y1(lcd, params[0]);
            break;

        case JBT6K71_VERTICAL_RAM_END:
            lcd_set_window_y2(lcd, params[0]);
            break;

        default:
            // Nothing
            break;
    }
}

static uint32_t jbt6k71_on_read(LCDState *lcd, uint32_t index) {
    JBT6K71State *priv = JBT6K71(lcd);
    uint16_t value = 0;
    uint32_t shift = (1 - index % JBT6K71_BUS_WIDTH) * 8;

    if (lcd->current_cmd == JBT6K71_OSCILLATION) {
        value = JBT6K71_DEVICE_CODE;
    } else if (lcd->current_cmd < JBT6K71_MAX_REGS) {
        value = priv->regs[lcd->current_cmd];
    }
    if (index % JBT6K71_BUS_WIDTH == 0)
        DEBUG_IO_READ(&lcd->dbg, lcd->current_cmd, 2, value);
    return (value >> shift) & 0xFF;
}

static void jbt6k71_reset(LCDState *lcd) {
    JBT6K71State *priv = JBT6K71(lcd);
    memset(priv->regs, 0, sizeof(priv->regs));
    memcpy(priv->regs, DEFAULT_REGS, sizeof(DEFAULT_REGS));
    priv->regs[JBT6K71_HORIZONTAL_RAM_END] = lcd->gram_width - 1;
    priv->regs[JBT6K71_VERTICAL_RAM_END] = lcd->gram_height - 1;

    jbt6k71_update_state(lcd);
    lcd_set_x(lcd, priv->regs[JBT6K71_RAM_ADDRESS_LOW]);
    lcd_set_y(lcd, priv->regs[JBT6K71_RAM_ADDRESS_HIGH]);
    lcd_set_window_x1(lcd, priv->regs[JBT6K71_HORIZONTAL_RAM_START]);
    lcd_set_window_x2(lcd, priv->regs[JBT6K71_HORIZONTAL_RAM_END]);
    lcd_set_window_y1(lcd, priv->regs[JBT6K71_VERTICAL_RAM_START]);
    lcd_set_window_y2(lcd, priv->regs[JBT6K71_VERTICAL_RAM_END]);
}

static void jbt6k71_realize(LCDState *lcd, Error **_errp) {
    jbt6k71_reset(lcd);
}

static void jbt6k71_class_init(ObjectClass *oc, const void *_data) {
    LCDClass *k = LCD_CLASS(oc);
    k->debug_regs = &jbt6k71_debug_regs;
    k->cmd_width = 2;
    k->param_width = 2;
    k->gram_read_dummy_pixels = 1;
    k->on_cmd = jbt6k71_on_cmd;
    k->on_cmd_with_params = jbt6k71_on_cmd_with_params;
    k->on_read = jbt6k71_on_read;
    k->reset = jbt6k71_reset;
    k->realize = jbt6k71_realize;
}

static const TypeInfo jbt6k71_info = {
    .name           = TYPE_JBT6K71,
    .parent         = TYPE_LCD,
    .instance_size  = sizeof(JBT6K71State),
    .class_init     = jbt6k71_class_init,
};

static void jbt6k71_register_types(void) {
    type_register_static(&jbt6k71_info);
}
type_init(jbt6k71_register_types)
