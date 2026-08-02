#define PMB887X_TRACE_ID		DSP_EQUALIZER
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-equalizer"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"
#include "qemu/bitops.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define EQUALIZER_STATES			8U
#define EQUALIZER_SYMBOLS		64U
#define EQUALIZER_RECEIVED_VALUES	32U
#define EQUALIZER_BRANCH_VALUES		64U
#define EQUALIZER_WORKING_WORDS		64U
#define EQUALIZER_RAM2_WORDS		256U
#define EQUALIZER_TIMESTAMP_CYCLES	208U
#define EQUALIZER_TRAINING_SYMBOLS	8U
#define EQUALIZER_SOFT_DELAY		1U
#define EQUALIZER_HARD_DELAY		6U
#define EQUALIZER_PATH_MASK		0x3FFFFFFFU
#define EQUALIZER_HARD_HISTORY_WORDS	(EQUALIZER_SYMBOLS + EQUALIZER_TRAINING_SYMBOLS + 1U)
#define EQUALIZER_COMBINED_WORDS		96U
#define EQUALIZER_BRANCH_WORD_BASE	32U
#define EQUALIZER_INTERRUPT_GROUP	0U

static const uint8_t EQUALIZER_SCRATCH_ORDER[EQUALIZER_STATES] = { 0, 2, 4, 6, 1, 3, 5, 7 };

typedef enum equalizer_external_target_t equalizer_external_target_t;

enum equalizer_external_target_t {
	EQUALIZER_EXTERNAL_NONE,
	EQUALIZER_EXTERNAL_RX,
	EQUALIZER_EXTERNAL_BPAR,
	EQUALIZER_EXTERNAL_SOUT,
	EQUALIZER_EXTERNAL_HOUT,
	EQUALIZER_EXTERNAL_ELAT,
	EQUALIZER_EXTERNAL_EMR,
	EQUALIZER_EXTERNAL_EML,
	EQUALIZER_EXTERNAL_EPR,
	EQUALIZER_EXTERNAL_EPL,
	EQUALIZER_EXTERNAL_EB,
};

typedef struct equalizer_working_ram_t equalizer_working_ram_t;

struct equalizer_working_ram_t {
	uint32_t words[EQUALIZER_WORKING_WORDS];
};

typedef struct equalizer_state_t equalizer_state_t;

struct equalizer_state_t {
	dsp_device_t *interrupt;
	uint16_t config2;
	uint16_t configured_count;
	uint16_t completed_count;
	uint16_t soft_scale;
	uint16_t signal_quality[16];
	uint32_t received[EQUALIZER_RECEIVED_VALUES];
	uint32_t branch[EQUALIZER_BRANCH_VALUES];
	uint16_t ram2[EQUALIZER_RAM2_WORDS];
	equalizer_working_ram_t working[2];
	int16_t metrics[EQUALIZER_STATES];
	uint32_t paths[EQUALIZER_STATES];
	uint32_t survivor_paths[2][EQUALIZER_STATES];
	uint8_t survivor_history[2][EQUALIZER_STATES][EQUALIZER_HARD_HISTORY_WORDS];
	uint8_t decisions[EQUALIZER_SYMBOLS];
	uint8_t combined_hard[EQUALIZER_HARD_HISTORY_WORDS];
	equalizer_external_target_t external_target;
	size_t external_pointer;
	size_t external_bank;
	size_t working_source_bank[2];
	size_t side_processed_count[2];
	size_t processed_count;
	size_t signal_quality_pointer;
	size_t elapsed_cycles;
	bool external_high;
	bool context_valid[2];
	bool continuing;
	bool starting;
	bool active;
};

static void equalizer_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void equalizer_reset_state(equalizer_state_t *state) {
	dsp_device_t *interrupt = state->interrupt;

	memset(state, 0, sizeof(*state));
	state->interrupt = interrupt;
	state->signal_quality_pointer = ARRAY_SIZE(state->signal_quality) - 1;
}

static void equalizer_reset(dsp_device_t *device) {
	equalizer_state_t *state = device->state;
	equalizer_reset_state(state);
}

