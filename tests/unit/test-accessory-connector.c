/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/audio/audio-line.h"
#include "hw/audio/i2s.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/phone/accessory-connector.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/cpu-timers.h"
#include "system/debug-channel.h"

#define TYPE_TEST_BUS_OWNER "test-bus-owner"
#define TYPE_TEST_SINK "test-sink"
#define REPLUG_DELAY_MS 500
#define LEVEL_UNKNOWN -1

OBJECT_DECLARE_SIMPLE_TYPE(TestSink, TEST_SINK)

struct TestSink {
    DeviceState parent_obj;
    AudioLineSinkPort line_port;
    I2SSinkPort i2s_port;
    bool started;
};

enum {
    OUTPUT_DETECT,
    OUTPUT_KEY,
    OUTPUT_MIC,
    OUTPUT_COUNT,
};

typedef struct TestBench {
    DeviceState *bus_owner;
    AudioLineBus *line;
    AudioLineBus *other_line;
    I2STxBus *i2s;
    TestSink *sink;
    TestSink *second_sink;
    AccessoryConnector *connector;
} TestBench;

static int64_t virtual_clock_ns;
static int output_levels[OUTPUT_COUNT];

int64_t cpus_get_virtual_clock(void)
{
    return virtual_clock_ns;
}

void cpus_set_virtual_clock(int64_t new_time)
{
    virtual_clock_ns = new_time;
}

void debug_channel_init_device(DebugChannel *_ch, DeviceState *_dev)
{
}

static void advance_virtual_clock_ms(int64_t ms)
{
    virtual_clock_ns += ms * SCALE_MS;
    qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
}

static void test_sink_start(AudioLineSinkPort *port, unsigned _stream,
                            const AudioLineFormat *_format)
{
    container_of(port, TestSink, line_port)->started = true;
}

static void test_sink_stop(AudioLineSinkPort *port, unsigned _stream)
{
    container_of(port, TestSink, line_port)->started = false;
}

static void test_sink_set_gain(AudioLineSinkPort *_port, unsigned _stream,
                               int _gain_mb)
{
}

static void test_sink_write(AudioLineSinkPort *_port, unsigned _stream,
                            const int16_t *_frames, size_t _frame_count)
{
}

static void test_sink_init(Object *obj)
{
    TestSink *sink = TEST_SINK(obj);

    audio_line_sink_port_init(&sink->line_port, DEVICE(obj), "LINE",
                              AUDIO_LINE_SINK(obj));
    i2s_sink_port_init(&sink->i2s_port, DEVICE(obj), "I2S", I2S_SINK(obj));
}

static void test_sink_class_init(ObjectClass *klass, const void *_data)
{
    AudioLineSinkClass *sc = AUDIO_LINE_SINK_CLASS(klass);

    sc->start = test_sink_start;
    sc->stop = test_sink_stop;
    sc->set_gain = test_sink_set_gain;
    sc->write = test_sink_write;
}

static const TypeInfo test_types[] = {
    {
        .name = TYPE_TEST_BUS_OWNER,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(DeviceState),
    },
    {
        .name = TYPE_TEST_SINK,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(TestSink),
        .instance_init = test_sink_init,
        .class_init = test_sink_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_AUDIO_LINE_SINK },
            { TYPE_I2S_SINK },
            { }
        },
    },
};

static void record_output(void *_opaque, int output, int level)
{
    output_levels[output] = level != 0;
}

static void object_destroy(Object *obj)
{
    object_unparent(obj);
    object_unref(obj);
}

static TestSink *create_sink(void)
{
    TestSink *sink = TEST_SINK(qdev_new(TYPE_TEST_SINK));

    qdev_realize(DEVICE(sink), NULL, &error_fatal);
    return sink;
}

