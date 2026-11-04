/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qemu/debug-channel.h"

DEBUG_CHANNEL_INFO(unit_info, .name = "unit")

static QemuMutex out_lock;
static GString *out;
static QemuEvent gate;
static int64_t fake_ns;
static int output_delay_us;

static int64_t fake_clock(void)
{
    return qatomic_read(&fake_ns);
}

static void set_clock(int64_t ns)
{
    qatomic_set(&fake_ns, ns);
}

static void capture(const char *buf, size_t len, void *_opaque)
{
    qemu_event_wait(&gate);
    if (qatomic_read(&output_delay_us)) {
        g_usleep(qatomic_read(&output_delay_us));
    }
    qemu_mutex_lock(&out_lock);
    g_string_append_len(out, buf, len);
    qemu_mutex_unlock(&out_lock);
}

static char *output(void)
{
    char *copy;

    qemu_mutex_lock(&out_lock);
    copy = g_strdup(out->str);
    qemu_mutex_unlock(&out_lock);
    return copy;
}

static bool wait_output(const char *expected)
{
    for (int i = 0; i < 200; i++) {
        g_autofree char *now = output();

        if (!strcmp(now, expected)) {
            return true;
        }
        g_usleep(10 * 1000);
    }
    return false;
}

static void setup(DebugChannel *ch)
{
    qemu_mutex_lock(&out_lock);
    g_string_truncate(out, 0);
    qemu_mutex_unlock(&out_lock);
    qemu_event_set(&gate);
    set_clock(0);
    debug_channels_set_time(false);
    debug_channels_reset();
    debug_channel_init(ch, &unit_info, "UNIT0");
}

static void teardown(DebugChannel *ch)
{
    debug_channels_stop();
    debug_channel_cleanup(ch);
}

static void write_reg(DebugChannel *ch, uint64_t addr, uint64_t value)
{
    debug_channel_io(ch, addr, 4, value, true, false, 0, 0);
}

static void test_print_before_start(void)
{
    DebugChannel ch;
    g_autofree char *now = NULL;

    setup(&ch);
    debug_channel_log(&ch, "hello %d\n", 1);
    write_reg(&ch, 0x10, 1);
    now = output();
    g_assert_cmpstr(now, ==,
                    "[UNIT0]: hello 1\n"
                    "[UNIT0] WRITE[4] 00000010: 00000001\n");
    teardown(&ch);
}

static void test_order(void)
{
    DebugChannel ch;
    g_autofree char *now = NULL;

    setup(&ch);
    debug_channels_start(64);
    debug_channel_log(&ch, "a\n");
    write_reg(&ch, 0x10, 1);
    debug_channel_io(&ch, 0x14, 2, 0xbeef, false, true, 0xa0000010, 0x1234);
    debug_channel_log(&ch, "b\n");
    debug_channels_flush();
    now = output();
    g_assert_cmpstr(now, ==,
                    "[UNIT0]: a\n"
                    "[UNIT0] WRITE[4] 00000010: 00000001\n"
                    "[UNIT0]  READ[2] 00000014: BEEF"
                    " (PC: A0000010, LR: 00001234)\n"
                    "[UNIT0]: b\n");
    teardown(&ch);
}

static void test_merge_in_window(void)
{
    DebugChannel ch;
    g_autofree char *now = NULL;

    setup(&ch);
    debug_channels_start(64);
    write_reg(&ch, 0x10, 1);
    set_clock(50 * SCALE_MS);
    write_reg(&ch, 0x10, 1);
    set_clock(99 * SCALE_MS);
    write_reg(&ch, 0x10, 1);
    debug_channels_flush();
    now = output();
    g_assert_cmpstr(now, ==, "[UNIT0] WRITE[4] 00000010: 00000001 [x3]\n");
    teardown(&ch);
}

static void test_merge_window_ends(void)
{
    DebugChannel ch;
    g_autofree char *now = NULL;

    setup(&ch);
    debug_channels_start(64);
    write_reg(&ch, 0x10, 1);
    set_clock(100 * SCALE_MS);
    write_reg(&ch, 0x10, 1);
    write_reg(&ch, 0x10, 1);
    debug_channels_flush();
    now = output();
    g_assert_cmpstr(now, ==,
                    "[UNIT0] WRITE[4] 00000010: 00000001\n"
                    "[UNIT0] WRITE[4] 00000010: 00000001 [x2]\n");
    teardown(&ch);
}

