#define PMB887X_TRACE_ID		DSP_I2S
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-i2s"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"

#ifndef PMB887X_DSP_TESTS
#include "qemu/atomic.h"
#include "qemu/timer.h"
#endif

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define I2S_REGISTER_COUNT	(TEAK_I2S_TXINTADDR + 1)
#define I2S_CONTROL_MASK	(TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART | TEAK_I2S_CTRL_I2SRXSTART | \
	TEAK_I2S_CTRL_TXPCM | TEAK_I2S_CTRL_RXPCM | TEAK_I2S_CTRL_DAI_EN)
#define I2S_SAMPLE_CYCLES	16U
#define I2S_INTERRUPT_GROUP	1
/*
 * The fixed reference the fractional bit-clock divider can select. The field is
 * named for the 104 MHz system clock, which reaches the serial unit through a
 * fixed /4 prescaler - the 26 MHz crystal. Against the firmware: DEN0=1625 puts
 * the divider step at 26 MHz / 1625 = 16 kHz, and with a 32-clock frame the
 * melody path's NUM0=32 gives 16 kHz while a voice call's NUM0=16 gives 8 kHz.
 */
#define I2S_FIXED_CLOCK_HZ	(104000000U / 4)
/* Cap how far the paced serial clock can catch up in one go after a stall. */
#define I2S_MAX_CATCHUP_WORDS	512
/* How far ahead of the host backend the firmware is allowed to synthesise. */
#define I2S_QUEUE_AHEAD_MS	125
/* A frame is one ring word per channel, and the link is always stereo. */
#define I2S_OUT_CHANNELS	2
/* The transmit ring, as the 6-bit RD/WR pointers address it. */
#define I2S_RING_WORDS		(TEAK_I2S_RWADDR_RDADDR + 1)

typedef struct i2s_state_t i2s_state_t;

struct i2s_state_t {
	uint16_t registers[I2S_REGISTER_COUNT];
	dsp_device_t *interrupt;
	dsp_device_t *audio_sink;
	uint16_t ram_base;
	uint16_t transmit_interrupt_flag;
	uint16_t transmit_position;
	uint16_t receive_position;
	size_t sample_cycles;
	int64_t next_word_ns;
	uint32_t audio_rate;
	uint16_t frame[I2S_OUT_CHANNELS];
	uint32_t traced_rate;
	/* Ring slots the core has refilled since they were last shifted out. */
	uint64_t refilled;
	int64_t starved_since_ns;
};

static bool i2s_transmit_active(const i2s_state_t *state) {
	uint16_t control = state->registers[TEAK_I2S_CTRL];
	uint16_t active = TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART;
	return (control & active) == active;
}

static bool i2s_receive_active(const i2s_state_t *state) {
	uint16_t control = state->registers[TEAK_I2S_CTRL];
	uint16_t active = TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2SRXSTART;
	return (control & active) == active;
}

/*
 * Frames per second the transmit serial clock consumes: the fractional divider
 * NUM0/DEN0 makes the bit clock out of the fixed reference and TXCONF.PERIOD
 * says how many bit clocks one frame takes. Zero when the divider is not
 * programmed against that reference, which leaves the unit clocked by executed
 * DSP cycles as before.
 */
#ifndef PMB887X_DSP_TESTS
static uint32_t i2s_transmit_frame_rate(const i2s_state_t *state) {
	static const uint16_t period_clocks[] = { 64, 48, 32, 64 };
	uint16_t numerator_register = state->registers[TEAK_I2S_NUM0];
	uint32_t numerator = numerator_register & TEAK_I2S_NUM0_NUMERATOR;
	uint32_t denominator = state->registers[TEAK_I2S_DEN0];
	uint16_t period = state->registers[TEAK_I2S_TXCONF] & TEAK_I2S_TXCONF_PERIOD;
	uint32_t clocks = period_clocks[period >> TEAK_I2S_TXCONF_PERIOD_SHIFT];

	if ((numerator_register & TEAK_I2S_NUM0_FREF) != TEAK_I2S_NUM0_FREF_CLOCK_104MHZ)
		return 0;
	if (numerator == 0 || denominator == 0)
		return 0;
	return (uint32_t) ((uint64_t) I2S_FIXED_CLOCK_HZ * numerator / denominator / clocks);
}
#endif

