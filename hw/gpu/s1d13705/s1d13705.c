/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "hw/core/irq.h"
#include "hw/ssi/ssi.h"
#include "system/debug-channel.h"

#define TYPE_S1D13705 "s1d13705"
OBJECT_DECLARE_SIMPLE_TYPE(S1D13705State, S1D13705)

#define S1D13705_HOST_WORD_BITS 16
#define S1D13705_REG_COUNT (UINT16_MAX + 1)
#define S1D13705_CMD_MASK 0xF000
#define S1D13705_REG_ID 0x0000

/*
 * CMD: SA0=0, CS=0
 * ARGx: SA0=1, CS=0
 *
 * TYPE   CMD     ARG0      ARG1
 * WRITE  0x0000  <REG_ID>  <VALUE>  write reg value
 * WRITE  0x4000  <REG_ID>  0x0000   select reg for read
 * READ   0x8000                     read current reg
 *
 * WRITE  0xD000                     LCD data (bypass)
 * WRITE  0x3xx0                     xx - LCD command (bypass)
 * WRITE  0x1xx0                     xx - LCD data (bypass)
 */
enum {
    S1D13705_CMD_WRITE_REG = 0x0000,
    S1D13705_CMD_LCD_SINGLE_DATA = 0x1000,
    S1D13705_CMD_LCD_SINGLE_CMD = 0x3000,
    S1D13705_CMD_READ_REG = 0x4000,
    S1D13705_CMD_NOOP = 0x8000,
    S1D13705_CMD_LCD_DATA_BYPASS = 0xD000,
};

struct S1D13705State {
    SSIPeripheral parent_obj;
    DebugChannel dbg;
    SSIBus *bus;
    bool is_command;

    uint16_t request;
    uint16_t trx_bits;
    uint16_t response;
    bool lcd_data_bypass;

    uint16_t regs[S1D13705_REG_COUNT];

    uint32_t wcycle;
    uint32_t cmd;
    uint32_t arg0;
    qemu_irq fpline;
};

static uint32_t s1d13705_read_reg(S1D13705State *s, uint16_t reg) {
    uint16_t value = 0;

    switch (reg) {
        case S1D13705_REG_ID:
            value = 0x8090;
            break;
        case 0x0014:
            value = 0x04D1;
            break;
        case 0x0202:
            value = 0x0000;
            break;
        default:
            /* Unknown reg */
            /* value = s->regs[reg]; */
            break;
    }

    DEBUG_LOG(&s->dbg, "read reg %04X: %04X\n", reg, value);
    return value;
}

static void s1d13705_write_reg(S1D13705State *s, uint16_t reg, uint16_t value) {
    DEBUG_LOG(&s->dbg, "write reg %04X: %04X\n", reg, value);
    s->regs[reg] = value;
}

static uint32_t s1d13705_transfer_word(S1D13705State *s, uint32_t data) {
    uint32_t value = 0;

    DEBUG_LOG(&s->dbg, "%04X // cmd=%d\n", data, s->is_command);

    if (s->is_command) {
        s->lcd_data_bypass = false;

        if ((data & S1D13705_CMD_MASK) == S1D13705_CMD_LCD_SINGLE_CMD) {
            qemu_set_irq(s->fpline, 0);
            value = ssi_transfer(s->bus, extract32(data, 4, 8));
            qemu_set_irq(s->fpline, 1);
            s->wcycle = 0;
        } else if ((data & S1D13705_CMD_MASK) == S1D13705_CMD_LCD_SINGLE_DATA) {
            value = ssi_transfer(s->bus, extract32(data, 4, 8));
            s->wcycle = 0;
        } else if (data == S1D13705_CMD_WRITE_REG || data == S1D13705_CMD_READ_REG) {
            s->cmd = data;
            s->wcycle = 1;
        } else if (data == S1D13705_CMD_LCD_DATA_BYPASS) {
            s->lcd_data_bypass = true;
        } else if (data == S1D13705_CMD_NOOP) {
            s->wcycle = 0;
        }
    } else if (s->wcycle != 0) {
        if (s->cmd == S1D13705_CMD_WRITE_REG) {
            if (s->wcycle == 1) {
                s->arg0 = data;
                s->wcycle++;
            } else if (s->wcycle == 2) {
                s1d13705_write_reg(s, s->arg0, data);
                s->wcycle = 0;
            } else {
                DEBUG_GUEST_ERROR(&s->dbg, "invalid write register cycle: %u\n", s->wcycle);
                s->wcycle = 0;
            }
        } else if (s->cmd == S1D13705_CMD_READ_REG) {
            if (s->wcycle == 1) {
                s->arg0 = data;
                s->wcycle++;
            } else {
                value = s1d13705_read_reg(s, s->arg0);
                s->wcycle++;
                s->arg0 += 2;
            }
        } else {
            DEBUG_GUEST_ERROR(&s->dbg, "invalid wcycle for cmd %04X\n", s->cmd);
            s->wcycle = 0;
        }
    } else {
        DEBUG_GUEST_ERROR(&s->dbg, "ignored data: %04X [cmd=%04X]\n", data, s->cmd);
    }

    DEBUG_LOG(&s->dbg, "value=%08X\n", value);
    return value;
}

