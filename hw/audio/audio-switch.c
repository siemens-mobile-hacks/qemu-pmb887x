/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/audio/audio-router.h"
#include "system/debug-channel.h"

#define TYPE_AUDIO_SWITCH "audio-switch"
OBJECT_DECLARE_SIMPLE_TYPE(AudioSwitch, AUDIO_SWITCH)

#define AUDIO_SWITCH_INPUT_COUNT 2

struct AudioSwitch {
    DeviceState parent_obj;
    DebugChannel dbg;
    AudioRouterInput inputs[AUDIO_SWITCH_INPUT_COUNT];
    AudioLineBus *output;
    AudioRouter router;
};

static void audio_switch_select(void *opaque, int _line, int level)
{
    AudioSwitch *s = opaque;
    unsigned selected_input = level != 0;

    audio_router_set_route(&s->router, 0, 0, selected_input == 0, 0);
    audio_router_set_route(&s->router, 0, 1, selected_input == 1, 0);
    audio_router_update(&s->router);
}

static void audio_switch_realize(DeviceState *dev, Error **_errp)
{
    debug_channel_init_device(&AUDIO_SWITCH(dev)->dbg, dev);
}

static void audio_switch_unrealize(DeviceState *dev)
{
    debug_channel_cleanup(&AUDIO_SWITCH(dev)->dbg);
}

static void audio_switch_init(Object *obj)
{
    AudioSwitch *s = AUDIO_SWITCH(obj);

    s->output = audio_line_bus_init(DEVICE(obj), "out");
    audio_router_init(&s->router, &s->output, 1, AUDIO_SWITCH_INPUT_COUNT,
                      &s->dbg);
    audio_router_input_init(&s->inputs[0], DEVICE(obj), "in0", &s->router, 0);
    audio_router_input_init(&s->inputs[1], DEVICE(obj), "in1", &s->router, 1);
    audio_router_set_route(&s->router, 0, 0, true, 0);
    qdev_init_gpio_in_named(DEVICE(obj), audio_switch_select, "select", 1);
}

static void audio_switch_finalize(Object *obj)
{
    audio_router_cleanup(&AUDIO_SWITCH(obj)->router);
}

static void audio_switch_class_init(ObjectClass *klass, const void *_data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = audio_switch_realize;
    dc->unrealize = audio_switch_unrealize;
    audio_router_sink_class_init(AUDIO_LINE_SINK_CLASS(klass));
}

static const TypeInfo audio_switch_types[] = {
    {
        .name = TYPE_AUDIO_SWITCH,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(AudioSwitch),
        .instance_init = audio_switch_init,
        .instance_finalize = audio_switch_finalize,
        .class_init = audio_switch_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_AUDIO_LINE_SINK },
            { }
        },
    },
};

DEFINE_TYPES(audio_switch_types)
