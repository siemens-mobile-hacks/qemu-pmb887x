#include "qemu/osdep.h"
#include "qemu/audio.h"
#include "qemu/debug-channel.h"
#include "qemu/fifo8.h"
#include "qemu/thread.h"
#include "qemu/timer.h"

#include "hw/arm/pmb887x/audio/buffer.h"
#include "hw/arm/pmb887x/dsp/peripheral/afe-audio.h"

#define AFE_AUDIO_DEFAULT_RATE_HZ	8000
#define AFE_AUDIO_DEFAULT_CHANNELS	1
#define AFE_AUDIO_MAX_CHANNELS		2
#define AFE_AUDIO_MAX_RATE_HZ		48000
#define AFE_AUDIO_FIFO_BYTES		(AFE_AUDIO_MAX_RATE_HZ * AFE_AUDIO_MAX_CHANNELS * sizeof(int16_t))

struct afe_audio_t {
	const DebugChannel *dbg;
	AudioBackend *backend;
	SWVoiceOut *voice;
	QemuMutex lock;
	Fifo8 fifo;
	bool fifo_ready;
	pmb887x_audio_buffer_t buffer;
	int rate_hz;
	int channels;
	bool active;
	int64_t stats_deadline;
	uint64_t stats_total;
	uint64_t stats_nonzero;
	int16_t stats_min;
	int16_t stats_max;
};

static void afe_audio_out_callback(void *opaque, int free_bytes) {
	afe_audio_t *audio = opaque;

	while (free_bytes > 0) {
		size_t size;
		size_t written;
		const uint8_t *data = pmb887x_audio_buffer_peek(&audio->buffer,
			&audio->fifo, &audio->lock, (size_t) free_bytes, &size);
		if (size == 0) {
			/* A stopped receive path still plays the samples queued before it stopped. */
			if (!audio->active)
				audio_be_set_active_out(audio->backend, audio->voice, false);
			break;
		}

		written = audio_be_write(audio->backend, audio->voice, (void *) data, size);
		if (written == 0)
			break;
		pmb887x_audio_buffer_consume(&audio->buffer, written);
		free_bytes -= written;
	}
}

static void afe_audio_report_stats(afe_audio_t *audio) {
	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

	if (audio->stats_deadline == 0) {
		audio->stats_deadline = now + NANOSECONDS_PER_SECOND;
		return;
	}
	if (now < audio->stats_deadline)
		return;

	if (audio->stats_total != 0) {
		DEBUG_LOG(audio->dbg, "audio out: samples=%" PRIu64 " nonzero=%" PRIu64 " min=%d max=%d\n",
			audio->stats_total, audio->stats_nonzero, audio->stats_min, audio->stats_max);
	}

	audio->stats_deadline = now + NANOSECONDS_PER_SECOND;
	audio->stats_total = 0;
	audio->stats_nonzero = 0;
	audio->stats_min = 0;
	audio->stats_max = 0;
}

afe_audio_t *afe_audio_create(const DebugChannel *dbg) {
	afe_audio_t *audio = g_new0(afe_audio_t, 1);
	struct audsettings settings = {
		.freq = AFE_AUDIO_DEFAULT_RATE_HZ,
		.nchannels = AFE_AUDIO_DEFAULT_CHANNELS,
		.fmt = AUDIO_FORMAT_S16,
		.big_endian = false,
	};

	audio->dbg = dbg;
	audio->rate_hz = AFE_AUDIO_DEFAULT_RATE_HZ;
	audio->channels = AFE_AUDIO_DEFAULT_CHANNELS;
	pmb887x_audio_buffer_configure(&audio->buffer, audio->channels * sizeof(int16_t), audio->rate_hz);
	qemu_mutex_init(&audio->lock);
	if (!audio_be_check(&audio->backend, NULL)) {
		DEBUG_LOG(audio->dbg, "no audio backend configured; RX output disabled\n");
		return audio;
	}

	audio->voice = audio_be_open_out(audio->backend, NULL, "pmb887x-afe",
		audio, afe_audio_out_callback, &settings);
	if (!audio->voice) {
		DEBUG_LOG(audio->dbg, "could not open audio out voice; RX output disabled\n");
		return audio;
	}

	fifo8_create(&audio->fifo, AFE_AUDIO_FIFO_BYTES);
	audio->fifo_ready = true;
	return audio;
}

