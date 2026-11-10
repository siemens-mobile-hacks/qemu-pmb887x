/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/audio/audio-router.h"
#include "system/debug-channel.h"

#define TYPE_AUDIO_JUNCTION "audio-junction"
OBJECT_DECLARE_SIMPLE_TYPE(AudioJunction, AUDIO_JUNCTION)

#define AUDIO_JUNCTION_INPUT_COUNT 2

struct AudioJunction {
    DeviceState parent_obj;
    DebugChannel dbg;
    AudioRouterInput inputs[AUDIO_JUNCTION_INPUT_COUNT];
    AudioLineBus *output;
    AudioRouter router;
};

static void audio_junction_realize(DeviceState *dev, Error **_errp)
{
    debug_channel_init_device(&AUDIO_JUNCTION(dev)->dbg, dev);
}

static void audio_junction_unrealize(DeviceState *dev)
{
    debug_channel_cleanup(&AUDIO_JUNCTION(dev)->dbg);
}

static void audio_junction_init(Object *obj)
{
    AudioJunction *j = AUDIO_JUNCTION(obj);

    j->output = audio_line_bus_init(DEVICE(obj), "out");
    audio_router_init(&j->router, &j->output, 1, AUDIO_JUNCTION_INPUT_COUNT,
                      &j->dbg);
    audio_router_input_init(&j->inputs[0], DEVICE(obj), "in0", &j->router, 0);
    audio_router_input_init(&j->inputs[1], DEVICE(obj), "in1", &j->router, 1);
    audio_router_set_route(&j->router, 0, 0, true, 0);
    audio_router_set_route(&j->router, 0, 1, true, 0);
}

static void audio_junction_finalize(Object *obj)
{
    audio_router_cleanup(&AUDIO_JUNCTION(obj)->router);
}

static void audio_junction_class_init(ObjectClass *klass, const void *_data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = audio_junction_realize;
    dc->unrealize = audio_junction_unrealize;
    audio_router_sink_class_init(AUDIO_LINE_SINK_CLASS(klass));
}

static const TypeInfo audio_junction_types[] = {
    {
        .name = TYPE_AUDIO_JUNCTION,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(AudioJunction),
        .instance_init = audio_junction_init,
        .instance_finalize = audio_junction_finalize,
        .class_init = audio_junction_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_AUDIO_LINE_SINK },
            { }
        },
    },
};

DEFINE_TYPES(audio_junction_types)
