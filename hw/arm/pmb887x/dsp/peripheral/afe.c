#define PMB887X_TRACE_ID		DSP_AFE
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-afe"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"

#include "qemu/host-utils.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#ifndef PMB887X_DSP_TESTS
#include "qemu/audio.h"
#include "qemu/fifo8.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "hw/arm/pmb887x/pmic.h"
#endif

#define AFE_REGISTER_COUNT	(TEAK_AFE_RINGCTRL + 1)
#define AFE_CONTROL_MASK	(TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART | TEAK_AFE_BCON_RXRATE | \
	TEAK_AFE_BCON_TXSTART | TEAK_AFE_BCON_TXRATE)
/*
 * The voiceband converters run at 8 kHz off the crystal, whatever clock the
 * DSP runs at. They are counted in DSP cycles, so with the DSP clock gated
 * they stop where the crystal would keep them going: this assumes the
 * firmware never gates the DSP with a stream running.
 */
#define AFE_SAMPLE_RATE		8000U
#define AFE_INTERRUPT_GROUP	1
/*
 * The 0x80-word AFE RAM is split into two 0x40-word rings addressed by the
 * 6-bit RD/WR pointers. The DSP transmits its decoded playback samples to the
 * DAC through the upper ring (ram_base + 0x40), paced by the TX sample clock and
 * TXINTPTR; the lower ring (ram_base + 0) is the mic-ADC capture buffer. Host
 * audio-out must therefore read the upper (DAC) ring, not the lower one.
 */
#define AFE_DAC_RING_OFFSET	0x40U

/* Voiceband downlink: 8 kHz, mono, signed 16-bit PCM. */
#define AFE_OUT_FREQ		8000
#define AFE_OUT_CHANNELS	1
/* Upper bounds for the streamed-PCM path (PCMPLAY can run 48 kHz stereo). */
#define AFE_OUT_MAX_FREQ	48000
#define AFE_OUT_MAX_CHANNELS	2
/*
 * Ring buffer bridging producer -> audio thread, sized for ~1 s at the worst
 * case so it fits any configured rate/channel count without being resized.
 */
#define AFE_OUT_FIFO_BYTES	(AFE_OUT_MAX_FREQ * AFE_OUT_MAX_CHANNELS * sizeof(int16_t))
/* Quiet time after which the DSP counts as no longer streaming to the host. */
#define AFE_STREAM_IDLE_MS	200

static const uint16_t AFE_POWER_DOWN_SAMPLES[] = {
	0x85EA, 0x85F3, 0xB12F, 0x8000, 0x9048, 0x8A3B, 0x81C2, 0x8BCF,
	0x8000, 0x8B30, 0x8290, 0x87AF, 0x8556, 0x85B5, 0x86BA, 0x84BF,
	0x875B, 0x8465, 0x8773, 0x848D, 0x870E, 0x8524, 0x8659, 0x85DF,
	0x85AE, 0x8669, 0x854D, 0x869E, 0x8544, 0x8682, 0x857A, 0x863A,
	0x85C9, 0x85ED, 0x860D, 0x85B8, 0x8632, 0x85A6, 0x8633, 0x85B3,
	0x861B, 0x85D1, 0x85FA, 0x85F1, 0x85DE, 0x8606, 0x85D0, 0x860D,
	0x85D1, 0x8606, 0x85DC, 0x85F9, 0x85EA, 0x85EC, 0x85F5, 0x85E3,
	0x85FB, 0x85E1, 0x85FA, 0x85E4, 0x85F5, 0x85EA, 0x85EF, 0x85F0,
};

typedef struct afe_state_t afe_state_t;

#ifndef PMB887X_DSP_TESTS
/* A format the samples take from @position, in bytes ever pushed, on. */
typedef struct afe_audio_format_t {
	uint64_t position;
	int freq;
	int channels;
} afe_audio_format_t;

/*
 * Bridges the DSP worker thread (which produces RX-DAC samples in afe_advance)
 * to the QEMU audio backend (which must be driven from the main loop via the
 * audio_be callback). Producer pushes little-endian S16 samples into a
 * mutex-protected FIFO; the audio callback drains it into the SWVoiceOut.
 */