static void test_merge_only_last(void)
{
    DebugChannel ch, other;
    g_autofree char *now = NULL;

    setup(&ch);
    debug_channel_init(&other, &unit_info, "UNIT1");
    debug_channels_start(64);
    write_reg(&ch, 0x10, 1);
    write_reg(&ch, 0x10, 2);
    write_reg(&ch, 0x10, 1);
    write_reg(&other, 0x10, 1);
    debug_channels_flush();
    now = output();
    g_assert_cmpstr(now, ==,
                    "[UNIT0] WRITE[4] 00000010: 00000001\n"
                    "[UNIT0] WRITE[4] 00000010: 00000002\n"
                    "[UNIT0] WRITE[4] 00000010: 00000001\n"
                    "[UNIT1] WRITE[4] 00000010: 00000001\n");
    teardown(&ch);
    debug_channel_cleanup(&other);
}

static void test_last_held_until_window_ends(void)
{
    DebugChannel ch;
    g_autofree char *now = NULL;

    setup(&ch);
    debug_channels_start(64);
    write_reg(&ch, 0x10, 1);
    g_usleep(50 * 1000);
    now = output();
    g_assert_cmpstr(now, ==, "");

    set_clock(100 * SCALE_MS);
    g_assert_true(wait_output("[UNIT0] WRITE[4] 00000010: 00000001\n"));
    teardown(&ch);
}

static void test_stop_prints_last(void)
{
    DebugChannel ch;
    g_autofree char *now = NULL;

    setup(&ch);
    debug_channels_start(64);
    write_reg(&ch, 0x10, 1);
    write_reg(&ch, 0x10, 1);
    debug_channels_stop();
    now = output();
    g_assert_cmpstr(now, ==, "[UNIT0] WRITE[4] 00000010: 00000001 [x2]\n");
    debug_channel_cleanup(&ch);
}

static bool producer_done;

static void *producer(void *opaque)
{
    DebugChannel *ch = opaque;

    for (int i = 0; i < 10; i++) {
        debug_channel_log(ch, "%d\n", i);
    }
    qatomic_set(&producer_done, true);
    return NULL;
}

static void test_full_queue_waits(void)
{
    DebugChannel ch;
    QemuThread thread;
    g_autoptr(GString) expected = g_string_new(NULL);
    g_autofree char *now = NULL;

    setup(&ch);
    qemu_event_reset(&gate);
    debug_channels_start(2);
    qatomic_set(&producer_done, false);
    qemu_thread_create(&thread, "producer", producer, &ch,
                       QEMU_THREAD_JOINABLE);
    g_usleep(50 * 1000);
    g_assert_false(qatomic_read(&producer_done));

    qemu_event_set(&gate);
    qemu_thread_join(&thread);
    debug_channels_flush();
    for (int i = 0; i < 10; i++) {
        g_string_append_printf(expected, "[UNIT0]: %d\n", i);
    }
    now = output();
    g_assert_cmpstr(now, ==, expected->str);
    teardown(&ch);
}

#define PUSH_COUNT 100000

static bool producer_stop;
static int producer_pushed;

static void *endless_producer(void *opaque)
{
    DebugChannel *ch = opaque;

    while (!qatomic_read(&producer_stop)) {
        debug_channel_log(ch, "x\n");
        qatomic_inc(&producer_pushed);
    }
    return NULL;
}

static void test_flush_with_producer(void)
{
    DebugChannel ch;
    QemuThread thread;

    setup(&ch);
    qatomic_set(&output_delay_us, 1000);
    debug_channels_start(4);
    qatomic_set(&producer_stop, false);
    qatomic_set(&producer_pushed, 0);
    qemu_thread_create(&thread, "producer", endless_producer, &ch,
                       QEMU_THREAD_JOINABLE);
    while (qatomic_read(&producer_pushed) < 10) {
        g_usleep(100);
    }
    for (int i = 0; i < 10; i++) {
        debug_channels_flush();
    }
    qatomic_set(&producer_stop, true);
    qemu_thread_join(&thread);
    teardown(&ch);
    qatomic_set(&output_delay_us, 0);
}

static void *counting_producer(void *opaque)
{
    DebugChannel *ch = opaque;

    for (int i = 0; i < PUSH_COUNT; i++) {
        debug_channel_log(ch, "%d\n", i);
        qatomic_set(&producer_pushed, i + 1);
    }
    return NULL;
}