static AccessoryConnector *create_connector(const AccessoryProfile *profiles,
                                            size_t count)
{
    AccessoryConnector *connector =
        ACCESSORY_CONNECTOR(qdev_new(TYPE_ACCESSORY_CONNECTOR));
    qemu_irq *outputs = qemu_allocate_irqs(record_output, NULL, OUTPUT_COUNT);

    accessory_connector_set_profiles(connector, profiles, count);
    for (unsigned output = 0; output < OUTPUT_COUNT; output++) {
        qdev_connect_gpio_out_named(DEVICE(connector), "out", output,
                                    outputs[output]);
    }
    g_free(outputs);
    return connector;
}

/*
 * none:    detect high
 * headset: detect low, key follows the inverted input, mic follows the
 *          input, sink LINE on the line
 * speaker: detect low, key high, sink LINE on the other line
 * dock-a:  sink I2S on the I2S bus
 * dock-b:  second sink I2S on the same I2S bus
 */
static void bench_init(TestBench *bench, const char *unplugged)
{
    static const AudioLineFormat FORMAT = { 48000, 2 };
    static const AccessoryGpioRoute NONE_GPIO[] = {
        { .output = OUTPUT_DETECT, .level = true },
    };
    static const AccessoryGpioRoute HEADSET_GPIO[] = {
        { .output = OUTPUT_KEY, .from_input = true, .input = 0, .level = true },
        { .output = OUTPUT_MIC, .from_input = true, .input = 0 },
    };
    static const AccessoryGpioRoute SPEAKER_GPIO[] = {
        { .output = OUTPUT_KEY, .level = true },
    };

    for (unsigned output = 0; output < OUTPUT_COUNT; output++) {
        output_levels[output] = LEVEL_UNKNOWN;
    }
    bench->bus_owner = qdev_new(TYPE_TEST_BUS_OWNER);
    qdev_realize(bench->bus_owner, NULL, &error_fatal);
    bench->line = audio_line_bus_init(bench->bus_owner, "line");
    bench->other_line = audio_line_bus_init(bench->bus_owner, "other");
    bench->i2s = i2s_tx_bus_init(bench->bus_owner, "i2s");
    audio_line_start(bench->line, 0, &FORMAT);
    audio_line_start(bench->other_line, 0, &FORMAT);
    bench->sink = create_sink();
    bench->second_sink = create_sink();

    const AccessoryPortRoute headset_ports[] = {
        { DEVICE(bench->sink), "LINE", BUS(bench->line) },
    };
    const AccessoryPortRoute speaker_ports[] = {
        { DEVICE(bench->sink), "LINE", BUS(bench->other_line) },
    };
    const AccessoryPortRoute dock_a_ports[] = {
        { DEVICE(bench->sink), "I2S", BUS(bench->i2s) },
    };
    const AccessoryPortRoute dock_b_ports[] = {
        { DEVICE(bench->second_sink), "I2S", BUS(bench->i2s) },
    };
    const AccessoryProfile profiles[] = {
        { "none", NONE_GPIO, ARRAY_SIZE(NONE_GPIO), NULL, 0 },
        { "headset", HEADSET_GPIO, ARRAY_SIZE(HEADSET_GPIO),
          headset_ports, ARRAY_SIZE(headset_ports) },
        { "speaker", SPEAKER_GPIO, ARRAY_SIZE(SPEAKER_GPIO),
          speaker_ports, ARRAY_SIZE(speaker_ports) },
        { "dock-a", NULL, 0, dock_a_ports, ARRAY_SIZE(dock_a_ports) },
        { "dock-b", NULL, 0, dock_b_ports, ARRAY_SIZE(dock_b_ports) },
    };

    bench->connector = create_connector(profiles, ARRAY_SIZE(profiles));
    if (unplugged) {
        qdev_prop_set_string(DEVICE(bench->connector), "unplugged", unplugged);
    }
}

static void bench_realize(TestBench *bench, const char *accessory)
{
    object_property_set_str(OBJECT(bench->connector), "accessory", accessory,
                            &error_fatal);
    qdev_realize(DEVICE(bench->connector), NULL, &error_fatal);
}

static void bench_select(TestBench *bench, const char *accessory)
{
    object_property_set_str(OBJECT(bench->connector), "accessory", accessory,
                            &error_fatal);
}

