#define PMB887X_TRACE_ID		DSP_I2S
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-i2s"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define I2S_REGISTER_COUNT	(TEAK_I2S_TXINTADDR + 1)
#define I2S_CONTROL_MASK	(TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART | TEAK_I2S_CTRL_I2SRXSTART | \
	TEAK_I2S_CTRL_TXPCM | TEAK_I2S_CTRL_RXPCM | TEAK_I2S_CTRL_DAI_EN)
#define I2S_SAMPLE_CYCLES	16U
#define I2S_INTERRUPT_GROUP	1

typedef struct i2s_state_t i2s_state_t;

struct i2s_state_t {
	uint16_t registers[I2S_REGISTER_COUNT];
	dsp_device_t *interrupt;
	uint16_t transmit_interrupt_flag;
	uint16_t transmit_position;
	uint16_t receive_position;
	size_t sample_cycles;
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

static void i2s_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void i2s_reset(dsp_device_t *device) {
	i2s_state_t *state = device->state;
	dsp_device_t *interrupt = state->interrupt;
	uint16_t transmit_interrupt_flag = state->transmit_interrupt_flag;

	memset(state, 0, sizeof(*state));
	state->interrupt = interrupt;
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

dsp_device_t *i2s_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt, uint16_t interrupt_flag) {
	i2s_state_t *state = g_new0(i2s_state_t, 1);
	state->interrupt = interrupt;
	state->transmit_interrupt_flag = interrupt_flag;
	return dsp_device_create(config, &i2s_ops, state);
}

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
