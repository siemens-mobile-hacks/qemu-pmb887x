#define PMB887X_TRACE_ID		DSP_CHANNEL_DECODER
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-channel-decoder"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"
#include "qemu/bitops.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define CHANNEL_DECODER_CONF2_MODE_MASK	(TEAK_CHDEC_CONF2_HW_ENA_DEC | TEAK_CHDEC_CONF2_DEC_FLAG_RD | \
	TEAK_CHDEC_CONF2_PC_DEC_0 | TEAK_CHDEC_CONF2_PC_DEC_1 | TEAK_CHDEC_CONF2_OFLOW_PROT | \
	TEAK_CHDEC_CONF2_DEC_64)
#define CHANNEL_DECODER_CONF2_START_MASK	(TEAK_CHDEC_CONF2_HW_ENA_DEC | TEAK_CHDEC_CONF2_DEC_ON)
#define CHANNEL_DECODER_REFERENCE_BASE	TEAK_CHDEC_REF_BR_BFLY0
#define CHANNEL_DECODER_REFERENCE_COUNT	8
#define CHANNEL_DECODER_REFERENCE_END	(CHANNEL_DECODER_REFERENCE_BASE + CHANNEL_DECODER_REFERENCE_COUNT)
#define CHANNEL_DECODER_METRIC_COUNT	64
#define CHANNEL_DECODER_INPUT_WORDS	512
#define CHANNEL_DECODER_TRACE_WORDS	1024
#define CHANNEL_DECODER_TIMESTAMP_CYCLES 8U
#define CHANNEL_DECODER_INTERRUPT_GROUP	0

typedef enum channel_decoder_external_target_t channel_decoder_external_target_t;

enum channel_decoder_external_target_t {
	CHANNEL_DECODER_EXTERNAL_NONE,
	CHANNEL_DECODER_EXTERNAL_SIN01,
	CHANNEL_DECODER_EXTERNAL_SIN2,
	CHANNEL_DECODER_EXTERNAL_TRACE,
	CHANNEL_DECODER_EXTERNAL_RAMW1,
	CHANNEL_DECODER_EXTERNAL_RAMW2,
};

typedef struct chdec_state_t chdec_state_t;

struct chdec_state_t {
	dsp_device_t *interrupt;
	uint16_t config2;
	uint16_t configured_count;
	uint16_t completed_count;
	uint16_t references[CHANNEL_DECODER_REFERENCE_COUNT];
	uint16_t metrics[2][CHANNEL_DECODER_METRIC_COUNT];
	uint16_t sin01[CHANNEL_DECODER_INPUT_WORDS];
	uint16_t sin2[CHANNEL_DECODER_INPUT_WORDS];
	uint16_t trace[CHANNEL_DECODER_TRACE_WORDS];
	channel_decoder_external_target_t external_target;
	size_t external_pointer;
	size_t elapsed_cycles;
	uint8_t overflow_delay;
	bool active;
};

static void chdec_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void chdec_reset_state(chdec_state_t *state) {
	dsp_device_t *interrupt = state->interrupt;

	memset(state, 0, sizeof(*state));
	state->interrupt = interrupt;
}

static void chdec_reset(dsp_device_t *device) {
	chdec_state_t *state = device->state;
	chdec_reset_state(state);
}

static void chdec_reset_operation(chdec_state_t *state) {
	state->config2 &= CHANNEL_DECODER_CONF2_MODE_MASK;
	state->completed_count = 0;
	state->elapsed_cycles = 0;
	state->overflow_delay = 0;
	state->active = false;
}

static void chdec_select_external(chdec_state_t *state, uint16_t value) {
	if ((value & TEAK_CHDEC_CONF1_RES_DM_BASE) != 0) {
		if ((value & TEAK_CHDEC_CONF1_RES_RW1_RW2) != 0) {
			state->external_target = CHANNEL_DECODER_EXTERNAL_RAMW2;
		} else {
			state->external_target = CHANNEL_DECODER_EXTERNAL_RAMW1;
		}
	} else if ((value & TEAK_CHDEC_CONF1_RES_TR_BASE) != 0) {
		state->external_target = CHANNEL_DECODER_EXTERNAL_TRACE;
	} else if ((value & TEAK_CHDEC_CONF1_RES_SIN2_BASE) != 0) {
		state->external_target = CHANNEL_DECODER_EXTERNAL_SIN2;
	} else if ((value & TEAK_CHDEC_CONF1_RES_SIN01_BASE) != 0) {
		state->external_target = CHANNEL_DECODER_EXTERNAL_SIN01;
	} else {
		state->external_target = CHANNEL_DECODER_EXTERNAL_NONE;
	}

	state->external_pointer = 0;
}

static void chdec_start(chdec_state_t *state) {
	state->completed_count = 0;
	state->elapsed_cycles = 0;
	state->overflow_delay = 0;
	state->active = true;
}

