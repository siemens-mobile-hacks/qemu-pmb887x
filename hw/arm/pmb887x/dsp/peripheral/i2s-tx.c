#define PMB887X_TRACE_ID		DSP_I2S_TX
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-i2s-tx"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define I2S_TX_REGISTER_COUNT	(TEAK_I2S3_TXINTADDR + 1)
#define I2S_TX_FRACTIONAL_DIVIDER_FACTOR	4U
#define I2S_TX_FRAME_SLOTS	2U
#define I2S_TX_INTERRUPT_GROUP	1

typedef struct i2s_tx_state_t i2s_tx_state_t;

struct i2s_tx_state_t {
	uint16_t registers[I2S_TX_REGISTER_COUNT];
	dsp_device_t *interrupt;
	uint16_t position;
	uint32_t clock_hz;
	uint32_t ms_clock_hz;
	uint64_t phase;
};

static bool i2s_tx_active(const i2s_tx_state_t *state) {
	uint16_t control = state->registers[TEAK_I2S3_CTRL];
	uint16_t active = TEAK_I2S3_CTRL_I2SON | TEAK_I2S3_CTRL_I2STXSTART;
	return (control & active) == active;
}

static uint32_t i2s_tx_reference_hz(const i2s_tx_state_t *state) {
	switch (state->registers[TEAK_I2S3_NUM] & TEAK_I2S3_NUM_FREF) {
		case TEAK_I2S3_NUM_FREF_MODULE_CLOCK:
			return state->clock_hz;

		case TEAK_I2S3_NUM_FREF_CLOCK_104MHZ:
			return state->ms_clock_hz;
	}
	return 0;
}

static uint32_t i2s_tx_period_clocks(const i2s_tx_state_t *state) {
	switch (state->registers[TEAK_I2S3_TXCONF] & TEAK_I2S3_TXCONF_PERIOD) {
		case TEAK_I2S3_TXCONF_PERIOD_CLOCKS_64:
			return 64;

		case TEAK_I2S3_TXCONF_PERIOD_CLOCKS_48:
			return 48;

		case TEAK_I2S3_TXCONF_PERIOD_CLOCKS_32:
			return 32;
	}
	return 0;
}

static size_t i2s_tx_advance_frames(i2s_tx_state_t *state, size_t cycles) {
	if ((state->registers[TEAK_I2S3_CSEL] & TEAK_I2S3_CSEL_TXCLKSEL) != 0)
		return 0;

	uint32_t reference_hz = i2s_tx_reference_hz(state);
	uint16_t numerator = state->registers[TEAK_I2S3_NUM] & TEAK_I2S3_NUM_NUMERATOR;
	uint16_t denominator = state->registers[TEAK_I2S3_DEN];
	uint32_t period_clocks = i2s_tx_period_clocks(state);

	if (state->clock_hz == 0 || reference_hz == 0 || numerator == 0 || denominator == 0 || period_clocks == 0)
		return 0;

	uint64_t frame_phase = (uint64_t) state->clock_hz * denominator * period_clocks *
		I2S_TX_FRACTIONAL_DIVIDER_FACTOR;
	state->phase += (uint64_t) cycles * reference_hz * numerator;
	size_t frames = state->phase / frame_phase;
	state->phase %= frame_phase;
	return frames;
}

static void i2s_tx_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void i2s_tx_reset(dsp_device_t *device) {
	i2s_tx_state_t *state = device->state;
	dsp_device_t *interrupt = state->interrupt;
	uint32_t clock_hz = state->clock_hz;
	uint32_t ms_clock_hz = state->ms_clock_hz;

	memset(state, 0, sizeof(*state));
	state->interrupt = interrupt;
	state->clock_hz = clock_hz;
	state->ms_clock_hz = ms_clock_hz;
	state->registers[TEAK_I2S3_NUM] = 1;
	state->registers[TEAK_I2S3_DEN] = 2;
}

static bool i2s_tx_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	i2s_tx_state_t *state = device->state;

	switch (offset) {
		case TEAK_I2S3_RADDR:
			*value = state->position;
			break;

		default:
			*value = offset < I2S_TX_REGISTER_COUNT ? state->registers[offset] : 0;
			break;
	}

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool i2s_tx_register_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	i2s_tx_state_t *state = device->state;

	switch (offset) {
		case TEAK_I2S3_CTRL:
			state->registers[offset] = value &
				(TEAK_I2S3_CTRL_I2SON | TEAK_I2S3_CTRL_I2STXSTART | TEAK_I2S3_CTRL_TXPCM);
			if ((value & TEAK_I2S3_CTRL_I2SON) == 0) {
				state->position = 0;
				state->phase = 0;
			}
			break;

		case TEAK_I2S3_TXINTADDR:
			state->registers[offset] = value & TEAK_I2S3_RADDR_RDADDR;
			break;

		case TEAK_I2S3_RADDR:
			break;

		default:
			if (offset < I2S_TX_REGISTER_COUNT)
				state->registers[offset] = value;
			break;
	}

	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t i2s_tx_ops = {
	.destroy = i2s_tx_destroy,
	.reset = i2s_tx_reset,
	.read = i2s_tx_read,
	.write = i2s_tx_register_write,
};

dsp_device_t *dsp_i2s_tx_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt) {
	i2s_tx_state_t *state = g_new0(i2s_tx_state_t, 1);
	state->interrupt = interrupt;
	return dsp_device_create(config, &i2s_tx_ops, state);
}

void dsp_i2s_tx_set_clock_hz(dsp_device_t *device, uint32_t frequency_hz) {
	i2s_tx_state_t *state = device->state;

	if (state->clock_hz == frequency_hz)
		return;
	state->clock_hz = frequency_hz;
	state->phase = 0;
}

void dsp_i2s_tx_set_ms_clock_hz(dsp_device_t *device, uint32_t frequency_hz) {
	i2s_tx_state_t *state = device->state;

	if (state->ms_clock_hz == frequency_hz)
		return;
	state->ms_clock_hz = frequency_hz;
	state->phase = 0;
}

void dsp_i2s_tx_advance(dsp_device_t *device, size_t cycles) {
	i2s_tx_state_t *state = device->state;

	if (!i2s_tx_active(state))
		return;

	size_t frames = i2s_tx_advance_frames(state, cycles);
	while (i2s_tx_active(state) && frames-- != 0) {
		bool pcm = (state->registers[TEAK_I2S3_CTRL] & TEAK_I2S3_CTRL_TXPCM) != 0;
		state->position += pcm ? 1 : I2S_TX_FRAME_SLOTS;
		state->position &= TEAK_I2S3_RADDR_RDADDR;

		if (state->position == state->registers[TEAK_I2S3_TXINTADDR]) {
			if ((state->registers[TEAK_I2S3_CTRL] & TEAK_I2S3_CTRL_TXPCM) != 0)
				state->registers[TEAK_I2S3_CTRL] &= (uint16_t) ~TEAK_I2S3_CTRL_I2STXSTART;
			dsp_int_set_flags(state->interrupt, I2S_TX_INTERRUPT_GROUP, TEAK_INT_FINTB0_I2S3TX);
			state->phase = 0;
			break;
		}
	}
}

bool dsp_i2s_tx_is_active(const dsp_device_t *device) {
	const i2s_tx_state_t *state = device->state;
	return i2s_tx_active(state);
}
