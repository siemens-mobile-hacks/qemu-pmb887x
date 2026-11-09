/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Philips PCF8882 (TFT, 132x176) */
#include "qemu/osdep.h"
#include "qapi/error.h"

#include "hw/lcd/lcd.h"
#include "regs.h"

#define TYPE_PCF8882 "pcf8882"
OBJECT_DECLARE_SIMPLE_TYPE(PCF8882State, PCF8882)

#define PCF8882_REGISTER_COUNT 0x100
#define PCF8882_MAX_PARAMS 4

static const LCDGeometry PCF8882_GEOMETRY = {
    .width = 132,
    .height = 176,
};

struct PCF8882State {
    LCDState parent;
    uint8_t registers[PCF8882_REGISTER_COUNT][PCF8882_MAX_PARAMS];
};

static void pcf8882_update_state(LCDState *lcd) {
    PCF8882State *p = PCF8882(lcd);
    uint8_t madctl = p->registers[PCF8882_MADCTL][0];
    uint8_t colmod = p->registers[PCF8882_COLMOD][0] & PCF8882_COLMOD_P;
    LCDPixelFormat pixel_format;

    switch (colmod) {
        case PCF8882_COLMOD_P_PIXEL_16_BIT:
            pixel_format = LCD_PIXEL_FORMAT_RGB565;
            break;
        case PCF8882_COLMOD_P_PIXEL_18_BIT:
            pixel_format = LCD_PIXEL_FORMAT_RGB666_6_6_6;
            break;
        default:
            DEBUG_GUEST_ERROR(&lcd->dbg, "invalid color mode: %02X, using RGB565\n", colmod);
            pixel_format = LCD_PIXEL_FORMAT_RGB565;
            break;
    }

    lcd_set_addr_mode(
        lcd,
        madctl & PCF8882_MADCTL_V ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL,
        madctl & PCF8882_MADCTL_MX ? LCD_AC_DEC : LCD_AC_INC,
        madctl & PCF8882_MADCTL_MY ? LCD_AC_DEC : LCD_AC_INC
    );
    lcd_set_pixel_format(lcd, pixel_format);
    lcd_set_output_bgr(lcd, (madctl & PCF8882_MADCTL_RGB) != 0);
    lcd_set_transform(lcd, false, false);
}

static int pcf8882_on_cmd(LCDState *lcd, uint32_t command) {
    int params_n;
    switch (command) {
        case PCF8882_RAMWR:
            lcd_set_ram_mode(lcd, true);
            params_n = 0;
            break;

        case PCF8882_GAMSET:
        case PCF8882_MADCTL:
        case PCF8882_COLMOD:
            params_n = 1;
            break;

        case PCF8882_CASET:
        case PCF8882_PASET:
        case PCF8882_PTLAR:
            params_n = 2;
            break;

        case PCF8882_VSCRDEF:
            params_n = 3;
            break;

        case PCF8882_SEP:
            params_n = 1;
            break;

        case PCF8882_SLPOUT:
        case PCF8882_DISPON:
            params_n = 0;
            break;

        default:
            params_n = 0;
            break;
    }

    if (params_n == 0 && command != PCF8882_RAMWR)
        DEBUG_IO_WRITE(&lcd->dbg, command, 1, 0);
    return params_n;
}

static void pcf8882_on_cmd_with_params(LCDState *lcd, uint32_t command, const uint32_t *params, uint32_t params_n) {
    PCF8882State *p = PCF8882(lcd);
    uint32_t value = 0;

    g_assert(command < PCF8882_REGISTER_COUNT);
    g_assert(params_n <= PCF8882_MAX_PARAMS);

    for (uint32_t i = 0; i < params_n; i++)
        value = value << 8 | params[i];
    DEBUG_IO_WRITE(&lcd->dbg, command, params_n, value);

    for (uint32_t i = 0; i < params_n; i++)
        p->registers[command][i] = params[i];

    switch (command) {
        case PCF8882_MADCTL:
        case PCF8882_COLMOD:
            pcf8882_update_state(lcd);
            break;
        case PCF8882_CASET:
            lcd_set_x(lcd, params[0]);
            lcd_set_window_x1(lcd, params[0]);
            lcd_set_window_x2(lcd, params[1]);
            break;
        case PCF8882_PASET:
            lcd_set_y(lcd, params[0]);
            lcd_set_window_y1(lcd, params[0]);
            lcd_set_window_y2(lcd, params[1]);
            break;
    }
}

static void pcf8882_reset(LCDState *lcd) {
    PCF8882State *p = PCF8882(lcd);

    memset(p->registers, 0, sizeof(p->registers));
    p->registers[PCF8882_COLMOD][0] = PCF8882_COLMOD_P_PIXEL_18_BIT;
    pcf8882_update_state(lcd);
    lcd_set_x(lcd, 0);
    lcd_set_y(lcd, 0);
    lcd_set_window_x1(lcd, 0);
    lcd_set_window_x2(lcd, lcd->gram_width - 1);
    lcd_set_window_y1(lcd, 0);
    lcd_set_window_y2(lcd, lcd->gram_height - 1);
}

static void pcf8882_realize(LCDState *lcd, Error **_errp) {
    pcf8882_reset(lcd);
}

static void pcf8882_class_init(ObjectClass *oc, const void *_data) {
    LCDClass *k = LCD_CLASS(oc);
    k->geometry = &PCF8882_GEOMETRY;
    k->debug_regs = &pcf8882_debug_regs;
    k->cmd_width = 1;
    k->param_width = 1;
    k->on_cmd = pcf8882_on_cmd;
    k->on_cmd_with_params = pcf8882_on_cmd_with_params;
    k->reset = pcf8882_reset;
    k->realize = pcf8882_realize;
}

static const TypeInfo pcf8882_info = {
    .name = TYPE_PCF8882,
    .parent = TYPE_LCD,
    .instance_size = sizeof(PCF8882State),
    .class_init = pcf8882_class_init,
};

static void pcf8882_register_types(void) {
    type_register_static(&pcf8882_info);
}
type_init(pcf8882_register_types)