#ifdef PMB887X_DSP_TESTS
/* The tests drive the unit from executed cycles, with no host audio backend. */
void i2s_pace(dsp_device_t *device, int64_t now) {}
bool i2s_is_paced(const dsp_device_t *device) { return false; }
void i2s_apply_audio_format(dsp_device_t *device) {}
void i2s_note_ram_write(dsp_device_t *device, uint16_t address, uint16_t value) {}
#else
/*
 * Take the host's audio from the DSP filling the ring rather than from the
 * serial clock emptying it.
 *
 * The transmit ring holds 2 ms of audio and the firmware rewrites all of it
 * from one transmit interrupt, so on hardware the fill always stays ahead of
 * the shift-out. The emulated core produces the same words in bursts that are
 * neither that quick nor that even, and a serial clock paced against wall time
 * then laps it: it shifts words out twice and drops the ones written in
 * between, which leaves the melody's envelope and tempo intact but scrambles
 * the waveform into noise. Following the writes instead keeps every sample the
 * core computed, in order. i2s_pace holds the clock to the same words, so the
 * transmit interrupts stay in step with the stream the host is hearing.
 */
void i2s_note_ram_write(dsp_device_t *device, uint16_t address, uint16_t value) {
	i2s_state_t *state = device->state;
	uint16_t offset = address - state->ram_base;
	size_t channel = offset % I2S_OUT_CHANNELS;
	uint16_t mono;

	/*
	 * I2SON, not i2s_transmit_active(): in PCM mode the transmit interrupt
	 * clears I2STXSTART for the firmware to re-arm, so the unit reads as
	 * stopped for exactly the stretch in which the ring is refilled.
	 */
	if (offset >= I2S_RING_WORDS || state->audio_sink == NULL ||
		(state->registers[TEAK_I2S_CTRL] & TEAK_I2S_CTRL_I2SON) == 0)
		return;

	/*
	 * In mono the unit repeats one word of the pair on both channels, and the
	 * firmware fills only those slots - at whichever parity the ring stood at
	 * when the stream started - so every write is one frame. In stereo a frame
	 * is the pair, completed by the write to its second word.
	 */
	state->frame[channel] = value;
	mono = state->registers[TEAK_I2S_TXCONF] & TEAK_I2S_TXCONF_MONO;
	/*
	 * Mark the ring slot refilled so the serial clock may shift it out. In mono
	 * the unit repeats the word on both channels and the firmware only ever
	 * writes one slot of the pair, so both count as refilled.
	 */
	state->refilled |= 1ULL << offset;
	state->starved_since_ns = 0;
	if (mono != TEAK_I2S_TXCONF_MONO_STEREO) {
		state->refilled |= 1ULL << (offset ^ 1);
		state->frame[0] = state->frame[1] = value;
	} else if (channel != I2S_OUT_CHANNELS - 1) {
		return;
	}

	afe_audio_push_samples(state->audio_sink, state->frame, I2S_OUT_CHANNELS);
}
#endif

static void i2s_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void i2s_reset(dsp_device_t *device) {
	i2s_state_t *state = device->state;
	dsp_device_t *interrupt = state->interrupt;
	dsp_device_t *audio_sink = state->audio_sink;
	uint16_t ram_base = state->ram_base;
	uint16_t transmit_interrupt_flag = state->transmit_interrupt_flag;

	memset(state, 0, sizeof(*state));
	state->interrupt = interrupt;
	state->audio_sink = audio_sink;
	state->ram_base = ram_base;
	state->transmit_interrupt_flag = transmit_interrupt_flag;
	state->registers[TEAK_I2S_NUM0] = 1;
	state->registers[TEAK_I2S_DEN0] = 2;
	state->registers[TEAK_I2S_NUM1] = 1;
	state->registers[TEAK_I2S_DEN1] = 2;
}