static void equalizer_reset_operation(equalizer_state_t *state) {
	state->config2 &= TEAK_EQ_CONF2_HW_ENA_EQ;
	state->configured_count = 0;
	state->completed_count = 0;
	memset(state->working_source_bank, 0, sizeof(state->working_source_bank));
	memset(state->context_valid, 0, sizeof(state->context_valid));
	state->processed_count = 0;
	state->elapsed_cycles = 0;
	state->external_high = false;
	state->continuing = false;
	state->starting = false;
	state->active = false;
}

static void equalizer_reset_registers(equalizer_state_t *state) {
	state->config2 = 0;
	state->configured_count = 0;
	state->completed_count = 0;
	state->soft_scale = 0;
	memset(state->signal_quality, 0, sizeof(state->signal_quality));
	memset(state->metrics, 0, sizeof(state->metrics));
	memset(state->paths, 0, sizeof(state->paths));
	memset(state->survivor_paths, 0, sizeof(state->survivor_paths));
	memset(state->survivor_history, 0, sizeof(state->survivor_history));
	memset(state->decisions, 0, sizeof(state->decisions));
	memset(state->combined_hard, 0, sizeof(state->combined_hard));
	state->external_target = EQUALIZER_EXTERNAL_NONE;
	state->external_pointer = 0;
	state->external_bank = 0;
	memset(state->working_source_bank, 0, sizeof(state->working_source_bank));
	memset(state->side_processed_count, 0, sizeof(state->side_processed_count));
	memset(state->context_valid, 0, sizeof(state->context_valid));
	state->processed_count = 0;
	state->signal_quality_pointer = ARRAY_SIZE(state->signal_quality) - 1;
	state->elapsed_cycles = 0;
	state->external_high = false;
	state->continuing = false;
	state->starting = false;
	state->active = false;
}

static void equalizer_select_external(equalizer_state_t *state, uint16_t value) {
	if ((value & TEAK_EQ_CONF1_RES_EB_BASE) != 0) {
		state->external_target = EQUALIZER_EXTERNAL_EB;
	} else if ((value & TEAK_EQ_CONF1_RES_EPL_BASE) != 0) {
		state->external_target = EQUALIZER_EXTERNAL_EPL;
	} else if ((value & TEAK_EQ_CONF1_RES_EPR_BASE) != 0) {
		state->external_target = EQUALIZER_EXTERNAL_EPR;
	} else if ((value & TEAK_EQ_CONF1_RES_EML_BASE) != 0) {
		state->external_target = EQUALIZER_EXTERNAL_EML;
	} else if ((value & TEAK_EQ_CONF1_RES_EMR_BASE) != 0) {
		state->external_target = EQUALIZER_EXTERNAL_EMR;
	} else if ((value & TEAK_EQ_CONF1_RES_ELAT_BASE) != 0) {
		state->external_target = EQUALIZER_EXTERNAL_ELAT;
	} else if ((value & TEAK_EQ_CONF1_RES_HOUT_BASE) != 0) {
		state->external_target = EQUALIZER_EXTERNAL_HOUT;
	} else if ((value & TEAK_EQ_CONF1_RES_SOUT_BASE) != 0) {
		state->external_target = EQUALIZER_EXTERNAL_SOUT;
	} else if ((value & TEAK_EQ_CONF1_RES_BPAR_BASE) != 0) {
		state->external_target = EQUALIZER_EXTERNAL_BPAR;
	} else if ((value & TEAK_EQ_CONF1_RES_RX_BASE) != 0) {
		state->external_target = EQUALIZER_EXTERNAL_RX;
	} else {
		state->external_target = EQUALIZER_EXTERNAL_NONE;
	}

	state->external_pointer = 0;
	state->external_bank = (value & TEAK_EQ_CONF1_RES_RW1_RW2) != 0;
	state->external_high = false;
}

static uint16_t equalizer_working_read(const equalizer_state_t *state, size_t bank, size_t halfword) {
	uint32_t value = state->working[bank].words[halfword / 2];
	return halfword & 1 ? value >> 16 : value;
}

static void equalizer_working_write(equalizer_state_t *state, size_t bank, size_t halfword, uint16_t value) {
	uint32_t *word = &state->working[bank].words[halfword / 2];

	if ((halfword & 1) != 0) {
		*word = (*word & UINT16_MAX) | (uint32_t) value << 16;
	} else {
		*word = (*word & ~UINT16_MAX) | value;
	}
}

