/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/debug-channel.h"

static const char *const lcd_groups[] = { "pmb887x-lcd", "device", NULL };

DEBUG_CHANNEL_INFO(usart_info, .name = "usart")
DEBUG_CHANNEL_INFO(lcd_a_info, .name = "lcd")
DEBUG_CHANNEL_INFO(lcd_b_info, .name = "lcd")
DEBUG_CHANNEL_INFO(jbt6k71_info, .name = "jbt6k71", .groups = lcd_groups)
DEBUG_CHANNEL_INFO(adc_info, .name = "adc")
DEBUG_CHANNEL_INFO(dsp_info, .name = "dsp")
DEBUG_CHANNEL_INFO(dsp_timer1_info, .name = "dsp_timer1")
DEBUG_CHANNEL_INFO(dsp_cipher_info, .name = "dsp_cipher")

static void parse_ok(const char *spec, uint8_t flag)
{
    g_assert_true(debug_channels_parse(spec, flag, &error_abort));
}

static uint8_t flags_of(const DebugChannelInfo *info, const char *instance)
{
    DebugChannel ch;
    uint8_t flags;

    debug_channel_init(&ch, info, instance);
    flags = ch.flags;
    debug_channel_cleanup(&ch);
    return flags;
}

static void test_name(void)
{
    debug_channels_reset();
    parse_ok("adc", DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&adc_info, NULL), ==, DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&usart_info, NULL), ==, 0);
    g_assert_cmpint(flags_of(&dsp_info, NULL), ==, 0);
}

static void test_shared_name(void)
{
    debug_channels_reset();
    parse_ok("lcd", DEBUG_CH_IO);
    g_assert_cmpint(flags_of(&lcd_a_info, NULL), ==, DEBUG_CH_IO);
    g_assert_cmpint(flags_of(&lcd_b_info, NULL), ==, DEBUG_CH_IO);
}

static void test_case(void)
{
    debug_channels_reset();
    parse_ok("ADC", DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&adc_info, NULL), ==, DEBUG_CH_LOG);
}

static void test_star(void)
{
    debug_channels_reset();
    parse_ok("*", DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&usart_info, "usart0"), ==, DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&adc_info, NULL), ==, DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&dsp_timer1_info, NULL), ==, DEBUG_CH_LOG);

    debug_channels_reset();
    parse_ok("*,-dsp_*", DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&dsp_info, NULL), ==, DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&dsp_timer1_info, NULL), ==, 0);
    g_assert_cmpint(flags_of(&dsp_cipher_info, NULL), ==, 0);
}

static void test_pattern(void)
{
    debug_channels_reset();
    parse_ok("dsp_*", DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&dsp_timer1_info, NULL), ==, DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&dsp_cipher_info, NULL), ==, DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&dsp_info, NULL), ==, 0);
}

static void test_group(void)
{
    debug_channels_reset();
    parse_ok("pmb887x-lcd", DEBUG_CH_IO);
    g_assert_cmpint(flags_of(&jbt6k71_info, "LCD0"), ==, DEBUG_CH_IO);
    g_assert_cmpint(flags_of(&lcd_a_info, NULL), ==, 0);
}

static void test_order_and_minus(void)
{
    debug_channels_reset();
    parse_ok("dsp_*,-dsp_cipher", DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&dsp_timer1_info, NULL), ==, DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&dsp_cipher_info, NULL), ==, 0);

    parse_ok("-dsp_*,+dsp_cipher", DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&dsp_timer1_info, NULL), ==, 0);
    g_assert_cmpint(flags_of(&dsp_cipher_info, NULL), ==, DEBUG_CH_LOG);
}

static void test_flags_are_independent(void)
{
    debug_channels_reset();
    parse_ok("usart", DEBUG_CH_LOG);
    parse_ok("usart,adc", DEBUG_CH_IO);
    parse_ok("-usart", DEBUG_CH_IO);
    g_assert_cmpint(flags_of(&usart_info, NULL), ==, DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&adc_info, NULL), ==, DEBUG_CH_IO);
}

static void test_instances(void)
{
    debug_channels_reset();
    parse_ok("usart0", DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&usart_info, "usart0"), ==, DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&usart_info, "usart1"), ==, 0);

    debug_channels_reset();
    parse_ok("usart,-USART1", DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&usart_info, "usart0"), ==, DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&usart_info, "usart1"), ==, 0);

    debug_channels_reset();
    parse_ok("usart?", DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&usart_info, "usart1"), ==, DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&usart_info, NULL), ==, 0);
}

static void test_instance_name(void)
{
    DebugChannel ch;

    debug_channels_reset();
    debug_channel_init(&ch, &usart_info, "usart1");
    g_assert_cmpstr(ch.name, ==, "usart1");
    debug_channel_cleanup(&ch);

    debug_channel_init(&ch, &dsp_info, NULL);
    g_assert_cmpstr(ch.name, ==, "dsp");
    debug_channel_cleanup(&ch);
}

