/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/audio/audio-line.h"
#include "hw/core/irq.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/debug-channel.h"

#define TYPE_TEST_BUS_OWNER "test-bus-owner"
#define TYPE_TEST_LINE_SINK "test-line-sink"
#define TEST_STREAM_COUNT 4

OBJECT_DECLARE_SIMPLE_TYPE(TestLineSink, TEST_LINE_SINK)

typedef struct TestStream {
    AudioLineFormat format;
    int gain_mb;
    int16_t sample;
    size_t frames;
} TestStream;

struct TestLineSink {
    DeviceState parent_obj;
    AudioLineSinkPort port;
    TestStream streams[TEST_STREAM_COUNT];
};

void debug_channel_init_device(DebugChannel *_ch, DeviceState *_dev)
{
}

static TestStream *test_sink_get_stream(AudioLineSinkPort *port,
                                        unsigned stream)
{
    g_assert_cmpuint(stream, <, TEST_STREAM_COUNT);
    return &container_of(port, TestLineSink, port)->streams[stream];
}

static void test_sink_start(AudioLineSinkPort *port, unsigned stream,
                            const AudioLineFormat *format)
{
    test_sink_get_stream(port, stream)->format = *format;
}

static void test_sink_stop(AudioLineSinkPort *port, unsigned stream)
{
    test_sink_get_stream(port, stream)->format = (AudioLineFormat) { };
}

static void test_sink_set_gain(AudioLineSinkPort *port, unsigned stream,
                               int gain_mb)
{
    test_sink_get_stream(port, stream)->gain_mb = gain_mb;
}

static void test_sink_write(AudioLineSinkPort *port, unsigned stream,
                            const int16_t *frames, size_t frame_count)
{
    TestStream *s = test_sink_get_stream(port, stream);

    s->sample = frames[0];
    s->frames += frame_count;
}

static void test_sink_init(Object *obj)
{
    TestLineSink *sink = TEST_LINE_SINK(obj);

    audio_line_sink_port_init(&sink->port, DEVICE(obj), "in",
                              AUDIO_LINE_SINK(obj));
}

static void test_sink_class_init(ObjectClass *oc, const void *_data)
{
    AudioLineSinkClass *k = AUDIO_LINE_SINK_CLASS(oc);

    k->start = test_sink_start;
    k->stop = test_sink_stop;
    k->set_gain = test_sink_set_gain;
    k->write = test_sink_write;
}

static const TypeInfo test_types[] = {
    {
        .name = TYPE_TEST_BUS_OWNER,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(DeviceState),
    },
    {
        .name = TYPE_TEST_LINE_SINK,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(TestLineSink),
        .instance_init = test_sink_init,
        .class_init = test_sink_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_AUDIO_LINE_SINK },
            { }
        },
    },
};

typedef struct TestNode {
    DeviceState *owner;
    DeviceState *node;
    TestLineSink *sink;
    AudioLineBus *inputs[2];
} TestNode;

static TestNode test_node_create(const char *type)
{
    TestNode t = {
        .owner = qdev_new(TYPE_TEST_BUS_OWNER),
        .node = qdev_new(type),
        .sink = TEST_LINE_SINK(qdev_new(TYPE_TEST_LINE_SINK)),
    };

    qdev_realize(t.owner, NULL, &error_fatal);
    t.inputs[0] = audio_line_bus_init(t.owner, "in0");
    t.inputs[1] = audio_line_bus_init(t.owner, "in1");
    object_property_set_link(OBJECT(t.node), "port.in0", OBJECT(t.inputs[0]),
                             &error_fatal);
    object_property_set_link(OBJECT(t.node), "port.in1", OBJECT(t.inputs[1]),
                             &error_fatal);
    qdev_realize(t.node, NULL, &error_fatal);
    object_property_set_link(OBJECT(t.sink), "port.in",
                             OBJECT(qdev_get_child_bus(t.node, "out")),
                             &error_fatal);
    qdev_realize(DEVICE(t.sink), NULL, &error_fatal);
    return t;
}

static void test_node_destroy(TestNode *t)
{
    object_unparent(OBJECT(t->sink));
    object_unref(OBJECT(t->sink));
    object_unparent(OBJECT(t->node));
    object_unref(OBJECT(t->node));
    object_unparent(OBJECT(t->owner));
    object_unref(OBJECT(t->owner));
}

static const AudioLineFormat FORMAT_MONO = { 8000, 1 };
static const AudioLineFormat FORMAT_STEREO = { 48000, 2 };

static void test_junction(void)
{
    static const int16_t FIRST[] = { 100 };
    static const int16_t SECOND[] = { 200, 201 };
    TestNode t = test_node_create("audio-junction");
    TestStream *streams = t.sink->streams;

    audio_line_set_gain(t.inputs[0], 0, -300);
    audio_line_start(t.inputs[0], 0, &FORMAT_MONO);
    audio_line_start(t.inputs[1], 0, &FORMAT_STEREO);
    audio_line_write(t.inputs[0], 0, FIRST, 1);
    audio_line_write(t.inputs[1], 0, SECOND, 1);

    g_assert_cmpuint(streams[0].format.sample_rate_hz, ==, 8000);
    g_assert_cmpint(streams[0].gain_mb, ==, -300);
    g_assert_cmpint(streams[0].sample, ==, 100);
    g_assert_cmpuint(streams[1].format.channels, ==, 2);
    g_assert_cmpint(streams[1].sample, ==, 200);

    audio_line_stop(t.inputs[0], 0);
    g_assert_cmpuint(streams[0].format.channels, ==, 0);
    g_assert_cmpuint(streams[1].format.channels, ==, 2);
    test_node_destroy(&t);
}

static void test_switch(void)
{
    static const int16_t FRAME[] = { 300 };
    TestNode t = test_node_create("audio-switch");
    TestStream *streams = t.sink->streams;
    qemu_irq select = qdev_get_gpio_in_named(t.node, "select", 0);

    audio_line_start(t.inputs[0], 0, &FORMAT_MONO);
    audio_line_start(t.inputs[1], 0, &FORMAT_STEREO);
    g_assert_cmpuint(streams[0].format.channels, ==, 1);
    g_assert_cmpuint(streams[1].format.channels, ==, 0);

    qemu_set_irq(select, 1);
    g_assert_cmpuint(streams[0].format.channels, ==, 0);
    g_assert_cmpuint(streams[1].format.channels, ==, 2);

    audio_line_write(t.inputs[0], 0, FRAME, 1);
    audio_line_write(t.inputs[1], 0, FRAME, 1);
    g_assert_cmpuint(streams[0].frames, ==, 0);
    g_assert_cmpuint(streams[1].frames, ==, 1);
    test_node_destroy(&t);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    type_register_static_array(test_types, ARRAY_SIZE(test_types));
    object_property_add_new_container(
        object_property_add_new_container(object_get_root(), "machine"),
        "unattached");

    g_test_add_func("/audio/router/junction", test_junction);
    g_test_add_func("/audio/router/switch", test_switch);
    return g_test_run();
}
