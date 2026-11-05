/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/audio/pcm-out.h"

#define TEST_RATE_HZ        50
#define TEST_PREFILL_MS     60
#define TEST_PREFILL_FRAMES 3
#define TEST_FIFO_FRAMES    (48000 * 2)

DEBUG_CHANNEL_INFO(test_info, .name = "test")

void rust_bql_mock_lock(void);

static struct {
    int voice;
    audio_callback_fn callback;
    void *opaque;
    struct audsettings settings;
    int opens;
    bool active;
    size_t accept;
    GByteArray *played;
} be;

static DebugChannel test_dbg;

bool audio_be_check(AudioBackend **_be, Error **_errp)
{
    return true;
}

SWVoiceOut *audio_be_open_out(AudioBackend *_be, SWVoiceOut *_sw,
                              const char *_name, void *callback_opaque,
                              audio_callback_fn callback_fn,
                              const struct audsettings *as)
{
    be.callback = callback_fn;
    be.opaque = callback_opaque;
    be.settings = *as;
    be.opens++;
    return (SWVoiceOut *) &be.voice;
}

void audio_be_close_out(AudioBackend *_be, SWVoiceOut *_sw)
{
}

void audio_be_set_active_out(AudioBackend *_be, SWVoiceOut *_sw, bool on)
{
    be.active = on;
}

size_t audio_be_write(AudioBackend *_be, SWVoiceOut *_sw, void *buf,
                      size_t size)
{
    size_t n = MIN(size, be.accept);

    g_byte_array_append(be.played, buf, n);
    be.accept -= n;
    return n;
}

static void play(size_t free_bytes, size_t accept)
{
    be.accept = accept;
    be.callback(be.opaque, free_bytes);
}

static void assert_played(const int16_t *expected, size_t count)
{
    g_assert_cmpmem(be.played->data, be.played->len, expected,
                    count * sizeof(int16_t));
    g_byte_array_set_size(be.played, 0);
}

static void setup(PCMOut *out)
{
    be.opens = 0;
    be.active = false;
    g_byte_array_set_size(be.played, 0);
    pcm_out_init(out, NULL, "test", TEST_PREFILL_MS, &test_dbg);
    pcm_out_set_format(out, TEST_RATE_HZ, 1);
    pcm_out_set_active(out, true);
}

static void test_prefill(void)
{
    static const int16_t frames[] = { 1, 2, 3, 4, 5 };
    PCMOut out;

    setup(&out);
    pcm_out_write(&out, frames, TEST_PREFILL_FRAMES - 1);
    play(64, 64);
    g_assert_cmpuint(be.played->len, ==, 0);

    pcm_out_write(&out, frames + TEST_PREFILL_FRAMES - 1, 1);
    play(64, 64);
    assert_played(frames, TEST_PREFILL_FRAMES);

    pcm_out_write(&out, frames + TEST_PREFILL_FRAMES, 2);
    play(64, 64);
    g_assert_cmpuint(be.played->len, ==, 0);
    pcm_out_cleanup(&out);
}

static void test_short_write(void)
{
    static const int16_t frames[] = { 1, 2, 3, 4 };
    PCMOut out;

    setup(&out);
    pcm_out_write(&out, frames, 4);
    play(64, 3);
    play(64, 64);
    assert_played(frames, 4);
    pcm_out_cleanup(&out);
}

static void test_active(void)
{
    static const int16_t stale[] = { 1, 2, 3 };
    static const int16_t frames[] = { 4, 5, 6, 7 };
    PCMOut out;

    setup(&out);
    pcm_out_write(&out, stale, 3);
    pcm_out_set_active(&out, false);
    pcm_out_set_active(&out, true);
    g_assert_true(be.active);
    pcm_out_write(&out, frames, 4);
    pcm_out_set_active(&out, false);
    play(64, 64);
    assert_played(frames, 4);
    g_assert_false(be.active);
    pcm_out_cleanup(&out);
}

static void test_format(void)
{
    static const int16_t stale[] = { 1, 2, 3 };
    static const int16_t frames[] = { 4, 5, 6, 7, 8, 9 };
    PCMOut out;

    setup(&out);
    pcm_out_write(&out, stale, 3);
    pcm_out_set_format(&out, TEST_RATE_HZ, 1);
    g_assert_cmpint(be.opens, ==, 1);

    pcm_out_set_format(&out, TEST_RATE_HZ, 2);
    g_assert_cmpint(be.opens, ==, 2);
    g_assert_cmpint(be.settings.freq, ==, TEST_RATE_HZ);
    g_assert_cmpint(be.settings.nchannels, ==, 2);
    g_assert_cmpint(be.settings.big_endian, ==, HOST_BIG_ENDIAN);
    g_assert_true(be.active);
    pcm_out_write(&out, frames, 3);
    play(64, 64);
    assert_played(frames, 6);

    pcm_out_stop(&out);
    g_assert_false(be.active);
    pcm_out_write(&out, frames, 3);
    pcm_out_set_format(&out, TEST_RATE_HZ, 2);
    play(64, 64);
    g_assert_cmpuint(be.played->len, ==, 0);
    pcm_out_cleanup(&out);
}

static void test_overflow(void)
{
    static const int16_t frames[] = { 1, 2, 3 };
    size_t fill_frames = TEST_FIFO_FRAMES - 2;
    size_t fill_bytes = fill_frames * sizeof(int16_t);
    g_autofree int16_t *fill = g_new0(int16_t, fill_frames);
    PCMOut out;

    setup(&out);
    pcm_out_write(&out, fill, fill_frames);
    pcm_out_write(&out, frames, 3);
    play(fill_bytes + sizeof(frames), fill_bytes + sizeof(frames));
    assert_played(fill, fill_frames);
    pcm_out_cleanup(&out);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    rust_bql_mock_lock();
    be.played = g_byte_array_new();
    debug_channel_init(&test_dbg, &test_info, "TEST");
    g_test_add_func("/pcm-out/prefill", test_prefill);
    g_test_add_func("/pcm-out/short-write", test_short_write);
    g_test_add_func("/pcm-out/active", test_active);
    g_test_add_func("/pcm-out/format", test_format);
    g_test_add_func("/pcm-out/overflow", test_overflow);
    return g_test_run();
}
