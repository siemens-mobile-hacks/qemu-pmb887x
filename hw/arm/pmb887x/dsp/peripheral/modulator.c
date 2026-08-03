#define PMB887X_TRACE_ID		DSP_MODULATOR
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-modulator"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bitops.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define MODULATOR_REGISTER_COUNT	(TEAK_MOD_UNKA + 1)
#define MODULATOR_RESERVED	0x02
#define MODULATOR_SAMPLE_CYCLES	16U
#define MODULATOR_INTERRUPT_GROUP	0

typedef struct modulator_state_t modulator_state_t;

struct modulator_state_t {
	uint16_t registers[MODULATOR_REGISTER_COUNT];
	dsp_device_t *interrupt;
	uint16_t position;
	size_t sample_cycles;
	bool codon;
};

static bool modulator_active(const modulator_state_t *state) {
	bool software_active = qatomic_read(&state->registers[TEAK_MOD_CTRL]) & TEAK_MOD_CTRL_MSWACT;
	return qatomic_read(&state->codon) || software_active;
}

static void modulator_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void modulator_reset(dsp_device_t *device) {
	modulator_state_t *state = device->state;
	dsp_device_t *interrupt = state->interrupt;

	memset(state, 0, sizeof(*state));
	state->interrupt = interrupt;
	state->registers[TEAK_MOD_CTRL] = 0x0600;
	state->registers[TEAK_MOD_ACI] = 0x00FF;
	state->registers[TEAK_MOD_ACQ] = 0x00FF;
}

static bool modulator_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	modulator_state_t *state = device->state;

	switch (offset) {
		case TEAK_MOD_STAT:
			*value = modulator_active(state) * TEAK_MOD_STAT_MSTAT;
			break;

		case MODULATOR_RESERVED:
			*value = 0;
			break;

		default:
			*value = offset < ARRAY_SIZE(state->registers) ? state->registers[offset] : 0;
			break;
	}

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool modulator_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	modulator_state_t *state = device->state;

	switch (offset) {
		case TEAK_MOD_STAT:
		case MODULATOR_RESERVED:
			break;

		case TEAK_MOD_CTRL:
			qatomic_set(&state->registers[offset], value & (TEAK_MOD_CTRL_IQSWAP | TEAK_MOD_CTRL_MSWACT));
			if (!modulator_active(state))
				state->sample_cycles = 0;
			break;

		case TEAK_MOD_INT_ADDR:
			state->registers[offset] = value & TEAK_MOD_INT_ADDR_MINT_ADDR;
			break;

		case TEAK_MOD_OCI:
		case TEAK_MOD_OCQ:
		case TEAK_MOD_FC:
			state->registers[offset] = value & TEAK_MOD_OCI_VALUE;
			break;

		case TEAK_MOD_ACI:
		case TEAK_MOD_ACQ:
			state->registers[offset] = value & TEAK_MOD_ACI_VALUE;
			break;

		default:
			if (offset < ARRAY_SIZE(state->registers))
				state->registers[offset] = value;
			break;
	}

	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t modulator_ops = {
	.destroy = modulator_destroy,
	.reset = modulator_reset,
	.read = modulator_read,
	.write = modulator_write,
};

dsp_device_t *modulator_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt) {
	modulator_state_t *state = g_new0(modulator_state_t, 1);
	state->interrupt = interrupt;
	return dsp_device_create(config, &modulator_ops, state);
}

void modulator_set_codon(dsp_device_t *device, bool level) {
	modulator_state_t *state = device->state;

	qatomic_set(&state->codon, level);
}

void modulator_advance(dsp_device_t *device, size_t cycles) {
	modulator_state_t *state = device->state;

	if (!modulator_active(state)) {
		state->sample_cycles = 0;
		return;
	}

	state->sample_cycles += cycles;

	while (modulator_active(state) && state->sample_cycles >= MODULATOR_SAMPLE_CYCLES) {
		state->sample_cycles -= MODULATOR_SAMPLE_CYCLES;
		state->position++;
		state->position &= TEAK_MOD_INT_ADDR_MINT_ADDR;

		if (state->position == state->registers[TEAK_MOD_INT_ADDR])
			dsp_int_set_flags(state->interrupt, MODULATOR_INTERRUPT_GROUP, TEAK_INT_FINTA0_MODU);
	}
}

bool modulator_is_active(const dsp_device_t *device) {
	const modulator_state_t *state = device->state;
	return modulator_active(state);
}
