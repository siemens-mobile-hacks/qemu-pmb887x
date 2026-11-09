/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/timer.h"
#include "hw/audio/audio-line.h"
#include "hw/i2c/i2c.h"
#include "system/debug-channel.h"
#include "regs.h"

#define TYPE_TEA5761UK "tea5761uk"
OBJECT_DECLARE_SIMPLE_TYPE(TEA5761UKState, TEA5761UK)

#define TEA5761UK_REG_COUNT (TEA5761UK_CHIPID_LOW + 1)
#define TEA5761UK_VERSION 4
#define TEA5761UK_MANUFACTURER_ID 0x015
#define TEA5761UK_CHIP_ID 0x5761
#define TEA5761UK_AUDIO_RATE_HZ 48000
#define TEA5761UK_AUDIO_CHANNELS 2
#define TEA5761UK_AUDIO_PERIOD_NS (10 * SCALE_MS)
#define TEA5761UK_AUDIO_PERIOD_FRAMES (TEA5761UK_AUDIO_RATE_HZ / (NANOSECONDS_PER_SECOND / TEA5761UK_AUDIO_PERIOD_NS))
#define TEA5761UK_NOISE_RMS (INT16_MAX / 10)

struct TEA5761UKState {
    I2CSlave parent_obj;
    DebugChannel dbg;
    uint8_t regs[TEA5761UK_REG_COUNT];
    uint8_t read_index;
    uint8_t write_index;
    bool writing;
    bool frequency_written;
    AudioLineBus *audio_output;
    QEMUTimer *audio_timer;
    int64_t audio_deadline_ns;
    uint32_t noise_state;
    bool audio_active;
};

static const uint8_t TEA5761UK_RESET_REGS[TEA5761UK_REG_COUNT] = {
    [TEA5761UK_FRQSET_HIGH] = TEA5761UK_FRQSET_HIGH_SUD_SEARCH_UP,
    [TEA5761UK_TNCTRL_CONTROL] = TEA5761UK_TNCTRL_CONTROL_IFCTC_COUNT_15_625_MS,
    [TEA5761UK_TNCTRL_AUDIO] = TEA5761UK_TNCTRL_AUDIO_MU_HARD_MUTED | TEA5761UK_TNCTRL_AUDIO_SSL_ADC7 |
        TEA5761UK_TNCTRL_AUDIO_HLSI_HIGH_SIDE | TEA5761UK_TNCTRL_AUDIO_DTC_DEEMPHASIS_50_US,
    [TEA5761UK_TUNCHK_STATUS] = TEA5761UK_TUNCHK_STATUS_LEV_3_0,
    [TEA5761UK_MANID_HIGH] = (TEA5761UK_VERSION << TEA5761UK_MANID_HIGH_VERSION_SHIFT) |
        (TEA5761UK_MANUFACTURER_ID >> 7),
    [TEA5761UK_MANID_LOW] = ((TEA5761UK_MANUFACTURER_ID << TEA5761UK_MANID_LOW_MAN_ID_06_00_SHIFT) &
        TEA5761UK_MANID_LOW_MAN_ID_06_00) | TEA5761UK_MANID_LOW_IDAV_AVAILABLE,
    [TEA5761UK_CHIPID_HIGH] = TEA5761UK_CHIP_ID >> 8,
    [TEA5761UK_CHIPID_LOW] = TEA5761UK_CHIP_ID & TEA5761UK_CHIPID_LOW_CHIP_ID_07_00,
};

static const uint8_t TEA5761UK_WRITE_REGS[] = {
    TEA5761UK_INTREG_MASK,
    TEA5761UK_FRQSET_HIGH,
    TEA5761UK_FRQSET_LOW,
    TEA5761UK_TNCTRL_CONTROL,
    TEA5761UK_TNCTRL_AUDIO,
    TEA5761UK_TESTREG_CONTROL,
    TEA5761UK_TESTREG_CONFIG,
};

static int16_t tea5761uk_noise_sample(TEA5761UKState *s) {
    int uniform_peak = lround(TEA5761UK_NOISE_RMS * sqrt(3.0));

    s->noise_state ^= s->noise_state << 13;
    s->noise_state ^= s->noise_state >> 17;
    s->noise_state ^= s->noise_state << 5;
    return (int32_t) (s->noise_state % (2 * uniform_peak + 1)) - uniform_peak;
}

static void tea5761uk_audio_timer_cb(void *opaque) {
    TEA5761UKState *s = opaque;
    int16_t frames[TEA5761UK_AUDIO_PERIOD_FRAMES * TEA5761UK_AUDIO_CHANNELS];

    for (size_t i = 0; i < ARRAY_SIZE(frames); i++)
        frames[i] = tea5761uk_noise_sample(s);
    audio_line_write(s->audio_output, 0, frames, TEA5761UK_AUDIO_PERIOD_FRAMES);

    s->audio_deadline_ns += TEA5761UK_AUDIO_PERIOD_NS;
    timer_mod(s->audio_timer, s->audio_deadline_ns);
}

static bool tea5761uk_audio_is_active(const TEA5761UKState *s) {
    if ((s->regs[TEA5761UK_TNCTRL_CONTROL] & TEA5761UK_TNCTRL_CONTROL_PUPD) == 0)
        return false;
    if ((s->regs[TEA5761UK_TNCTRL_CONTROL] & TEA5761UK_TNCTRL_CONTROL_AFM) != 0)
        return false;
    return (s->regs[TEA5761UK_TNCTRL_AUDIO] & TEA5761UK_TNCTRL_AUDIO_MU) == 0;
}

