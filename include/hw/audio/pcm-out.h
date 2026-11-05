/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_AUDIO_PCM_OUT_H
#define HW_AUDIO_PCM_OUT_H

#include "qemu/audio.h"
#include "qemu/debug-channel.h"
#include "qemu/fifo8.h"

typedef struct PCMOut {
    AudioBackend *be;
    SWVoiceOut *voice;
    const char *name;
    const DebugChannel *dbg;
    Fifo8 fifo;
    bool has_fifo;
    uint32_t prefill_ms;
    uint32_t prefill_bytes;
    uint32_t rate_hz;
    uint32_t channels;
    bool active;
    bool playing;
    int64_t stats_deadline_ns;
    uint64_t stats_samples;
    uint64_t stats_nonzero;
    int16_t stats_min;
    int16_t stats_max;
} PCMOut;

void pcm_out_init(PCMOut *out, AudioBackend *be, const char *name,
                  uint32_t prefill_ms, const DebugChannel *dbg);
void pcm_out_cleanup(PCMOut *out);
void pcm_out_set_format(PCMOut *out, uint32_t rate_hz, uint32_t channels);
void pcm_out_set_active(PCMOut *out, bool active);
void pcm_out_stop(PCMOut *out);
void pcm_out_write(PCMOut *out, const int16_t *frames, size_t frame_count);

#endif
