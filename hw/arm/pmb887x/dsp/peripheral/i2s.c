#define PMB887X_TRACE_ID		DSP_I2S
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-i2s"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"

#include "qemu/host-utils.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define I2S_REGISTER_COUNT	(TEAK_I2S_TXINTADDR + 1)
#define I2S_CONTROL_MASK	(TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART | TEAK_I2S_CTRL_I2SRXSTART | \
	TEAK_I2S_CTRL_TXPCM | TEAK_I2S_CTRL_RXPCM | TEAK_I2S_CTRL_DAI_EN)
#define I2S_INTERRUPT_GROUP	1
/*
 * The fixed reference the fractional bit-clock divider can select. The field is
 * named for the 104 MHz system clock, which reaches the serial unit through a
 * fixed /4 prescaler - the 26 MHz crystal. Against the firmware: DEN0=1625 puts
 * the divider step at 26 MHz / 1625 = 16 kHz, and with a 32-clock frame the
 * melody path's NUM0=32 gives 16 kHz while a voice call's NUM0=16 gives 8 kHz.
 */
#define I2S_FIXED_CLOCK_HZ	(104000000U / 4)
/*
 * The module clock is assumed to take the same /4 prescaler. Either way the
 * word clock is counted in DSP cycles, so it stops while the DSP clock is
 * gated: this assumes the firmware never gates the DSP with a stream running.
 */
#define I2S_MODULE_CLOCK_PRESCALER	4U
/* A frame is one word per channel, and the link is always stereo. */
#define I2S_FRAME_WORDS		2
#define I2S_OUT_CHANNELS	2

typedef struct i2s_state_t i2s_state_t;

