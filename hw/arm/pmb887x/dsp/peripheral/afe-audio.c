#define PMB887X_TRACE_ID		DSP_AFE
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-afe"

#include "qemu/osdep.h"
#include "qemu/audio.h"
#include "qemu/fifo8.h"
#include "qemu/thread.h"
#include "qemu/timer.h"

#include "hw/arm/pmb887x/audio/buffer.h"
#include "hw/arm/pmb887x/dsp/peripheral/afe-audio.h"
#include "hw/arm/pmb887x/trace.h"

#define AFE_AUDIO_DEFAULT_RATE_HZ	8000
#define AFE_AUDIO_CHANNELS		1
#define AFE_AUDIO_MAX_RATE_HZ		48000
#define AFE_AUDIO_FIFO_BYTES		(AFE_AUDIO_MAX_RATE_HZ * sizeof(int16_t))

struct afe_audio_t {
	AudioBackend *backend;
	SWVoiceOut *voice;
	QemuMutex lock;
	Fifo8 fifo;
	bool fifo_ready;
	pmb887x_audio_buffer_t buffer;
	int rate_hz;
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
		DPRINTF("audio out: samples=%" PRIu64 " nonzero=%" PRIu64 " min=%d max=%d\n",
			audio->stats_total, audio->stats_nonzero, audio->stats_min, audio->stats_max);
	}

	audio->stats_deadline = now + NANOSECONDS_PER_SECOND;
	audio->stats_total = 0;
	audio->stats_nonzero = 0;
	audio->stats_min = 0;
	audio->stats_max = 0;
}

afe_audio_t *afe_audio_create(void) {
	afe_audio_t *audio = g_new0(afe_audio_t, 1);
	struct audsettings settings = {
		.freq = AFE_AUDIO_DEFAULT_RATE_HZ,
		.nchannels = AFE_AUDIO_CHANNELS,
		.fmt = AUDIO_FORMAT_S16,
		.big_endian = false,
	};

	audio->rate_hz = AFE_AUDIO_DEFAULT_RATE_HZ;
	qemu_mutex_init(&audio->lock);
	if (!audio_be_check(&audio->backend, NULL)) {
		DPRINTF("no audio backend configured; RX output disabled\n");
		return audio;
	}

	audio->voice = audio_be_open_out(audio->backend, NULL, "pmb887x-afe",
		audio, afe_audio_out_callback, &settings);
	if (!audio->voice) {
		EPRINTF("could not open audio out voice\n");
		return audio;
	}

	fifo8_create(&audio->fifo, AFE_AUDIO_FIFO_BYTES);
	audio->fifo_ready = true;
	audio_be_set_active_out(audio->backend, audio->voice, true);
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

void afe_audio_reset(afe_audio_t *audio) {
	if (!audio->fifo_ready)
		return;

	qemu_mutex_lock(&audio->lock);
	fifo8_reset(&audio->fifo);
	qemu_mutex_unlock(&audio->lock);
	pmb887x_audio_buffer_reset(&audio->buffer);
}

void afe_audio_set_rate(afe_audio_t *audio, unsigned frequency_hz) {
	struct audsettings settings = {
		.freq = frequency_hz,
		.nchannels = AFE_AUDIO_CHANNELS,
		.fmt = AUDIO_FORMAT_S16,
		.big_endian = false,
	};

	if ((int) frequency_hz == audio->rate_hz)
		return;

	DPRINTF("audio out rate %d Hz -> %u Hz\n", audio->rate_hz, frequency_hz);
	afe_audio_reset(audio);
	audio->rate_hz = frequency_hz;
	if (!audio->voice)
		return;

	audio_be_set_active_out(audio->backend, audio->voice, false);
	audio_be_close_out(audio->backend, audio->voice);
	audio->voice = audio_be_open_out(audio->backend, NULL, "pmb887x-afe",
		audio, afe_audio_out_callback, &settings);
	if (audio->voice)
		audio_be_set_active_out(audio->backend, audio->voice, true);
}

void afe_audio_produce(afe_audio_t *audio, uint16_t sample_word) {
	int16_t sample = (int16_t) sample_word;
	uint8_t bytes[2] = { (uint8_t) sample_word, (uint8_t) (sample_word >> 8) };

	audio->stats_total++;
	if (sample != 0) {
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
	afe_audio_report_stats(audio);

	if (!audio->fifo_ready)
		return;

	qemu_mutex_lock(&audio->lock);
	if (fifo8_num_used(&audio->fifo) + sizeof(bytes) <= AFE_AUDIO_FIFO_BYTES)
		fifo8_push_all(&audio->fifo, bytes, sizeof(bytes));
	qemu_mutex_unlock(&audio->lock);
}