static bool equalizer_external_working(const equalizer_state_t *state, size_t *word_base, size_t *word_count) {
	switch (state->external_target) {
		case EQUALIZER_EXTERNAL_EMR:
			*word_base = 0;
			*word_count = 16;
			return true;

		case EQUALIZER_EXTERNAL_EML:
			*word_base = 8;
			*word_count = 16;
			return true;

		case EQUALIZER_EXTERNAL_EPR:
			*word_base = 16;
			*word_count = 16;
			return true;

		case EQUALIZER_EXTERNAL_EPL:
			*word_base = 24;
			*word_count = 16;
			return true;

		case EQUALIZER_EXTERNAL_EB:
			*word_base = EQUALIZER_BRANCH_WORD_BASE;
			*word_count = 64;
			return true;

		case EQUALIZER_EXTERNAL_RX:
		case EQUALIZER_EXTERNAL_BPAR:
		case EQUALIZER_EXTERNAL_SOUT:
		case EQUALIZER_EXTERNAL_HOUT:
		case EQUALIZER_EXTERNAL_ELAT:
		case EQUALIZER_EXTERNAL_NONE:
			return false;
	}
	g_assert_not_reached();
}

static void equalizer_start(equalizer_state_t *state) {
	bool right = (state->config2 & TEAK_EQ_CONF2_EQ_RIGHT) != 0;
	size_t side = right;
	size_t metric_base = right ? 0 : 8;
	size_t path_base = right ? 16 : 24;
	size_t metric_bank;

	state->continuing = state->context_valid[side];
	if (!state->continuing) {
		metric_bank = 1;
		state->working_source_bank[side] = 1;
		state->context_valid[side] = true;
	} else {
		metric_bank = state->working_source_bank[side];
	}

	for (size_t i = 0; i < EQUALIZER_STATES; i++) {
		state->metrics[i] = equalizer_working_read(state, metric_bank, metric_base * 2 + i);
		if (state->continuing) {
			state->paths[i] = state->survivor_paths[side][i];
		} else {
			state->paths[i] = state->working[0].words[path_base + i] & EQUALIZER_PATH_MASK;
		}
	}

	size_t best_state = 0;

	for (size_t i = 1; i < EQUALIZER_STATES; i++) {
		if (state->metrics[i] < state->metrics[best_state])
			best_state = i;
	}

	if ((state->config2 & TEAK_EQ_CONF2_EQ_EDGE) != 0) {
		if (state->continuing) {
			for (size_t i = 0; i < EQUALIZER_SYMBOLS; i++) {
				size_t source = state->side_processed_count[side] + i;

				state->ram2[128 + i] = 0;
				state->combined_hard[i] = 0;

				if (source < EQUALIZER_HARD_HISTORY_WORDS) {
					state->ram2[128 + i] = state->survivor_history[side][best_state][source];
					state->combined_hard[i] = state->ram2[128 + i];
				}
			}
		} else {
			memset(state->survivor_history[side], 0, sizeof(state->survivor_history[side]));
			state->side_processed_count[side] = 0;
			memset(state->ram2 + 128, 0, EQUALIZER_SYMBOLS * sizeof(state->ram2[0]));

			for (size_t path = 0; path < EQUALIZER_STATES; path++) {
				for (size_t group = 1; group < 7; group++) {
					size_t output = EQUALIZER_TRAINING_SYMBOLS + 1 - group;
					uint8_t symbol = state->paths[path] >> ((group - 1) * 3) & 7;
					state->survivor_history[side][path][output] = symbol;
				}
			}

			for (size_t i = 0; i < EQUALIZER_TRAINING_SYMBOLS + 1; i++)
				state->ram2[128 + i] = state->survivor_history[side][best_state][i];
			for (size_t i = 0; i < EQUALIZER_TRAINING_SYMBOLS + 1; i++)
				state->combined_hard[i] = state->ram2[128 + i];
		}
	}

	memset(state->decisions, 0, sizeof(state->decisions));
	state->completed_count = 0;
	state->processed_count = 0;
	state->elapsed_cycles = 0;
	state->starting = true;
	state->active = true;
}