typedef struct afe_audio_t {
	AudioBackend *backend;
	SWVoiceOut *voice;
	QemuMutex lock;
	Fifo8 fifo;
	bool fifo_ready;
	uint64_t pushed;
	uint64_t popped;
	/*
	 * Format changes still in the FIFO: the voice is reopened with each when
	 * playback reaches it, so no sample plays at another stream's rate.
	 */
	GArray *formats;
	QEMUBH *format_bh;

	/*
	 * The format the voice plays. Defaults to the voiceband 8 kHz mono, but
	 * streamed PCM can run at up to 48 kHz stereo.
	 */
	int out_freq;
	int out_channels;

	/* When the producer last handed over a sample, in host milliseconds. */
	uint32_t last_push_ms;

	/* Rate-limited non-zero-sample diagnostics (worker thread only). */
	int64_t stats_deadline;
	uint64_t stats_total;
	uint64_t stats_nonzero;
	int16_t stats_min;
	int16_t stats_max;
} afe_audio_t;
#endif

struct afe_state_t {
	uint16_t registers[AFE_REGISTER_COUNT];
	dsp_device_t *interrupt;
	dsp_host_t host;
	uint16_t ram_base;
	uint16_t receive_position;
	uint16_t transmit_position;
	uint32_t frequency;
	/* The clock the phases count in: the last one that ran. */
	uint32_t phase_frequency;
	uint64_t receive_phase;
	uint64_t transmit_phase;
#ifndef PMB887X_DSP_TESTS
	afe_audio_t audio;
#endif
};

static bool afe_receive_active(const afe_state_t *state) {
	uint16_t control = state->registers[TEAK_AFE_BCON];
	uint16_t active = TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART;
	return (control & active) == active;
}

static bool afe_transmit_active(const afe_state_t *state) {
	uint16_t control = state->registers[TEAK_AFE_BCON];
	uint16_t active = TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_TXSTART;
	return (control & active) == active;
}

#ifndef PMB887X_DSP_TESTS
/* Wrapping 32-bit host milliseconds; only differences are ever compared. */
static uint32_t afe_audio_host_ms(void) {
	return (uint32_t) (qemu_clock_get_ns(QEMU_CLOCK_HOST) / SCALE_MS);
}

/* Under the lock: bytes that play before the next format change. */
static uint64_t afe_audio_bytes_to_format(afe_audio_t *audio) {
	if (audio->formats->len == 0)
		return UINT64_MAX;
	return g_array_index(audio->formats, afe_audio_format_t, 0).position - audio->popped;
}