static uint32_t s1d13705_transfer(SSIPeripheral *dev, uint32_t in) {
    S1D13705State *s = S1D13705(dev);

    if (!s->is_command && s->lcd_data_bypass)
        return ssi_transfer(s->bus, in);

    uint32_t shift = (S1D13705_HOST_WORD_BITS - BITS_PER_BYTE) - s->trx_bits;
    uint16_t out = (s->response >> shift) & 0xFF;

    s->request |= (in & 0xFF) << shift;
    s->trx_bits += BITS_PER_BYTE;
    if (s->trx_bits == S1D13705_HOST_WORD_BITS) {
        s->response = s1d13705_transfer_word(s, s->request);
        s->trx_bits = 0;
        s->request = 0;
    }
    return out;
}

static void s1d13705_sa0_handler(void *opaque, int _n, int level) {
    S1D13705State *s = S1D13705(opaque);
    bool is_command = level == 0;

    if (s->is_command == is_command)
        return;

    s->is_command = is_command;
    s->trx_bits = 0;
    s->request = 0;
    if (is_command) {
        s->wcycle = 0;
        s->cmd = 0;
        s->lcd_data_bypass = false;
    }
}

static void s1d13705_reset_handler(void *opaque, int _n, int level) {
    S1D13705State *s = S1D13705(opaque);

    if (level != 0)
        return;

    DEBUG_LOG(&s->dbg, "reset!\n");
    s->wcycle = 0;
    s->trx_bits = 0;
    s->request = 0;
    s->response = 0;
    s->lcd_data_bypass = false;
}

static void s1d13705_realize(SSIPeripheral *ssi, Error **_errp) {
    DeviceState *dev = DEVICE(ssi);
    S1D13705State *s = S1D13705(ssi);

    debug_channel_init_device(&s->dbg, dev);
    s->bus = ssi_create_bus(dev, "LCD");
    qdev_init_gpio_in_named(dev, s1d13705_sa0_handler, "SA0", 1);
    qdev_init_gpio_in_named(dev, s1d13705_reset_handler, "RESET", 1);
    qdev_init_gpio_out_named(dev, &s->fpline, "FPLINE", 1);
}

static void s1d13705_unrealize(DeviceState *dev) {
    debug_channel_cleanup(&S1D13705(dev)->dbg);
}

static void s1d13705_class_init(ObjectClass *klass, const void *_data) {
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    DEVICE_CLASS(klass)->unrealize = s1d13705_unrealize;
    k->realize = s1d13705_realize;
    k->transfer = s1d13705_transfer;
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo s1d13705_info = {
    .name = TYPE_S1D13705,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(S1D13705State),
    .class_init = s1d13705_class_init,
};

static void s1d13705_register_types(void) {
    type_register_static(&s1d13705_info);
}
type_init(s1d13705_register_types)
