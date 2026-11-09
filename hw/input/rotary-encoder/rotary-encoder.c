/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "system/debug-channel.h"

#define TYPE_ROTARY_ENCODER "rotary-encoder"
OBJECT_DECLARE_SIMPLE_TYPE(RotaryEncoder, ROTARY_ENCODER)

struct RotaryEncoder {
    SysBusDevice parent_obj;
    DebugChannel dbg;

    uint32_t steps;
    uint32_t edge_us;
    uint32_t repeat_delay_us;
    uint32_t detent_us;

    qemu_irq a_out;
    qemu_irq b_out;

    QEMUTimer *timer;
    int64_t quarter_ns;
    bool cw_held;
    bool ccw_held;
    int dir;
    bool repeating;
    uint32_t quarters_left;
    bool a_level;
    bool b_level;
};

static void rotary_encoder_set_a(RotaryEncoder *s, bool level) {
    s->a_level = level;
    qemu_set_irq(s->a_out, level);
}

static void rotary_encoder_set_b(RotaryEncoder *s, bool level) {
    s->b_level = level;
    qemu_set_irq(s->b_out, level);
}

static void rotary_encoder_stop(RotaryEncoder *s) {
    timer_del(s->timer);
    s->dir = 0;
    s->quarters_left = 0;
    rotary_encoder_set_a(s, false);
    rotary_encoder_set_b(s, false);
}

static void rotary_encoder_arm(RotaryEncoder *s, int64_t delay_ns) {
    timer_mod_ns(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
}

static int rotary_encoder_held_dir(RotaryEncoder *s) {
    return s->cw_held ? 1 : (s->ccw_held ? -1 : 0);
}

static void rotary_encoder_start_detent(RotaryEncoder *s, int dir) {
    s->dir = dir;
    s->quarters_left = s->steps * 4;

    DEBUG_LOG(&s->dbg, "detent %s, %u pulses\n", dir > 0 ? "CW" : "CCW", s->steps);
    rotary_encoder_arm(s, s->quarter_ns);
}

static void rotary_encoder_timer_cb(void *opaque) {
    RotaryEncoder *s = opaque;

    if (s->quarters_left > 0) {
        bool b_leads = s->dir > 0;
        bool toggle_b = (s->quarters_left % 2 == 0) == b_leads;
        if (toggle_b) {
            rotary_encoder_set_b(s, !s->b_level);
        } else {
            rotary_encoder_set_a(s, !s->a_level);
        }
        s->quarters_left--;

        if (s->quarters_left > 0) {
            rotary_encoder_arm(s, s->quarter_ns);
            return;
        }

        s->dir = rotary_encoder_held_dir(s);
        if (!s->dir) {
            s->repeating = false;
            return;
        }

        uint32_t delay_us = s->repeating ? s->detent_us : s->repeat_delay_us;
        rotary_encoder_arm(s, (int64_t) delay_us * SCALE_US);
        s->repeating = true;
        return;
    }

    int dir = rotary_encoder_held_dir(s);
    if (!dir) {
        s->dir = 0;
        s->repeating = false;
        return;
    }
    rotary_encoder_start_detent(s, dir);
}

static void rotary_encoder_update(RotaryEncoder *s) {
    if (s->quarters_left > 0)
        return;

    int dir = rotary_encoder_held_dir(s);
    if (!dir) {
        timer_del(s->timer);
        s->dir = 0;
        s->repeating = false;
        return;
    }

    if (dir != s->dir) {
        s->repeating = false;
        rotary_encoder_start_detent(s, dir);
    }
}

static void rotary_encoder_cw_handler(void *opaque, int _n, int level) {
    RotaryEncoder *s = opaque;
    if (s->cw_held == !!level)
        return;
    s->cw_held = !!level;
    rotary_encoder_update(s);
}

static void rotary_encoder_ccw_handler(void *opaque, int _n, int level) {
    RotaryEncoder *s = opaque;
    if (s->ccw_held == !!level)
        return;
    s->ccw_held = !!level;
    rotary_encoder_update(s);
}

static void rotary_encoder_init(Object *obj) {
    DeviceState *dev = DEVICE(obj);
    RotaryEncoder *s = ROTARY_ENCODER(obj);

    qdev_init_gpio_out_named(dev, &s->a_out, "A", 1);
    qdev_init_gpio_out_named(dev, &s->b_out, "B", 1);
    qdev_init_gpio_in_named(dev, rotary_encoder_cw_handler, "CW", 1);
    qdev_init_gpio_in_named(dev, rotary_encoder_ccw_handler, "CCW", 1);
}

static void rotary_encoder_realize(DeviceState *dev, Error **errp) {
    RotaryEncoder *s = ROTARY_ENCODER(dev);

    if (!s->steps) {
        error_setg(errp, "rotary-encoder: 'steps' must be non-zero");
        return;
    }

    if (!s->edge_us || !s->detent_us || !s->repeat_delay_us) {
        error_setg(errp, "rotary-encoder: 'edge_us', 'detent_us' and 'repeat_delay_us' must be non-zero");
        return;
    }

    s->quarter_ns = (int64_t) s->edge_us * SCALE_US / 2;
    debug_channel_init_device(&s->dbg, dev);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rotary_encoder_timer_cb, s);
}

static void rotary_encoder_unrealize(DeviceState *dev) {
    RotaryEncoder *s = ROTARY_ENCODER(dev);
    timer_free(s->timer);
    s->timer = NULL;
    debug_channel_cleanup(&s->dbg);
}

static void rotary_encoder_reset(DeviceState *dev) {
    RotaryEncoder *s = ROTARY_ENCODER(dev);

    s->cw_held = false;
    s->ccw_held = false;
    s->repeating = false;
    rotary_encoder_stop(s);
}

static const Property rotary_encoder_properties[] = {
    DEFINE_PROP_UINT32("steps", RotaryEncoder, steps, 2),
    DEFINE_PROP_UINT32("edge_us", RotaryEncoder, edge_us, 2000),
    DEFINE_PROP_UINT32("repeat_delay_us", RotaryEncoder, repeat_delay_us, 400000),
    DEFINE_PROP_UINT32("detent_us", RotaryEncoder, detent_us, 200000),
};

static void rotary_encoder_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, rotary_encoder_properties);
    device_class_set_legacy_reset(dc, rotary_encoder_reset);
    dc->realize = rotary_encoder_realize;
    dc->unrealize = rotary_encoder_unrealize;
}

static const TypeInfo rotary_encoder_info = {
    .name = TYPE_ROTARY_ENCODER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RotaryEncoder),
    .instance_init = rotary_encoder_init,
    .class_init = rotary_encoder_class_init,
};

static void rotary_encoder_register_types(void) {
    type_register_static(&rotary_encoder_info);
}
type_init(rotary_encoder_register_types)
