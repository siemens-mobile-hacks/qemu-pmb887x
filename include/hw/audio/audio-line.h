/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_AUDIO_AUDIO_LINE_H
#define HW_AUDIO_AUDIO_LINE_H

#include "hw/core/bus-port.h"
#include "qom/object.h"

/*
 * A line connects one audio output to one input. Sources routed to the
 * same output stay separate streams; the host audio backend mixes them.
 */
#define TYPE_AUDIO_LINE_BUS "audio-line-bus"
OBJECT_DECLARE_SIMPLE_TYPE(AudioLineBus, AUDIO_LINE_BUS)
#define TYPE_AUDIO_LINE_SINK "audio-line-sink"

typedef struct AudioLineSink AudioLineSink;
typedef struct AudioLineSinkClass AudioLineSinkClass;
typedef struct AudioLineSinkPort AudioLineSinkPort;
typedef struct AudioLineFormat AudioLineFormat;

DECLARE_CLASS_CHECKERS(AudioLineSinkClass, AUDIO_LINE_SINK,
                       TYPE_AUDIO_LINE_SINK)
#define AUDIO_LINE_SINK(obj) \
    INTERFACE_CHECK(AudioLineSink, (obj), TYPE_AUDIO_LINE_SINK)

struct AudioLineFormat {
    uint32_t sample_rate_hz;
    uint32_t channels;
};

struct AudioLineSinkClass {
    InterfaceClass parent_class;
    void (*start)(AudioLineSink *sink, unsigned stream,
                  const AudioLineFormat *format);
    void (*stop)(AudioLineSink *sink, unsigned stream);
    void (*set_gain)(AudioLineSink *sink, unsigned stream, int gain_db);
    void (*write)(AudioLineSink *sink, unsigned stream,
                  const int16_t *frames, size_t frame_count);
};

struct AudioLineSinkPort {
    QDevBusPort parent;
    AudioLineSink *sink;
};

AudioLineBus *audio_line_bus_init(DeviceState *parent, const char *name);
void audio_line_sink_port_init(AudioLineSinkPort *port, DeviceState *dev,
                               const char *name, AudioLineSink *sink);
void audio_line_start(AudioLineBus *bus, unsigned stream,
                      const AudioLineFormat *format);
void audio_line_stop(AudioLineBus *bus, unsigned stream);
void audio_line_set_gain(AudioLineBus *bus, unsigned stream, int gain_db);
void audio_line_write(AudioLineBus *bus, unsigned stream,
                      const int16_t *frames, size_t frame_count);

#endif
