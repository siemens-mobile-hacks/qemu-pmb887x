/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/audio/audio-router.h"
#include "hw/i2c/i2c.h"
#include "system/debug-channel.h"
#include "regs.h"

#define TYPE_PMB6812 "pmb6812"
OBJECT_DECLARE_SIMPLE_TYPE(PMB6812State, PMB6812)

#define PMB6812_REG_COUNT (UINT8_MAX + 1)
#define PMB6812_IDLE_BYTE 0xFF
#define PMB6812_PEC_POLYNOMIAL 0x07

enum {
    PMB6812_WRITE_REG,
    PMB6812_WRITE_VALUE,
    PMB6812_WRITE_PEC,
};

static const int16_t PMB6812_AUDIO_GAIN_MB[] = {
    [PMB6812_AUDCTRL_AUDGAIN_MINUS_6DB >> PMB6812_AUDCTRL_AUDGAIN_SHIFT] = -600,
    [PMB6812_AUDCTRL_AUDGAIN_MINUS_1_2DB >> PMB6812_AUDCTRL_AUDGAIN_SHIFT] = -120,
    [PMB6812_AUDCTRL_AUDGAIN_PLUS_2_7DB >> PMB6812_AUDCTRL_AUDGAIN_SHIFT] = 270,
    [PMB6812_AUDCTRL_AUDGAIN_PLUS_6DB >> PMB6812_AUDCTRL_AUDGAIN_SHIFT] = 600,
};

static const uint8_t PMB6812_RESET_REGS[PMB6812_REG_COUNT] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x38, 0x00, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

struct PMB6812State {
    I2CSlave parent_obj;
    DebugChannel dbg;
    AudioRouterInput audio_input;
    AudioLineBus *audio_output;
    AudioRouter router;
    uint8_t reg;
    uint8_t wcycle;
    uint8_t rcycle;
    uint8_t wr_val;
    uint8_t confirmation;
    uint8_t regs[PMB6812_REG_COUNT];
    bool confirmation_pending;
    bool on_level;
    bool lon_frozen;
};

static void pmb6812_audio_update(PMB6812State *s) {
    uint8_t control = s->regs[PMB6812_AUDCTRL];
    bool enabled = (control & (PMB6812_AUDCTRL_AUDON | PMB6812_AUDCTRL_MUTE)) == PMB6812_AUDCTRL_AUDON;
    uint8_t gain = (control & PMB6812_AUDCTRL_AUDGAIN) >> PMB6812_AUDCTRL_AUDGAIN_SHIFT;

    audio_router_set_route(&s->router, 0, 0, enabled, PMB6812_AUDIO_GAIN_MB[gain]);
    audio_router_update(&s->router);
}

static void pmb6812_update_on_level(PMB6812State *s) {
    if (s->on_level) {
        s->regs[PMB6812_ISF] |= PMB6812_ISF_LON;
    } else {
        s->regs[PMB6812_ISF] &= ~PMB6812_ISF_LON;
    }
}

static void pmb6812_on_handler(void *opaque, int _n, int level) {
    PMB6812State *s = opaque;
    bool on_level = level != 0;

    if (s->on_level == on_level)
        return;

    s->on_level = on_level;
    if (!s->lon_frozen)
        pmb6812_update_on_level(s);
    if ((s->regs[PMB6812_INTCTRL2] & PMB6812_INTCTRL2_EION) != 0)
        s->lon_frozen = true;
}

static int pmb6812_event(I2CSlave *i2c, enum i2c_event event) {
    PMB6812State *s = PMB6812(i2c);

    switch (event) {
        case I2C_START_SEND:
        case I2C_START_SEND_ASYNC:
            s->wcycle = 0;
            s->confirmation_pending = false;
            break;
        case I2C_START_RECV:
            s->wcycle = 0;
            s->rcycle = 0;
            break;
        case I2C_NACK:
        case I2C_FINISH:
            s->wcycle = 0;
            break;
    }

    return 0;
}

static uint8_t pmb6812_recv(I2CSlave *i2c) {
    PMB6812State *s = PMB6812(i2c);
    bool is_confirmation = s->rcycle == 0 && s->confirmation_pending;
    bool is_register = !is_confirmation && s->rcycle == 0 && s->reg >= PMB6812_GEF1 && s->reg <= PMB6812_GEF2;
    uint8_t data = PMB6812_IDLE_BYTE;

    if (is_confirmation) {
        data = s->confirmation;
    } else if (is_register) {
        data = s->regs[s->reg];
    }

    DEBUG_IO_READ(&s->dbg, s->reg, 1, data);
    if (is_confirmation)
        s->confirmation_pending = false;
    if (is_register && s->reg == PMB6812_ISF) {
        s->lon_frozen = false;
        pmb6812_update_on_level(s);
    }
    s->rcycle++;

    return data;
}

