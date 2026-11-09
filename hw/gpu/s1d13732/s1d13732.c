/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "hw/core/irq.h"
#include "hw/ssi/ssi.h"
#include "system/debug-channel.h"

#define TYPE_S1D13732 "s1d13732"
OBJECT_DECLARE_SIMPLE_TYPE(S1D13732State, S1D13732)

#define S1D13732_HOST_WORD_BITS 16
#define S1D13732_REG_COUNT (UINT16_MAX + 1)
#define S1D13732_CMD_MASK 0xF000
#define S1D13732_SRAM_SIZE 0x70000
#define S1D13732_SRAM_WORDS (S1D13732_SRAM_SIZE / 2)
#define S1D13732_FPDAT_COUNT 8
#define S1D13732_PRODUCT_CODE 0x706B
#define S1D13732_MISC_CONFIG_RESET 0x04D1
#define S1D13732_SPI_HEADER_RESET 0x0001
#define S1D13732_MEMORY_ADDRESS_READ BIT(0)
#define S1D13732_MEMORY_ADDRESS_HIGH_MASK 0x7
#define S1D13732_MEMORY_WRITE_ERROR BIT(0)
#define S1D13732_MEMORY_READ_ERROR BIT(1)

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
    S1D13732_CMD_WRITE_REG = 0x0000,
    S1D13732_CMD_LCD_SINGLE_DATA = 0x1000,
    S1D13732_CMD_LCD_SINGLE_CMD = 0x3000,
    S1D13732_CMD_READ_REG = 0x4000,
    S1D13732_CMD_NOOP = 0x8000,
    S1D13732_CMD_LCD_DATA_BYPASS = 0xD000,
};

enum {
    S1D13732_REG_PRODUCT_CODE = 0x0000,
    S1D13732_REG_MISC_CONFIG = 0x0014,
    S1D13732_REG_SOFTWARE_RESET = 0x0016,
    S1D13732_REG_SYSTEM_CLOCK = 0x0018,
    S1D13732_REG_MEMORY_ADDRESS_LOW = 0x0022,
    S1D13732_REG_MEMORY_ADDRESS_HIGH = 0x0024,
    S1D13732_REG_MEMORY_DATA = 0x0028,
    S1D13732_REG_SPI_HEADER = 0x0060,
    S1D13732_REG_DISPLAY_MODE = 0x0202,
    S1D13732_REG_INDIRECT_INTERRUPT = 0x0A20,
    S1D13732_REG_SD_CONFIG2 = 0x6004,
    S1D13732_REG_SD_INTERRUPT_FLAGS = 0x6008,
    S1D13732_REG_SD_FUNCTION = 0x6104,
    S1D13732_REG_SD_STATUS = 0x6106,
};

struct S1D13732State {
    SSIPeripheral parent_obj;
    DebugChannel dbg;
    SSIBus *bus;
    bool is_command;

    uint16_t request;
    uint16_t trx_bits;
    uint16_t response;
    bool lcd_data_bypass;
    bool reset_active;

    uint16_t regs[S1D13732_REG_COUNT];
    uint16_t sram[S1D13732_SRAM_WORDS];
    uint32_t memory_address;

    uint32_t wcycle;
    uint32_t cmd;
    uint32_t arg0;
    qemu_irq fpline;
    qemu_irq fpdat[S1D13732_FPDAT_COUNT];
};

static void s1d13732_update_fpdat(S1D13732State *s) {
    uint16_t value = s->regs[S1D13732_REG_SPI_HEADER];

    for (size_t i = 0; i < ARRAY_SIZE(s->fpdat); i++)
        qemu_set_irq(s->fpdat[i], (value >> i) & 1);
}

