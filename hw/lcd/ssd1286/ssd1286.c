/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Solomon SSD1286 */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/lcd/lcd.h"
#include "regs.h"

#define TYPE_SSD1286 "ssd1286"
OBJECT_DECLARE_SIMPLE_TYPE(SSD1286State, SSD1286)

#define SSD1286_MAX_BPP     18
#define SSD1286_MAX_REGS    0x100

static const LCDGeometry SSD1286_GEOMETRY = {
    .width = 132,
    .height = 176,
};

static const uint16_t DEFAULT_REGS[] = {
    [SSD1286_DRIVER_OUTPUT_CONTROL] = 0x0001,
    [SSD1286_ENTRY_MODE] = 0x6830,
};

struct SSD1286State {
    LCDState parent;
    uint16_t regs[SSD1286_MAX_REGS];
};

static void ssd1286_update_state(LCDState *lcd) {
    SSD1286State *priv = SSD1286(lcd);
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

    DEBUG_LOG(&lcd->dbg, "am=%d, id1=%d, id0=%d, ss=%d, sm=%d, dfm0=%d, dfm1=%d, bgr=%d\n",
        am, id1, id0, ss, sm, dfm0, dfm1, bgr);

    lcd_set_addr_mode(
        lcd,
        (am ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL),
        (id0 ? LCD_AC_INC : LCD_AC_DEC),
        (id1 ? LCD_AC_INC : LCD_AC_DEC)
    );

    LCDPixelFormat pixel_format = dfm0 == 0 && dfm1 == 1 ?
        LCD_PIXEL_FORMAT_RGB666_6_6_6 : LCD_PIXEL_FORMAT_RGB565;
    lcd_set_pixel_format(lcd, pixel_format);
    lcd_set_output_bgr(lcd, bgr);
    lcd_set_transform(lcd, false, false);
}

static int ssd1286_on_cmd(LCDState *lcd, uint32_t cmd) {
    if (cmd == SSD1286_GRAM_DATA) {
        lcd_set_ram_mode(lcd, true);
        return 0;
    }
    return 1;
}

static void ssd1286_on_cmd_with_params(LCDState *lcd, uint32_t cmd, const uint32_t *params, uint32_t params_n) {
    SSD1286State *priv = SSD1286(lcd);

    g_assert(params_n == 1);
    g_assert(cmd < SSD1286_MAX_REGS);

    DEBUG_IO_WRITE(&lcd->dbg, cmd, 2, params[0]);
    priv->regs[cmd] = params[0];

    switch (cmd) {
        case SSD1286_DRIVER_OUTPUT_CONTROL:
        case SSD1286_ENTRY_MODE:
            ssd1286_update_state(lcd);
            break;

        case SSD1286_RAM_ADDRESS:
            lcd_set_x(lcd, params[0] & 0xFF);
            lcd_set_y(lcd, params[0] >> 8);
            break;

        case SSD1286_HORIZONTAL_RAM_ADDRESS:
            lcd_set_window_x1(lcd, (params[0] & SSD1286_HORIZONTAL_RAM_ADDRESS_HSA) >>
                SSD1286_HORIZONTAL_RAM_ADDRESS_HSA_SHIFT);
            lcd_set_window_x2(lcd, (params[0] & SSD1286_HORIZONTAL_RAM_ADDRESS_HEA) >>
                SSD1286_HORIZONTAL_RAM_ADDRESS_HEA_SHIFT);
            break;

        case SSD1286_VERTICAL_RAM_ADDRESS:
            lcd_set_window_y1(lcd, (params[0] & SSD1286_VERTICAL_RAM_ADDRESS_VSA) >>
                SSD1286_VERTICAL_RAM_ADDRESS_VSA_SHIFT);
            lcd_set_window_y2(lcd, (params[0] & SSD1286_VERTICAL_RAM_ADDRESS_VEA) >>
                SSD1286_VERTICAL_RAM_ADDRESS_VEA_SHIFT);
            break;

        default:
            // Nothing
            break;
    }
}

static void ssd1286_realize(LCDState *lcd, Error **_errp) {
    SSD1286State *priv = SSD1286(lcd);
    memset(priv->regs, 0, sizeof(priv->regs));
    memcpy(priv->regs, DEFAULT_REGS, sizeof(DEFAULT_REGS));
    ssd1286_update_state(lcd);
}

static void ssd1286_class_init(ObjectClass *oc, const void *_data) {
    LCDClass *k = LCD_CLASS(oc);
    k->geometry = &SSD1286_GEOMETRY;
    k->debug_regs = &ssd1286_debug_regs;
    k->cmd_width = 1;
    k->param_width = 2;
    k->on_cmd = ssd1286_on_cmd;
    k->on_cmd_with_params = ssd1286_on_cmd_with_params;
    k->realize = ssd1286_realize;
}

static const TypeInfo ssd1286_info = {
    .name           = TYPE_SSD1286,
    .parent         = TYPE_LCD,
    .instance_size  = sizeof(SSD1286State),
    .class_init     = ssd1286_class_init,
};

static void ssd1286_register_types(void) {
    type_register_static(&ssd1286_info);
}
type_init(ssd1286_register_types)