static bool equalizer_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	equalizer_state_t *state = device->state;

	switch (offset) {
		case TEAK_EQ_CONF2:
			*value = state->config2;
			break;

		case TEAK_EQ_STATUS:
			*value = state->active ? TEAK_EQ_STATUS_EQ_BUSY : 0;
			break;

		case TEAK_EQ_CONF_CNT:
			*value = state->configured_count;
			break;

		case TEAK_EQ_STAT_CNT:
			*value = state->completed_count;
			break;

		case TEAK_EQ_SC_SOUT:
			*value = state->soft_scale;
			break;

		case TEAK_EQ_SQUAL:
			*value = state->signal_quality[state->signal_quality_pointer];
			if (state->signal_quality_pointer != 0)
				state->signal_quality_pointer--;
			break;

		default:
			*value = 0;
			break;
	}

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool equalizer_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	equalizer_state_t *state = device->state;

	switch (offset) {
		case TEAK_EQ_CONF1:
			equalizer_select_external(state, value);
			break;

		case TEAK_EQ_CONF2:
			if ((value & TEAK_EQ_CONF2_RES_ALL) != 0) {
				equalizer_reset_registers(state);
			} else {
				bool enabled = value & TEAK_EQ_CONF2_HW_ENA_EQ;
				bool start_requested = value & TEAK_EQ_CONF2_EQ_ON;

				state->config2 = value & ~TEAK_EQ_CONF2_RES_EQ;

				if ((value & TEAK_EQ_CONF2_RES_EQ) != 0)
					equalizer_reset_operation(state);
				if (enabled && start_requested)
					equalizer_start(state);
			}
			break;

		case TEAK_EQ_CONF_CNT:
			state->configured_count = value & TEAK_EQ_CONF_CNT_C_EQ;
			break;

		case TEAK_EQ_SC_SOUT:
			state->soft_scale = value;
			break;
	}

	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t equalizer_ops = {
	.destroy = equalizer_destroy,
	.reset = equalizer_reset,
	.read = equalizer_read,
	.write = equalizer_write,
};

static uint32_t *equalizer_external_ram32(equalizer_state_t *state, size_t *word_count) {
	switch (state->external_target) {
		case EQUALIZER_EXTERNAL_RX:
			*word_count = ARRAY_SIZE(state->received);
			return state->received;

		case EQUALIZER_EXTERNAL_BPAR:
			*word_count = ARRAY_SIZE(state->branch);
			return state->branch;

		case EQUALIZER_EXTERNAL_SOUT:
		case EQUALIZER_EXTERNAL_HOUT:
		case EQUALIZER_EXTERNAL_ELAT:
		case EQUALIZER_EXTERNAL_EMR:
		case EQUALIZER_EXTERNAL_EML:
		case EQUALIZER_EXTERNAL_EPR:
		case EQUALIZER_EXTERNAL_EPL:
		case EQUALIZER_EXTERNAL_EB:
		case EQUALIZER_EXTERNAL_NONE:
			*word_count = 0;
			return NULL;
	}
	g_assert_not_reached();
}

static uint16_t *equalizer_external_ram16(equalizer_state_t *state, size_t *word_count) {
	switch (state->external_target) {
		case EQUALIZER_EXTERNAL_SOUT:
			*word_count = 128;
			return state->ram2;

		case EQUALIZER_EXTERNAL_HOUT:
			*word_count = 64;
			return state->ram2 + 128;

		case EQUALIZER_EXTERNAL_ELAT:
			*word_count = 64;
			return state->ram2 + 192;

		case EQUALIZER_EXTERNAL_RX:
		case EQUALIZER_EXTERNAL_BPAR:
		case EQUALIZER_EXTERNAL_EMR:
		case EQUALIZER_EXTERNAL_EML:
		case EQUALIZER_EXTERNAL_EPR:
		case EQUALIZER_EXTERNAL_EPL:
		case EQUALIZER_EXTERNAL_EB:
		case EQUALIZER_EXTERNAL_NONE:
			*word_count = 0;
			return NULL;
	}
	g_assert_not_reached();
}

static uint8_t equalizer_combine_soft(const equalizer_state_t *state, size_t source) {
	int8_t soft = state->ram2[source / 2] >> ((source & 1) * 8);
	size_t hard_index = EQUALIZER_HARD_DELAY - EQUALIZER_SOFT_DELAY + source / 2;
	size_t bit = source & 3;
	bool valid_hard_index = hard_index < ARRAY_SIZE(state->combined_hard);
	bool negative = valid_hard_index && (state->combined_hard[hard_index] & BIT(bit)) != 0;

	if (negative)
		return soft < 0 ? (uint8_t) soft : UINT8_MAX;
	return soft > 0 ? (uint8_t) soft : 1;
}