static void s1d13732_reset_state(S1D13732State *s, bool hardware_reset) {
    s->request = 0;
    s->trx_bits = 0;
    s->response = 0;
    s->lcd_data_bypass = false;
    s->memory_address = 0;
    s->wcycle = 0;
    s->cmd = 0;
    s->arg0 = 0;

    if (hardware_reset) {
        memset(s->regs, 0, sizeof(s->regs));
        s->regs[S1D13732_REG_PRODUCT_CODE] = S1D13732_PRODUCT_CODE;
        s->regs[S1D13732_REG_MISC_CONFIG] = S1D13732_MISC_CONFIG_RESET;
    } else {
        size_t first_reset_reg = S1D13732_REG_SYSTEM_CLOCK + 1;
        memset(&s->regs[first_reset_reg], 0, (ARRAY_SIZE(s->regs) - first_reset_reg) * sizeof(s->regs[0]));
    }
    s->regs[S1D13732_REG_SPI_HEADER] = S1D13732_SPI_HEADER_RESET;
    s->regs[S1D13732_REG_DISPLAY_MODE] = 0;
    /* Reset does not define display SRAM contents, so preserve the existing data. */

    qemu_set_irq(s->fpline, 1);
    s1d13732_update_fpdat(s);
}

static void s1d13732_update_memory_address_regs(S1D13732State *s) {
    uint16_t direction = s->regs[S1D13732_REG_MEMORY_ADDRESS_LOW] & S1D13732_MEMORY_ADDRESS_READ;
    uint16_t high = s->regs[S1D13732_REG_MEMORY_ADDRESS_HIGH] & ~S1D13732_MEMORY_ADDRESS_HIGH_MASK;

    s->regs[S1D13732_REG_MEMORY_ADDRESS_LOW] = (uint16_t) s->memory_address | direction;
    s->regs[S1D13732_REG_MEMORY_ADDRESS_HIGH] = high | ((s->memory_address >> 16) & S1D13732_MEMORY_ADDRESS_HIGH_MASK);
}

static void s1d13732_advance_memory_address(S1D13732State *s) {
    s->memory_address += 2;
    s1d13732_update_memory_address_regs(s);
}

static uint16_t s1d13732_read_memory(S1D13732State *s) {
    if ((s->regs[S1D13732_REG_MEMORY_ADDRESS_LOW] & S1D13732_MEMORY_ADDRESS_READ) == 0) {
        s->regs[S1D13732_REG_INDIRECT_INTERRUPT] |= S1D13732_MEMORY_READ_ERROR;
        return 0;
    }
    if (s->memory_address > S1D13732_SRAM_SIZE - 2) {
        s->regs[S1D13732_REG_INDIRECT_INTERRUPT] |= S1D13732_MEMORY_READ_ERROR;
        return 0;
    }

    uint16_t value = s->sram[s->memory_address / 2];
    s1d13732_advance_memory_address(s);
    return value;
}

static void s1d13732_write_memory(S1D13732State *s, uint16_t value) {
    if ((s->regs[S1D13732_REG_MEMORY_ADDRESS_LOW] & S1D13732_MEMORY_ADDRESS_READ) != 0) {
        s->regs[S1D13732_REG_INDIRECT_INTERRUPT] |= S1D13732_MEMORY_WRITE_ERROR;
        return;
    }
    if (s->memory_address > S1D13732_SRAM_SIZE - 2) {
        s->regs[S1D13732_REG_INDIRECT_INTERRUPT] |= S1D13732_MEMORY_WRITE_ERROR;
        return;
    }

    s->sram[s->memory_address / 2] = value;
    s1d13732_advance_memory_address(s);
}

static uint32_t s1d13732_read_reg(S1D13732State *s, uint16_t reg) {
    uint16_t value = reg == S1D13732_REG_MEMORY_DATA ? s1d13732_read_memory(s) : s->regs[reg];

    /* Temporary: report no SD card until the controller has a storage backend. */
    if (reg == S1D13732_REG_SD_CONFIG2) {
        value |= BIT(0);
    } else if (reg == S1D13732_REG_SD_INTERRUPT_FLAGS) {
        value |= BIT(9);
    } else if (reg == S1D13732_REG_SD_STATUS) {
        value |= BIT(6);
    }

    DEBUG_LOG(&s->dbg, "read reg %04X: %04X\n", reg, value);
    return value;
}

