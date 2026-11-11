#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bitops.h"
#include "qemu/timer.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/gsm/iq-bus.h"

#define MODULATOR_REGISTER_COUNT	(TEAK_MOD_UNKA + 1)
#define MODULATOR_RESERVED	0x02
#define MODULATOR_INTERRUPT_GROUP	0
#define MODULATOR_RAM_WORDS	(TEAK_MOD_INT_ADDR_MINT_ADDR + 1U)
#define MODULATOR_MS_CYCLES_PER_SYMBOL	96U
/* E101 reads one RAM word per four GMSK symbols, measured on hardware. */
#define MODULATOR_E101_SYMBOLS_PER_WORD	4U
#define MODULATOR_SYMBOL_BIT	BIT(0)
/* RAM word read to antenna: six GSM symbols of 48/13 us. */
#define MODULATOR_OUTPUT_DELAY_NS	22154

typedef struct modulator_state_t modulator_state_t;

struct modulator_state_t {
	uint16_t registers[MODULATOR_REGISTER_COUNT];
	dsp_device_t *interrupt;
	dsp_host_t host;
	IQBus *iq_bus;
	uint32_t clock_hz;
	uint32_t ms_clock_hz;
	uint64_t word_phase;
	int64_t start_ns;
	uint64_t words_sent;
	uint16_t ram_base;
	uint16_t position;
	uint16_t symbols_per_word;
	bool codon;
};

static bool modulator_active(const modulator_state_t *state) {
	bool software_active = qatomic_read(&state->registers[TEAK_MOD_CTRL]) & TEAK_MOD_CTRL_MSWACT;
	return qatomic_read(&state->codon) || software_active;
}

static int64_t modulator_next_word_ns(const modulator_state_t *state) {
	uint64_t sent_cycles = state->words_sent * MODULATOR_MS_CYCLES_PER_SYMBOL * state->symbols_per_word;
	return state->start_ns + MODULATOR_OUTPUT_DELAY_NS + muldiv64(sent_cycles, NANOSECONDS_PER_SECOND, state->ms_clock_hz);
}

static void modulator_read_word(modulator_state_t *state) {
	uint16_t address = state->position;
	uint16_t word = state->host.data_read(state->host.opaque, state->ram_base + address);

	iq_bus_transmit(state->iq_bus, modulator_next_word_ns(state), (word & MODULATOR_SYMBOL_BIT) != 0);
	state->words_sent++;
	state->position = (state->position + 1U) % MODULATOR_RAM_WORDS;
	if (address == state->registers[TEAK_MOD_INT_ADDR])
		dsp_int_set_flags(state->interrupt, MODULATOR_INTERRUPT_GROUP, TEAK_INT_FINTA0_MODU);
}

static void modulator_start(modulator_state_t *state, int64_t timestamp_ns) {
	state->position = 0;
	state->word_phase = 0;
	state->start_ns = timestamp_ns;
	state->words_sent = 0;
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
	dsp_host_t host = state->host;
	IQBus *iq_bus = state->iq_bus;
	uint32_t clock_hz = state->clock_hz;
	uint32_t ms_clock_hz = state->ms_clock_hz;
	uint16_t ram_base = state->ram_base;

	memset(state, 0, sizeof(*state));
	state->interrupt = interrupt;
	state->host = host;
	state->iq_bus = iq_bus;
	state->clock_hz = clock_hz;
	state->ms_clock_hz = ms_clock_hz;
	state->ram_base = ram_base;
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
				modulator_start(state, state->host.get_time_ns(state->host.opaque));
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
	dsp_device_t *interrupt,
	const dsp_host_t *host
) {
	modulator_state_t *state = g_new0(modulator_state_t, 1);
	state->interrupt = interrupt;
	state->host = *host;
	state->ram_base = config->ram_base;
	return dsp_device_create(config, parent, &modulator_ops, state);
}

void modulator_set_codon(dsp_device_t *device, bool level, int64_t timestamp_ns) {
	modulator_state_t *state = device->state;
	bool was_active = modulator_active(state);

	qatomic_set(&state->codon, level);
	if (!was_active && modulator_active(state))
		modulator_start(state, timestamp_ns);
}

void modulator_set_iq_bus(dsp_device_t *device, IQBus *iq_bus) {
	modulator_state_t *state = device->state;
	state->iq_bus = iq_bus;
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

void modulator_commit_transmit(dsp_device_t *device, int64_t idle_until_ns) {
	modulator_state_t *state = device->state;
	int64_t until_ns = modulator_active(state) ? modulator_next_word_ns(state) : idle_until_ns;

	iq_bus_transmit_until(state->iq_bus, until_ns);
}

bool modulator_is_active(const dsp_device_t *device) {
	const modulator_state_t *state = device->state;
	return modulator_active(state);
}
