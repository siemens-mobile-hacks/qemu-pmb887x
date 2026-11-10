#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bitops.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"

#define MODULATOR_REGISTER_COUNT	(TEAK_MOD_UNKA + 1)
#define MODULATOR_RESERVED	0x02
#define MODULATOR_INTERRUPT_GROUP	0
#define MODULATOR_RAM_WORDS	(TEAK_MOD_INT_ADDR_MINT_ADDR + 1U)
#define MODULATOR_MS_CYCLES_PER_SYMBOL	96U
/* E101 reads one RAM word per four GMSK symbols, measured on hardware. */
#define MODULATOR_E101_SYMBOLS_PER_WORD	4U

typedef struct modulator_state_t modulator_state_t;

struct modulator_state_t {
	uint16_t registers[MODULATOR_REGISTER_COUNT];
	dsp_device_t *interrupt;
	uint32_t clock_hz;
	uint32_t ms_clock_hz;
	uint64_t word_phase;
	uint16_t position;
	uint16_t symbols_per_word;
	bool codon;
};

static bool modulator_active(const modulator_state_t *state) {
	bool software_active = qatomic_read(&state->registers[TEAK_MOD_CTRL]) & TEAK_MOD_CTRL_MSWACT;
	return qatomic_read(&state->codon) || software_active;
}

static void modulator_read_word(modulator_state_t *state) {
	uint16_t address = state->position;

	state->position = (state->position + 1U) % MODULATOR_RAM_WORDS;
	if (address == state->registers[TEAK_MOD_INT_ADDR])
		dsp_int_set_flags(state->interrupt, MODULATOR_INTERRUPT_GROUP, TEAK_INT_FINTA0_MODU);
}

static void modulator_start(modulator_state_t *state) {
	state->position = 0;
	state->word_phase = 0;
	modulator_read_word(state);
}

static uint64_t modulator_word_phase_period(const modulator_state_t *state) {
	return (uint64_t) state->clock_hz * MODULATOR_MS_CYCLES_PER_SYMBOL * state->symbols_per_word;
}

static void modulator_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void modulator_reset(dsp_device_t *device) {
	modulator_state_t *state = device->state;
	dsp_device_t *interrupt = state->interrupt;
	uint32_t clock_hz = state->clock_hz;
	uint32_t ms_clock_hz = state->ms_clock_hz;

	memset(state, 0, sizeof(*state));
	state->interrupt = interrupt;
	state->clock_hz = clock_hz;
	state->ms_clock_hz = ms_clock_hz;
	state->symbols_per_word = device->dsp_config->hardware_version == PMB887X_DSP_HW_VERSION_E101 ?
		MODULATOR_E101_SYMBOLS_PER_WORD : 1U;
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

	dsp_device_debug_io(device, offset, *value, false, pc);
	return true;
}

static bool modulator_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	modulator_state_t *state = device->state;

	switch (offset) {
		case TEAK_MOD_STAT:
		case MODULATOR_RESERVED:
			break;

		case TEAK_MOD_CTRL: {
			bool was_active = modulator_active(state);

			qatomic_set(&state->registers[offset], value & (TEAK_MOD_CTRL_IQSWAP | TEAK_MOD_CTRL_MSWACT));
			if (!was_active && modulator_active(state))
				modulator_start(state);
			break;
		}

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

	dsp_device_debug_io(device, offset, value, true, pc);
	return true;
}


static const dsp_device_ops_t modulator_ops = {
	.destroy = modulator_destroy,
	.reset = modulator_reset,
	.read = modulator_read,
	.write = modulator_write,
};

uint16_t modulator_ram_stored_bits(uint16_t hardware_version) {
	return hardware_version == PMB887X_DSP_HW_VERSION_E101 ? 0x000F : 0x0001;
}

dsp_device_t *modulator_create(
	const pmb887x_dsp_peripheral_config_t *config,
	const DebugChannel *parent,
	dsp_device_t *interrupt
) {
	modulator_state_t *state = g_new0(modulator_state_t, 1);
	state->interrupt = interrupt;
	return dsp_device_create(config, parent, &modulator_ops, state);
}

void modulator_set_codon(dsp_device_t *device, bool level) {
	modulator_state_t *state = device->state;
	bool was_active = modulator_active(state);

	qatomic_set(&state->codon, level);
	if (!was_active && modulator_active(state))
		modulator_start(state);
}

void modulator_set_clock_hz(dsp_device_t *device, uint32_t frequency_hz) {
	modulator_state_t *state = device->state;
	state->clock_hz = frequency_hz;
	state->word_phase = 0;
}

void modulator_set_ms_clock_hz(dsp_device_t *device, uint32_t frequency_hz) {
	modulator_state_t *state = device->state;
	state->ms_clock_hz = frequency_hz;
}

void modulator_advance(dsp_device_t *device, size_t cycles) {
	modulator_state_t *state = device->state;
	uint64_t period = modulator_word_phase_period(state);

	if (period == 0)
		return;

	state->word_phase += (uint64_t) cycles * state->ms_clock_hz;
	while (state->word_phase >= period) {
		state->word_phase -= period;
		modulator_read_word(state);
	}
}

size_t modulator_get_cycles_until_interrupt(const dsp_device_t *device) {
	const modulator_state_t *state = device->state;
	uint64_t period = modulator_word_phase_period(state);

	if (period == 0 || state->ms_clock_hz == 0)
		return SIZE_MAX;

	uint16_t words_before = (uint16_t) (state->registers[TEAK_MOD_INT_ADDR] - state->position) % MODULATOR_RAM_WORDS;
	return DIV_ROUND_UP((words_before + 1U) * period - state->word_phase, state->ms_clock_hz);
}

bool modulator_is_active(const dsp_device_t *device) {
	const modulator_state_t *state = device->state;
	return modulator_active(state);
}