static void s1d13732_write_reg(S1D13732State *s, uint16_t reg, uint16_t value) {
    DEBUG_LOG(&s->dbg, "write reg %04X: %04X\n", reg, value);

    switch (reg) {
        case S1D13732_REG_PRODUCT_CODE:
            return;
        case S1D13732_REG_SOFTWARE_RESET:
            s1d13732_reset_state(s, false);
            return;
        case S1D13732_REG_MEMORY_DATA:
            s1d13732_write_memory(s, value);
            return;
        case S1D13732_REG_INDIRECT_INTERRUPT:
            s->regs[reg] &= ~value;
            return;
        case S1D13732_REG_SD_FUNCTION:
            /* Temporary: complete SD controller operations immediately until its backend is implemented. */
            s->regs[reg] = 0;
            return;
    }

    s->regs[reg] = value;
    if (reg == S1D13732_REG_MEMORY_ADDRESS_LOW) {
        uint32_t high = s->regs[S1D13732_REG_MEMORY_ADDRESS_HIGH] & S1D13732_MEMORY_ADDRESS_HIGH_MASK;
        s->memory_address = (high << 16) | (value & ~S1D13732_MEMORY_ADDRESS_READ);
    } else if (reg == S1D13732_REG_SPI_HEADER) {
        /* In panel SPI mode the header value is presented on FPDAT[7:0]. */
        s1d13732_update_fpdat(s);
    }
}

static uint32_t s1d13732_transfer_word(S1D13732State *s, uint32_t data) {
    uint32_t value = 0;

    DEBUG_LOG(&s->dbg, "%04X // cmd=%d\n", data, s->is_command);

    if (s->is_command) {
        s->lcd_data_bypass = false;

        if ((data & S1D13732_CMD_MASK) == S1D13732_CMD_LCD_SINGLE_CMD) {
            qemu_set_irq(s->fpline, 0);
            value = ssi_transfer(s->bus, extract32(data, 4, 8));
            qemu_set_irq(s->fpline, 1);
            s->wcycle = 0;
        } else if ((data & S1D13732_CMD_MASK) == S1D13732_CMD_LCD_SINGLE_DATA) {
            value = ssi_transfer(s->bus, extract32(data, 4, 8));
            s->wcycle = 0;
        } else if (data == S1D13732_CMD_WRITE_REG || data == S1D13732_CMD_READ_REG) {
            s->cmd = data;
            s->wcycle = 1;
        } else if (data == S1D13732_CMD_LCD_DATA_BYPASS) {
            s->lcd_data_bypass = true;
        } else if (data == S1D13732_CMD_NOOP) {
            s->wcycle = 0;
        }
    } else if (s->wcycle != 0) {
        if (s->cmd == S1D13732_CMD_WRITE_REG) {
            if (s->wcycle == 1) {
                s->arg0 = data;
                s->wcycle++;
            } else if (s->wcycle == 2) {
                s1d13732_write_reg(s, s->arg0, data);
                if (s->arg0 != S1D13732_REG_MEMORY_DATA)
                    s->wcycle = 0;
            } else {
                DEBUG_GUEST_ERROR(&s->dbg, "invalid write register cycle: %u\n", s->wcycle);
                s->wcycle = 0;
            }
        } else if (s->cmd == S1D13732_CMD_READ_REG) {
            if (s->wcycle == 1) {
                s->arg0 = data;
                s->wcycle++;
            } else {
                value = s1d13732_read_reg(s, s->arg0);
                s->wcycle++;
                if (s->arg0 != S1D13732_REG_MEMORY_DATA)
                    s->arg0 += 2;
            }
        } else {
            DEBUG_GUEST_ERROR(&s->dbg, "invalid wcycle for cmd %04X\n", s->cmd);
            s->wcycle = 0;
        }
    } else {
        DEBUG_GUEST_ERROR(&s->dbg, "ignored data: %04X [cmd=%04X]\n", data, s->cmd);
    }

    return value;
}