static uint16_t equalizer_combine_pair(const equalizer_state_t *state, size_t high_source, size_t low_source) {
	uint8_t high = equalizer_combine_soft(state, high_source);
	uint8_t low = equalizer_combine_soft(state, low_source);
	return (uint16_t) high << 8 | low;
}

static uint16_t equalizer_combined_read(const equalizer_state_t *state, size_t word) {
	bool segment = (state->config2 & TEAK_EQ_CONF2_S_SEG) != 0;

	if (word >= EQUALIZER_COMBINED_WORDS)
		return 0;

	if (!segment) {
		if (word == 3 || word == 5)
			return 0x0101;
		if (word == 7)
			return 0x0100 | equalizer_combine_soft(state, 0);
		if (word < 9 || (word & 1) == 0)
			return 0;
		return equalizer_combine_pair(state, word * 2 - 16, word * 2 - 14);
	}

	if (word == 1) {
		if (state->processed_count != 0)
			return 0;
		return equalizer_combine_pair(state, 178, 180);
	}
	if (word == 2) {
		uint8_t low = equalizer_combine_soft(state, 182);
		if (state->processed_count != 0)
			return low;
		return (uint16_t) equalizer_combine_soft(state, 180) << 8 | low;
	}
	if (word == 4)
		return equalizer_combine_pair(state, 184, 186);
	if (word == 6)
		return equalizer_combine_pair(state, 188, 190);
	if (word < 8 || (word & 1) != 0)
		return 0;
	return equalizer_combine_pair(state, word * 2 - 16, word * 2 - 14);
}

uint16_t equalizer_external_read(dsp_device_t *device) {
	equalizer_state_t *state = device->state;
	size_t word_count;
	uint16_t *ram16 = equalizer_external_ram16(state, &word_count);

	if (ram16 != NULL) {
		bool packed = (state->config2 & TEAK_EQ_CONF2_PC_EQ_1) != 0;
		bool combined = state->external_target == EQUALIZER_EXTERNAL_SOUT &&
			(state->config2 & TEAK_EQ_CONF2_S_COMB) != 0;
		uint16_t value;

		if (combined)
			return equalizer_combined_read(state, state->external_pointer++);
		if (state->external_pointer >= word_count)
			return 0;

		value = ram16[state->external_pointer];
		if (!packed) {
			state->external_pointer++;
			return value;
		}
		value = state->external_high ? value >> 8 : value & UINT8_MAX;
		state->external_high = !state->external_high;
		if (!state->external_high)
			state->external_pointer++;
		return (uint16_t) (int16_t) (int8_t) value;
	}

	size_t working_word_base;

	if (equalizer_external_working(state, &working_word_base, &word_count)) {
		bool packed = (state->config2 & TEAK_EQ_CONF2_PC_EQ_0) != 0;
		size_t halfword;

		if (state->external_pointer >= word_count)
			return 0;
		if (packed) {
			halfword = working_word_base * 2 + state->external_pointer++;
			return equalizer_working_read(state, state->external_bank, halfword);
		}

		size_t word = working_word_base + state->external_pointer++;

		if (word >= EQUALIZER_WORKING_WORDS)
			return 0;
		return state->working[state->external_bank].words[word];
	}

	uint32_t *ram32 = equalizer_external_ram32(state, &word_count);
	bool packed = (state->config2 & TEAK_EQ_CONF2_PC_EQ_0) != 0;
	uint32_t value;

	if (ram32 == NULL || state->external_pointer >= word_count)
		return 0;
	value = ram32[state->external_pointer];
	if (!packed) {
		state->external_pointer++;
		return value;
	}
	value = state->external_high ? value >> 16 : value & UINT16_MAX;
	state->external_high = !state->external_high;
	if (!state->external_high)
		state->external_pointer++;
	return value;
}