static void tea5761uk_audio_update(TEA5761UKState *s) {
    static const AudioLineFormat FORMAT = { TEA5761UK_AUDIO_RATE_HZ, TEA5761UK_AUDIO_CHANNELS };
    bool active = tea5761uk_audio_is_active(s);

    if (active == s->audio_active)
        return;

    s->audio_active = active;
    if (active) {
        audio_line_start(s->audio_output, 0, &FORMAT);
        s->audio_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TEA5761UK_AUDIO_PERIOD_NS;
        timer_mod(s->audio_timer, s->audio_deadline_ns);
    } else {
        timer_del(s->audio_timer);
        audio_line_stop(s->audio_output, 0);
    }
}

static void tea5761uk_finish_write(TEA5761UKState *s) {
    if (!s->frequency_written)
        return;

    if ((s->regs[TEA5761UK_TNCTRL_CONTROL] & TEA5761UK_TNCTRL_CONTROL_PUPD) == 0) {
        s->regs[TEA5761UK_INTREG_STATUS] &= ~TEA5761UK_INTREG_STATUS_FRRFLAG;
        s->regs[TEA5761UK_TUNCHK_STATUS] &= ~TEA5761UK_TUNCHK_STATUS_LD;
        return;
    }

    s->regs[TEA5761UK_FRQCHK_HIGH] = s->regs[TEA5761UK_FRQSET_HIGH] & TEA5761UK_FRQSET_HIGH_FR_13_08;
    s->regs[TEA5761UK_FRQCHK_LOW] = s->regs[TEA5761UK_FRQSET_LOW];
    s->regs[TEA5761UK_TUNCHK_STATUS] |= TEA5761UK_TUNCHK_STATUS_LD;
    s->regs[TEA5761UK_INTREG_STATUS] |= TEA5761UK_INTREG_STATUS_FRRFLAG;
}

static int tea5761uk_event(I2CSlave *i2c, enum i2c_event event) {
    TEA5761UKState *s = TEA5761UK(i2c);

    switch (event) {
        case I2C_START_SEND:
        case I2C_START_SEND_ASYNC:
            s->write_index = 0;
            s->writing = true;
            s->frequency_written = false;
            break;

        case I2C_START_RECV:
            s->read_index = 0;
            s->writing = false;
            break;

        case I2C_FINISH:
            if (s->writing) {
                tea5761uk_finish_write(s);
                tea5761uk_audio_update(s);
            }
            s->writing = false;
            break;

        case I2C_NACK:
            s->writing = false;
            break;
    }

    return 0;
}

static uint8_t tea5761uk_recv(I2CSlave *i2c) {
    TEA5761UKState *s = TEA5761UK(i2c);
    uint8_t reg = s->read_index;
    uint8_t data = reg < ARRAY_SIZE(s->regs) ? s->regs[reg] : 0;

    DEBUG_IO_READ(&s->dbg, reg, 1, data);
    s->read_index++;
    if (reg == TEA5761UK_INTREG_STATUS || reg == TEA5761UK_INTREG_MASK)
        s->regs[reg] = 0;

    return data;
}

static int tea5761uk_send(I2CSlave *i2c, uint8_t data) {
    TEA5761UKState *s = TEA5761UK(i2c);
    uint8_t index = s->write_index;

    if (index < ARRAY_SIZE(TEA5761UK_WRITE_REGS)) {
        uint8_t reg = TEA5761UK_WRITE_REGS[index];
        DEBUG_IO_WRITE(&s->dbg, reg, 1, data);
        s->regs[reg] = data;
        if (reg == TEA5761UK_FRQSET_HIGH)
            s->frequency_written = true;
    }
    s->write_index++;

    return 0;
}

static void tea5761uk_reset(DeviceState *dev) {
    TEA5761UKState *s = TEA5761UK(dev);

    memcpy(s->regs, TEA5761UK_RESET_REGS, sizeof(TEA5761UK_RESET_REGS));
    s->read_index = 0;
    s->write_index = 0;
    s->writing = false;
    s->frequency_written = false;
    s->noise_state = 1;
    tea5761uk_audio_update(s);
}

static void tea5761uk_realize(DeviceState *dev, Error **_errp) {
    TEA5761UKState *s = TEA5761UK(dev);

    debug_channel_init_device_regs(&s->dbg, dev, &tea5761uk_debug_regs);
    s->audio_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tea5761uk_audio_timer_cb, s);
}

static void tea5761uk_unrealize(DeviceState *dev) {
    TEA5761UKState *s = TEA5761UK(dev);

    timer_free(s->audio_timer);
    debug_channel_cleanup(&s->dbg);
}

static void tea5761uk_init(Object *obj) {
    TEA5761UK(obj)->audio_output = audio_line_bus_init(DEVICE(obj), "OUT");
}

static void tea5761uk_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    device_class_set_legacy_reset(dc, tea5761uk_reset);
    dc->realize = tea5761uk_realize;
    dc->unrealize = tea5761uk_unrealize;
    k->event = tea5761uk_event;
    k->recv = tea5761uk_recv;
    k->send = tea5761uk_send;
}

static const TypeInfo tea5761uk_info = {
    .name = TYPE_TEA5761UK,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TEA5761UKState),
    .instance_init = tea5761uk_init,
    .class_init = tea5761uk_class_init,
};

static void tea5761uk_register_types(void) {
    type_register_static(&tea5761uk_info);
}
type_init(tea5761uk_register_types)
