/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/audio/audio-router.h"
#include "hw/i2c/i2c.h"
#include "system/debug-channel.h"
#include "regs.h"

#define TYPE_LM4946 "lm4946"
OBJECT_DECLARE_SIMPLE_TYPE(LM4946State, LM4946)

#define LM4946_COMMAND_3BIT 0xE0
#define LM4946_COMMAND_4BIT 0xF0
#define LM4946_LINE_TO_SPEAKER_MB 602
#define LM4946_PHONE_TO_HEADPHONE_MB (-602)

enum {
    LM4946_INPUT_PHONE,
    LM4946_INPUT_LINE,
    LM4946_INPUT_COUNT,
};

enum {
    LM4946_OUTPUT_SPEAKER,
    LM4946_OUTPUT_HEADPHONE,
    LM4946_OUTPUT_COUNT,
};

typedef struct LM4946Mode LM4946Mode;

struct LM4946Mode {
    bool phone_to_speaker;
    bool line_to_speaker;
    bool phone_to_headphone;
    bool line_to_headphone;
};

static const LM4946Mode LM4946_MODES[] = {
    [LM4946_MODE_MC_ALL_SHUTDOWN] = { },
    [LM4946_MODE_MC_MONO_IN_TO_SPK] = { .phone_to_speaker = true },
    [LM4946_MODE_MC_MONO_IN_TO_HP] = { .phone_to_headphone = true },
    [LM4946_MODE_MC_LINE_IN_TO_SPK] = { .line_to_speaker = true },
    [LM4946_MODE_MC_LINE_IN_TO_HP] = { .line_to_headphone = true },
    [LM4946_MODE_MC_LINE_MONO_TO_SPK] = { .phone_to_speaker = true, .line_to_speaker = true },
    [LM4946_MODE_MC_LINE_MONO_TO_HP] = { .phone_to_headphone = true, .line_to_headphone = true },
    [LM4946_MODE_MC_LINE_IN_TO_SPK_HP] = { .line_to_speaker = true, .line_to_headphone = true },
};

static const int16_t LM4946_VOLUME_MB[] = {
    -5400, -4650, -4050, -3450, -3000, -2700, -2400, -2100,
    -1800, -1500, -1350, -1200, -1050, -900, -750, -600,
    -450, -300, -150, 0, 150, 300, 450, 600,
    750, 900, 1050, 1200, 1350, 1500, 1650, 1800,
};

struct LM4946State {
    I2CSlave parent_obj;
    DebugChannel dbg;
    AudioRouterInput phone_input;
    AudioRouterInput line_input;
    AudioLineBus *output_buses[LM4946_OUTPUT_COUNT];
    AudioRouter router;
    uint8_t mode;
    uint8_t n3d;
    uint8_t mono_vol;
    uint8_t left_vol;
    uint8_t right_vol;
};

static void lm4946_routes_update(LM4946State *s) {
    const LM4946Mode *mode = &LM4946_MODES[s->mode & LM4946_MODE_MC];
    int phone_mb = LM4946_VOLUME_MB[s->mono_vol];
    int line_mb = (LM4946_VOLUME_MB[s->left_vol] + LM4946_VOLUME_MB[s->right_vol]) / 2;

    audio_router_set_route(&s->router, LM4946_OUTPUT_SPEAKER, LM4946_INPUT_PHONE, mode->phone_to_speaker, phone_mb);
    audio_router_set_route(&s->router, LM4946_OUTPUT_SPEAKER, LM4946_INPUT_LINE, mode->line_to_speaker,
        line_mb + LM4946_LINE_TO_SPEAKER_MB);
    audio_router_set_route(&s->router, LM4946_OUTPUT_HEADPHONE, LM4946_INPUT_PHONE, mode->phone_to_headphone,
        phone_mb + LM4946_PHONE_TO_HEADPHONE_MB);
    audio_router_set_route(&s->router, LM4946_OUTPUT_HEADPHONE, LM4946_INPUT_LINE, mode->line_to_headphone, line_mb);
    audio_router_update(&s->router);
}