static void test_whitespace_and_empty(void)
{
    debug_channels_reset();
    parse_ok(" us art , ,adc,", DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&usart_info, NULL), ==, DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&adc_info, NULL), ==, DEBUG_CH_LOG);

    debug_channels_reset();
    parse_ok("", DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&usart_info, NULL), ==, 0);
}

static void test_lone_sign_fails(void)
{
    Error *err = NULL;

    debug_channels_reset();
    g_assert_false(debug_channels_parse("usart,-", DEBUG_CH_LOG, &err));
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpint(flags_of(&usart_info, NULL), ==, 0);
}

static void test_unused(void)
{
    Error *err = NULL;

    debug_channels_reset();
    parse_ok("adc,usart3,*", DEBUG_CH_LOG);
    flags_of(&usart_info, "usart0");
    g_assert_false(debug_channels_check_unused(NULL, &err));
    g_assert_nonnull(strstr(error_get_pretty(err), "usart3"));
    error_free(err);

    flags_of(&usart_info, "usart3");
    g_assert_true(debug_channels_check_unused(NULL, &error_abort));
}

static bool is_qom_type(const char *name)
{
    return !strcmp(name, "ssd1286");
}

static void test_unused_known(void)
{
    Error *err = NULL;

    debug_channels_reset();
    parse_ok("ssd1286", DEBUG_CH_LOG);
    g_assert_true(debug_channels_check_unused(is_qom_type, &error_abort));
    g_assert_false(debug_channels_check_unused(NULL, &err));
    error_free(err);
}

static void test_option_sections(void)
{
    debug_channels_reset();
    g_assert_true(debug_channels_parse_option(
        "log=usart,adc,-usart, io = dsp ,usart,,log=ADC", &error_abort));
    g_assert_cmpint(flags_of(&usart_info, NULL), ==, DEBUG_CH_IO);
    g_assert_cmpint(flags_of(&adc_info, NULL), ==, DEBUG_CH_LOG);
    g_assert_cmpint(flags_of(&dsp_info, NULL), ==, DEBUG_CH_IO);

    debug_channels_reset();
    g_assert_true(debug_channels_parse_option(",log=,io=", &error_abort));
    g_assert_true(debug_channels_parse_option("", &error_abort));
    g_assert_cmpint(flags_of(&usart_info, NULL), ==, 0);
}

static void option_fails(const char *str, const char *message)
{
    Error *err = NULL;

    g_assert_false(debug_channels_parse_option(str, &err));
    g_assert_nonnull(strstr(error_get_pretty(err), message));
    error_free(err);
}

static void test_option_errors(void)
{
    debug_channels_reset();
    option_fails("usart", "Expected log=, io= or time=");
    option_fails("time=on,usart", "Expected log=, io= or time=");
    option_fails("trace=usart", "Unknown debug channels key 'trace'");
    option_fails("time=maybe", "time");
    option_fails("log=usart,-", "Invalid debug channel name");
}

static void test_has_enabled(void)
{
    debug_channels_reset();
    parse_ok("adc", DEBUG_CH_IO);
    flags_of(&usart_info, NULL);
    g_assert_false(debug_channels_has_enabled());
    flags_of(&adc_info, NULL);
    g_assert_true(debug_channels_has_enabled());
}

static void test_is_requested(void)
{
    debug_channels_reset();
    parse_ok("-adc", DEBUG_CH_IO);
    g_assert_false(debug_channels_is_requested());
    parse_ok("adc", DEBUG_CH_IO);
    g_assert_true(debug_channels_is_requested());
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/debug-channel/name", test_name);
    g_test_add_func("/debug-channel/shared-name", test_shared_name);
    g_test_add_func("/debug-channel/case", test_case);
    g_test_add_func("/debug-channel/star", test_star);
    g_test_add_func("/debug-channel/pattern", test_pattern);
    g_test_add_func("/debug-channel/group", test_group);
    g_test_add_func("/debug-channel/order-and-minus", test_order_and_minus);
    g_test_add_func("/debug-channel/flags-are-independent",
                    test_flags_are_independent);
    g_test_add_func("/debug-channel/instances", test_instances);
    g_test_add_func("/debug-channel/instance-name", test_instance_name);
    g_test_add_func("/debug-channel/whitespace-and-empty",
                    test_whitespace_and_empty);
    g_test_add_func("/debug-channel/lone-sign-fails", test_lone_sign_fails);
    g_test_add_func("/debug-channel/unused", test_unused);
    g_test_add_func("/debug-channel/unused-known", test_unused_known);
    g_test_add_func("/debug-channel/has-enabled", test_has_enabled);
    g_test_add_func("/debug-channel/is-requested", test_is_requested);
    g_test_add_func("/debug-channel/option-sections", test_option_sections);
    g_test_add_func("/debug-channel/option-errors", test_option_errors);
    return g_test_run();
}