static void bench_set_input(TestBench *bench, int level)
{
    qemu_set_irq(qdev_get_gpio_in_named(DEVICE(bench->connector), "in", 0),
                 level);
}

static void bench_cleanup(TestBench *bench)
{
    object_destroy(OBJECT(bench->connector));
    object_destroy(OBJECT(bench->second_sink));
    object_destroy(OBJECT(bench->sink));
    object_destroy(OBJECT(bench->bus_owner));
}

static void test_initial_profile(void)
{
    TestBench bench;

    bench_init(&bench, "none");
    bench_set_input(&bench, 1);
    bench_realize(&bench, "headset");
    g_assert_cmpint(output_levels[OUTPUT_DETECT], ==, 0);
    g_assert_cmpint(output_levels[OUTPUT_KEY], ==, 0);
    g_assert_cmpint(output_levels[OUTPUT_MIC], ==, 1);
    g_assert_true(bench.sink->line_port.parent.bus == BUS(bench.line));
    g_assert_true(bench.sink->started);

    bench_set_input(&bench, 0);
    g_assert_cmpint(output_levels[OUTPUT_KEY], ==, 1);
    g_assert_cmpint(output_levels[OUTPUT_MIC], ==, 0);
    bench_cleanup(&bench);
}

static void test_replug(void)
{
    TestBench bench;
    g_autofree char *name = NULL;

    bench_init(&bench, "none");
    bench_realize(&bench, "headset");
    bench_select(&bench, "speaker");
    name = object_property_get_str(OBJECT(bench.connector), "accessory",
                                   &error_fatal);
    g_assert_cmpstr(name, ==, "speaker");
    g_assert_cmpint(output_levels[OUTPUT_DETECT], ==, 1);
    g_assert_null(bench.sink->line_port.parent.bus);
    g_assert_false(bench.sink->started);

    advance_virtual_clock_ms(REPLUG_DELAY_MS - 1);
    g_assert_cmpint(output_levels[OUTPUT_DETECT], ==, 1);
    g_assert_null(bench.sink->line_port.parent.bus);

    advance_virtual_clock_ms(1);
    g_assert_cmpint(output_levels[OUTPUT_DETECT], ==, 0);
    g_assert_cmpint(output_levels[OUTPUT_KEY], ==, 1);
    g_assert_true(bench.sink->line_port.parent.bus == BUS(bench.other_line));
    g_assert_true(bench.sink->started);
    bench_cleanup(&bench);
}

static void test_replug_cancel(void)
{
    TestBench bench;

    bench_init(&bench, "none");
    bench_realize(&bench, "headset");
    bench_select(&bench, "speaker");
    bench_select(&bench, "none");
    advance_virtual_clock_ms(2 * REPLUG_DELAY_MS);
    g_assert_cmpint(output_levels[OUTPUT_DETECT], ==, 1);
    g_assert_null(bench.sink->line_port.parent.bus);
    bench_cleanup(&bench);
}

static void test_direct_switch(void)
{
    TestBench bench;

    bench_init(&bench, NULL);
    bench_realize(&bench, "headset");
    bench_select(&bench, "speaker");
    g_assert_cmpint(output_levels[OUTPUT_KEY], ==, 1);
    g_assert_true(bench.sink->line_port.parent.bus == BUS(bench.other_line));
    bench_cleanup(&bench);
}

static void test_single_endpoint_bus_move(void)
{
    TestBench bench;

    bench_init(&bench, NULL);
    bench_realize(&bench, "dock-a");
    g_assert_true(bench.sink->i2s_port.parent.bus == BUS(bench.i2s));

    bench_select(&bench, "dock-b");
    g_assert_null(bench.sink->i2s_port.parent.bus);
    g_assert_true(bench.second_sink->i2s_port.parent.bus == BUS(bench.i2s));
    bench_cleanup(&bench);
}