void equalizer_external_write(dsp_device_t *device, uint16_t value) {
	equalizer_state_t *state = device->state;
	size_t word_count;
	uint16_t *ram16 = equalizer_external_ram16(state, &word_count);

	if (ram16 != NULL) {
		bool packed = (state->config2 & TEAK_EQ_CONF2_PC_EQ_1) != 0;

		if (state->external_pointer >= word_count)
			return;

		if (!packed) {
			ram16[state->external_pointer++] = value;
			return;
		}

		if (state->external_high) {
			ram16[state->external_pointer] &= UINT8_MAX;
			ram16[state->external_pointer] |= (value & UINT8_MAX) << 8;
		} else {
			ram16[state->external_pointer] = value & UINT8_MAX;
		}
		state->external_high = !state->external_high;
		if (!state->external_high)
			state->external_pointer++;
		return;
	}

	size_t working_word_base;

	if (equalizer_external_working(state, &working_word_base, &word_count)) {
		bool packed = (state->config2 & TEAK_EQ_CONF2_PC_EQ_0) != 0;
		size_t halfword;

		if (state->external_pointer >= word_count)
			return;

		if (packed) {
			halfword = working_word_base * 2 + state->external_pointer++;
			equalizer_working_write(state, state->external_bank, halfword, value);
			return;
		}

		size_t word = working_word_base + state->external_pointer++;

		if (word >= EQUALIZER_WORKING_WORDS)
			return;
		state->working[state->external_bank].words[word] = value;
		return;
	}

	uint32_t *ram32 = equalizer_external_ram32(state, &word_count);
	bool packed = (state->config2 & TEAK_EQ_CONF2_PC_EQ_0) != 0;

	if (ram32 == NULL || state->external_pointer >= word_count)
		return;

	if (!packed) {
		ram32[state->external_pointer++] = value;
		return;
	}
	if (state->external_high) {
		ram32[state->external_pointer] &= UINT16_MAX;
		ram32[state->external_pointer] |= (uint32_t) value << 16;
	} else {
		ram32[state->external_pointer] = value;
	}
	state->external_high = !state->external_high;
	if (!state->external_high)
		state->external_pointer++;
}

static int16_t equalizer_saturate_int16(int32_t value) {
	if (value > INT16_MAX)
		return INT16_MAX;
	if (value < INT16_MIN)
		return INT16_MIN;
	return value;
}

static int32_t equalizer_arithmetic_shift_right(int32_t value, size_t shift) {
	if (value >= 0)
		return value >> shift;
	return -((-value + (1 << shift) - 1) >> shift);
}

static int16_t equalizer_scale_soft(int16_t value, uint16_t scale) {
	uint16_t first_stage = scale & UINT8_MAX;
	int32_t scaled = value;

	if (first_stage == UINT8_MAX) {
		scaled *= 2;
	} else if (first_stage != 0) {
		size_t shift = 0;
		for (size_t bit = 0; bit < 8; bit++) {
			if ((first_stage & BIT(bit)) != 0)
				shift = bit + 1;
		}
		scaled = equalizer_arithmetic_shift_right(scaled, shift);
	}
	if ((scale & BIT(8)) != 0)
		scaled = equalizer_arithmetic_shift_right(scaled, 3);

	int32_t shifted = scaled;

	if ((scale & BIT(13)) != 0)
		scaled += equalizer_arithmetic_shift_right(shifted, 3);
	if ((scale & BIT(14)) != 0)
		scaled += equalizer_arithmetic_shift_right(shifted, 2);
	if ((scale & BIT(15)) != 0)
		scaled += equalizer_arithmetic_shift_right(shifted, 1);

	uint16_t saturation = scale & 0x1E00;
	int32_t limit = 127;

	if (saturation == 0x0200) {
		limit = 15;
	} else if (saturation == 0x0400) {
		limit = 31;
	} else if (saturation == 0x0800) {
		limit = 63;
	}

	if (scaled > limit)
		scaled = limit;
	if (scaled < -limit)
		scaled = -limit;
	return scaled;
}

static void equalizer_write_soft(equalizer_state_t *state, size_t offset, int16_t value) {
	uint16_t *word = &state->ram2[offset / 2];

	if ((offset & 1) != 0) {
		*word = (*word & UINT8_MAX) | (value & UINT8_MAX) << 8;
	} else {
		*word = (*word & ~UINT8_MAX) | (value & UINT8_MAX);
	}
}