static bool i2s_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	i2s_state_t *state = device->state;

	switch (offset) {
		case TEAK_I2S_RWADDR:
			*value = state->receive_position << TEAK_I2S_RWADDR_WRADDR_SHIFT | state->transmit_position;
			break;

		default:
			*value = offset < I2S_REGISTER_COUNT ? state->registers[offset] : 0;
			break;
	}

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool i2s_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	i2s_state_t *state = device->state;

	switch (offset) {
		case TEAK_I2S_CTRL:
			state->registers[offset] = value & I2S_CONTROL_MASK;
			if ((value & TEAK_I2S_CTRL_I2SON) == 0) {
				state->transmit_position = 0;
				state->receive_position = 0;
				state->sample_cycles = 0;
				state->refilled = 0;
				state->starved_since_ns = 0;
			}
			break;

		case TEAK_I2S_RXINTADDR:
		case TEAK_I2S_TXINTADDR:
			state->registers[offset] = value & TEAK_I2S_RXINTADDR_RXINTPTR;
			break;

		case TEAK_I2S_RWADDR:
			break;

		default:
			if (offset < I2S_REGISTER_COUNT)
				state->registers[offset] = value;
			break;
	}

	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t i2s_ops = {
	.destroy = i2s_destroy,
	.reset = i2s_reset,
	.read = i2s_read,
	.write = i2s_write,
};

dsp_device_t *i2s_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt,
	uint16_t interrupt_flag, dsp_device_t *audio_sink) {
	i2s_state_t *state = g_new0(i2s_state_t, 1);
	state->interrupt = interrupt;
	state->transmit_interrupt_flag = interrupt_flag;
	state->audio_sink = audio_sink;
	state->ram_base = config->ram_base;
	return dsp_device_create(config, &i2s_ops, state);
}

#ifndef PMB887X_DSP_TESTS
/*
 * How long the serial clock may wait for the core to refill a slot before it
 * gives up and free-runs again. Waiting is what keeps the stream intact, but the
 * firmware refills the ring from the transmit interrupt, so a firmware that
 * refills less of the ring than the clock is waiting on would otherwise never be
 * asked for more. The wait resets the moment anything is written to the ring.
 */
#define I2S_STARVE_LIMIT_NS	(50 * SCALE_MS)

static bool i2s_starved_too_long(i2s_state_t *state, int64_t now) {
	if (state->starved_since_ns == 0) {
		state->starved_since_ns = now;
		return false;
	}
	return now - state->starved_since_ns > I2S_STARVE_LIMIT_NS;
}

/*
 * Advance the transmit ring by however many words are due in wall-clock time.
 * Like the AFE this is a real-time audio clock: driving it from executed DSP
 * cycles instead runs it at whatever speed the core happens to reach, which
 * both plays the stream at the wrong rate and buries the core in transmit
 * interrupts. Worse, a core parked idle executes no cycles at all, so the
 * interrupt that would wake it to service the MCU never arrives.
 * Runs on the DSP worker thread, which owns the peripheral state.
 */
