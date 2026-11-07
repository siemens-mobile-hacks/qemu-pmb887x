/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/audio/audio-line.h"
#include "hw/audio/pcm-out.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/debug-channel.h"

#define TEST_BUS_OWNER "test-bus-owner"

static int16_t written[4];
static size_t written_frames;

void debug_channel_init_device(DebugChannel *_ch, DeviceState *_dev)
{
}

void debug_channel_cleanup(DebugChannel *_ch)
{
}

void pcm_out_init(PCMOut *out, AudioBackend *_be, const char *_name,
                  uint32_t _prefill_ms, const DebugChannel *_dbg)
{
    *out = (PCMOut) { };
}

void pcm_out_cleanup(PCMOut *_out)
{
}

void pcm_out_set_format(PCMOut *out, uint32_t _rate_hz, uint32_t channels)
{
    out->channels = channels;
}

void pcm_out_set_active(PCMOut *_out, bool _active)
{
}

void pcm_out_stop(PCMOut *out)
{
    out->channels = 0;
}

void pcm_out_write(PCMOut *out, const int16_t *frames, size_t frame_count)
{
    memcpy(written, frames, frame_count * out->channels * sizeof(*frames));
    written_frames = frame_count;
}

static const TypeInfo test_bus_owner_info = {
    .name = TEST_BUS_OWNER,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(DeviceState),
};

typedef struct TestSpeaker {
    DeviceState *owner;
    DeviceState *speaker;
    AudioLineBus *line;
} TestSpeaker;

static TestSpeaker test_speaker_create(int32_t gain_mb)
{
    TestSpeaker t = {
        .owner = qdev_new(TEST_BUS_OWNER),
        .speaker = qdev_new("speaker"),
    };

    qdev_realize(t.owner, NULL, &error_fatal);
    t.line = audio_line_bus_init(t.owner, "line");
    qdev_prop_set_int32(t.speaker, "gain", gain_mb);
    object_property_set_link(OBJECT(t.speaker), "port.IN", OBJECT(t.line),
                             &error_fatal);
    qdev_realize(t.speaker, NULL, &error_fatal);
    return t;
}

static void test_speaker_destroy(TestSpeaker *t)
{
    object_unparent(OBJECT(t->speaker));
    object_unref(OBJECT(t->speaker));
    object_unparent(OBJECT(t->owner));
    object_unref(OBJECT(t->owner));
}

static void test_gain(void)
{
    static const AudioLineFormat FORMAT = { 8000, 2 };
    static const struct {
        int32_t speaker_gain_mb;
        int line_gain_mb;
        int16_t input[2];
        int16_t output[2];
    } cases[] = {
        { 0, 0, { 10000, -10000 }, { 10000, -10000 } },
        /* round(65536 * 10^(-6/20)) = 32846, round(10000 * 32846 / 65536) = 5012 */
        { 0, -600, { 10000, -10000 }, { 5012, -5012 } },
        { 2250, -2250, { 1234, -1234 }, { 1234, -1234 } },
        { 0, 2000, { 32767, -32768 }, { 32767, -32768 } },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        TestSpeaker t = test_speaker_create(cases[i].speaker_gain_mb);

        audio_line_set_gain(t.line, 3, cases[i].line_gain_mb);
        audio_line_start(t.line, 3, &FORMAT);
        audio_line_write(t.line, 3, cases[i].input, 1);
        g_assert_cmpuint(written_frames, ==, 1);
        g_assert_cmpint(written[0], ==, cases[i].output[0]);
        g_assert_cmpint(written[1], ==, cases[i].output[1]);
        test_speaker_destroy(&t);
    }
}

static void test_write_before_start(void)
{
    static const int16_t FRAME[] = { 1000 };
    TestSpeaker t = test_speaker_create(0);

    written_frames = 0;
    audio_line_write(t.line, 1, FRAME, 1);
    audio_line_set_gain(t.line, 1, -600);
    audio_line_write(t.line, 1, FRAME, 1);
    g_assert_cmpuint(written_frames, ==, 0);
    test_speaker_destroy(&t);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    type_register_static(&test_bus_owner_info);
    object_property_add_new_container(
        object_property_add_new_container(object_get_root(), "machine"),
        "unattached");

    g_test_add_func("/audio/speaker/gain", test_gain);
    g_test_add_func("/audio/speaker/write-before-start",
                    test_write_before_start);
    return g_test_run();
}
