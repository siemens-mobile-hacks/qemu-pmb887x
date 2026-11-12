/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/ssi/ssi.h"
#include "system/debug-channel.h"

#define TYPE_S1D13716 "s1d13716"
OBJECT_DECLARE_SIMPLE_TYPE(S1D13716State, S1D13716)

#define S1D13716_REG_COUNT (UINT16_MAX + 1)

/*
 * 0x40 <REG_L> <REG_H>  select reg
 * 0xC0                  read reg value
 * 0x80 <VALUE>          write reg value
 */
enum {
    S1D13716_CMD_SELECT_REG = 0x40,
    S1D13716_CMD_WRITE_REG = 0x80,
    S1D13716_CMD_READ_REG = 0xC0,
};

struct S1D13716State {
    SSIPeripheral parent_obj;
    DebugChannel dbg;
    SSIBus *bus;

    uint8_t regs[S1D13716_REG_COUNT];

    uint32_t wcycle;
    uint8_t cmd;
    uint16_t reg;

    bool cs_app;
    bool cs_lcd;
    bool is_command;

    qemu_irq int_out;
    qemu_irq nscs_out;
    qemu_irq srs_out;
};

static uint32_t s1d13716_transfer(SSIPeripheral *dev, uint32_t in) {
    /* WASM PORT FAST PATH (removable): per transferred byte; the class
     * installs this transfer only on S1D13716 peripherals, so the checked
     * cast is skipped */
    S1D13716State *s = (S1D13716State *)dev;
    uint32_t response = 0;

    if (!s->cs_app && s->cs_lcd)
        return ssi_transfer(s->bus, in);

    if (s->is_command) {
        s->cmd = in;
        s->wcycle = 0;
        return 0;
    }

    switch (s->cmd) {
        case S1D13716_CMD_SELECT_REG:
            if (s->wcycle == 0) {
                s->reg |= in & 0xFF;
                s->wcycle++;
            } else if (s->wcycle == 1) {
                s->reg |= (in & 0xFF) << BITS_PER_BYTE;
                s->wcycle = 0;
            }
            break;

        case S1D13716_CMD_WRITE_REG:
            s->regs[s->reg] = in;
            DEBUG_LOG(&s->dbg, "write reg %04X = %02X\n", s->reg, in);
            break;

        case S1D13716_CMD_READ_REG:
            response = s->regs[s->reg];
            DEBUG_LOG(&s->dbg, "read reg %04X = %02X\n", s->reg, response);
            break;
    }

    return response;
}

/*
 * WASM PORT FAST PATH (removable): pass-through is the display stream, so the
 * whole block goes to the LCD.  Anything addressed to the chip's own
 * registers goes word by word.
 */
static void s1d13716_transfer_block(SSIPeripheral *dev, const uint32_t *tx, uint32_t *rx, size_t count) {
    S1D13716State *s = (S1D13716State *)dev;

    if (!s->cs_app && s->cs_lcd) {
        ssi_transfer_block(s->bus, tx, rx, count);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        uint32_t reply = s1d13716_transfer(dev, tx[i]);
        if (rx)
            rx[i] = reply;
    }
}

static void s1d13716_transfer_bytes(SSIPeripheral *dev, const uint8_t *tx, size_t count) {
    S1D13716State *s = (S1D13716State *)dev;

    if (!s->cs_app && s->cs_lcd) {
        ssi_transfer_bytes(s->bus, tx, count);
        return;
    }
    for (size_t i = 0; i < count; i++)
        s1d13716_transfer(dev, tx[i]);
}

static void s1d13716_rs_handler(void *opaque, int _n, int level) {
    S1D13716State *s = S1D13716(opaque);

    s->is_command = level == 0;
    qemu_set_irq(s->srs_out, level);
}

static void s1d13716_ncs_app_handler(void *opaque, int _n, int level) {
    S1D13716(opaque)->cs_app = level == 0;
}

static void s1d13716_ncs_lcd_handler(void *opaque, int _n, int level) {
    S1D13716State *s = S1D13716(opaque);

    s->cs_lcd = level == 0;
    qemu_set_irq(s->nscs_out, level);
}

static void s1d13716_nreset_handler(void *opaque, int _n, int level) {
    S1D13716State *s = S1D13716(opaque);

    if (level != 0)
        return;

    DEBUG_LOG(&s->dbg, "reset!\n");
    s->wcycle = 0;
    s->cmd = 0;
}

static void s1d13716_realize(SSIPeripheral *ssi, Error **_errp) {
    DeviceState *dev = DEVICE(ssi);
    S1D13716State *s = S1D13716(ssi);

    debug_channel_init_device(&s->dbg, dev);
    s->bus = ssi_create_bus(dev, "LCD");
    qdev_init_gpio_in_named(dev, s1d13716_rs_handler, "RS", 1);
    qdev_init_gpio_in_named(dev, s1d13716_ncs_app_handler, "NCS_APP", 1);
    qdev_init_gpio_in_named(dev, s1d13716_ncs_lcd_handler, "NCS_LCD", 1);
    qdev_init_gpio_in_named(dev, s1d13716_nreset_handler, "NRESET", 1);
    qdev_init_gpio_out_named(dev, &s->int_out, "INT", 1);
    qdev_init_gpio_out_named(dev, &s->nscs_out, "NSCS", 1);
    qdev_init_gpio_out_named(dev, &s->srs_out, "SRS", 1);
}

static void s1d13716_unrealize(DeviceState *dev) {
    debug_channel_cleanup(&S1D13716(dev)->dbg);
}

static void s1d13716_class_init(ObjectClass *klass, const void *_data) {
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    DEVICE_CLASS(klass)->unrealize = s1d13716_unrealize;
    k->realize = s1d13716_realize;
    k->transfer = s1d13716_transfer;
    k->transfer_block = s1d13716_transfer_block;
    k->transfer_bytes = s1d13716_transfer_bytes;
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo s1d13716_info = {
    .name = TYPE_S1D13716,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(S1D13716State),
    .class_init = s1d13716_class_init,
};

static void s1d13716_register_types(void) {
    type_register_static(&s1d13716_info);
}
type_init(s1d13716_register_types)