static bool chdec_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	chdec_state_t *state = device->state;
	bool reference_register = offset >= CHANNEL_DECODER_REFERENCE_BASE && offset < CHANNEL_DECODER_REFERENCE_END;

	switch (offset) {
		case TEAK_CHDEC_CONF2:
			*value = state->config2;
			break;

		case TEAK_CHDEC_STATUS:
			*value = state->active ? TEAK_CHDEC_STATUS_DEC_BUSY : 0;
			break;

		case TEAK_CHDEC_CONF_CNT:
			*value = state->configured_count;
			break;

		case TEAK_CHDEC_STAT_CNT:
			*value = state->completed_count;
			break;

		default:
			*value = reference_register ? state->references[offset - CHANNEL_DECODER_REFERENCE_BASE] : 0;
			break;
	}

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool chdec_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	chdec_state_t *state = device->state;
	bool reference_register = offset >= CHANNEL_DECODER_REFERENCE_BASE && offset < CHANNEL_DECODER_REFERENCE_END;

	switch (offset) {
		case TEAK_CHDEC_CONF1:
			chdec_select_external(state, value);
			break;

		case TEAK_CHDEC_CONF2:
			if ((value & TEAK_CHDEC_CONF2_RES_ALL) != 0) {
				chdec_reset_state(state);
			} else {
				bool start = (value & CHANNEL_DECODER_CONF2_START_MASK) == CHANNEL_DECODER_CONF2_START_MASK;

				state->config2 = value & ~TEAK_CHDEC_CONF2_RES_DEC;

				if ((value & TEAK_CHDEC_CONF2_RES_DEC) != 0)
					chdec_reset_operation(state);
				if (start)
					chdec_start(state);
			}
			break;

		case TEAK_CHDEC_CONF_CNT:
			state->configured_count = value & UINT8_MAX;
			break;

		default:
			if (reference_register)
				state->references[offset - CHANNEL_DECODER_REFERENCE_BASE] = value;
			break;
	}

	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t chdec_ops = {
	.destroy = chdec_destroy,
	.reset = chdec_reset,
	.read = chdec_read,
	.write = chdec_write,
};

static uint16_t *chdec_external_memory(chdec_state_t *state, size_t *word_count) {
	switch (state->external_target) {
		case CHANNEL_DECODER_EXTERNAL_SIN01:
			*word_count = ARRAY_SIZE(state->sin01);
			return state->sin01;

		case CHANNEL_DECODER_EXTERNAL_SIN2:
			*word_count = ARRAY_SIZE(state->sin2);
			return state->sin2;

		case CHANNEL_DECODER_EXTERNAL_TRACE:
			*word_count = ARRAY_SIZE(state->trace);
			return state->trace;

		case CHANNEL_DECODER_EXTERNAL_RAMW1:
			*word_count = ARRAY_SIZE(state->metrics[0]);
			return state->metrics[0];

		case CHANNEL_DECODER_EXTERNAL_RAMW2:
			*word_count = ARRAY_SIZE(state->metrics[1]);
			return state->metrics[1];

		case CHANNEL_DECODER_EXTERNAL_NONE:
			*word_count = 0;
			return NULL;
	}
	g_assert_not_reached();
}

static bool chdec_external_byte_mode(const chdec_state_t *state) {
	bool sin01 = state->external_target == CHANNEL_DECODER_EXTERNAL_SIN01;
	bool sin2 = state->external_target == CHANNEL_DECODER_EXTERNAL_SIN2;
	return (sin01 || sin2) && (state->config2 & TEAK_CHDEC_CONF2_PC_DEC_1) != 0;
}

uint16_t chdec_external_read(dsp_device_t *device) {
	chdec_state_t *state = device->state;
	size_t word_count;
	uint16_t *memory = chdec_external_memory(state, &word_count);
	uint16_t value;

	if (memory == NULL || state->external_pointer >= word_count)
		return 0;

	value = memory[state->external_pointer++];
	if (chdec_external_byte_mode(state))
		value = (uint16_t) (int16_t) (int8_t) value;
	return value;
}

void chdec_external_write(dsp_device_t *device, uint16_t value) {
	chdec_state_t *state = device->state;
	size_t word_count;
	uint16_t *memory = chdec_external_memory(state, &word_count);

	if (memory == NULL || state->external_pointer >= word_count)
		return;

	if (chdec_external_byte_mode(state))
		value &= UINT8_MAX;

	memory[state->external_pointer++] = value;
}

static int16_t chdec_branch_metric(uint16_t reference, int8_t sin0, int8_t sin1, int8_t sin2) {
	int16_t metric = (reference & BIT(3)) != 0 ? -sin0 : sin0;
	metric += (reference & BIT(2)) != 0 ? -sin1 : sin1;
	metric += (reference & BIT(1)) != 0 ? -sin2 : sin2;
	return metric;
}

