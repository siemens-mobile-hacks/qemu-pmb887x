/*
 * Philips PCF8882 (TFT, 132x176)
 */
#define PMB887X_TRACE_ID LCD
#define PMB887X_TRACE_PREFIX "pcf8882"
#define PMB887X_TRACE_IO PMB887X_TRACE_IO_PCF8882

#include "qemu/osdep.h"
#include "qapi/error.h"

#include "hw/arm/pmb887x/gen/peripheral/PCF8882.h"
#include "hw/arm/pmb887x/ssc/lcd_common.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_LCD_PCF8882 "pcf8882"
#define PMB887X_LCD_PCF8882(obj) OBJECT_CHECK(pmb887x_lcd_pcf8882_t, (obj), TYPE_PMB887X_LCD_PCF8882)

#define PCF8882_REGISTER_COUNT 0x100
#define PCF8882_MAX_PARAMS 4

typedef struct pmb887x_lcd_pcf8882_t pmb887x_lcd_pcf8882_t;

struct pmb887x_lcd_pcf8882_t {
    pmb887x_lcd_t parent;
    uint8_t registers[PCF8882_REGISTER_COUNT][PCF8882_MAX_PARAMS];
};

static void pcf8882_update_state(pmb887x_lcd_t *lcd) {
    pmb887x_lcd_pcf8882_t *p = PMB887X_LCD_PCF8882(lcd);
    uint8_t madctl = p->registers[PCF8882_MADCTL][0];
    uint8_t colmod = p->registers[PCF8882_COLMOD][0] & PCF8882_COLMOD_P;
    enum pmb887x_lcd_pixel_format_t pixel_format;

    switch (colmod) {
        case PCF8882_COLMOD_P_PIXEL_16_BIT:
            pixel_format = LCD_PIXEL_FORMAT_RGB565;
            break;
        case PCF8882_COLMOD_P_PIXEL_18_BIT:
            pixel_format = LCD_PIXEL_FORMAT_RGB666_6_6_6;
            break;
        default:
            EPRINTF("invalid color mode: %02X, using RGB565\n", colmod);
            pixel_format = LCD_PIXEL_FORMAT_RGB565;
            break;
    }

    pmb887x_lcd_set_addr_mode(
        lcd,
        madctl & PCF8882_MADCTL_V ? LCD_AM_VERTICAL : LCD_AM_HORIZONTAL,
        madctl & PCF8882_MADCTL_MX ? LCD_AC_DEC : LCD_AC_INC,
        madctl & PCF8882_MADCTL_MY ? LCD_AC_DEC : LCD_AC_INC
    );
    pmb887x_lcd_set_pixel_format(lcd, pixel_format);
    pmb887x_lcd_set_output_bgr(lcd, (madctl & PCF8882_MADCTL_RGB) != 0);
    pmb887x_lcd_set_transform(lcd, false, false);
}

static int pcf8882_on_cmd(pmb887x_lcd_t *lcd, uint32_t command) {
    int params_n;
    switch (command) {
        case PCF8882_RAMWR:
            pmb887x_lcd_set_ram_mode(lcd, true);
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
        IO_DUMP_WRITE(command, 1, 0);
    return params_n;
}

static void pcf8882_on_cmd_with_params(pmb887x_lcd_t *lcd, uint32_t command, const uint32_t *params, uint32_t params_n) {
    pmb887x_lcd_pcf8882_t *p = PMB887X_LCD_PCF8882(lcd);
    uint32_t value = 0;

    g_assert(command < PCF8882_REGISTER_COUNT);
    g_assert(params_n <= PCF8882_MAX_PARAMS);

    for (uint32_t i = 0; i < params_n; i++)
        value = value << 8 | params[i];
    IO_DUMP_WRITE(command, params_n, value);

    for (uint32_t i = 0; i < params_n; i++)
        p->registers[command][i] = params[i];

    switch (command) {
        case PCF8882_MADCTL:
        case PCF8882_COLMOD:
            pcf8882_update_state(lcd);
            break;
        case PCF8882_CASET:
            pmb887x_lcd_set_x(lcd, params[0]);
            pmb887x_lcd_set_window_x1(lcd, params[0]);
            pmb887x_lcd_set_window_x2(lcd, params[1]);
            break;
        case PCF8882_PASET:
            pmb887x_lcd_set_y(lcd, params[0]);
            pmb887x_lcd_set_window_y1(lcd, params[0]);
            pmb887x_lcd_set_window_y2(lcd, params[1]);
            break;
    }
}

static void pcf8882_reset(pmb887x_lcd_t *lcd) {
    pmb887x_lcd_pcf8882_t *p = PMB887X_LCD_PCF8882(lcd);

    memset(p->registers, 0, sizeof(p->registers));
    p->registers[PCF8882_COLMOD][0] = PCF8882_COLMOD_P_PIXEL_18_BIT;
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