static uint16_t equalizer_branch_metric(const equalizer_state_t *state, size_t timestamp, uint32_t path, size_t next) {
	uint32_t received = state->received[timestamp % EQUALIZER_RECEIVED_VALUES];
	int16_t real = received;
	int16_t imaginary = received >> 16;

	for (size_t group = 0; group < 7; group++) {
		size_t selector;

		if (group == 0) {
			selector = next;
		} else if (group < 6) {
			selector = path >> ((group - 1) * 3) & 7;
		} else {
			selector = path >> 15 & 7;
		}

		uint32_t branch = state->branch[group * EQUALIZER_STATES + selector];
		real = equalizer_saturate_int16(real + (int16_t) branch);
		imaginary = equalizer_saturate_int16(imaginary + (int16_t) (branch >> 16));
	}

	uint32_t real_square = (int32_t) real * real;
	uint32_t imaginary_square = (int32_t) imaginary * imaginary;
	uint32_t distance = (real_square >> 10) + (imaginary_square >> 10);
	return distance >> 1 & 0x7FFF;
}

static uint8_t equalizer_step(equalizer_state_t *state, size_t timestamp) {
	int16_t candidates[EQUALIZER_STATES][EQUALIZER_STATES];
	uint16_t branch_metrics[EQUALIZER_STATES][EQUALIZER_STATES];
	int16_t next_metrics[EQUALIZER_STATES];
	uint16_t survivor_branches[EQUALIZER_STATES];
	uint32_t next_paths[EQUALIZER_STATES];
	uint8_t next_history[EQUALIZER_STATES][EQUALIZER_HARD_HISTORY_WORDS];
	uint8_t best_state = 0;
	bool edge = (state->config2 & TEAK_EQ_CONF2_EQ_EDGE) != 0;
	bool right = (state->config2 & TEAK_EQ_CONF2_EQ_RIGHT) != 0;
	size_t side = right;
	size_t history_index = EQUALIZER_TRAINING_SYMBOLS + 1 + state->side_processed_count[side] + timestamp;

	for (size_t next = 0; next < EQUALIZER_STATES; next++) {
		size_t best_predecessor = 0;

		for (size_t previous = 0; previous < EQUALIZER_STATES; previous++) {
			uint16_t branch = equalizer_branch_metric(state, timestamp, state->paths[previous], next);
			branch_metrics[next][previous] = branch;
			candidates[next][previous] = equalizer_saturate_int16(state->metrics[previous] + branch);
			if (candidates[next][previous] < candidates[next][best_predecessor])
				best_predecessor = previous;
		}

		next_metrics[next] = candidates[next][best_predecessor];
		survivor_branches[next] = branch_metrics[next][best_predecessor];
		next_paths[next] = (state->paths[best_predecessor] << 3 | next) & EQUALIZER_PATH_MASK;
		memcpy(next_history[next], state->survivor_history[side][best_predecessor], history_index);
		next_history[next][history_index] = next;
		if (next_metrics[next] < next_metrics[best_state])
			best_state = next;
	}

	if (edge) {
		for (size_t bit = 0; bit < 3; bit++) {
			int16_t minimum[2] = { INT16_MAX, INT16_MAX };
			for (size_t next = 0; next < EQUALIZER_STATES; next++) {
				for (size_t previous = 0; previous < EQUALIZER_STATES; previous++) {
					size_t hypothesis = previous >> bit & 1;
					if (candidates[next][previous] < minimum[hypothesis])
						minimum[hypothesis] = candidates[next][previous];
				}
			}
			int16_t soft = equalizer_scale_soft(minimum[1] - minimum[0], state->soft_scale);
			equalizer_write_soft(state, timestamp * 4 + bit, soft);
		}
		equalizer_write_soft(state, timestamp * 4 + 3, 0);
	}

	if (edge) {
		for (size_t next = 0; next < EQUALIZER_STATES; next++)
			state->ram2[192 + next] = branch_metrics[next][EQUALIZER_STATES - 1];

		size_t output_count = MIN(history_index + 1 - state->side_processed_count[side], EQUALIZER_SYMBOLS);
		for (size_t i = 0; i < output_count; i++) {
			state->ram2[128 + i] = next_history[best_state][state->side_processed_count[side] + i];
			state->combined_hard[i] = state->ram2[128 + i];
		}
	}

	state->signal_quality[ARRAY_SIZE(state->signal_quality) - 1] = 3;

	size_t destination_bank = state->working_source_bank[side] ^ 1;
	size_t metric_base = right ? 0 : 8;
	size_t path_base = right ? 16 : 24;
	for (size_t i = 0; i < EQUALIZER_STATES; i++) {
		state->metrics[i] = next_metrics[i];
		state->paths[i] = next_paths[i];
		equalizer_working_write(state, destination_bank, metric_base * 2 + i, next_metrics[i]);
		state->working[0].words[path_base + i] = next_paths[i];
		state->working[1].words[path_base + i] = next_paths[i];
		state->survivor_paths[side][i] = next_paths[i];
		memcpy(state->survivor_history[side][i], next_history[i], sizeof(next_history[i]));
	}

	size_t scratch_bank = destination_bank ^ 1;
	size_t scratch_base = EQUALIZER_BRANCH_WORD_BASE + side * EQUALIZER_STATES;
	for (size_t i = 0; i < EQUALIZER_STATES; i++) {
		uint32_t *scratch = &state->working[scratch_bank].words[scratch_base + i];
		*scratch = (*scratch & ~UINT16_MAX) | survivor_branches[EQUALIZER_SCRATCH_ORDER[i]];
	}

	state->working_source_bank[side] = destination_bank;
	return best_state;
}

