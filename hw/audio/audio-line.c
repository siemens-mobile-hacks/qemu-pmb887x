/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/audio/audio-line.h"
#include "qapi/error.h"

#define TYPE_AUDIO_LINE_SINK_ENDPOINT "audio-line-sink-endpoint"
OBJECT_DECLARE_SIMPLE_TYPE(AudioLineSinkEndpoint, AUDIO_LINE_SINK_ENDPOINT)

struct AudioLineBus {
    BusState parent_obj;
};

struct AudioLineSinkEndpoint {
    DeviceState parent_obj;
    AudioLineSink *sink;
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

    AUDIO_LINE_SINK_ENDPOINT(dev)->sink = port->sink;
    return dev;
}

void audio_line_sink_port_init(AudioLineSinkPort *port, DeviceState *dev,
                               const char *name, AudioLineSink *sink)
{
    port->sink = sink;
    qdev_init_bus_port(dev, &port->parent, name, TYPE_AUDIO_LINE_BUS,
                       audio_line_sink_port_create_endpoint);
}

static AudioLineSink *audio_line_get_sink(AudioLineBus *bus)
{
    BusChild *child = QTAILQ_FIRST(&BUS(bus)->children);

    return child ? AUDIO_LINE_SINK_ENDPOINT(child->child)->sink : NULL;
}

void audio_line_start(AudioLineBus *bus, unsigned stream,
                      const AudioLineFormat *format)
{
    AudioLineSink *sink = audio_line_get_sink(bus);

    assert(format->sample_rate_hz != 0 && format->channels != 0);
    if (sink) {
        AUDIO_LINE_SINK_GET_CLASS(sink)->start(sink, stream, format);
    }
}

void audio_line_stop(AudioLineBus *bus, unsigned stream)
{
    AudioLineSink *sink = audio_line_get_sink(bus);

    if (sink) {
        AUDIO_LINE_SINK_GET_CLASS(sink)->stop(sink, stream);
    }
}

void audio_line_write(AudioLineBus *bus, unsigned stream,
                      const int16_t *frames, size_t frame_count)
{
    AudioLineSink *sink = audio_line_get_sink(bus);

    if (sink) {
        AUDIO_LINE_SINK_GET_CLASS(sink)->write(sink, stream, frames,
                                               frame_count);
    }
}

static bool audio_line_bus_check_address(BusState *bus, DeviceState *_dev,
                                         Error **errp)
{
    if (!QTAILQ_EMPTY(&bus->children)) {
        error_setg(errp, "audio line already has an input");
        return false;
    }
    return true;
}

static void audio_line_bus_class_init(ObjectClass *klass, const void *_data)
{
    BUS_CLASS(klass)->check_address = audio_line_bus_check_address;
}

static void audio_line_sink_endpoint_class_init(ObjectClass *klass,
                                                const void *_data)
{
    DEVICE_CLASS(klass)->bus_type = TYPE_AUDIO_LINE_BUS;
}

static const TypeInfo audio_line_types[] = {
    {
        .name = TYPE_AUDIO_LINE_BUS,
        .parent = TYPE_BUS,
        .instance_size = sizeof(AudioLineBus),
        .class_init = audio_line_bus_class_init,
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