static uint8_t pmb6812_pec(uint8_t address_write, uint8_t reg, uint8_t value) {
    const uint8_t bytes[] = { address_write, reg, value };
    uint8_t crc = 0;

    for (size_t i = 0; i < ARRAY_SIZE(bytes); i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < BITS_PER_BYTE; bit++)
            crc = (crc & BIT(7)) ? (crc << 1) ^ PMB6812_PEC_POLYNOMIAL : (crc << 1);
    }

    return crc;
}

static void pmb6812_write_reg(PMB6812State *s) {
    DEBUG_IO_WRITE(&s->dbg, s->reg, 1, s->wr_val);
    s->regs[s->reg] = s->wr_val;

    if (s->reg == PMB6812_INTCTRL2) {
        if ((s->wr_val & PMB6812_INTCTRL2_EION) == 0)
            s->lon_frozen = false;
        if (!s->lon_frozen)
            pmb6812_update_on_level(s);
    }
    if (s->reg == PMB6812_AUDCTRL)
        pmb6812_audio_update(s);
}

static int pmb6812_send(I2CSlave *i2c, uint8_t data) {
    PMB6812State *s = PMB6812(i2c);

    switch (s->wcycle++) {
        case PMB6812_WRITE_REG:
            s->reg = data;
            return 0;
        case PMB6812_WRITE_VALUE:
            s->wr_val = data;
            return 0;
        case PMB6812_WRITE_PEC: {
            uint8_t expect = pmb6812_pec(i2c->address << 1, s->reg, s->wr_val);
            s->confirmation_pending = true;
            if (data != expect) {
                s->confirmation = ~data;
                DEBUG_GUEST_ERROR(&s->dbg, "bad CRC on write reg=0x%02X val=0x%02X: got 0x%02X expected 0x%02X\n",
                    s->reg, s->wr_val, data, expect);
                return 1;
            }

            s->confirmation = data;
            pmb6812_write_reg(s);
            return 0;
        }
        default:
            /* The device does not support continued writes. */
            return 1;
    }
}

static void pmb6812_reset(DeviceState *dev) {
    PMB6812State *s = PMB6812(dev);

    s->reg = 0;
    s->wcycle = 0;
    s->rcycle = 0;
    s->confirmation = 0;
    s->confirmation_pending = false;
    s->lon_frozen = false;
    memcpy(s->regs, PMB6812_RESET_REGS, sizeof(PMB6812_RESET_REGS));
    pmb6812_update_on_level(s);
    audio_router_reset(&s->router);
    pmb6812_audio_update(s);
}

static void pmb6812_realize(DeviceState *dev, Error **_errp) {
    debug_channel_init_device_regs(&PMB6812(dev)->dbg, dev, &pmb6812_debug_regs);
    pmb6812_reset(dev);
}

static void pmb6812_unrealize(DeviceState *dev) {
    debug_channel_cleanup(&PMB6812(dev)->dbg);
}

static void pmb6812_init(Object *obj) {
    DeviceState *dev = DEVICE(obj);
    PMB6812State *s = PMB6812(obj);

    qdev_init_gpio_in_named(dev, pmb6812_on_handler, "ON", 1);
    s->audio_output = audio_line_bus_init(dev, "OUT");
    audio_router_init(&s->router, &s->audio_output, 1, 1, &s->dbg);
    audio_router_input_init(&s->audio_input, dev, "IN", &s->router, 0);
}

static void pmb6812_finalize(Object *obj) {
    audio_router_cleanup(&PMB6812(obj)->router);
}

static void pmb6812_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    device_class_set_legacy_reset(dc, pmb6812_reset);
    dc->realize = pmb6812_realize;
    dc->unrealize = pmb6812_unrealize;
    k->event = pmb6812_event;
    k->recv = pmb6812_recv;
    k->send = pmb6812_send;
    audio_router_sink_class_init(AUDIO_LINE_SINK_CLASS(klass));
}

static const TypeInfo pmb6812_info = {
    .name = TYPE_PMB6812,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(PMB6812State),
    .instance_init = pmb6812_init,
    .instance_finalize = pmb6812_finalize,
    .class_init = pmb6812_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_AUDIO_LINE_SINK },
        { NULL },
    },
};

static void pmb6812_register_types(void) {
    type_register_static(&pmb6812_info);
}
type_init(pmb6812_register_types)
