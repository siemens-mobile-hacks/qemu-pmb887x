#define PMB887X_TRACE_ID		DSP_I2S_TX
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-i2s-tx"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define I2S_TX_REGISTER_COUNT	(TEAK_I2S3_TXINTADDR + 1)
#define I2S_TX_SAMPLE_CYCLES	16U
#define I2S_TX_INTERRUPT_GROUP	1

typedef struct i2s_tx_state_t i2s_tx_state_t;

struct i2s_tx_state_t {
	uint16_t registers[I2S_TX_REGISTER_COUNT];
	dsp_device_t *interrupt;
	uint16_t position;
	size_t sample_cycles;
};

static bool i2s_tx_active(const i2s_tx_state_t *state) {
	uint16_t control = state->registers[TEAK_I2S3_CTRL];
	uint16_t active = TEAK_I2S3_CTRL_I2SON | TEAK_I2S3_CTRL_I2STXSTART;
	return (control & active) == active;
}

static void i2s_tx_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void i2s_tx_reset(dsp_device_t *device) {
	i2s_tx_state_t *state = device->state;
	dsp_device_t *interrupt = state->interrupt;

	memset(state, 0, sizeof(*state));
	state->interrupt = interrupt;
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

static bool i2s_tx_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	i2s_tx_state_t *state = device->state;

	switch (offset) {
		case TEAK_I2S3_CTRL:
			state->registers[offset] = value & 0x0023U;
			if ((value & TEAK_I2S3_CTRL_I2SON) == 0) {
				state->position = 0;
				state->sample_cycles = 0;
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
	.write = i2s_tx_write,
};

dsp_device_t *i2s_tx_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt) {
	i2s_tx_state_t *state = g_new0(i2s_tx_state_t, 1);
	state->interrupt = interrupt;
	return dsp_device_create(config, &i2s_tx_ops, state);
}

void i2s_tx_advance(dsp_device_t *device, size_t cycles) {
	i2s_tx_state_t *state = device->state;

	if (!i2s_tx_active(state))
		return;

	state->sample_cycles += cycles;

	while (i2s_tx_active(state) && state->sample_cycles >= I2S_TX_SAMPLE_CYCLES) {
		state->sample_cycles -= I2S_TX_SAMPLE_CYCLES;
		state->position++;
		state->position &= TEAK_I2S3_RADDR_RDADDR;

		if (state->position == state->registers[TEAK_I2S3_TXINTADDR]) {
			if ((state->registers[TEAK_I2S3_CTRL] & TEAK_I2S3_CTRL_TXPCM) != 0)
				state->registers[TEAK_I2S3_CTRL] &= (uint16_t) ~TEAK_I2S3_CTRL_I2STXSTART;
			dsp_int_set_flags(state->interrupt, I2S_TX_INTERRUPT_GROUP, TEAK_INT_FINTB0_I2S3TX);
			state->sample_cycles = 0;
			break;
		}
	}
}

bool i2s_tx_is_active(const dsp_device_t *device) {
	const i2s_tx_state_t *state = device->state;
	return i2s_tx_active(state);
}
