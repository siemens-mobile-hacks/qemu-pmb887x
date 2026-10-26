#define PMB887X_TRACE_ID		DSP_I2S
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-i2s"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define I2S_REGISTER_COUNT	(TEAK_I2S_TXINTADDR + 1)
#define I2S_FRACTIONAL_DIVIDER_FACTOR	4U
#define I2S_FRAME_SLOTS			2U
#define I2S_RING_WORDS			(TEAK_I2S_RWADDR_RDADDR + 1U)
#define I2S_OUTPUT_BATCH_FRAMES		256U
#define I2S_INTERRUPT_GROUP	1
#define I2S_CLOCK_DIVIDER_0		0
#define I2S_CLOCK_DIVIDER_1		3
#define I2S_TRANSMIT_FIFO_WORDS		4U

static const uint16_t I2S_CONTROL_MASK = TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART |
	TEAK_I2S_CTRL_I2SRXSTART | TEAK_I2S_CTRL_TXPCM | TEAK_I2S_CTRL_RXPCM | TEAK_I2S_CTRL_DAI_EN;

typedef struct i2s_state_t i2s_state_t;

struct i2s_state_t {
	uint16_t registers[I2S_REGISTER_COUNT];
	dsp_device_t *interrupt;
	dsp_host_t host;
	dsp_i2s_port_t port;
	uint16_t transmit_interrupt_flag;
	uint16_t transmit_position;
	uint16_t transmit_fifo_words;
	int16_t transmit_fifo[I2S_TRANSMIT_FIFO_WORDS];
	uint16_t receive_position;
	uint32_t clock_hz;
	uint32_t ms_clock_hz;
	uint32_t transmit_sample_rate_hz;
	uint32_t transmit_channels;
	uint32_t receive_sample_rate_hz;
	uint32_t receive_channels;
	uint64_t transmit_phase;
	uint64_t receive_phase;
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

static uint32_t i2s_reference_hz(const i2s_state_t *state, uint16_t numerator_register) {
	switch (numerator_register & TEAK_I2S_NUM0_FREF) {
		case TEAK_I2S_NUM0_FREF_MODULE_CLOCK:
			return state->clock_hz;

		case TEAK_I2S_NUM0_FREF_CLOCK_104MHZ:
			return state->ms_clock_hz;
	}
	return 0;
}

static uint32_t i2s_period_clocks(uint16_t configuration) {
	switch (configuration & TEAK_I2S_TXCONF_PERIOD) {
		case TEAK_I2S_TXCONF_PERIOD_CLOCKS_64:
			return 64;

		case TEAK_I2S_TXCONF_PERIOD_CLOCKS_48:
			return 48;

		case TEAK_I2S_TXCONF_PERIOD_CLOCKS_32:
			return 32;
	}
	return 0;
}

static uint16_t i2s_positions_per_frame(bool pcm) {
	return pcm ? 1 : I2S_FRAME_SLOTS;
}

static bool i2s_transmit_pcm(const i2s_state_t *state) {
	return (state->registers[TEAK_I2S_CTRL] & TEAK_I2S_CTRL_TXPCM) != 0;
}

static uint16_t i2s_transmit_frame_words(const i2s_state_t *state) {
	bool wide_slots = (state->registers[TEAK_I2S_TXCONF] & TEAK_I2S_TXCONF_WIDTH) != TEAK_I2S_TXCONF_WIDTH_BITS_16;
	uint16_t slot_words = wide_slots ? 2 : 1;
	return slot_words * i2s_positions_per_frame(i2s_transmit_pcm(state));
}

static uint16_t i2s_transmit_fifo_capacity(const i2s_state_t *state) {
	uint16_t start_words = i2s_transmit_pcm(state) ? 1 : I2S_TRANSMIT_FIFO_WORDS;
	return MAX(start_words, i2s_transmit_frame_words(state));
}

static void i2s_transmit_fetch(dsp_device_t *device, uint16_t words) {
	i2s_state_t *state = device->state;

	for (uint16_t i = 0; i < words && i2s_transmit_active(state); i++) {
		uint16_t address = device->config->ram_base + state->transmit_position;
		state->transmit_fifo[state->transmit_fifo_words++] = state->host.data_read(state->host.opaque, address);
		state->transmit_position = (state->transmit_position + 1) & TEAK_I2S_RWADDR_RDADDR;
		if (state->transmit_position == state->registers[TEAK_I2S_TXINTADDR]) {
			if (i2s_transmit_pcm(state))
				state->registers[TEAK_I2S_CTRL] &= (uint16_t) ~TEAK_I2S_CTRL_I2STXSTART;
			dsp_int_set_flags(state->interrupt, I2S_INTERRUPT_GROUP, state->transmit_interrupt_flag);
		}
	}
}

static void i2s_transmit_fill(dsp_device_t *device) {
	i2s_state_t *state = device->state;
	uint16_t capacity = i2s_transmit_fifo_capacity(state);

	if (state->transmit_fifo_words < capacity)
		i2s_transmit_fetch(device, capacity - state->transmit_fifo_words);
}

static void i2s_configure(i2s_state_t *state, bool transmit, uint32_t sample_rate_hz, uint32_t channels) {
	if (transmit) {
		if (state->transmit_sample_rate_hz == sample_rate_hz && state->transmit_channels == channels)
			return;
		state->transmit_sample_rate_hz = sample_rate_hz;
		state->transmit_channels = channels;
	} else {
		if (state->receive_sample_rate_hz == sample_rate_hz && state->receive_channels == channels)
			return;
		state->receive_sample_rate_hz = sample_rate_hz;
		state->receive_channels = channels;
	}

	I2SFormat format = {
		.sample_rate_hz = sample_rate_hz,
		.channels = channels,
	};
	if (transmit) {
		i2s_tx_configure(state->port.tx_bus, &format);
	} else {
		i2s_rx_configure(state->port.rx_bus, &format);
	}
}

static bool i2s_divider_get(const i2s_state_t *state, bool transmit, uint16_t *numerator, uint16_t *denominator) {
	uint16_t select;
	uint16_t divider;

	if (transmit) {
		select = (state->registers[TEAK_I2S_CSEL] & TEAK_I2S_CSEL_TXCLKSEL) >>
			TEAK_I2S_CSEL_TXCLKSEL_SHIFT;
		if (select != I2S_CLOCK_DIVIDER_0 && select != I2S_CLOCK_DIVIDER_1)
			return false;
		divider = select == I2S_CLOCK_DIVIDER_0 ? 0 : 1;
	} else {
		select = (state->registers[TEAK_I2S_CSEL] & TEAK_I2S_CSEL_RXCLKSEL) >>
			TEAK_I2S_CSEL_RXCLKSEL_SHIFT;
		if (select != I2S_CLOCK_DIVIDER_0 && select != I2S_CLOCK_DIVIDER_1)
			return false;
		divider = select == I2S_CLOCK_DIVIDER_0 ? 1 : 0;
	}

	*numerator = state->registers[divider == 0 ? TEAK_I2S_NUM0 : TEAK_I2S_NUM1];
	*denominator = state->registers[divider == 0 ? TEAK_I2S_DEN0 : TEAK_I2S_DEN1];
	return true;
}

static uint32_t i2s_sample_rate_hz(const i2s_state_t *state, bool transmit, uint16_t configuration) {
	uint16_t numerator_register;
	uint16_t denominator;

	if (!i2s_divider_get(state, transmit, &numerator_register, &denominator))
		return 0;

	uint32_t reference_hz = i2s_reference_hz(state, numerator_register);
	uint16_t numerator = numerator_register & TEAK_I2S_NUM0_NUMERATOR;
	uint32_t period_clocks = i2s_period_clocks(configuration);

	if (reference_hz == 0 || numerator == 0 || denominator == 0 || period_clocks == 0)
		return 0;
	return (uint64_t) reference_hz * numerator /
		((uint64_t) denominator * period_clocks * I2S_FRACTIONAL_DIVIDER_FACTOR);
}

static size_t i2s_advance_frames(
	const i2s_state_t *state,
	bool transmit,
	uint16_t configuration,
	uint64_t *phase,
	size_t cycles
) {
	uint16_t numerator_register;
	uint16_t denominator;

	if (!i2s_divider_get(state, transmit, &numerator_register, &denominator))
		return 0;

	uint32_t reference_hz = i2s_reference_hz(state, numerator_register);
	uint16_t numerator = numerator_register & TEAK_I2S_NUM0_NUMERATOR;
	uint32_t period_clocks = i2s_period_clocks(configuration);

	if (state->clock_hz == 0 || reference_hz == 0 || numerator == 0 || denominator == 0 || period_clocks == 0)
		return 0;

	uint64_t frame_phase = (uint64_t) state->clock_hz * denominator * period_clocks *
		I2S_FRACTIONAL_DIVIDER_FACTOR;
	*phase += (uint64_t) cycles * reference_hz * numerator;
	size_t frames = *phase / frame_phase;
	*phase %= frame_phase;
	return frames;
}

static void i2s_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void i2s_reset(dsp_device_t *device) {
	i2s_state_t *state = device->state;
	dsp_device_t *interrupt = state->interrupt;
	dsp_host_t host = state->host;
	dsp_i2s_port_t port = state->port;
	uint16_t transmit_interrupt_flag = state->transmit_interrupt_flag;
	uint32_t clock_hz = state->clock_hz;
	uint32_t ms_clock_hz = state->ms_clock_hz;

	memset(state, 0, sizeof(*state));
	state->interrupt = interrupt;
	state->host = host;
	state->port = port;
	state->transmit_interrupt_flag = transmit_interrupt_flag;
	state->clock_hz = clock_hz;
	state->ms_clock_hz = ms_clock_hz;
	state->registers[TEAK_I2S_NUM0] = 1;
	state->registers[TEAK_I2S_DEN0] = 2;
	state->registers[TEAK_I2S_NUM1] = 1;
	state->registers[TEAK_I2S_DEN1] = 2;
}

static bool i2s_register_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
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

static bool i2s_register_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	i2s_state_t *state = device->state;