void i2s_pace(dsp_device_t *device, int64_t now) {
	i2s_state_t *state = device->state;
	uint32_t rate = i2s_transmit_frame_rate(state);
	int64_t period;
	size_t words = 0;

	if (rate == 0 || !i2s_transmit_active(state))
		return;

	if (rate != state->traced_rate) {
		state->traced_rate = rate;
		DPRINTF("frame rate %u (NUM0=%04X DEN0=%04X TXCONF=%04X CTRL=%04X TXINT=%04X)\n", rate,
			state->registers[TEAK_I2S_NUM0], state->registers[TEAK_I2S_DEN0],
			state->registers[TEAK_I2S_TXCONF], state->registers[TEAK_I2S_CTRL],
			state->registers[TEAK_I2S_TXINTADDR]);
	}
	qatomic_set(&state->audio_rate, rate);

	/*
	 * Hold the serial clock while the backend still has a comfortable amount
	 * of audio to play out. The firmware refills the ring from the transmit
	 * interrupt, so withholding that interrupt is what makes it wait; left
	 * free-running against wall-clock time it outpaces a backend clocked by
	 * the (slower) virtual clock, and the surplus is simply dropped.
	 */
	if (state->audio_sink != NULL &&
		afe_audio_queued_samples(state->audio_sink) >= rate * I2S_QUEUE_AHEAD_MS / 1000) {
		state->next_word_ns = now;
		return;
	}

	period = NANOSECONDS_PER_SECOND / (rate * I2S_OUT_CHANNELS);
	if (state->next_word_ns == 0 || state->next_word_ns > now + period)
		state->next_word_ns = now;	/* first word or clock skew: (re)sync */

	/*
	 * Only shift out ring slots the core has refilled since they last went out.
	 * The emulated DSP cannot always synthesise the higher melody rates in real
	 * time, and a serial clock that runs on regardless simply skips whatever has
	 * not been written yet: those samples never reach the host at all, while the
	 * firmware keeps counting the transmit interrupts it schedules notes
	 * against, so the rest of the melody plays back short and too fast. Waiting
	 * for the core instead costs wall-clock time and keeps the stream intact.
	 */
	while (state->next_word_ns <= now && words < I2S_MAX_CATCHUP_WORDS) {
		uint16_t slot = (state->transmit_position + 1) & TEAK_I2S_RWADDR_RDADDR;
		bool starving = (state->refilled & (1ULL << slot)) == 0;

		if (starving && !i2s_starved_too_long(state, now))
			break;
		state->refilled &= ~(1ULL << slot);
		words++;
		state->next_word_ns += period;
		i2s_advance(device, I2S_SAMPLE_CYCLES);
	}

	/*
	 * Waiting for the core is not a backlog to catch up on later: resync, or the
	 * next call shifts the whole wait out in one burst of transmit interrupts.
	 */
	if (state->next_word_ns <= now)
		state->next_word_ns = now;
}

bool i2s_is_paced(const dsp_device_t *device) {
	return i2s_transmit_frame_rate(device->state) != 0;
}

/* Main loop only: reopening the shared voice must not race its drain callback. */
void i2s_apply_audio_format(dsp_device_t *device) {
	i2s_state_t *state = device->state;
	uint32_t rate = qatomic_read(&state->audio_rate);

	if (rate != 0 && state->audio_sink != NULL)
		afe_audio_set_format(state->audio_sink, rate, I2S_OUT_CHANNELS);
}
#endif

void i2s_advance(dsp_device_t *device, size_t cycles) {
	i2s_state_t *state = device->state;

	if (!i2s_transmit_active(state) && !i2s_receive_active(state))
		return;

	state->sample_cycles += cycles;

	while (state->sample_cycles >= I2S_SAMPLE_CYCLES) {
		bool event = false;

		state->sample_cycles -= I2S_SAMPLE_CYCLES;
		if (i2s_transmit_active(state)) {
			state->transmit_position++;
			state->transmit_position &= TEAK_I2S_RWADDR_RDADDR;
		}
		if (i2s_receive_active(state)) {
			state->receive_position++;
			state->receive_position &= TEAK_I2S_RWADDR_RDADDR;
		}

		if (i2s_transmit_active(state) && state->transmit_position == state->registers[TEAK_I2S_TXINTADDR]) {
			if ((state->registers[TEAK_I2S_CTRL] & TEAK_I2S_CTRL_TXPCM) != 0)
				state->registers[TEAK_I2S_CTRL] &= (uint16_t) ~TEAK_I2S_CTRL_I2STXSTART;
			dsp_int_set_flags(state->interrupt, I2S_INTERRUPT_GROUP, state->transmit_interrupt_flag);
			event = true;
		}

		if (i2s_receive_active(state) && state->receive_position == state->registers[TEAK_I2S_RXINTADDR]) {
			if ((state->registers[TEAK_I2S_CTRL] & TEAK_I2S_CTRL_RXPCM) != 0)
				state->registers[TEAK_I2S_CTRL] &= (uint16_t) ~TEAK_I2S_CTRL_I2SRXSTART;
			dsp_int_set_flags(state->interrupt, I2S_INTERRUPT_GROUP, state->transmit_interrupt_flag << 1);
			event = true;
		}

		if (event) {
			state->sample_cycles = 0;
			break;
		}
	}
}

bool i2s_is_active(const dsp_device_t *device) {
	const i2s_state_t *state = device->state;
	return i2s_transmit_active(state) || i2s_receive_active(state);
}
