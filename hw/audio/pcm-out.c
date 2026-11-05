/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "hw/audio/pcm-out.h"

#define PCM_OUT_MAX_RATE_HZ     48000
#define PCM_OUT_MAX_CHANNELS    2
#define PCM_OUT_FIFO_BYTES      (PCM_OUT_MAX_RATE_HZ * PCM_OUT_MAX_CHANNELS * \
                                 sizeof(int16_t))

static void pcm_out_drop_queue(PCMOut *out)
{
    fifo8_reset(&out->fifo);
    out->playing = false;
}

static void pcm_out_play(void *opaque, int free_bytes)
{
    PCMOut *out = opaque;

    assert(bql_locked());
    while (free_bytes > 0) {
        uint32_t queued = fifo8_num_used(&out->fifo);

        out->playing = queued != 0 &&
                       (out->playing || queued >= out->prefill_bytes);
        if (!out->playing) {
            if (!out->active) {
                audio_be_set_active_out(out->be, out->voice, false);
            }
            return;
        }

        uint32_t max = MIN(queued, (uint32_t) free_bytes);
        uint32_t size;
        const uint8_t *data = fifo8_peek_bufptr(&out->fifo, max, &size);
        size_t written = audio_be_write(out->be, out->voice, (void *) data,
                                        size);
        if (written == 0) {
            return;
        }
        fifo8_drop(&out->fifo, written);
        free_bytes -= written;
    }
}

void pcm_out_init(PCMOut *out, AudioBackend *be, const char *name,
                  uint32_t prefill_ms, const DebugChannel *dbg)
{
    *out = (PCMOut) {
        .be = be,
        .name = name,
        .dbg = dbg,
        .prefill_ms = prefill_ms,
    };
    if (!audio_be_check(&out->be, NULL)) {
        DEBUG_LOG(dbg, "no audio backend, %s is silent\n", name);
        return;
    }
    fifo8_create(&out->fifo, PCM_OUT_FIFO_BYTES);
    out->has_fifo = true;
}

void pcm_out_cleanup(PCMOut *out)
{
    if (out->voice) {
        audio_be_set_active_out(out->be, out->voice, false);
        audio_be_close_out(out->be, out->voice);
        out->voice = NULL;
    }
    if (out->has_fifo) {
        fifo8_destroy(&out->fifo);
        out->has_fifo = false;
    }
}

void pcm_out_set_format(PCMOut *out, uint32_t rate_hz, uint32_t channels)
{
    struct audsettings settings = {
        .freq = rate_hz,
        .nchannels = channels,
        .fmt = AUDIO_FORMAT_S16,
        .big_endian = HOST_BIG_ENDIAN,
    };

    assert(bql_locked());
    if (!out->has_fifo) {
        return;
    }
    if (out->voice && out->rate_hz == rate_hz && out->channels == channels) {
        return;
    }

    DEBUG_LOG(out->dbg, "%s: %u Hz x%u -> %u Hz x%u\n", out->name,
              out->rate_hz, out->channels, rate_hz, channels);
    pcm_out_drop_queue(out);
    out->rate_hz = rate_hz;
    out->channels = channels;
    out->prefill_bytes = channels * sizeof(int16_t) * rate_hz *
                         out->prefill_ms / 1000;
    out->voice = audio_be_open_out(out->be, out->voice, out->name, out,
                                   pcm_out_play, &settings);
    if (!out->voice) {
        DEBUG_LOG(out->dbg, "cannot open audio voice %s\n", out->name);
        return;
    }
    audio_be_set_active_out(out->be, out->voice, out->active);
}

void pcm_out_set_active(PCMOut *out, bool active)
{
    assert(bql_locked());
    if (active == out->active) {
        return;
    }
    out->active = active;
    if (!active || !out->voice) {
        return;
    }
    pcm_out_drop_queue(out);
    audio_be_set_active_out(out->be, out->voice, true);
}

void pcm_out_stop(PCMOut *out)
{
    assert(bql_locked());
    if (out->voice) {
        audio_be_set_active_out(out->be, out->voice, false);
    }
    if (out->has_fifo) {
        pcm_out_drop_queue(out);
    }
    out->rate_hz = 0;
    out->channels = 0;
}

static void pcm_out_update_stats(PCMOut *out, const int16_t *samples,
                                 size_t count)
{
    for (size_t i = 0; i < count; i++) {
        int16_t sample = samples[i];

        out->stats_samples++;
        if (sample == 0) {
            continue;
        }
        if (out->stats_nonzero++ == 0) {
            out->stats_min = sample;
            out->stats_max = sample;
        }
        out->stats_min = MIN(out->stats_min, sample);
        out->stats_max = MAX(out->stats_max, sample);
    }

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now < out->stats_deadline_ns) {
        return;
    }
    if (out->stats_deadline_ns != 0 && out->stats_samples != 0) {
        DEBUG_LOG(out->dbg, "%s: samples=%" PRIu64 " nonzero=%" PRIu64
                  " min=%d max=%d\n", out->name, out->stats_samples,
                  out->stats_nonzero, out->stats_min, out->stats_max);
    }
    out->stats_deadline_ns = now + NANOSECONDS_PER_SECOND;
    out->stats_samples = 0;
    out->stats_nonzero = 0;
}

void pcm_out_write(PCMOut *out, const int16_t *frames, size_t frame_count)
{
    size_t count = frame_count * out->channels;
    size_t size = count * sizeof(int16_t);

    assert(bql_locked());
    if (out->channels == 0) {
        return;
    }
    if (unlikely(debug_channel_is_enabled(out->dbg, DEBUG_CH_LOG))) {
        pcm_out_update_stats(out, frames, count);
    }
    if (fifo8_num_free(&out->fifo) < size) {
        return;
    }
    fifo8_push_all(&out->fifo, (const uint8_t *) frames, size);
}