struct i2s_state_t {
	uint16_t registers[I2S_REGISTER_COUNT];
	dsp_device_t *interrupt;
	dsp_device_t *audio_sink;
	dsp_host_t host;
	uint16_t ram_base;
	uint16_t transmit_interrupt_flag;
	uint16_t transmit_position;
	uint16_t receive_position;
	uint32_t frequency;
	/*
	 * Word clock: runs while the unit is on, advancing by word_rate per DSP
	 * cycle, one word per word_period. A stopped clock keeps its period, for
	 * it to go on from the same place.
	 */
	uint64_t phase;
	uint64_t word_rate;
	uint64_t word_period;
	/* The word slot of the frame the clock is in: 0 is the left channel. */
	uint8_t frame_word;
	bool transmitting;
	/* A started transmitter waits for a frame to begin before it shifts out a word. */
	bool transmit_synced;
	uint16_t frame[I2S_FRAME_WORDS];
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

static uint32_t i2s_frame_clocks(const i2s_state_t *state) {
	static const uint16_t period_clocks[] = { 64, 48, 32, 64 };
	uint16_t period = state->registers[TEAK_I2S_TXCONF] & TEAK_I2S_TXCONF_PERIOD;
	return period_clocks[period >> TEAK_I2S_TXCONF_PERIOD_SHIFT];
}

/*
 * The fractional divider NUM0/DEN0 makes the bit clock out of its reference
 * and TXCONF.PERIOD says how many bit clocks one frame of two words takes. In
 * DSP cycles a word then lasts word_period / word_rate.
 */
static void i2s_update_clock(i2s_state_t *state) {
	uint16_t numerator_register = state->registers[TEAK_I2S_NUM0];
	uint64_t numerator = numerator_register & TEAK_I2S_NUM0_NUMERATOR;
	uint64_t denominator = state->registers[TEAK_I2S_DEN0];
	uint64_t clocks = i2s_frame_clocks(state);
	uint64_t word_rate, word_period;

	if ((numerator_register & TEAK_I2S_NUM0_FREF) == TEAK_I2S_NUM0_FREF_CLOCK_104MHZ) {
		word_rate = I2S_FIXED_CLOCK_HZ * numerator * I2S_FRAME_WORDS;
		word_period = (uint64_t) state->frequency * denominator * clocks;
	} else {
		word_rate = numerator * I2S_FRAME_WORDS;
		word_period = I2S_MODULE_CLOCK_PRESCALER * denominator * clocks;
	}
	if (word_rate == 0 || word_period == 0) {
		state->word_rate = 0;
		return;
	}

	if (word_period != state->word_period && state->word_period != 0) {
		uint64_t low, high;

		/* The word under way is as far along at the new clock. */
		mulu64(&low, &high, state->phase, word_period);
		divu128(&low, &high, state->word_period);
		state->phase = low;
	}
	state->word_rate = word_rate;
	state->word_period = word_period;
}

static uint32_t i2s_transmit_frame_rate(const i2s_state_t *state) {
	uint16_t numerator_register = state->registers[TEAK_I2S_NUM0];
	uint64_t numerator = numerator_register & TEAK_I2S_NUM0_NUMERATOR;
	uint64_t divisor = (uint64_t) state->registers[TEAK_I2S_DEN0] * i2s_frame_clocks(state);

	if (state->word_rate == 0)
		return 0;
	if ((numerator_register & TEAK_I2S_NUM0_FREF) == TEAK_I2S_NUM0_FREF_CLOCK_104MHZ)
		return I2S_FIXED_CLOCK_HZ * numerator / divisor;
	return (uint64_t) state->frequency * numerator / (divisor * I2S_MODULE_CLOCK_PRESCALER);
}

/* The codec plays what the transmitter sends at the rate it sends it. */
static void i2s_update_audio_format(i2s_state_t *state) {
#ifndef PMB887X_DSP_TESTS
	uint32_t rate = i2s_transmit_frame_rate(state);

	if (rate != 0 && state->audio_sink != NULL && i2s_transmit_active(state))
		afe_audio_set_format(state->audio_sink, rate, I2S_OUT_CHANNELS);
#endif
}

static void i2s_update_transmit(i2s_state_t *state) {
	bool transmitting = i2s_transmit_active(state);

	if (transmitting && !state->transmitting) {
		state->transmit_synced = false;
		DPRINTF("transmit start: position=%u rate=%u Hz txconf=%04X\n", state->transmit_position,
			i2s_transmit_frame_rate(state), state->registers[TEAK_I2S_TXCONF]);
	}
	state->transmitting = transmitting;
	i2s_update_audio_format(state);
}

static void i2s_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void i2s_reset(dsp_device_t *device) {
	i2s_state_t *state = device->state;
	dsp_device_t *interrupt = state->interrupt;
	dsp_device_t *audio_sink = state->audio_sink;
	dsp_host_t host = state->host;
	uint16_t ram_base = state->ram_base;
	uint16_t transmit_interrupt_flag = state->transmit_interrupt_flag;
	uint32_t frequency = state->frequency;

	memset(state, 0, sizeof(*state));
	state->interrupt = interrupt;
	state->audio_sink = audio_sink;
	state->host = host;
	state->ram_base = ram_base;
	state->transmit_interrupt_flag = transmit_interrupt_flag;
	state->frequency = frequency;
	state->registers[TEAK_I2S_NUM0] = 1;
	state->registers[TEAK_I2S_DEN0] = 2;
	state->registers[TEAK_I2S_NUM1] = 1;
	state->registers[TEAK_I2S_DEN1] = 2;
	i2s_update_clock(state);
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
				state->phase = 0;
				state->frame_word = 0;
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

	i2s_update_clock(state);
	i2s_update_transmit(state);

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
	uint16_t interrupt_flag, dsp_device_t *audio_sink, const dsp_host_t *host) {
	i2s_state_t *state = g_new0(i2s_state_t, 1);
	state->interrupt = interrupt;
	state->transmit_interrupt_flag = interrupt_flag;
	state->audio_sink = audio_sink;
	state->host = *host;
	state->ram_base = config->ram_base;
	return dsp_device_create(config, &i2s_ops, state);
}

/*
 * The frame just shifted out reaches the codec. In mono the unit sends the
 * selected word of the frame on both channels; the firmware fills only that
 * word of each frame.
 */
static void i2s_output_frame(i2s_state_t *state) {
#ifndef PMB887X_DSP_TESTS
	uint16_t config = state->registers[TEAK_I2S_TXCONF];
	uint16_t samples[I2S_OUT_CHANNELS] = { state->frame[0], state->frame[1] };

	switch (config & TEAK_I2S_TXCONF_MONO) {
		case TEAK_I2S_TXCONF_MONO_LEFT:
			samples[1] = samples[0];
			break;

		case TEAK_I2S_TXCONF_MONO_RIGHT:
			samples[0] = samples[1];
			break;
	}
	if ((config & TEAK_I2S_TXCONF_MUTE_L) != 0)
		samples[0] = 0;
	if ((config & TEAK_I2S_TXCONF_MUTE_R) != 0)
		samples[1] = 0;

	if (state->audio_sink != NULL)
		afe_audio_push_samples(state->audio_sink, samples, I2S_OUT_CHANNELS);
#endif
}

static void i2s_transmit_word(i2s_state_t *state, uint8_t slot) {
	state->frame[slot] = state->host.data_read(state->host.opaque, state->ram_base + state->transmit_position);
	if (slot == I2S_FRAME_WORDS - 1)
		i2s_output_frame(state);

	state->transmit_position++;
	state->transmit_position &= TEAK_I2S_RWADDR_RDADDR;

	if (state->transmit_position == state->registers[TEAK_I2S_TXINTADDR]) {
		if ((state->registers[TEAK_I2S_CTRL] & TEAK_I2S_CTRL_TXPCM) != 0) {
			state->registers[TEAK_I2S_CTRL] &= (uint16_t) ~TEAK_I2S_CTRL_I2STXSTART;
			state->transmitting = false;
		}
		dsp_int_set_flags(state->interrupt, I2S_INTERRUPT_GROUP, state->transmit_interrupt_flag);
	}
}

static void i2s_receive_word(i2s_state_t *state) {
	state->receive_position++;
	state->receive_position &= TEAK_I2S_RWADDR_RDADDR;

	if (state->receive_position == state->registers[TEAK_I2S_RXINTADDR]) {
		if ((state->registers[TEAK_I2S_CTRL] & TEAK_I2S_CTRL_RXPCM) != 0)
			state->registers[TEAK_I2S_CTRL] &= (uint16_t) ~TEAK_I2S_CTRL_I2SRXSTART;
		dsp_int_set_flags(state->interrupt, I2S_INTERRUPT_GROUP, state->transmit_interrupt_flag << 1);
	}
}

void i2s_set_frequency(dsp_device_t *device, uint32_t frequency) {
	i2s_state_t *state = device->state;

	state->frequency = frequency;
	i2s_update_clock(state);
	i2s_update_audio_format(state);
}

static void i2s_clock_word(i2s_state_t *state) {
	uint8_t slot = state->frame_word;

	state->frame_word = (slot + 1) % I2S_FRAME_WORDS;
	if (i2s_transmit_active(state)) {
		if (slot == 0)
			state->transmit_synced = true;
		if (state->transmit_synced)
			i2s_transmit_word(state, slot);
	}
	if (i2s_receive_active(state))
		i2s_receive_word(state);
}

void i2s_advance(dsp_device_t *device, size_t cycles) {
	i2s_state_t *state = device->state;

	if (!i2s_is_active(device))
		return;

	state->phase += (uint64_t) cycles * state->word_rate;
	while (state->phase >= state->word_period) {
		if (!i2s_transmit_active(state) && !i2s_receive_active(state)) {
			uint64_t words = state->phase / state->word_period;

			state->phase -= words * state->word_period;
			state->frame_word = (state->frame_word + words) % I2S_FRAME_WORDS;
			break;
		}
		state->phase -= state->word_period;
		i2s_clock_word(state);
	}
}

size_t i2s_next_event(dsp_device_t *device) {
	i2s_state_t *state = device->state;

	if (!i2s_transmit_active(state) && !i2s_receive_active(state))
		return SIZE_MAX;
	return dsp_rate_cycles_until(state->phase, state->word_rate, state->word_period);
}

bool i2s_is_active(const dsp_device_t *device) {
	const i2s_state_t *state = device->state;
	return (state->registers[TEAK_I2S_CTRL] & TEAK_I2S_CTRL_I2SON) != 0 && state->word_rate != 0;
}