static void test_run_time_bind_error(void)
{
    TestBench bench;
    TestSink *occupant;
    Error *err = NULL;

    bench_init(&bench, NULL);
    bench_realize(&bench, "none");
    occupant = create_sink();
    qdev_bus_port_bind(DEVICE(occupant), "I2S", BUS(bench.i2s), &error_fatal);

    g_assert_false(object_property_set_str(OBJECT(bench.connector),
                                           "accessory", "dock-a", &err));
    g_assert_nonnull(strstr(error_get_pretty(err), "dock-a"));
    error_free(err);
    g_assert_null(bench.sink->i2s_port.parent.bus);

    object_destroy(OBJECT(occupant));
    bench_cleanup(&bench);
}

static void test_unknown_profiles(void)
{
    TestBench bench;
    Error *err = NULL;

    bench_init(&bench, "empty");
    object_property_set_str(OBJECT(bench.connector), "accessory", "carkit",
                            &error_fatal);
    g_assert_false(qdev_realize(DEVICE(bench.connector), NULL, &err));
    g_assert_nonnull(strstr(error_get_pretty(err), "none, headset"));
    error_free(g_steal_pointer(&err));

    object_property_set_str(OBJECT(bench.connector), "accessory", "none",
                            &error_fatal);
    g_assert_false(qdev_realize(DEVICE(bench.connector), NULL, &err));
    g_assert_nonnull(strstr(error_get_pretty(err), "'empty'"));
    error_free(g_steal_pointer(&err));

    qdev_prop_set_string(DEVICE(bench.connector), "unplugged", "none");
    qdev_realize(DEVICE(bench.connector), NULL, &error_fatal);
    g_assert_false(object_property_set_str(OBJECT(bench.connector),
                                           "accessory", "carkit", &err));
    error_free(g_steal_pointer(&err));
    g_assert_cmpint(output_levels[OUTPUT_DETECT], ==, 1);
    bench_cleanup(&bench);
}

static void check_invalid_port(TestBench *bench,
                               const AccessoryPortRoute *route,
                               const char *message)
{
    static const AccessoryGpioRoute NONE_GPIO[] = {
        { .output = OUTPUT_MIC },
    };
    const AccessoryProfile profiles[] = {
        { "none", NONE_GPIO, ARRAY_SIZE(NONE_GPIO), NULL, 0 },
        { "broken", NULL, 0, route, 1 },
    };
    AccessoryConnector *connector = create_connector(profiles,
                                                     ARRAY_SIZE(profiles));
    Error *err = NULL;

    object_property_set_str(OBJECT(connector), "accessory", "none",
                            &error_fatal);
    g_assert_false(qdev_realize(DEVICE(connector), NULL, &err));
    g_assert_nonnull(strstr(error_get_pretty(err), message));
    error_free(err);
    g_assert_null(bench->sink->line_port.parent.bus);
    object_unref(OBJECT(connector));
}

static void test_invalid_port(void)
{
    TestBench bench;

    bench_init(&bench, NULL);
    check_invalid_port(&bench, &(AccessoryPortRoute) {
        DEVICE(bench.sink), "LINE", BUS(bench.i2s),
    }, "accessory 'broken'");
    check_invalid_port(&bench, &(AccessoryPortRoute) {
        DEVICE(bench.sink), "MISSING", BUS(bench.line),
    }, "no bus port 'MISSING'");
    bench_cleanup(&bench);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    type_register_static_array(test_types, ARRAY_SIZE(test_types));
    qemu_init_clocks(NULL);
    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
    object_property_add_new_container(
        object_property_add_new_container(object_get_root(), "machine"),
        "unattached");

    g_test_add_func("/accessory-connector/initial-profile",
                    test_initial_profile);
    g_test_add_func("/accessory-connector/replug", test_replug);
    g_test_add_func("/accessory-connector/replug-cancel", test_replug_cancel);
    g_test_add_func("/accessory-connector/direct-switch", test_direct_switch);
    g_test_add_func("/accessory-connector/single-endpoint-bus-move",
                    test_single_endpoint_bus_move);
    g_test_add_func("/accessory-connector/run-time-bind-error",
                    test_run_time_bind_error);
    g_test_add_func("/accessory-connector/unknown-profiles",
                    test_unknown_profiles);
    g_test_add_func("/accessory-connector/invalid-port", test_invalid_port);
    return g_test_run();
}
