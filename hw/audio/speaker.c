/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include <math.h>

#include "hw/audio/audio-line.h"
#include "hw/audio/pcm-out.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "system/debug-channel.h"

#define TYPE_SPEAKER "speaker"
OBJECT_DECLARE_SIMPLE_TYPE(Speaker, SPEAKER)

/* Covers sources that deliver bursts of about 20 ms plus the 10 ms audio mixer period. */
#define SPEAKER_PREFILL_MS 60
#define SPEAKER_UNITY_GAIN_Q16 (1 << 16)
#define SPEAKER_SCALE_SAMPLES 256

typedef struct SpeakerStream {
    PCMOut out;
    char *name;
    uint32_t channels;
    int line_gain_db;
    uint32_t gain_q16;
} SpeakerStream;

struct Speaker {
    DeviceState parent_obj;
    DebugChannel dbg;
    AudioLineSinkPort input;
    GPtrArray *streams;
    int32_t gain_db;
};

static uint32_t speaker_db_to_q16(int gain_db)
{
    return lround(SPEAKER_UNITY_GAIN_Q16 * pow(10.0, gain_db / 20.0));
}

static SpeakerStream *speaker_find_stream(Speaker *s, unsigned stream)
{
    return stream < s->streams->len ? g_ptr_array_index(s->streams, stream) : NULL;
}

static SpeakerStream *speaker_add_stream(Speaker *s, unsigned stream)
{
    SpeakerStream *entry = g_new0(SpeakerStream, 1);
    const char *name = DEVICE(s)->id ? DEVICE(s)->id : TYPE_SPEAKER;

    entry->name = g_strdup_printf("%s.%u", name, stream);
    entry->gain_q16 = speaker_db_to_q16(s->gain_db);
    pcm_out_init(&entry->out, NULL, entry->name, SPEAKER_PREFILL_MS, &s->dbg);
    if (stream >= s->streams->len) {
        g_ptr_array_set_size(s->streams, stream + 1);
    }
    g_ptr_array_index(s->streams, stream) = entry;
    return entry;
}

static void speaker_free_stream(gpointer data)
{
    SpeakerStream *entry = data;

    if (!entry) {
        return;
    }
    pcm_out_cleanup(&entry->out);
    g_free(entry->name);
    g_free(entry);
}

static SpeakerStream *speaker_get_stream(Speaker *s, unsigned stream)
{
    SpeakerStream *entry = speaker_find_stream(s, stream);

    return entry ? entry : speaker_add_stream(s, stream);
}

static void speaker_start(AudioLineSink *sink, unsigned stream,
                          const AudioLineFormat *format)
{
    SpeakerStream *entry = speaker_get_stream(SPEAKER(sink), stream);

    entry->channels = format->channels;
    pcm_out_set_format(&entry->out, format->sample_rate_hz, format->channels);
    pcm_out_set_active(&entry->out, true);
}

static void speaker_stop(AudioLineSink *sink, unsigned stream)
{
    SpeakerStream *entry = speaker_find_stream(SPEAKER(sink), stream);

    if (entry) {
        pcm_out_set_active(&entry->out, false);
    }
}

static void speaker_set_gain(AudioLineSink *sink, unsigned stream, int gain_db)
{
    Speaker *s = SPEAKER(sink);
    SpeakerStream *entry = speaker_get_stream(s, stream);

    if (entry->line_gain_db == gain_db) {
        return;
    }
    entry->line_gain_db = gain_db;
    entry->gain_q16 = speaker_db_to_q16(s->gain_db + gain_db);
}

static void speaker_write_scaled(SpeakerStream *entry, const int16_t *frames,
                                 size_t frame_count)
{
    int16_t scaled[SPEAKER_SCALE_SAMPLES];
    size_t chunk_frames = ARRAY_SIZE(scaled) / entry->channels;

    while (frame_count > 0) {
        size_t count = MIN(frame_count, chunk_frames);
        size_t samples = count * entry->channels;

        for (size_t i = 0; i < samples; i++) {
            int64_t product = (int64_t) frames[i] * entry->gain_q16;
            int64_t sample = (product + (SPEAKER_UNITY_GAIN_Q16 / 2)) >> 16;

            scaled[i] = MIN(MAX(sample, INT16_MIN), INT16_MAX);
        }
        pcm_out_write(&entry->out, scaled, count);
        frames += samples;
        frame_count -= count;
    }
}

static void speaker_write(AudioLineSink *sink, unsigned stream,
                          const int16_t *frames, size_t frame_count)
{
    SpeakerStream *entry = speaker_find_stream(SPEAKER(sink), stream);

    if (!entry || entry->channels == 0) {
        return;
    }
    if (entry->gain_q16 == SPEAKER_UNITY_GAIN_Q16) {
        pcm_out_write(&entry->out, frames, frame_count);
        return;
    }
    speaker_write_scaled(entry, frames, frame_count);
}

static void speaker_reset(DeviceState *dev)
{
    Speaker *s = SPEAKER(dev);

    for (unsigned i = 0; i < s->streams->len; i++) {
        SpeakerStream *entry = g_ptr_array_index(s->streams, i);

        if (entry) {
            entry->channels = 0;
            pcm_out_stop(&entry->out);
        }
    }
}

static void speaker_realize(DeviceState *dev, Error **_errp)
{
    Speaker *s = SPEAKER(dev);

    debug_channel_init_device(&s->dbg, dev);
    s->streams = g_ptr_array_new_with_free_func(speaker_free_stream);
}

static void speaker_unrealize(DeviceState *dev)
{
    Speaker *s = SPEAKER(dev);

    g_ptr_array_unref(s->streams);
    s->streams = NULL;
    debug_channel_cleanup(&s->dbg);
}

static const Property speaker_properties[] = {
    DEFINE_PROP_INT32("gain_db", Speaker, gain_db, 0),
};

static void speaker_init(Object *obj)
{
    Speaker *s = SPEAKER(obj);

    audio_line_sink_port_init(&s->input, DEVICE(obj), "IN",
                              AUDIO_LINE_SINK(obj));
}

static void speaker_class_init(ObjectClass *klass, const void *_data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    AudioLineSinkClass *sink_class = AUDIO_LINE_SINK_CLASS(klass);

    device_class_set_props(dc, speaker_properties);
    dc->realize = speaker_realize;
    dc->unrealize = speaker_unrealize;
    device_class_set_legacy_reset(dc, speaker_reset);
    sink_class->start = speaker_start;
    sink_class->stop = speaker_stop;
    sink_class->set_gain = speaker_set_gain;
    sink_class->write = speaker_write;
}

static const TypeInfo speaker_types[] = {
    {
        .name = TYPE_SPEAKER,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(Speaker),
        .instance_init = speaker_init,
        .class_init = speaker_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_AUDIO_LINE_SINK },
            { }
        },
    },
};

DEFINE_TYPES(speaker_types)