void afe_audio_destroy(afe_audio_t *audio) {
	if (audio == NULL)
		return;

	if (audio->voice) {
		audio_be_set_active_out(audio->backend, audio->voice, false);
		audio_be_close_out(audio->backend, audio->voice);
	}
	if (audio->fifo_ready)
		fifo8_destroy(&audio->fifo);
	qemu_mutex_destroy(&audio->lock);
	g_free(audio);
}

static void afe_audio_reset(afe_audio_t *audio) {
	if (!audio->fifo_ready)
		return;

	qemu_mutex_lock(&audio->lock);
	fifo8_reset(&audio->fifo);
	qemu_mutex_unlock(&audio->lock);
	pmb887x_audio_buffer_reset(&audio->buffer);
}

void afe_audio_set_active(afe_audio_t *audio, bool active) {
	if (active == audio->active)
		return;

	audio->active = active;
	if (!active)
		return;

	afe_audio_reset(audio);
	if (audio->voice)
		audio_be_set_active_out(audio->backend, audio->voice, true);
}

void afe_audio_set_format(afe_audio_t *audio, unsigned frequency_hz, unsigned channels) {
	struct audsettings settings = {
		.freq = frequency_hz,
		.nchannels = channels,
		.fmt = AUDIO_FORMAT_S16,
		.big_endian = false,
	};

	if ((int) frequency_hz == audio->rate_hz && (int) channels == audio->channels)
		return;

	DEBUG_LOG(audio->dbg, "audio out %d Hz x%d -> %u Hz x%u\n", audio->rate_hz, audio->channels, frequency_hz, channels);
	afe_audio_reset(audio);
	audio->rate_hz = frequency_hz;
	audio->channels = channels;
	pmb887x_audio_buffer_configure(&audio->buffer, channels * sizeof(int16_t), frequency_hz);
	if (!audio->voice)
		return;

	audio->voice = audio_be_open_out(audio->backend, audio->voice, "pmb887x-afe",
		audio, afe_audio_out_callback, &settings);
	if (audio->voice)
		audio_be_set_active_out(audio->backend, audio->voice, audio->active);
}

static void afe_audio_count_sample(afe_audio_t *audio, int16_t sample) {
	audio->stats_total++;
	if (sample == 0)
		return;

	audio->stats_nonzero++;
	if (audio->stats_nonzero == 1) {
		audio->stats_min = sample;
		audio->stats_max = sample;
	} else if (sample < audio->stats_min) {
		audio->stats_min = sample;
	} else if (sample > audio->stats_max) {
		audio->stats_max = sample;
	}
}

void afe_audio_produce(afe_audio_t *audio, const uint16_t *frame, size_t word_count) {
	uint8_t bytes[AFE_AUDIO_MAX_CHANNELS * sizeof(int16_t)];

	for (size_t i = 0; i < word_count; i++) {
		afe_audio_count_sample(audio, (int16_t) frame[i]);
		bytes[i * 2] = (uint8_t) frame[i];
		bytes[i * 2 + 1] = (uint8_t) (frame[i] >> 8);
	}
	afe_audio_report_stats(audio);

	if (!audio->fifo_ready)
		return;

	size_t size = word_count * sizeof(int16_t);
	qemu_mutex_lock(&audio->lock);
	if (fifo8_num_used(&audio->fifo) + size <= AFE_AUDIO_FIFO_BYTES)
		fifo8_push_all(&audio->fifo, bytes, size);
	qemu_mutex_unlock(&audio->lock);
}
