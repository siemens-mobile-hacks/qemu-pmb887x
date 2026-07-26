/*
 * Philips PCF8882 (TFT, 132x176)
 */
#define PMB887X_TRACE_ID LCD
#define PMB887X_TRACE_PREFIX "pcf8882"

#include "qemu/osdep.h"
#include "qapi/error.h"

#include "hw/arm/pmb887x/ssc/lcd_common.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_LCD_PCF8882 "pcf8882"
#define PMB887X_LCD_PCF8882(obj) OBJECT_CHECK(pmb887x_lcd_pcf8882_t, (obj), TYPE_PMB887X_LCD_PCF8882)

#define PCF8882_CMD_SLPOUT 0x11
#define PCF8882_CMD_DISPON 0x29
#define PCF8882_CMD_CASET 0x2A
#define PCF8882_CMD_PASET 0x2B
#define PCF8882_CMD_RAMWR 0x2C
#define PCF8882_CMD_MADCTL 0x36
#define PCF8882_CMD_COLMOD 0x3A
#define PCF8882_CMD_GAMSET 0x26

#define PCF8882_MADCTL_REVERSE_Y BIT(7)
#define PCF8882_MADCTL_REVERSE_X BIT(6)
#define PCF8882_MADCTL_SWAP_AXES BIT(5)
#define PCF8882_MADCTL_BGR BIT(3)

#define PCF8882_COLMOD_RGB565 0x05
#define PCF8882_COLMOD_RGB666 0x07
#define PCF8882_REGISTER_COUNT 0x100
#define PCF8882_MAX_PARAMS 4

typedef struct pmb887x_lcd_pcf8882_t pmb887x_lcd_pcf8882_t;

struct pmb887x_lcd_pcf8882_t {
    pmb887x_lcd_t parent;
    uint8_t registers[PCF8882_REGISTER_COUNT][PCF8882_MAX_PARAMS];
};

static void pcf8882_update_state(pmb887x_lcd_t *lcd) {
    pmb887x_lcd_pcf8882_t *p = PMB887X_LCD_PCF8882(lcd);
    uint8_t madctl = p->registers[PCF8882_CMD_MADCTL][0];
    uint8_t colmod = p->registers[PCF8882_CMD_COLMOD][0] & 7;
    enum pmb887x_lcd_pixel_format_t pixel_format;

    switch (colmod) {
        case PCF8882_COLMOD_RGB565:
            pixel_format = LCD_PIXEL_FORMAT_RGB565;
            break;
        case PCF8882_COLMOD_RGB666:
            pixel_format = LCD_PIXEL_FORMAT_RGB666_6_6_6;
            break;
        default:
            EPRINTF("invalid color mode: %02X, using RGB565\n", colmod);
            pixel_format = LCD_PIXEL_FORMAT_RGB565;
            break;
    }

    pmb887x_lcd_set_addr_mode(
        lcd,
        madctl & PCF8882_MADCTL_SWAP_AXES ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL,
        madctl & PCF8882_MADCTL_REVERSE_X ? LCD_AC_DEC : LCD_AC_INC,
        madctl & PCF8882_MADCTL_REVERSE_Y ? LCD_AC_DEC : LCD_AC_INC
    );
    pmb887x_lcd_set_pixel_format(lcd, pixel_format);
    pmb887x_lcd_set_output_bgr(lcd, (madctl & PCF8882_MADCTL_BGR) != 0);
    pmb887x_lcd_set_transform(lcd, false, false);
}

static int pcf8882_on_cmd(pmb887x_lcd_t *lcd, uint32_t command) {
    switch (command) {
        case PCF8882_CMD_RAMWR:
            pmb887x_lcd_set_ram_mode(lcd, true);
            return 0;
        case PCF8882_CMD_GAMSET:
        case PCF8882_CMD_MADCTL:
        case PCF8882_CMD_COLMOD:
            return 1;
        case PCF8882_CMD_CASET:
        case PCF8882_CMD_PASET:
            return 2;
        case PCF8882_CMD_SLPOUT:
        case PCF8882_CMD_DISPON:
            return 0;
        default:
            return 0;
    }
}

static void pcf8882_on_cmd_with_params(pmb887x_lcd_t *lcd, uint32_t command, const uint32_t *params, uint32_t params_n) {
    pmb887x_lcd_pcf8882_t *p = PMB887X_LCD_PCF8882(lcd);

    g_assert(command < PCF8882_REGISTER_COUNT);
    g_assert(params_n <= PCF8882_MAX_PARAMS);

    if (command == PCF8882_CMD_COLMOD)
        DPRINTF("COLMOD: %02X\n", params[0]);
    for (uint32_t i = 0; i < params_n; i++)
        p->registers[command][i] = params[i];

    switch (command) {
        case PCF8882_CMD_MADCTL:
        case PCF8882_CMD_COLMOD:
            pcf8882_update_state(lcd);
            break;
        case PCF8882_CMD_CASET:
            pmb887x_lcd_set_x(lcd, params[0]);
            pmb887x_lcd_set_window_x1(lcd, params[0]);
            pmb887x_lcd_set_window_x2(lcd, params[1]);
            break;
        case PCF8882_CMD_PASET:
            pmb887x_lcd_set_y(lcd, params[0]);
            pmb887x_lcd_set_window_y1(lcd, params[0]);
            pmb887x_lcd_set_window_y2(lcd, params[1]);
            break;
    }
}

static void pcf8882_reset(pmb887x_lcd_t *lcd) {
    pmb887x_lcd_pcf8882_t *p = PMB887X_LCD_PCF8882(lcd);

    memset(p->registers, 0, sizeof(p->registers));
    p->registers[PCF8882_CMD_COLMOD][0] = PCF8882_COLMOD_RGB666;
    pcf8882_update_state(lcd);
    pmb887x_lcd_set_x(lcd, 0);
    pmb887x_lcd_set_y(lcd, 0);
    pmb887x_lcd_set_window_x1(lcd, 0);
    pmb887x_lcd_set_window_x2(lcd, lcd->width - 1);
    pmb887x_lcd_set_window_y1(lcd, 0);
    pmb887x_lcd_set_window_y2(lcd, lcd->height - 1);
}

static void pcf8882_realize(pmb887x_lcd_t *lcd, Error **errp) {
    pcf8882_reset(lcd);
}

static void pcf8882_class_init(ObjectClass *oc, const void *data) {
    pmb887x_lcd_class_t *k = PMB887X_LCD_CLASS(oc);
    k->cmd_width = 1;
    k->param_width = 1;
    k->on_cmd = pcf8882_on_cmd;
    k->on_cmd_with_params = pcf8882_on_cmd_with_params;
    k->reset = pcf8882_reset;
    k->realize = pcf8882_realize;
}

static const TypeInfo pcf8882_info = {
    .name = TYPE_PMB887X_LCD_PCF8882,
    .parent = TYPE_PMB887X_LCD,
    .instance_size = sizeof(pmb887x_lcd_pcf8882_t),
    .class_init = pcf8882_class_init,
};

static void pcf8882_register_types(void) {
    type_register_static(&pcf8882_info);
}
type_init(pcf8882_register_types)