	switch (offset) {
		case TEAK_I2S_CTRL:
			state->registers[offset] = value & I2S_CONTROL_MASK;
			if ((value & TEAK_I2S_CTRL_I2SON) == 0) {
				state->transmit_position = 0;
				state->receive_position = 0;
				state->transmit_phase = 0;
				state->receive_phase = 0;
				state->transmit_fifo_words = 0;
				state->registers[TEAK_I2S_RXINTADDR] = 0;
				state->registers[TEAK_I2S_TXINTADDR] = 0;
			}
			i2s_transmit_fill(device);
			break;

		case TEAK_I2S_RXINTADDR:
		case TEAK_I2S_TXINTADDR:
			if ((state->registers[TEAK_I2S_CTRL] & TEAK_I2S_CTRL_I2SON) != 0)
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
	.read = i2s_register_read,
	.write = i2s_register_write,
};

dsp_device_t *dsp_i2s_create(
	const pmb887x_dsp_peripheral_config_t *config,
	dsp_device_t *interrupt,
	const dsp_host_t *host,
	const dsp_i2s_port_t *i2s_port,
	uint16_t interrupt_flag
) {
	i2s_state_t *state = g_new0(i2s_state_t, 1);
	state->interrupt = interrupt;
	state->host = *host;
	state->port = *i2s_port;
	state->transmit_interrupt_flag = interrupt_flag;
	return dsp_device_create(config, &i2s_ops, state);
}

void dsp_i2s_set_clock_hz(dsp_device_t *device, uint32_t frequency_hz) {
	i2s_state_t *state = device->state;

	if (state->clock_hz == frequency_hz)
		return;
	state->clock_hz = frequency_hz;
	state->transmit_phase = 0;
	state->receive_phase = 0;
}

void dsp_i2s_set_ms_clock_hz(dsp_device_t *device, uint32_t frequency_hz) {
	i2s_state_t *state = device->state;

	if (state->ms_clock_hz == frequency_hz)
		return;
	state->ms_clock_hz = frequency_hz;
	state->transmit_phase = 0;
	state->receive_phase = 0;
}

void dsp_i2s_advance(dsp_device_t *device, size_t cycles) {
	i2s_state_t *state = device->state;

	if (i2s_transmit_active(state)) {
		uint16_t configuration = state->registers[TEAK_I2S_TXCONF];
		size_t frames = i2s_advance_frames(state, true, configuration,
			&state->transmit_phase, cycles);
		uint32_t sample_rate_hz = i2s_sample_rate_hz(state, true, configuration);
		bool pcm = i2s_transmit_pcm(state);
		uint32_t channels = pcm ? 1 : I2S_FRAME_SLOTS;
		i2s_configure(state, true, sample_rate_hz, channels);

		while (i2s_transmit_active(state) && frames != 0) {
			int16_t samples[I2S_OUTPUT_BATCH_FRAMES * I2S_FRAME_SLOTS];
			size_t output_frames = 0;

			while (i2s_transmit_active(state) && frames != 0 && output_frames < I2S_OUTPUT_BATCH_FRAMES) {
				uint16_t frame_words = i2s_transmit_frame_words(state);
				int16_t *frame = &samples[output_frames * channels];

				i2s_transmit_fill(device);
				if (state->transmit_fifo_words < frame_words)
					break;

				frame[0] = state->transmit_fifo[0];
				if (!pcm) {
					frame[1] = state->transmit_fifo[frame_words / I2S_FRAME_SLOTS];

					switch (configuration & TEAK_I2S_TXCONF_MONO) {
						case TEAK_I2S_TXCONF_MONO_RIGHT:
							frame[0] = frame[1];
							break;

						case TEAK_I2S_TXCONF_MONO_LEFT:
							frame[1] = frame[0];
							break;
					}
				}

				state->transmit_fifo_words -= frame_words;
				memmove(state->transmit_fifo, &state->transmit_fifo[frame_words],
					state->transmit_fifo_words * sizeof(*state->transmit_fifo));
				i2s_transmit_fill(device);

				frames--;
				output_frames++;
			}
			i2s_tx_write(state->port.tx_bus, samples, output_frames);
		}
	}
	if (i2s_receive_active(state)) {
		uint16_t configuration = state->registers[TEAK_I2S_RXCONF];
		size_t frames = i2s_advance_frames(state, false, configuration,
			&state->receive_phase, cycles);
		uint32_t sample_rate_hz = i2s_sample_rate_hz(state, false, configuration);
		bool pcm = (state->registers[TEAK_I2S_CTRL] & TEAK_I2S_CTRL_RXPCM) != 0;
		uint32_t channels = pcm ? 1 : I2S_FRAME_SLOTS;
		i2s_configure(state, false, sample_rate_hz, channels);

		while (i2s_receive_active(state) && frames != 0) {
			int16_t samples[I2S_OUTPUT_BATCH_FRAMES * I2S_FRAME_SLOTS] = {};
			size_t input_frames = MIN(frames, (size_t) I2S_OUTPUT_BATCH_FRAMES);
			uint16_t position = state->receive_position;

			for (size_t i = 0; i < input_frames; i++) {
				position += i2s_positions_per_frame(pcm);
				position &= TEAK_I2S_RWADDR_RDADDR;
				if (position == state->registers[TEAK_I2S_RXINTADDR]) {
					input_frames = i + 1;
					break;
				}
			}
			i2s_rx_read(state->port.rx_bus, samples, input_frames);

			for (size_t i = 0; i < input_frames; i++) {
				uint16_t address = device->config->ram_base + I2S_RING_WORDS + state->receive_position;
				const int16_t *frame = &samples[i * channels];

				state->host.data_write(state->host.opaque, address, frame[0]);
				if (!pcm)
					state->host.data_write(state->host.opaque, address + 1, frame[1]);

				frames--;
				state->receive_position += i2s_positions_per_frame(pcm);
				state->receive_position &= TEAK_I2S_RWADDR_RDADDR;
				if (state->receive_position == state->registers[TEAK_I2S_RXINTADDR]) {
					if (pcm)
						state->registers[TEAK_I2S_CTRL] &= (uint16_t) ~TEAK_I2S_CTRL_I2SRXSTART;
					dsp_int_set_flags(state->interrupt, I2S_INTERRUPT_GROUP,
						state->transmit_interrupt_flag << 1);
					state->receive_phase = 0;
					frames = 0;
					break;
				}
			}
		}
	}
}

bool dsp_i2s_is_active(const dsp_device_t *device) {
	const i2s_state_t *state = device->state;
	return i2s_transmit_active(state) || i2s_receive_active(state);
}
