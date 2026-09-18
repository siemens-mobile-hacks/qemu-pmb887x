/*
 * QEMU audio backend for the browser (emscripten)
 *
 * SPDX-License-Identifier: MIT
 *
 * The mixeng output lands in a single-producer/single-consumer ring that
 * lives in the wasm heap.  The page sees the same bytes (the heap is a
 * SharedArrayBuffer under -pthread), so an AudioWorklet on the audio render
 * thread drains the ring directly - no message passing per buffer, and
 * nothing to copy on the main thread.
 *
 * The ring is static: its address never changes, so the page can map it once
 * before qemu has opened a voice.  A short write is the whole pacing story -
 * audio_generic_run_buffer_out() stops on it, the mixeng buffer fills, and
 * the device's callback stops being asked for samples, which is what keeps a
 * guest that renders faster than real time from running away.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/audio.h"
#include "qemu/atomic.h"
#include "qom/object.h"

#include <emscripten.h>

#include "audio_int.h"

#define TYPE_AUDIO_WASM "audio-wasm"
OBJECT_DECLARE_SIMPLE_TYPE(AudioWasm, AUDIO_WASM)

struct AudioWasm {
    AudioMixengBackend parent_obj;
};

#define WASM_AUDIO_MAGIC        0x57534e44  /* "WSND" */
#define WASM_AUDIO_MAX_CHANNELS 2
#define WASM_AUDIO_RING_FRAMES  32768       /* power of two, 0.68 s @ 48 kHz */

/*
 * Header fields are int32 so the page can view them as one Int32Array and use
 * Atomics on the two cursors.  @read and @underruns are written only by the
 * consumer, @write only by the producer, @active only by the page.  Both
 * cursors are free-running frame counters; they wrap together, so the
 * difference stays correct as two's complement.
 */
typedef struct WasmAudioRing {
    int32_t magic;
    int32_t capacity;                   /* frames, power of two */
    int32_t channels;
    int32_t freq;
    int32_t read;
    int32_t write;
    int32_t active;
    int32_t generation;                 /* bumped whenever the format changes */
    int32_t underruns;
    int32_t reserved[7];
    int16_t data[WASM_AUDIO_RING_FRAMES * WASM_AUDIO_MAX_CHANNELS];
} WasmAudioRing;

static WasmAudioRing wasm_audio_ring = {
    .magic = WASM_AUDIO_MAGIC,
    .capacity = WASM_AUDIO_RING_FRAMES,
};

/* Exported to JS: the ring's wasm heap offset, valid from startup. */
EMSCRIPTEN_KEEPALIVE
void *wasm_audio_ring_ptr(void)
{
    return &wasm_audio_ring;
}

EMSCRIPTEN_KEEPALIVE
uint32_t wasm_audio_ring_bytes(void)
{
    return sizeof(wasm_audio_ring);
}

typedef struct WasmVoiceOut {
    HWVoiceOut hw;
    RateCtl rate;
    bool listening;
} WasmVoiceOut;

static size_t wasm_write(HWVoiceOut *hw, void *buf, size_t len)
{
    WasmVoiceOut *w = (WasmVoiceOut *) hw;
    WasmAudioRing *ring = &wasm_audio_ring;
    size_t bpf = hw->info.bytes_per_frame;
    const int16_t *src = buf;
    bool listening = qatomic_read(&ring->active);
    int32_t rd, wr, used;
    size_t frames, room, n;

    if (listening != w->listening) {
        /* The rate control only counts what the silent path consumed, so its
         * debt grows for as long as the ring was carrying the audio instead.
         * Restart it on the edge: otherwise the first muted buffer is asked
         * to make up every second spent playing, swallows whatever it is
         * offered, and only then resets itself. */
        w->listening = listening;
        audio_rate_start(&w->rate);
    }

    if (!listening) {
        /* Nobody is listening: consume at the nominal rate so the guest's
         * audio pipeline keeps draining instead of wedging. */
        return audio_rate_get_bytes(&w->rate, &hw->info, len);
    }

    frames = len / bpf;
    rd = qatomic_load_acquire(&ring->read);
    wr = ring->write;
    used = wr - rd;
    room = (used >= 0 && used < ring->capacity) ? ring->capacity - used : 0;
    if (frames > room) {
        frames = room;
    }

    n = 0;
    while (n < frames) {
        size_t pos = (size_t) ((uint32_t) (wr + n) & (WASM_AUDIO_RING_FRAMES - 1));
        size_t run = MIN(frames - n, WASM_AUDIO_RING_FRAMES - pos);

        memcpy(&ring->data[pos * ring->channels],
               &src[n * ring->channels],
               run * ring->channels * sizeof(int16_t));
        n += run;
    }

    qatomic_store_release(&ring->write, wr + (int32_t) n);
    return n * bpf;
}

static int wasm_init_out(HWVoiceOut *hw, struct audsettings *as)
{
    WasmVoiceOut *w = (WasmVoiceOut *) hw;
    WasmAudioRing *ring = &wasm_audio_ring;

    if (as->nchannels > WASM_AUDIO_MAX_CHANNELS) {
        return -1;
    }

    /* The ring carries native-endian S16 frames; mixeng converts for us. */
    as->fmt = AUDIO_FORMAT_S16;
    as->big_endian = false;

    audio_pcm_init_info(&hw->info, as);
    hw->samples = 4096;

    qatomic_set(&ring->read, 0);
    qatomic_set(&ring->write, 0);
    ring->channels = as->nchannels;
    ring->freq = as->freq;
    qatomic_store_release(&ring->generation, ring->generation + 1);

    w->listening = qatomic_read(&ring->active);
    audio_rate_start(&w->rate);
    return 0;
}

static void wasm_fini_out(HWVoiceOut *hw)
{
    WasmAudioRing *ring = &wasm_audio_ring;

    ring->freq = 0;
    qatomic_store_release(&ring->generation, ring->generation + 1);
}

static void wasm_enable_out(HWVoiceOut *hw, bool enable)
{
    WasmVoiceOut *w = (WasmVoiceOut *) hw;

    if (enable) {
        audio_rate_start(&w->rate);
    }
}

static void audio_wasm_class_init(ObjectClass *klass, const void *data)
{
    AudioMixengBackendClass *k = AUDIO_MIXENG_BACKEND_CLASS(klass);

    k->max_voices_out = 1;
    k->max_voices_in = 0;
    k->voice_size_out = sizeof(WasmVoiceOut);

    k->init_out = wasm_init_out;
    k->fini_out = wasm_fini_out;
    k->write = wasm_write;
    k->buffer_get_free = audio_generic_buffer_get_free;
    k->run_buffer_out = audio_generic_run_buffer_out;
    k->enable_out = wasm_enable_out;
}

static const TypeInfo audio_types[] = {
    {
        .name = TYPE_AUDIO_WASM,
        .parent = TYPE_AUDIO_MIXENG_BACKEND,
        .instance_size = sizeof(AudioWasm),
        .class_init = audio_wasm_class_init,
    },
};

DEFINE_TYPES(audio_types)
module_obj(TYPE_AUDIO_WASM);