static void test_stop_while_pushing(void)
{
    DebugChannel ch;
    QemuThread thread;
    g_autoptr(GString) expected = g_string_new(NULL);
    g_autofree char *now = NULL;

    setup(&ch);
    debug_channels_start(4);
    qatomic_set(&producer_pushed, 0);
    qemu_thread_create(&thread, "producer", counting_producer, &ch,
                       QEMU_THREAD_JOINABLE);
    while (qatomic_read(&producer_pushed) < PUSH_COUNT / 10) {
        g_usleep(100);
    }
    debug_channels_stop();
    qemu_thread_join(&thread);
    for (int i = 0; i < PUSH_COUNT; i++) {
        g_string_append_printf(expected, "[UNIT0]: %d\n", i);
    }
    now = output();
    g_assert_cmpstr(now, ==, expected->str);
    debug_channel_cleanup(&ch);
}

static void test_time_prefix(void)
{
    DebugChannel ch;
    g_autofree char *now = NULL;

    setup(&ch);
    g_assert_true(debug_channels_parse_option("time=on", &error_abort));
    set_clock(1500 * SCALE_MS);
    debug_channel_log(&ch, "t\n");
    g_assert_true(debug_channels_parse_option("time=off", &error_abort));
    debug_channel_log(&ch, "u\n");
    now = output();
    g_assert_cmpstr(now, ==, "[   1.500000000] [UNIT0]: t\n[UNIT0]: u\n");
    teardown(&ch);
}

static void print_custom(const char *name, const void *opaque,
                         const DebugIoAccess *io, GString *out)
{
    g_string_append_printf(out, "%s:%s:%" PRIx64, (const char *) opaque, name,
                           io->value);
}

static const DebugIoFormat custom_format = {
    .print = print_custom,
};

static void test_io_format(void)
{
    DebugChannel ch;
    g_autofree char *now = NULL;

    setup(&ch);
    ch.io_format = &custom_format;
    ch.io_format_opaque = "fmt";
    debug_channels_start(64);
    write_reg(&ch, 0x10, 0xab);
    write_reg(&ch, 0x10, 0xab);
    debug_channels_flush();
    now = output();
    g_assert_cmpstr(now, ==, "fmt:UNIT0:ab [x2]\n");
    teardown(&ch);
}

static void test_macros(void)
{
    DebugChannel ch;
    unsigned saved_loglevel = qemu_loglevel;
    g_autofree char *now = NULL;

    setup(&ch);
    DEBUG_LOG(&ch, "off\n");
    DEBUG_IO_WRITE(&ch, 0x10, 4, 1);
    qemu_loglevel = 0;
    DEBUG_UNIMP(&ch, "off\n");

    g_assert_true(debug_channels_parse("unit0", DEBUG_CH_LOG | DEBUG_CH_IO,
                                       &error_abort));
    debug_channel_cleanup(&ch);
    debug_channel_init(&ch, &unit_info, "UNIT0");
    DEBUG_LOG(&ch, "log\n");
    DEBUG_IO_WRITE(&ch, 0x10, 4, 1);
    qemu_loglevel = LOG_UNIMP;
    DEBUG_UNIMP(&ch, "unimp\n");
    DEBUG_GUEST_ERROR(&ch, "off\n");
    qemu_loglevel = saved_loglevel;

    now = output();
    g_assert_cmpstr(now, ==,
                    "[UNIT0]: log\n"
                    "[UNIT0] WRITE[4] 00000010: 00000001\n"
                    "[UNIT0]: unimp\n");
    teardown(&ch);
}

int main(int argc, char **argv)
{
    qemu_mutex_init(&out_lock);
    qemu_event_init(&gate, true);
    out = g_string_new(NULL);
    debug_channels_set_clock(fake_clock);
    debug_channels_set_output(capture, NULL);

    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/debug-channel-queue/print-before-start",
                    test_print_before_start);
    g_test_add_func("/debug-channel-queue/order", test_order);
    g_test_add_func("/debug-channel-queue/merge-in-window",
                    test_merge_in_window);
    g_test_add_func("/debug-channel-queue/merge-window-ends",
                    test_merge_window_ends);
    g_test_add_func("/debug-channel-queue/merge-only-last",
                    test_merge_only_last);
    g_test_add_func("/debug-channel-queue/last-held-until-window-ends",
                    test_last_held_until_window_ends);
    g_test_add_func("/debug-channel-queue/stop-prints-last",
                    test_stop_prints_last);
    g_test_add_func("/debug-channel-queue/full-queue-waits",
                    test_full_queue_waits);
    g_test_add_func("/debug-channel-queue/flush-with-producer",
                    test_flush_with_producer);
    g_test_add_func("/debug-channel-queue/stop-while-pushing",
                    test_stop_while_pushing);
    g_test_add_func("/debug-channel-queue/time-prefix", test_time_prefix);
    g_test_add_func("/debug-channel-queue/io-format", test_io_format);
    g_test_add_func("/debug-channel-queue/macros", test_macros);
    return g_test_run();
}