static int lm4946_event(I2CSlave *_i2c, enum i2c_event _event) {
    return 0;
}

static uint8_t lm4946_recv(I2CSlave *_i2c) {
    return 0;
}

static int lm4946_send(I2CSlave *i2c, uint8_t data) {
    LM4946State *s = LM4946(i2c);

    if ((data & LM4946_COMMAND_3BIT) == LM4946_MONO_VOL) {
        s->mono_vol = data & LM4946_MONO_VOL_VOL;
    } else if ((data & LM4946_COMMAND_3BIT) == LM4946_LEFT_VOL) {
        s->left_vol = data & LM4946_LEFT_VOL_VOL;
    } else if ((data & LM4946_COMMAND_3BIT) == LM4946_RIGHT_VOL) {
        s->right_vol = data & LM4946_RIGHT_VOL_VOL;
    } else if ((data & LM4946_COMMAND_4BIT) == LM4946_N3D) {
        s->n3d = data & ~LM4946_COMMAND_4BIT;
    } else if ((data & LM4946_COMMAND_4BIT) == LM4946_MODE) {
        s->mode = data & ~LM4946_COMMAND_4BIT;
    } else {
        DEBUG_UNIMP(&s->dbg, "unknown command %02X\n", data);
        return 0;
    }

    DEBUG_IO_WRITE(&s->dbg, data & LM4946_COMMAND_3BIT, 1, data & ~LM4946_COMMAND_3BIT);
    lm4946_routes_update(s);
    return 0;
}

static void lm4946_reset(DeviceState *dev) {
    LM4946State *s = LM4946(dev);

    s->mode = LM4946_MODE_MC_ALL_SHUTDOWN;
    s->n3d = 0;
    s->mono_vol = 0;
    s->left_vol = 0;
    s->right_vol = 0;
    audio_router_reset(&s->router);
    lm4946_routes_update(s);
}

static void lm4946_realize(DeviceState *dev, Error **_errp) {
    debug_channel_init_device_regs(&LM4946(dev)->dbg, dev, &lm4946_debug_regs);
}

static void lm4946_unrealize(DeviceState *dev) {
    debug_channel_cleanup(&LM4946(dev)->dbg);
}

static void lm4946_init(Object *obj) {
    DeviceState *dev = DEVICE(obj);
    LM4946State *s = LM4946(obj);

    s->output_buses[LM4946_OUTPUT_SPEAKER] = audio_line_bus_init(dev, "SPEAKER");
    s->output_buses[LM4946_OUTPUT_HEADPHONE] = audio_line_bus_init(dev, "HEADPHONE");
    audio_router_init(&s->router, s->output_buses, LM4946_OUTPUT_COUNT, LM4946_INPUT_COUNT, &s->dbg);
    audio_router_input_init(&s->phone_input, dev, "PHONE_IN", &s->router, LM4946_INPUT_PHONE);
    audio_router_input_init(&s->line_input, dev, "LINE_IN", &s->router, LM4946_INPUT_LINE);
}

static void lm4946_finalize(Object *obj) {
    audio_router_cleanup(&LM4946(obj)->router);
}

static void lm4946_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->realize = lm4946_realize;
    dc->unrealize = lm4946_unrealize;
    device_class_set_legacy_reset(dc, lm4946_reset);
    k->event = lm4946_event;
    k->recv = lm4946_recv;
    k->send = lm4946_send;
    audio_router_sink_class_init(AUDIO_LINE_SINK_CLASS(klass));
}

static const TypeInfo lm4946_info = {
    .name = TYPE_LM4946,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(LM4946State),
    .instance_init = lm4946_init,
    .instance_finalize = lm4946_finalize,
    .class_init = lm4946_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_AUDIO_LINE_SINK },
        { NULL },
    },
};

static void lm4946_register_types(void) {
    type_register_static(&lm4946_info);
}
type_init(lm4946_register_types)