static uint32_t s1d13732_transfer(SSIPeripheral *dev, uint32_t in) {
    S1D13732State *s = S1D13732(dev);

    if (s->reset_active)
        return 0;
    if (!s->is_command && s->lcd_data_bypass)
        return ssi_transfer(s->bus, in);

    uint32_t shift = (S1D13732_HOST_WORD_BITS - BITS_PER_BYTE) - s->trx_bits;
    uint16_t out = (s->response >> shift) & 0xFF;

    s->request |= (in & 0xFF) << shift;
    s->trx_bits += BITS_PER_BYTE;
    if (s->trx_bits == S1D13732_HOST_WORD_BITS) {
        s->response = s1d13732_transfer_word(s, s->request);
        s->trx_bits = 0;
        s->request = 0;
    }
    return out;
}

static uint32_t s1d13732_transfer_raw(SSIPeripheral *dev, uint32_t in) {
    bool selected = !dev->cs;

    if (!selected && !S1D13732(dev)->lcd_data_bypass)
        return 0;
    return s1d13732_transfer(dev, in);
}

static void s1d13732_cs_handler(void *opaque, int _n, int level) {
    SSI_PERIPHERAL(opaque)->cs = level != 0;
}

static void s1d13732_sa0_handler(void *opaque, int _n, int level) {
    S1D13732State *s = S1D13732(opaque);
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

static void s1d13732_reset(DeviceState *dev) {
    s1d13732_reset_state(S1D13732(dev), true);
}

static void s1d13732_reset_handler(void *opaque, int _n, int level) {
    S1D13732State *s = S1D13732(opaque);

    if (level == 0) {
        DEBUG_LOG(&s->dbg, "reset!\n");
        s1d13732_reset_state(s, true);
        s->reset_active = true;
    } else {
        s->reset_active = false;
    }
}

static void s1d13732_realize(SSIPeripheral *ssi, Error **_errp) {
    static const char *const FPDAT_NAMES[S1D13732_FPDAT_COUNT] = {
        "FPDAT0", "FPDAT1", "FPDAT2", "FPDAT3", "FPDAT4", "FPDAT5", "FPDAT6", "FPDAT7",
    };
    DeviceState *dev = DEVICE(ssi);
    S1D13732State *s = S1D13732(ssi);

    debug_channel_init_device(&s->dbg, dev);
    s->bus = ssi_create_bus(dev, "LCD2");
    qdev_init_gpio_in_named(dev, s1d13732_cs_handler, SSI_GPIO_CS, 1);
    qdev_init_gpio_in_named(dev, s1d13732_sa0_handler, "SA0", 1);
    qdev_init_gpio_in_named(dev, s1d13732_reset_handler, "RESET", 1);
    qdev_init_gpio_out_named(dev, &s->fpline, "FPLINE", 1);
    for (size_t i = 0; i < ARRAY_SIZE(s->fpdat); i++)
        qdev_init_gpio_out_named(dev, &s->fpdat[i], FPDAT_NAMES[i], 1);
}

static void s1d13732_unrealize(DeviceState *dev) {
    debug_channel_cleanup(&S1D13732(dev)->dbg);
}

static void s1d13732_class_init(ObjectClass *klass, const void *_data) {
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, s1d13732_reset);
    dc->unrealize = s1d13732_unrealize;
    k->realize = s1d13732_realize;
    k->transfer = s1d13732_transfer;
    k->transfer_raw = s1d13732_transfer_raw;
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo s1d13732_info = {
    .name = TYPE_S1D13732,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(S1D13732State),
    .class_init = s1d13732_class_init,
};

static void s1d13732_register_types(void) {
    type_register_static(&s1d13732_info);
}
type_init(s1d13732_register_types)