static void equalizer_store_gmsk_outputs(equalizer_state_t *state, size_t timestamp) {
	if (timestamp >= EQUALIZER_TRAINING_SYMBOLS + EQUALIZER_SOFT_DELAY) {
		uint8_t decision = state->decisions[timestamp - EQUALIZER_SOFT_DELAY];
		state->ram2[timestamp] = decision != 0 ? UINT8_MAX : 1;
	}
	if (timestamp >= EQUALIZER_TRAINING_SYMBOLS + EQUALIZER_HARD_DELAY)
		state->ram2[128 + timestamp] = state->decisions[timestamp - EQUALIZER_HARD_DELAY];
}

static void equalizer_complete(equalizer_state_t *state) {
	bool right = (state->config2 & TEAK_EQ_CONF2_EQ_RIGHT) != 0;
	state->side_processed_count[right] += state->processed_count;
	state->active = false;
	state->config2 &= ~TEAK_EQ_CONF2_EQ_ON;
	state->completed_count = (state->config2 & TEAK_EQ_CONF2_EQ_EDGE) != 0 ? state->configured_count : 0;

	dsp_int_set_flags(state->interrupt, EQUALIZER_INTERRUPT_GROUP, TEAK_INT_FINTA0_EQ);
}

static void equalizer_advance_timestamp(equalizer_state_t *state) {
	size_t count = state->configured_count == 0 ? 1 : state->configured_count;
	size_t timestamp = state->processed_count;
	uint8_t symbol = equalizer_step(state, timestamp);

	if ((state->config2 & TEAK_EQ_CONF2_EQ_EDGE) != 0) {
		state->decisions[timestamp] = symbol;
	} else {
		state->decisions[timestamp] = (int16_t) state->received[timestamp % EQUALIZER_RECEIVED_VALUES] >= 0;
		equalizer_store_gmsk_outputs(state, timestamp);
	}

	state->processed_count++;
	state->completed_count = state->processed_count;

	if (state->processed_count >= count)
		equalizer_complete(state);
}

void equalizer_advance(dsp_device_t *device, size_t cycles) {
	equalizer_state_t *state = device->state;

	if (!state->active)
		return;

	state->elapsed_cycles += cycles;

	while (state->active && state->elapsed_cycles >= EQUALIZER_TIMESTAMP_CYCLES) {
		state->elapsed_cycles -= EQUALIZER_TIMESTAMP_CYCLES;
		if (state->starting) {
			state->starting = false;
			state->config2 &= ~TEAK_EQ_CONF2_EQ_ON;
		} else {
			equalizer_advance_timestamp(state);
		}
	}
}

bool equalizer_is_active(const dsp_device_t *device) {
	const equalizer_state_t *state = device->state;
	return state->active;
}

dsp_device_t *equalizer_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt) {
	equalizer_state_t *state = g_new0(equalizer_state_t, 1);
	state->interrupt = interrupt;
	equalizer_reset_state(state);
	return dsp_device_create(config, &equalizer_ops, state);
}
