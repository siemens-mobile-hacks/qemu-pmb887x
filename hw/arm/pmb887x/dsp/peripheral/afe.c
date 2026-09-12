#define PMB887X_TRACE_ID		DSP_AFE
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-afe"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#ifndef PMB887X_DSP_TESTS
#include "qemu/audio.h"
#include "qemu/fifo8.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#endif

#define AFE_REGISTER_COUNT	(TEAK_AFE_RINGCTRL + 1)
#define AFE_CONTROL_MASK	(TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART | TEAK_AFE_BCON_RXRATE | \
	TEAK_AFE_BCON_TXSTART | TEAK_AFE_BCON_TXRATE)
#define AFE_SAMPLE_CYCLES	16U
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

	/*
	 * Output sample rate. Defaults to the voiceband 8 kHz, but streamed PCM
	 * (PCMPLAY) can run at higher rates, so the host driving the direct bridge
	 * sets it per stream via afe_audio_set_rate() -- otherwise a 16 kHz clip
	 * played at 8 kHz drops an octave and runs at half speed.
	 */
	int out_freq;
	/* Output channel count (1 mono / 2 interleaved LR); set per stream too. */
	int out_channels;

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
	size_t receive_cycles;
	size_t transmit_cycles;
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
static void afe_audio_out_callback(void *opaque, int free_bytes) {
	afe_state_t *state = opaque;
	afe_audio_t *audio = &state->audio;
	uint8_t chunk[512];

	while (free_bytes > 0) {
		size_t want = MIN((size_t) free_bytes, sizeof(chunk));
		size_t got;
		size_t written;

		qemu_mutex_lock(&audio->lock);
		size_t avail = fifo8_num_used(&audio->fifo);
		size_t take = MIN(want, avail);
		got = take ? fifo8_pop_buf(&audio->fifo, chunk, take) : 0;
		qemu_mutex_unlock(&audio->lock);

		if (got == 0) {
			/*
			 * Producer starved: emit silence so the voice keeps running and
			 * we never have to toggle AUD_set_active_out from the wrong
			 * thread. Bounded by one chunk per idle callback.
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
	uint8_t bytes[2] = { (uint8_t) sample_word, (uint8_t) (sample_word >> 8) };

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
	if (fifo8_num_used(&audio->fifo) + sizeof(bytes) <= AFE_OUT_FIFO_BYTES)
		fifo8_push_all(&audio->fifo, bytes, sizeof(bytes));
	qemu_mutex_unlock(&audio->lock);
}

static void afe_audio_init(afe_state_t *state) {
	afe_audio_t *audio = &state->audio;
	struct audsettings as = {
		.freq = AFE_OUT_FREQ,
		.nchannels = AFE_OUT_CHANNELS,
		.fmt = AUDIO_FORMAT_S16,
		.big_endian = false,
	};
	audio->out_freq = AFE_OUT_FREQ;
	audio->out_channels = AFE_OUT_CHANNELS;
	qemu_mutex_init(&audio->lock);

	/* Lazily bind the default -audiodev; degrade to silence if none. */
	if (!audio_be_check(&audio->backend, NULL)) {
		DPRINTF("no audio backend configured; RX output disabled\n");
		return;
	}

	audio->voice = audio_be_open_out(audio->backend, NULL, "pmb887x-afe",
		state, afe_audio_out_callback, &as);
	if (!audio->voice) {
		EPRINTF("could not open audio out voice\n");
		return;
	}

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
		fifo8_destroy(&audio->fifo);
		audio->fifo_ready = false;
	}
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
	dsp_device_t *interrupt = state->interrupt;
	dsp_host_t host = state->host;
	uint16_t ram_base = state->ram_base;

#ifndef PMB887X_DSP_TESTS
	afe_audio_t audio = state->audio;

	afe_audio_reset(state);
	memset(state, 0, sizeof(*state));
	state->audio = audio;
#else
	memset(state, 0, sizeof(*state));
#endif
	state->interrupt = interrupt;
	state->host = host;
	state->ram_base = ram_base;
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
				state->receive_cycles = 0;
			}
			if (!afe_transmit_active(state)) {
				state->transmit_position = 0;
				state->transmit_cycles = 0;
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

void afe_advance(dsp_device_t *device, size_t cycles) {
	afe_state_t *state = device->state;

	if (afe_receive_active(state)) {
		uint16_t interrupt_position = state->registers[TEAK_AFE_INTPTR] & TEAK_AFE_INTPTR_RXINTPTR;

		state->receive_cycles += cycles;
		while (state->receive_cycles >= AFE_SAMPLE_CYCLES) {
			state->receive_cycles -= AFE_SAMPLE_CYCLES;
			state->receive_position++;
			state->receive_position &= TEAK_AFE_RWADDR_RDADDR;

			if (state->receive_position == interrupt_position) {
				dsp_int_set_flags(state->interrupt, AFE_INTERRUPT_GROUP, TEAK_INT_FINTB0_VBRX);
				state->receive_cycles = 0;
				break;
			}
		}
	}

	if (afe_transmit_active(state)) {
		uint16_t interrupt_position = state->registers[TEAK_AFE_INTPTR] >> TEAK_AFE_INTPTR_TXINTPTR_SHIFT;

		state->transmit_cycles += cycles;
		while (state->transmit_cycles >= AFE_SAMPLE_CYCLES) {
			bool power_down = (state->registers[TEAK_AFE_VTXCTRL] & TEAK_AFE_VTXCTRL_TXMODE) ==
				TEAK_AFE_VTXCTRL_TXMODE_POWER_DOWN;

			state->transmit_cycles -= AFE_SAMPLE_CYCLES;

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

			if (state->transmit_position == interrupt_position) {
				dsp_int_set_flags(state->interrupt, AFE_INTERRUPT_GROUP, TEAK_INT_FINTB0_VBTX);
				state->transmit_cycles = 0;
				break;
			}
		}
	}
}

bool afe_is_active(const dsp_device_t *device) {
	const afe_state_t *state = device->state;
	return afe_receive_active(state) || afe_transmit_active(state);
}

#ifndef PMB887X_DSP_TESTS
size_t afe_audio_push_samples(dsp_device_t *device, const uint16_t *samples, size_t count) {
	afe_state_t *state = device->state;

	for (size_t i = 0; i < count; i++)
		afe_audio_produce(state, samples[i]);
	return count;
}

void afe_audio_set_format(dsp_device_t *device, unsigned freq, unsigned channels) {
	afe_state_t *state = device->state;
	afe_audio_t *audio = &state->audio;
	struct audsettings as = {
		.fmt = AUDIO_FORMAT_S16,
		.big_endian = false,
	};

	if (freq == 0)
		freq = audio->out_freq;
	if (channels == 0)
		channels = audio->out_channels;
	if (channels > AFE_OUT_MAX_CHANNELS)
		channels = AFE_OUT_MAX_CHANNELS;
	if ((int) freq == audio->out_freq && (int) channels == audio->out_channels)
		return;

	DPRINTF("audio out format %d Hz/%dch -> %u Hz/%uch\n",
		audio->out_freq, audio->out_channels, freq, channels);
	audio->out_freq = freq;
	audio->out_channels = channels;
	as.freq = freq;
	as.nchannels = channels;

	/* Reopen the live voice with the new format. Runs under the BQL alongside
	 * the drain callback, so there is no concurrent access to the voice. */
	if (audio->voice) {
		audio_be_set_active_out(audio->backend, audio->voice, false);
		audio_be_close_out(audio->backend, audio->voice);
		audio->voice = audio_be_open_out(audio->backend, NULL, "pmb887x-afe",
			state, afe_audio_out_callback, &as);
		if (audio->voice)
			audio_be_set_active_out(audio->backend, audio->voice, true);
	}
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