static uint64_t chdec_step(chdec_state_t *state, size_t metric_count, int8_t sin0, int8_t sin1, int8_t sin2) {
	size_t source_bank = state->completed_count & 1;
	size_t destination_bank = source_bank ^ 1;
	uint16_t *source = state->metrics[source_bank];
	uint16_t *destination = state->metrics[destination_bank];
	uint64_t decisions = 0;

	for (size_t butterfly = 0; butterfly < metric_count / 2; butterfly++) {
		uint16_t reference = state->references[butterfly / 4] >> (butterfly % 4 * 4) & 0x0F;
		int16_t branch_metric = chdec_branch_metric(reference, sin0, sin1, sin2);
		int16_t even_metric = source[butterfly * 2];
		int16_t odd_metric = source[butterfly * 2 + 1];
		int16_t lower_first = even_metric - branch_metric;
		int16_t lower_second = odd_metric + branch_metric;
		int16_t upper_first = even_metric + branch_metric;
		int16_t upper_second = odd_metric - branch_metric;

		destination[butterfly] = lower_first >= lower_second ? lower_first : lower_second;
		destination[butterfly + metric_count / 2] = upper_first >= upper_second ? upper_first : upper_second;

		if (lower_first >= lower_second)
			decisions |= UINT64_C(1) << butterfly;
		if (upper_first >= upper_second)
			decisions |= UINT64_C(1) << (butterfly + metric_count / 2);
	}
	return decisions;
}

static void chdec_store_trace(chdec_state_t *state, size_t metric_count, uint64_t decisions) {
	size_t offset;

	if (metric_count == 16) {
		uint16_t decision_word = decisions;
		uint16_t packed = (uint16_t) ~(decision_word << 8 | decision_word >> 8);

		offset = state->completed_count * 2;
		state->trace[offset] = packed & UINT8_MAX;
		state->trace[offset + 1] = packed >> 8;
		return;
	}
	offset = state->completed_count * 4;
	for (size_t group = 0; group < 4; group++)
		state->trace[offset + group] = (uint16_t) ~(decisions >> ((3 - group) * 16));
}

static bool chdec_overflow(const uint16_t *metrics, size_t metric_count) {
	for (size_t i = 0; i < metric_count; i++) {
		if ((metrics[i] & 0x4000U) != 0)
			return true;
	}
	return false;
}

static void chdec_apply_overflow_protection(chdec_state_t *state, size_t metric_count) {
	uint16_t *destination = state->metrics[(state->completed_count & 1) ^ 1];

	if ((state->config2 & TEAK_CHDEC_CONF2_OFLOW_PROT) == 0)
		return;

	if (state->overflow_delay != 0) {
		state->overflow_delay--;
		if (state->overflow_delay == 0) {
			for (size_t i = 0; i < metric_count; i++)
				destination[i] -= 0x1000;
		}
		return;
	}

	if (chdec_overflow(destination, metric_count))
		state->overflow_delay = 2;
}

static void chdec_complete(chdec_state_t *state) {
	state->active = false;
	state->config2 &= ~TEAK_CHDEC_CONF2_DEC_ON;

	dsp_int_set_flags(state->interrupt, CHANNEL_DECODER_INTERRUPT_GROUP, TEAK_INT_FINTA0_CHADEC);
}

static void chdec_advance_timestamp(chdec_state_t *state) {
	size_t metric_count = (state->config2 & TEAK_CHDEC_CONF2_DEC_64) != 0 ? 64 : 16;
	size_t input_offset = state->completed_count * 2;
	int8_t sin0 = state->sin01[input_offset];
	int8_t sin1 = state->sin01[input_offset + 1];
	int8_t sin2 = state->sin2[input_offset];

	uint64_t decisions = chdec_step(state, metric_count, sin0, sin1, sin2);

	chdec_store_trace(state, metric_count, decisions);
	chdec_apply_overflow_protection(state, metric_count);

	state->completed_count++;
	if (state->completed_count >= state->configured_count)
		chdec_complete(state);
}

void chdec_advance(dsp_device_t *device, size_t cycles) {
	chdec_state_t *state = device->state;

	if (!state->active)
		return;

	state->elapsed_cycles += cycles;

	while (state->active && state->elapsed_cycles >= CHANNEL_DECODER_TIMESTAMP_CYCLES) {
		state->elapsed_cycles -= CHANNEL_DECODER_TIMESTAMP_CYCLES;
		chdec_advance_timestamp(state);
	}
}

bool chdec_is_active(const dsp_device_t *device) {
	const chdec_state_t *state = device->state;
	return state->active;
}

dsp_device_t *chdec_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt) {
	chdec_state_t *state = g_new0(chdec_state_t, 1);
	state->interrupt = interrupt;
	return dsp_device_create(config, &chdec_ops, state);
}