static void afe_audio_out_callback(void *opaque, int free_bytes) {
	afe_state_t *state = opaque;
	afe_audio_t *audio = &state->audio;
	uint8_t chunk[512];

	while (free_bytes > 0) {
		size_t want = MIN((size_t) free_bytes, sizeof(chunk));
		uint64_t until_format;
		size_t got;
		size_t written;

		qemu_mutex_lock(&audio->lock);
		until_format = afe_audio_bytes_to_format(audio);
		if (until_format == 0) {
			qemu_mutex_unlock(&audio->lock);
			qemu_bh_schedule(audio->format_bh);
			break;
		}
		got = MIN(MIN(want, fifo8_num_used(&audio->fifo)), until_format);
		got = got ? fifo8_pop_buf(&audio->fifo, chunk, got) : 0;
		audio->popped += got;
		qemu_mutex_unlock(&audio->lock);

		if (got == 0) {
			/*
			 * Mid-stream underrun: write nothing. The DSP emits silence of
			 * its own between sounds, so a gap here is only the emulated core
			 * failing to synthesise in real time, and padding it over would
			 * stretch out the stream that does arrive.
			 */
			if (afe_audio_host_ms() - qatomic_read(&audio->last_push_ms) < AFE_STREAM_IDLE_MS)
				break;
			/*
			 * Idle: keep feeding the voice. It shares the host's mixer with
			 * the other sound sources in the machine, and one that is enabled
			 * but never writes holds all of them up.
			 */
			memset(chunk, 0, want);
			got = want;
		}

		written = audio_be_write(audio->backend, audio->voice, chunk, got);
		if (written == 0)
			break;
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

static void afe_audio_produce(afe_state_t *state, uint16_t sample_word) {
	afe_audio_t *audio = &state->audio;
	int16_t sample = (int16_t) sample_word;
	uint8_t bytes[2];

	/*
	 * The DSP reaches the speaker through the codec's amplifier path 4, which is
	 * where the phone's volume setting lands.
	 */
	sample = (int16_t) ((sample * (int64_t) pmb887x_pmic_output_gain(PMB887X_PMIC_PATH_STREAM)) /
		PMB887X_PMIC_GAIN_UNITY);
	sample_word = (uint16_t) sample;
	bytes[0] = (uint8_t) sample_word;
	bytes[1] = (uint8_t) (sample_word >> 8);

	audio->stats_total++;
	if (sample != 0) {
		audio->stats_nonzero++;
		if (sample < audio->stats_min)
			audio->stats_min = sample;
		if (sample > audio->stats_max)
			audio->stats_max = sample;
	}
	afe_audio_report_stats(audio);

	if (!audio->fifo_ready)
		return;

	qemu_mutex_lock(&audio->lock);
	if (fifo8_num_used(&audio->fifo) + sizeof(bytes) <= AFE_OUT_FIFO_BYTES) {
		fifo8_push_all(&audio->fifo, bytes, sizeof(bytes));
		audio->pushed += sizeof(bytes);
	}
	qemu_mutex_unlock(&audio->lock);
}

static SWVoiceOut *afe_audio_open(afe_state_t *state, int freq, int channels) {
	struct audsettings as = {
		.freq = freq,
		.nchannels = channels,
		.fmt = AUDIO_FORMAT_S16,
		.big_endian = false,
	};

	return audio_be_open_out(state->audio.backend, NULL, "pmb887x-afe", state, afe_audio_out_callback, &as);
}

/* In the main loop, once playback has reached a format change. */
static void afe_audio_format_bh(void *opaque) {
	afe_state_t *state = opaque;
	afe_audio_t *audio = &state->audio;
	int old_freq, old_channels;
	int freq, channels;

	qemu_mutex_lock(&audio->lock);
	old_freq = freq = audio->out_freq;
	old_channels = channels = audio->out_channels;
	while (afe_audio_bytes_to_format(audio) == 0) {
		afe_audio_format_t *format = &g_array_index(audio->formats, afe_audio_format_t, 0);

		freq = format->freq;
		channels = format->channels;
		g_array_remove_index(audio->formats, 0);
	}
	audio->out_freq = freq;
	audio->out_channels = channels;
	qemu_mutex_unlock(&audio->lock);

	if ((freq == old_freq && channels == old_channels) || audio->voice == NULL)
		return;

	DPRINTF("audio out format %d Hz/%dch -> %d Hz/%dch\n", old_freq, old_channels, freq, channels);
	audio_be_set_active_out(audio->backend, audio->voice, false);
	audio_be_close_out(audio->backend, audio->voice);
	audio->voice = afe_audio_open(state, freq, channels);
	if (audio->voice)
		audio_be_set_active_out(audio->backend, audio->voice, true);
}

static void afe_audio_init(afe_state_t *state) {
	afe_audio_t *audio = &state->audio;

	audio->out_freq = AFE_OUT_FREQ;
	audio->out_channels = AFE_OUT_CHANNELS;
	audio->formats = g_array_new(false, false, sizeof(afe_audio_format_t));
	qemu_mutex_init(&audio->lock);

	/* Lazily bind the default -audiodev; degrade to silence if none. */
	if (!audio_be_check(&audio->backend, NULL)) {
		DPRINTF("no audio backend configured; RX output disabled\n");
		return;
	}

	audio->voice = afe_audio_open(state, AFE_OUT_FREQ, AFE_OUT_CHANNELS);
	if (!audio->voice) {
		EPRINTF("could not open audio out voice\n");
		return;
	}

	audio->format_bh = qemu_bh_new(afe_audio_format_bh, state);
	fifo8_create(&audio->fifo, AFE_OUT_FIFO_BYTES);
	audio->fifo_ready = true;
	/* Keep the voice active; the callback fills silence when idle. */
	audio_be_set_active_out(audio->backend, audio->voice, true);
}

static void afe_audio_reset(afe_state_t *state) {
	afe_audio_t *audio = &state->audio;

	if (!audio->fifo_ready)
		return;

	qemu_mutex_lock(&audio->lock);
	fifo8_reset(&audio->fifo);
	audio->pushed = audio->popped = 0;
	/* Nothing is left to play in the old formats: the latest one applies at once. */
	for (size_t i = 0; i < audio->formats->len; i++)
		g_array_index(audio->formats, afe_audio_format_t, i).position = 0;
	qemu_mutex_unlock(&audio->lock);
}

static void afe_audio_destroy(afe_state_t *state) {
	afe_audio_t *audio = &state->audio;

	if (audio->voice) {
		audio_be_set_active_out(audio->backend, audio->voice, false);
		audio_be_close_out(audio->backend, audio->voice);
		audio->voice = NULL;
	}
	if (audio->fifo_ready) {
		qemu_bh_delete(audio->format_bh);
		fifo8_destroy(&audio->fifo);
		audio->fifo_ready = false;
	}
	g_array_free(audio->formats, true);
	qemu_mutex_destroy(&audio->lock);
}
#endif /* PMB887X_DSP_TESTS */

static void afe_destroy(dsp_device_t *device) {
#ifndef PMB887X_DSP_TESTS
	afe_audio_destroy(device->state);
#endif
	g_free(device->state);
}

static void afe_reset(dsp_device_t *device) {
	afe_state_t *state = device->state;

	memset(state->registers, 0, sizeof(state->registers));
	state->receive_position = 0;
	state->transmit_position = 0;
	state->receive_phase = 0;
	state->transmit_phase = 0;
#ifndef PMB887X_DSP_TESTS
	afe_audio_reset(state);
#endif
}

static bool afe_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	afe_state_t *state = device->state;

	switch (offset) {
		case TEAK_AFE_INTPTR:
			*value = 0;
			break;

		case TEAK_AFE_RWADDR:
			*value = state->receive_position | state->transmit_position << TEAK_AFE_RWADDR_WRADDR_SHIFT;
			break;

		default:
			*value = offset < AFE_REGISTER_COUNT ? state->registers[offset] : 0;
			break;
	}

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool afe_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	afe_state_t *state = device->state;

	switch (offset) {
		case TEAK_AFE_INTPTR:
			state->registers[offset] = value & (TEAK_AFE_INTPTR_RXINTPTR | TEAK_AFE_INTPTR_TXINTPTR);
			break;

		case TEAK_AFE_BCON:
			state->registers[offset] = value & AFE_CONTROL_MASK;
			if (!afe_receive_active(state)) {
				state->receive_position = 0;
				state->receive_phase = 0;
			}
			if (!afe_transmit_active(state)) {
				state->transmit_position = 0;
				state->transmit_phase = 0;
			}
			break;

		case TEAK_AFE_RWADDR:
			break;

		default:
			if (offset < AFE_REGISTER_COUNT)
				state->registers[offset] = value;
			break;
	}

	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t afe_ops = {
	.destroy = afe_destroy,
	.reset = afe_reset,
	.read = afe_read,
	.write = afe_write,
};

dsp_device_t *afe_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt, const dsp_host_t *host) {
	afe_state_t *state = g_new0(afe_state_t, 1);
	state->interrupt = interrupt;
	state->host = *host;
	state->ram_base = config->ram_base;
#ifndef PMB887X_DSP_TESTS
	afe_audio_init(state);
#endif
	return dsp_device_create(config, &afe_ops, state);
}

static void afe_receive_sample(afe_state_t *state) {
	uint16_t interrupt_position = state->registers[TEAK_AFE_INTPTR] & TEAK_AFE_INTPTR_RXINTPTR;

	state->receive_position++;
	state->receive_position &= TEAK_AFE_RWADDR_RDADDR;

	if (state->receive_position == interrupt_position)
		dsp_int_set_flags(state->interrupt, AFE_INTERRUPT_GROUP, TEAK_INT_FINTB0_VBRX);
}

static void afe_transmit_sample(afe_state_t *state) {
	uint16_t interrupt_position = state->registers[TEAK_AFE_INTPTR] >> TEAK_AFE_INTPTR_TXINTPTR_SHIFT;
	bool power_down = (state->registers[TEAK_AFE_VTXCTRL] & TEAK_AFE_VTXCTRL_TXMODE) ==
		TEAK_AFE_VTXCTRL_TXMODE_POWER_DOWN;

#ifndef PMB887X_DSP_TESTS
	/* Play out the DSP's decoded sample sitting at the DAC read pointer. */
	afe_audio_produce(state, state->host.data_read(state->host.opaque,
		state->ram_base + AFE_DAC_RING_OFFSET + state->transmit_position));
#endif

	if (power_down) {
		state->host.data_write(state->host.opaque, state->ram_base + state->transmit_position,
			AFE_POWER_DOWN_SAMPLES[state->transmit_position]);
	} else {
		state->host.data_write(state->host.opaque, state->ram_base + state->transmit_position, 0);
	}

	state->transmit_position++;
	state->transmit_position &= TEAK_AFE_RWADDR_WRADDR >> TEAK_AFE_RWADDR_WRADDR_SHIFT;

	if (state->transmit_position == interrupt_position)
		dsp_int_set_flags(state->interrupt, AFE_INTERRUPT_GROUP, TEAK_INT_FINTB0_VBTX);
}

/* The sample clocks keep their place across a change of DSP clock, and across a gap in it. */
void afe_set_frequency(dsp_device_t *device, uint32_t frequency) {
	afe_state_t *state = device->state;

	state->frequency = frequency;
	if (frequency == 0)
		return;
	if (state->phase_frequency != 0) {
		state->receive_phase = muldiv64(state->receive_phase, frequency, state->phase_frequency);
		state->transmit_phase = muldiv64(state->transmit_phase, frequency, state->phase_frequency);
	}
	state->phase_frequency = frequency;
}

void afe_advance(dsp_device_t *device, size_t cycles) {
	afe_state_t *state = device->state;

	if (state->frequency == 0)
		return;

	if (afe_receive_active(state)) {
		state->receive_phase += (uint64_t) cycles * AFE_SAMPLE_RATE;
		while (state->receive_phase >= state->frequency) {
			state->receive_phase -= state->frequency;
			afe_receive_sample(state);
		}
	}

	if (afe_transmit_active(state)) {
		state->transmit_phase += (uint64_t) cycles * AFE_SAMPLE_RATE;
		while (state->transmit_phase >= state->frequency) {
			state->transmit_phase -= state->frequency;
			afe_transmit_sample(state);
		}
	}
}

size_t afe_next_event(dsp_device_t *device) {
	afe_state_t *state = device->state;
	size_t cycles = SIZE_MAX;

	if (afe_receive_active(state))
		cycles = dsp_rate_cycles_until(state->receive_phase, AFE_SAMPLE_RATE, state->frequency);
	if (afe_transmit_active(state))
		cycles = MIN(cycles, dsp_rate_cycles_until(state->transmit_phase, AFE_SAMPLE_RATE, state->frequency));
	return cycles;
}

bool afe_is_active(const dsp_device_t *device) {
	const afe_state_t *state = device->state;
	return afe_receive_active(state) || afe_transmit_active(state);
}

#ifndef PMB887X_DSP_TESTS
size_t afe_audio_push_samples(dsp_device_t *device, const uint16_t *samples, size_t count) {
	afe_state_t *state = device->state;

	qatomic_set(&state->audio.last_push_ms, afe_audio_host_ms());
	for (size_t i = 0; i < count; i++)
		afe_audio_produce(state, samples[i]);
	return count;
}

/*
 * From any thread: samples pushed from now on take this format. The voice
 * switches to it when playback gets to them.
 */
void afe_audio_set_format(dsp_device_t *device, unsigned freq, unsigned channels) {
	afe_state_t *state = device->state;
	afe_audio_t *audio = &state->audio;
	afe_audio_format_t format;

	qemu_mutex_lock(&audio->lock);
	if (audio->formats->len != 0)
		format = g_array_index(audio->formats, afe_audio_format_t, audio->formats->len - 1);
	else
		format = (afe_audio_format_t) { .freq = audio->out_freq, .channels = audio->out_channels };
	if (freq == 0)
		freq = format.freq;
	channels = MIN(channels == 0 ? format.channels : channels, AFE_OUT_MAX_CHANNELS);
	if ((int) freq != format.freq || (int) channels != format.channels) {
		format = (afe_audio_format_t) { .position = audio->pushed, .freq = freq, .channels = channels };
		if (audio->fifo_ready) {
			g_array_append_val(audio->formats, format);
		} else {
			audio->out_freq = freq;
			audio->out_channels = channels;
		}
	}
	qemu_mutex_unlock(&audio->lock);
}

/* How much audio the backend still has to play out, in samples. */
size_t afe_audio_queued_samples(dsp_device_t *device) {
	afe_state_t *state = device->state;
	afe_audio_t *audio = &state->audio;
	size_t used;

	if (!audio->fifo_ready)
		return 0;

	qemu_mutex_lock(&audio->lock);
	used = fifo8_num_used(&audio->fifo) / (sizeof(int16_t) * audio->out_channels);
	qemu_mutex_unlock(&audio->lock);
	return used;
}

bool afe_audio_has_room(dsp_device_t *device, size_t count) {
	afe_state_t *state = device->state;
	afe_audio_t *audio = &state->audio;
	bool room;

	/* No live backend (silent): never back-pressure the producer. */
	if (!audio->fifo_ready)
		return true;

	qemu_mutex_lock(&audio->lock);
	room = fifo8_num_used(&audio->fifo) + count * sizeof(int16_t) <= AFE_OUT_FIFO_BYTES;
	qemu_mutex_unlock(&audio->lock);
	return room;
}
#endif
