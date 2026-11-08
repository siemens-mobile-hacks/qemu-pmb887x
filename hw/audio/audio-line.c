/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/audio/audio-line.h"

#define TYPE_AUDIO_LINE_SINK_ENDPOINT "audio-line-sink-endpoint"
OBJECT_DECLARE_SIMPLE_TYPE(AudioLineSinkEndpoint, AUDIO_LINE_SINK_ENDPOINT)

typedef struct AudioLineStream {
    bool started;
    AudioLineFormat format;
    bool gain_set;
    int gain_mb;
} AudioLineStream;

struct AudioLineBus {
    BusState parent_obj;
    GArray *streams;
};

struct AudioLineSinkEndpoint {
    DeviceState parent_obj;
    AudioLineSinkPort *port;
};

AudioLineBus *audio_line_bus_init(DeviceState *parent, const char *name)
{
    return AUDIO_LINE_BUS(qbus_new(TYPE_AUDIO_LINE_BUS, parent, name));
}

static DeviceState *audio_line_sink_port_create_endpoint(QDevBusPort *base,
                                                         Error **_errp)
{
    AudioLineSinkPort *port = container_of(base, AudioLineSinkPort, parent);
    DeviceState *dev = qdev_new(TYPE_AUDIO_LINE_SINK_ENDPOINT);

    AUDIO_LINE_SINK_ENDPOINT(dev)->port = port;
    return dev;
}

void audio_line_sink_port_init(AudioLineSinkPort *port, DeviceState *dev,
                               const char *name, AudioLineSink *sink)
{
    port->sink = sink;
    qdev_init_bus_port(dev, &port->parent, name, TYPE_AUDIO_LINE_BUS,
                       audio_line_sink_port_create_endpoint);
}

static AudioLineSinkPort *audio_line_child_get_port(BusChild *kid)
{
    return AUDIO_LINE_SINK_ENDPOINT(kid->child)->port;
}

static AudioLineStream *audio_line_get_stream(AudioLineBus *bus,
                                              unsigned stream)
{
    if (stream >= bus->streams->len) {
        g_array_set_size(bus->streams, stream + 1);
    }
    return &g_array_index(bus->streams, AudioLineStream, stream);
}

void audio_line_start(AudioLineBus *bus, unsigned stream,
                      const AudioLineFormat *format)
{
    AudioLineStream *state = audio_line_get_stream(bus, stream);
    BusChild *kid;

    assert(format->sample_rate_hz != 0 && format->channels != 0);
    state->started = true;
    state->format = *format;
    QTAILQ_FOREACH(kid, &BUS(bus)->children, sibling) {
        AudioLineSinkPort *port = audio_line_child_get_port(kid);

        AUDIO_LINE_SINK_GET_CLASS(port->sink)->start(port, stream, format);
    }
}

void audio_line_stop(AudioLineBus *bus, unsigned stream)
{
    BusChild *kid;

    audio_line_get_stream(bus, stream)->started = false;
    QTAILQ_FOREACH(kid, &BUS(bus)->children, sibling) {
        AudioLineSinkPort *port = audio_line_child_get_port(kid);

        AUDIO_LINE_SINK_GET_CLASS(port->sink)->stop(port, stream);
    }
}

void audio_line_set_gain(AudioLineBus *bus, unsigned stream, int gain_mb)
{
    AudioLineStream *state = audio_line_get_stream(bus, stream);
    BusChild *kid;

    state->gain_set = true;
    state->gain_mb = gain_mb;
    QTAILQ_FOREACH(kid, &BUS(bus)->children, sibling) {
        AudioLineSinkPort *port = audio_line_child_get_port(kid);

        AUDIO_LINE_SINK_GET_CLASS(port->sink)->set_gain(port, stream, gain_mb);
    }
}

void audio_line_write(AudioLineBus *bus, unsigned stream,
                      const int16_t *frames, size_t frame_count)
{
    BusChild *kid;

    QTAILQ_FOREACH(kid, &BUS(bus)->children, sibling) {
        AudioLineSinkPort *port = audio_line_child_get_port(kid);

        AUDIO_LINE_SINK_GET_CLASS(port->sink)->write(port, stream, frames,
                                                     frame_count);
    }
}

static void audio_line_bus_instance_init(Object *obj)
{
    AUDIO_LINE_BUS(obj)->streams = g_array_new(false, true,
                                               sizeof(AudioLineStream));
}

static void audio_line_bus_finalize(Object *obj)
{
    g_array_free(AUDIO_LINE_BUS(obj)->streams, true);
}

static void audio_line_sink_endpoint_realize(DeviceState *dev, Error **_errp)
{
    AudioLineBus *bus = AUDIO_LINE_BUS(qdev_get_parent_bus(dev));
    AudioLineSinkPort *port = AUDIO_LINE_SINK_ENDPOINT(dev)->port;
    AudioLineSinkClass *klass = AUDIO_LINE_SINK_GET_CLASS(port->sink);

    for (unsigned stream = 0; stream < bus->streams->len; stream++) {
        AudioLineStream *state = &g_array_index(bus->streams, AudioLineStream,
                                                stream);

        if (state->gain_set) {
            klass->set_gain(port, stream, state->gain_mb);
        }
        if (state->started) {
            klass->start(port, stream, &state->format);
        }
    }
}

static void audio_line_sink_endpoint_unrealize(DeviceState *dev)
{
    AudioLineBus *bus = AUDIO_LINE_BUS(qdev_get_parent_bus(dev));
    AudioLineSinkPort *port = AUDIO_LINE_SINK_ENDPOINT(dev)->port;
    AudioLineSinkClass *klass = AUDIO_LINE_SINK_GET_CLASS(port->sink);

    for (unsigned stream = 0; stream < bus->streams->len; stream++) {
        AudioLineStream *state = &g_array_index(bus->streams, AudioLineStream,
                                                stream);

        if (state->started) {
            klass->stop(port, stream);
        }
        if (state->gain_set) {
            klass->set_gain(port, stream, 0);
        }
    }
}

static void audio_line_sink_endpoint_class_init(ObjectClass *klass,
                                                const void *_data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->bus_type = TYPE_AUDIO_LINE_BUS;
    dc->realize = audio_line_sink_endpoint_realize;
    dc->unrealize = audio_line_sink_endpoint_unrealize;
}

static const TypeInfo audio_line_types[] = {
    {
        .name = TYPE_AUDIO_LINE_BUS,
        .parent = TYPE_BUS,
        .instance_size = sizeof(AudioLineBus),
        .instance_init = audio_line_bus_instance_init,
        .instance_finalize = audio_line_bus_finalize,
    },
    {
        .name = TYPE_AUDIO_LINE_SINK_ENDPOINT,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(AudioLineSinkEndpoint),
        .class_init = audio_line_sink_endpoint_class_init,
    },
    {
        .name = TYPE_AUDIO_LINE_SINK,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(AudioLineSinkClass),
    },
};

DEFINE_TYPES(audio_line_types)
