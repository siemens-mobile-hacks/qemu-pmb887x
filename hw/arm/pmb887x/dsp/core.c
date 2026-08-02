#include "qemu/osdep.h"
#include "qemu/atomic.h"

#include "hw/arm/pmb887x/dsp/core.h"
#include "hw/arm/pmb887x/dsp/tcg.h"

#define TEAK_OPCODE_MOV_IMM_REGISTER_MASK	0xFEE0U
#define TEAK_OPCODE_MOV_IMM_B_ACCUMULATOR_MASK	0xFEE0U
#define TEAK_OPCODE_MOV_SHORT_ACCUMULATOR_HIGH_MASK	0xEF00U
#define TEAK_OPCODE_MOV_SHORT_RN_MASK	0xE300U
#define TEAK_OPCODE_MOV_SHORT_REGISTER_MASK	0xFF00U
#define TEAK_OPCODE_MOV_REGISTER_REGISTER_MASK	0xFC00U
#define TEAK_OPCODE_MOV_REGISTER_B_ACCUMULATOR_MASK	0xFFC0U
#define TEAK_OPCODE_MOV_ACCUMULATOR_ACCUMULATOR_MASK	0xF398U
#define TEAK_OPCODE_MOV_ACCUMULATOR_LOW_ALIAS_MASK	0xF3D8U
#define TEAK_OPCODE_MOV_ACCUMULATOR_LOW_X1_MASK	0xFFFCU
#define TEAK_OPCODE_MOV_ACCUMULATOR_LOW_Y1_MASK	0xFFFCU
#define TEAK_OPCODE_MOV_SPECIAL_ACCUMULATOR_MASK	0xFE9BU
#define TEAK_OPCODE_MOV_X1_ACCUMULATOR_MASK	0xFFCFU
#define TEAK_OPCODE_MOV_Y1_ACCUMULATOR_MASK	0xF3FFU
#define TEAK_OPCODE_MOV_MIXP_REGISTER_MASK	0xFFE0U
#define TEAK_OPCODE_MOV_REGISTER_MIXP_MASK	0xFFE0U
#define TEAK_OPCODE_MOV_REGISTER_ICR_MASK	0xFFC0U
#define TEAK_OPCODE_MOV_DATA_IMM8_REGISTER_MASK	0xE300U
#define TEAK_OPCODE_MOV_DATA_IMM8_ACCUMULATOR_MASK	0xE700U
#define TEAK_OPCODE_MOV_DATA_IMM8_ACCUMULATOR_HIGH_EU_MASK	0xEF00U
#define TEAK_OPCODE_MOV_DATA_IMM8_SPECIAL_MASK	0xFF00U
#define TEAK_OPCODE_MOV_REGISTER_DATA_IMM8_MASK	0xF100U
#define TEAK_OPCODE_MOV_DATA_R7_OFFSET7_ACCUMULATOR_MASK	0xFE80U
#define TEAK_OPCODE_MOV_DATA_R7_OFFSET16_ACCUMULATOR_MASK	0xFEFCU
#define TEAK_OPCODE_MOV_DATA_RN_STEP_REGISTER_MASK	0xFC00U
#define TEAK_OPCODE_MOV_REGISTER_DATA_RN_STEP_MASK	0xFC00U
#define TEAK_OPCODE_MOV_DATA_RN_STEP_B_ACCUMULATOR_MASK	0xFEC0U
#define TEAK_OPCODE_MOV_STACK_REGISTER_MASK	0xFFE0U
#define TEAK_OPCODE_MOV_IMM_ICR_MASK	0xFFC0U
#define TEAK_OPCODE_MOV_IMM8_ACCUMULATOR_LOW_MASK	0xEF00U
#define TEAK_OPCODE_MOV_DATA_IMM16_ACCUMULATOR_MASK	0xFEFCU
#define TEAK_OPCODE_MOV_ACCUMULATOR_LOW_DATA_IMM16_MASK	0xFEFCU
#define TEAK_OPCODE_MOVP_ACCUMULATOR_LOW_REGISTER_MASK	0xFFC0U
#define TEAK_OPCODE_MOVP_RN_RN_MASK	0xFE00U
#define TEAK_OPCODE_MOVD_RN_RN_MASK	0xFF80U
#define TEAK_OPCODE_MOVS_REGISTER_MASK	0xFF80U
#define TEAK_OPCODE_MOVS_RN_STEP_MASK	0xFF80U
#define TEAK_OPCODE_MOVS_DATA_IMM8_MASK	0xE700U
#define TEAK_OPCODE_MOVS_R6_MASK	0xFFFEU
#define TEAK_OPCODE_MOVSI_REGISTER_MASK	0xF180U
#define TEAK_OPCODE_MOVR_REGISTER_MASK	0xFEE0U
#define TEAK_OPCODE_MOVR_RN_STEP_MASK	0xFEE0U
#define TEAK_OPCODE_MOVR_RN_HIGH_MASK	0xFCE4U
#define TEAK_OPCODE_MOVR_B_ACCUMULATOR_MASK	0xFFFCU
#define TEAK_OPCODE_MOVR_R6_MASK	0xFFF7U
#define TEAK_OPCODE_ALB_DATA_IMM8_MASK	0xF100U
#define TEAK_OPCODE_ALB_RN_STEP_MASK	0xF1E0U
#define TEAK_OPCODE_ALB_REGISTER_MASK	0xF1E0U
#define TEAK_OPCODE_ALU_IMM8_ACCUMULATOR_MASK	0xFE00U
#define TEAK_OPCODE_ALU_IMM16_ACCUMULATOR_MASK	0xFEE0U
#define TEAK_OPCODE_ALU_IMM16_ACCUMULATOR_FAMILY_MASK	0xF0E0U
#define TEAK_OPCODE_ALU_DATA_IMM8_ACCUMULATOR_MASK	0xFE00U
#define TEAK_OPCODE_ALU_DATA_IMM16_ACCUMULATOR_MASK	0xFEFFU
#define TEAK_OPCODE_ALU_R7_OFFSET7_ACCUMULATOR_MASK	0xFE80U
#define TEAK_OPCODE_ALU_R7_OFFSET16_ACCUMULATOR_MASK	0xFEFFU
#define TEAK_OPCODE_ALU_RN_STEP_ACCUMULATOR_MASK	0xFEE0U
#define TEAK_OPCODE_ALU_REGISTER_ACCUMULATOR_MASK	0xFEE0U
#define TEAK_OPCODE_TEST_ACCUMULATOR_DATA_IMM8_MASK	0xFE00U
#define TEAK_OPCODE_MODIFY_ACCUMULATOR_MASK	0xEFF0U
#define TEAK_OPCODE_MODB_CLEAR_ACCUMULATOR_MASK	0xEF70U
#define TEAK_OPCODE_LIMIT_ACCUMULATOR_MASK	0xFFC0U
#define TEAK_OPCODE_EXPONENT_B_SV_MASK	0xFEE0U
#define TEAK_OPCODE_EXPONENT_B_ACCUMULATOR_MASK	0xFEE0U
#define TEAK_OPCODE_EXPONENT_RN_SV_MASK	0xFEC0U
#define TEAK_OPCODE_EXPONENT_RN_ACCUMULATOR_MASK	0xFEC0U
#define TEAK_OPCODE_EXPONENT_REGISTER_SV_MASK	0xFEE0U
#define TEAK_OPCODE_EXPONENT_REGISTER_ACCUMULATOR_MASK	0xFEE0U
#define TEAK_OPCODE_EXPONENT_R6_ACCUMULATOR_MASK	0xFFEFU
#define TEAK_OPCODE_NORMALIZE_MASK	0xFEC0U
#define TEAK_OPCODE_SWAP_ACCUMULATORS_MASK	0xFFC0U
#define TEAK_OPCODE_BANK_EXCHANGE_MASK	0xFF80U
#define TEAK_OPCODE_DIVISION_STEP_MASK	0xFE00U
#define TEAK_OPCODE_MAXD_MASK	0xFCE0U
#define TEAK_OPCODE_MAX_MASK	0xFCE0U
#define TEAK_OPCODE_MIN_MASK	0xF8E0U
#define TEAK_OPCODE_MINMAX_DATA_MIN_MASK	0xFFF4U
#define TEAK_OPCODE_BLOCK_REPEAT_IMMEDIATE_MASK	0xFF00U
#define TEAK_OPCODE_BLOCK_REPEAT_REGISTER_MASK	0xFF00U
#define TEAK_OPCODE_TSTB_IMM8_MASK	0xF000U
#define TEAK_OPCODE_TSTB_RN_STEP_MASK	0xF0E0U
#define TEAK_OPCODE_TSTB_REGISTER_MASK	0xF0E0U
#define TEAK_OPCODE_LOAD_PAGE_MASK	0xFF00U
#define TEAK_OPCODE_LOAD_STEP_MASK	0xFF80U
#define TEAK_OPCODE_LOAD_MOD_MASK	0xFE00U
#define TEAK_OPCODE_LOAD_PRODUCT_SHIFT_MASK	0xFF80U
#define TEAK_OPCODE_SHIFT_CONDITIONAL_MASK	0xF390U
#define TEAK_OPCODE_SHIFT_IMMEDIATE_MASK	0xF240U
#define TEAK_OPCODE_MULTIPLY_IMMEDIATE_MASK	0xFF00U
#define TEAK_OPCODE_MULTIPLY_DATA_IMM8_MASK	0xF700U
#define TEAK_OPCODE_MULTIPLY_DUAL_RN_MASK	0xF780U
#define TEAK_OPCODE_MULTIPLY_MASK	0xF7E0U
#define TEAK_OPCODE_MULTIPLY_R6_MASK	0xFFF0U
#define TEAK_OPCODE_MSU_DUAL_RN_MASK	0xFE80U
#define TEAK_OPCODE_MSU_RN_IMMEDIATE_MASK	0xFEC0U
#define TEAK_OPCODE_MODIFY_RN_MASK	0xFF80U
#define TEAK_OPCODE_BRANCH_ABSOLUTE_MASK	0xFFC0U
#define TEAK_OPCODE_BRANCH_RELATIVE_MASK	0xF800U
#define TEAK_OPCODE_CALL_ABSOLUTE_MASK	0xFFC0U
#define TEAK_OPCODE_CALL_ACCUMULATOR_MASK	0xFE90U
#define TEAK_OPCODE_CALL_RELATIVE_MASK	0xF800U
#define TEAK_OPCODE_PUSH_IMMEDIATE_MASK	0xFFC0U
#define TEAK_OPCODE_PUSH_REGISTER_MASK	0xFFE0U
#define TEAK_OPCODE_POP_REGISTER_MASK	0xFFE0U
#define TEAK_OPCODE_REPEAT_IMMEDIATE_MASK	0xFF00U
#define TEAK_OPCODE_REPEAT_REGISTER_MASK	0xFF00U
#define TEAK_OPCODE_RETURN_MASK	0xFFC0U
#define TEAK_OPCODE_RETURN_INTERRUPT_MASK	0xFFC0U
#define TEAK_OPCODE_RETURN_STACK_MASK	0xFF00U
#define TEAK_OPCODE_BREAK_MASK	0xFFC0U
#define TEAK_OPCODE_DELAYED_RETURN_INTERRUPT_MASK	0xFFD0U
#define TEAK_OPCODE_CONTEXT_STORE_MASK	0xFFD0U
#define TEAK_OPCODE_NOP_MASK	0xFFE0U
#define TEAK_OPCODE_TRAP_MASK	0xFFE0U
#define TEAK_OPCODE_INTERRUPT_ENABLE_MASK	0xFFC0U
#define TEAK_OPCODE_INTERRUPT_DISABLE_MASK	0xFFC0U

static const teak_step_t DSP_MINMAX_ALIAS_STEPS[] = {
	TEAK_STEP_INCREASE,
	TEAK_STEP_DECREASE,
	TEAK_STEP_ZERO,
	TEAK_STEP_PLUS_STEP,
};

static uint64_t teak_next_cache_id;

void teak_tcg_init(teak_tcg_core_t *core, const teak_memory_t *memory) {
	g_assert(memory->program.read != NULL);
	g_assert(memory->program.write != NULL);
	g_assert(memory->data.read != NULL);
	g_assert(memory->data.write != NULL);
	g_assert(memory->cycle_sensitive_size == 0 || memory->advance_cycles != NULL);

	memset(core, 0, sizeof(*core));
	core->memory = *memory;
	core->cache_id = qatomic_fetch_inc(&teak_next_cache_id) + 1;
	core->state.sata = 1;
	core->state.cpc = 1;
}

void teak_tcg_reset(teak_tcg_core_t *core, uint32_t pc) {
	teak_memory_t memory = core->memory;
	uint64_t cache_id = core->cache_id;

	memset(core, 0, sizeof(*core));
	core->memory = memory;
	core->cache_id = cache_id;
	core->state.pc = pc & TEAK_PROGRAM_ADDRESS_MASK;
	core->state.sata = 1;
	core->state.cpc = 1;
}

uint16_t teak_program_read(teak_tcg_core_t *core, uint32_t address) {
	return core->memory.program.read(core->memory.program.opaque, address);
}

void teak_program_write(teak_tcg_core_t *core, uint32_t address, uint16_t value) {
	core->memory.program.write(core->memory.program.opaque, address, value);
}

uint16_t teak_data_read(teak_tcg_core_t *core, uint32_t address) {
	return core->memory.data.read(core->memory.data.opaque, address);
}

void teak_data_write(teak_tcg_core_t *core, uint32_t address, uint16_t value) {
	core->memory.data.write(core->memory.data.opaque, address, value);
}

uint16_t teak_modulo_address(const teak_state_t *state, uint8_t register_index, uint16_t address, int16_t step) {
	uint16_t modulo = register_index < 4 ? state->modi : state->modj;
	uint16_t combined;
	uint16_t mask = 0;
	uint16_t low;
	uint16_t updated;

	g_assert(register_index < 6);

	if (step == 0 || modulo == 0)
		return address;

	if (step < 0) {
		combined = modulo | (uint16_t) ~step;
	} else {
		combined = modulo | (uint16_t) step;
	}
	while (combined != 0) {
		mask = mask << 1 | 1U;
		combined >>= 1;
	}

	low = address & mask;
	updated = low + step;
	updated &= mask;
	if (step >= 0 && low == modulo)
		updated = 0;
	if (step < 0 && low == 0)
		updated = modulo;

	address &= ~mask;
	address |= updated;
	return address;
}

static bool teak_is_minmax_word(uint16_t word) {
	bool maxd = (word & TEAK_OPCODE_MAXD_MASK) == 0x8060U;
	bool maximum = (word & TEAK_OPCODE_MAX_MASK) == 0x8460U;
	bool minimum = (word & TEAK_OPCODE_MIN_MASK) == 0x8860U;
	bool standard_minimum = (word & 7U) == 0;
	bool minimum_alias = (word & 0x0318U) == 0;
	return maxd || maximum || (minimum && (standard_minimum || minimum_alias));
}

static teak_opcode_t teak_decode_word(uint16_t word) {
	bool mov_accumulator_low_alias = (word & TEAK_OPCODE_MOV_ACCUMULATOR_LOW_ALIAS_MASK) == 0xD298U ||
		(word & TEAK_OPCODE_MOV_ACCUMULATOR_LOW_ALIAS_MASK) == 0xD2D8U;
	bool exponent_b = (word & TEAK_OPCODE_EXPONENT_B_SV_MASK) == 0x9460U ||
		(word & TEAK_OPCODE_EXPONENT_B_ACCUMULATOR_MASK) == 0x9060U;
	bool exponent_rn = (word & TEAK_OPCODE_EXPONENT_RN_SV_MASK) == 0x9C40U ||
		(word & TEAK_OPCODE_EXPONENT_RN_ACCUMULATOR_MASK) == 0x9840U;
	bool exponent_register = (word & TEAK_OPCODE_EXPONENT_REGISTER_SV_MASK) == 0x9440U ||
		(word & TEAK_OPCODE_EXPONENT_REGISTER_ACCUMULATOR_MASK) == 0x9040U;
	bool swap_accumulators = (word & TEAK_OPCODE_SWAP_ACCUMULATORS_MASK) == 0x4980U &&
		(word & 0xFU) < TEAK_SWAP_OPERATION_COUNT;
	bool data_minmax = (word & TEAK_OPCODE_MINMAX_DATA_MIN_MASK) == 0x47A0U ||
		(word & TEAK_OPCODE_MINMAX_DATA_MIN_MASK) == 0x47A4U;
	bool short_register = (word & TEAK_OPCODE_MOV_SHORT_ACCUMULATOR_HIGH_MASK) == 0x2500U ||
		(word & TEAK_OPCODE_MOV_SHORT_RN_MASK) == 0x2300U;
	bool special_to_accumulator = (word & TEAK_OPCODE_MOV_X1_ACCUMULATOR_MASK) == 0x49C1U ||
		(word & TEAK_OPCODE_MOV_Y1_ACCUMULATOR_MASK) == 0xD299U;
	bool accumulator_to_special = (word & TEAK_OPCODE_MOV_ACCUMULATOR_LOW_X1_MASK) == 0xD394U ||
		(word & TEAK_OPCODE_MOV_ACCUMULATOR_LOW_Y1_MASK) == 0xD384U;
	bool special_alias = (word & TEAK_OPCODE_MOV_SPECIAL_ACCUMULATOR_MASK) == 0xD490U ||
		(word & TEAK_OPCODE_MOV_SPECIAL_ACCUMULATOR_MASK) == 0xD491U ||
		(word & TEAK_OPCODE_MOV_SPECIAL_ACCUMULATOR_MASK) == 0xD492U ||
		(word & TEAK_OPCODE_MOV_SPECIAL_ACCUMULATOR_MASK) == 0xD493U;
	bool data_to_register = (word & TEAK_OPCODE_MOV_DATA_IMM8_REGISTER_MASK) == 0x6000U ||
		(word & TEAK_OPCODE_MOV_DATA_IMM8_REGISTER_MASK) == 0x6200U ||
		(word & TEAK_OPCODE_MOV_DATA_IMM8_SPECIAL_MASK) == 0x6D00U;
	bool register_to_data = (word & TEAK_OPCODE_MOV_REGISTER_DATA_IMM8_MASK) == 0x2000U ||
		(word & TEAK_OPCODE_MOV_REGISTER_DATA_IMM8_MASK) == 0x3000U ||
		(word & TEAK_OPCODE_MOV_DATA_IMM8_SPECIAL_MASK) == 0x7D00U;

	if ((word & TEAK_OPCODE_CONTEXT_STORE_MASK) == 0xD380U)
		return TEAK_OP_CONTEXT_STORE;
	if ((word & TEAK_OPCODE_PUSH_IMMEDIATE_MASK) == 0x5F40U)
		return TEAK_OP_PUSH_IMMEDIATE;
	if ((word & TEAK_OPCODE_DELAYED_RETURN_INTERRUPT_MASK) == 0xD7C0U)
		return TEAK_OP_DELAYED_RETURN_INTERRUPT;
	if ((word & TEAK_OPCODE_LIMIT_ACCUMULATOR_MASK) == 0x49C0U)
		return TEAK_OP_LIMIT_ACCUMULATOR;
	if (mov_accumulator_low_alias)
		return TEAK_OP_MOV_ACCUMULATOR_LOW_SPECIAL;
	if ((word & TEAK_OPCODE_MOV_ACCUMULATOR_LOW_Y1_MASK) == 0xD384U)
		return TEAK_OP_MOV_ACCUMULATOR_LOW_SPECIAL;
	if (word == 0xD7C1U || (word & TEAK_OPCODE_EXPONENT_R6_ACCUMULATOR_MASK) == 0xD382U)
		return TEAK_OP_EXPONENT;
	if ((word & TEAK_OPCODE_MOVS_R6_MASK) == 0x5F42U)
		return TEAK_OP_MOVS_R6;
	if ((word & TEAK_OPCODE_MOVR_RN_HIGH_MASK) == 0x8864U)
		return TEAK_OP_MOVR_RN_HIGH;
	if (exponent_b)
		return TEAK_OP_EXPONENT;
	if (exponent_rn)
		return TEAK_OP_EXPONENT;
	if (exponent_register)
		return TEAK_OP_EXPONENT;
	if ((word & TEAK_OPCODE_NORMALIZE_MASK) == 0x94C0U)
		return TEAK_OP_NORMALIZE;
	if (swap_accumulators)
		return TEAK_OP_SWAP_ACCUMULATORS;
	if ((word & TEAK_OPCODE_BANK_EXCHANGE_MASK) == 0x4B80U)
		return TEAK_OP_BANK_EXCHANGE;
	if ((word & TEAK_OPCODE_DIVISION_STEP_MASK) == 0x0E00U)
		return TEAK_OP_DIVISION_STEP;
	if (teak_is_minmax_word(word))
		return TEAK_OP_MINIMUM_MAXIMUM;
	if (data_minmax)
		return TEAK_OP_MINIMUM_MAXIMUM;
	if ((word & TEAK_OPCODE_MOV_IMM_REGISTER_MASK) == 0x5E00U)
		return TEAK_OP_MOV_IMM_REGISTER;
	if ((word & TEAK_OPCODE_MOV_IMM_B_ACCUMULATOR_MASK) == 0x5E20U)
		return TEAK_OP_MOV_IMM_B_ACCUMULATOR;
	if (short_register)
		return TEAK_OP_MOV_SHORT_REGISTER;
	switch (word & TEAK_OPCODE_MOV_SHORT_REGISTER_MASK) {
		case 0x0500:
		case 0x2900:
		case 0x2D00:
		case 0x3900:
		case 0x3D00:
			return TEAK_OP_MOV_SHORT_REGISTER;
	}
	if ((word & TEAK_OPCODE_MOVD_RN_RN_MASK) == 0x5F80U)
		return TEAK_OP_MOVD_RN_RN;
	if ((word & TEAK_OPCODE_MOV_ACCUMULATOR_ACCUMULATOR_MASK) == 0xD290U)
		return TEAK_OP_MOV_ACCUMULATOR_ACCUMULATOR;
	if (special_to_accumulator)
		return TEAK_OP_MOV_SPECIAL_ACCUMULATOR;
	if (accumulator_to_special)
		return TEAK_OP_MOV_ACCUMULATOR_LOW_SPECIAL;
	if (special_alias)
		return TEAK_OP_MOV_SPECIAL_ACCUMULATOR;
	if ((word & TEAK_OPCODE_MOV_MIXP_REGISTER_MASK) == 0x47C0U)
		return TEAK_OP_MOV_MIXP_REGISTER;
	if ((word & TEAK_OPCODE_MOV_REGISTER_MIXP_MASK) == 0x5E80U)
		return TEAK_OP_MOV_REGISTER_MIXP;
	if ((word & TEAK_OPCODE_MOV_REGISTER_ICR_MASK) == 0x4FC0U)
		return TEAK_OP_MOV_REGISTER_ICR;
	if ((word & TEAK_OPCODE_MOV_REGISTER_B_ACCUMULATOR_MASK) == 0x5EC0U)
		return TEAK_OP_MOV_REGISTER_B_ACCUMULATOR;
	if ((word & TEAK_OPCODE_MOV_REGISTER_REGISTER_MASK) == 0x5800U)
		return TEAK_OP_MOV_REGISTER_REGISTER;
	if ((word & TEAK_OPCODE_MOVP_ACCUMULATOR_LOW_REGISTER_MASK) == 0x0040U)
		return TEAK_OP_MOVP_ACCUMULATOR_LOW_REGISTER;
	if ((word & TEAK_OPCODE_MOVP_RN_RN_MASK) == 0x0600U)
		return TEAK_OP_MOVP_RN_RN;
	if ((word & TEAK_OPCODE_MOVS_REGISTER_MASK) == 0x0100U)
		return TEAK_OP_MOVS_REGISTER;
	if ((word & TEAK_OPCODE_MOVS_RN_STEP_MASK) == 0x0180U)
		return TEAK_OP_MOVS_RN_STEP;
	if ((word & TEAK_OPCODE_MOVS_DATA_IMM8_MASK) == 0x6300U)
		return TEAK_OP_MOVS_DATA_IMM8;
	if ((word & TEAK_OPCODE_MOVSI_REGISTER_MASK) == 0x4080U)
		return TEAK_OP_MOVSI_REGISTER;
	if ((word & TEAK_OPCODE_MOVR_REGISTER_MASK) == 0x9CC0U)
		return TEAK_OP_MOVR_REGISTER;
	if ((word & TEAK_OPCODE_MOVR_RN_STEP_MASK) == 0x9CE0U)
		return TEAK_OP_MOVR_RN_STEP;
	if ((word & TEAK_OPCODE_MOVR_B_ACCUMULATOR_MASK) == 0x5DF4U)
		return TEAK_OP_MOVR_B_ACCUMULATOR;
	if ((word & TEAK_OPCODE_MOVR_R6_MASK) == 0x8961U)
		return TEAK_OP_MOVR_R6;
	if ((word & TEAK_OPCODE_MOV_DATA_IMM8_ACCUMULATOR_MASK) == 0x6100U)
		return TEAK_OP_MOV_DATA_IMM8_ACCUMULATOR;
	if ((word & TEAK_OPCODE_MOV_DATA_IMM8_ACCUMULATOR_HIGH_EU_MASK) == 0x6500U)
		return TEAK_OP_MOV_DATA_IMM8_ACCUMULATOR_HIGH_EU;
	if (data_to_register)
		return TEAK_OP_MOV_DATA_IMM8_REGISTER;
	if (register_to_data)
		return TEAK_OP_MOV_REGISTER_DATA_IMM8;
	if ((word & TEAK_OPCODE_MOV_DATA_R7_OFFSET7_ACCUMULATOR_MASK) == 0xD880U)
		return TEAK_OP_MOV_DATA_R7_OFFSET7_ACCUMULATOR;
	if ((word & TEAK_OPCODE_MOV_DATA_R7_OFFSET7_ACCUMULATOR_MASK) == 0xDC80U)
		return TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET7;
	if ((word & TEAK_OPCODE_MOV_DATA_R7_OFFSET16_ACCUMULATOR_MASK) == 0xD498U)
		return TEAK_OP_MOV_DATA_R7_OFFSET16_ACCUMULATOR;
	if ((word & TEAK_OPCODE_MOV_DATA_R7_OFFSET16_ACCUMULATOR_MASK) == 0xD49CU)
		return TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET16;
	if ((word & TEAK_OPCODE_MOV_DATA_RN_STEP_REGISTER_MASK) == 0x1C00U)
		return TEAK_OP_MOV_DATA_RN_STEP_REGISTER;
	if ((word & TEAK_OPCODE_MOV_REGISTER_DATA_RN_STEP_MASK) == 0x1800U)
		return TEAK_OP_MOV_REGISTER_DATA_RN_STEP;
	if ((word & TEAK_OPCODE_MOV_DATA_RN_STEP_B_ACCUMULATOR_MASK) == 0x98C0U)
		return TEAK_OP_MOV_DATA_RN_STEP_B_ACCUMULATOR;
	if ((word & TEAK_OPCODE_MOV_STACK_REGISTER_MASK) == 0x47E0U)
		return TEAK_OP_MOV_STACK_REGISTER;
	if ((word & TEAK_OPCODE_MOV_IMM_ICR_MASK) == 0x4F80U)
		return TEAK_OP_MOV_IMM_ICR;
	if ((word & TEAK_OPCODE_MOV_IMM8_ACCUMULATOR_LOW_MASK) == 0x2100U)
		return TEAK_OP_MOV_IMM8_ACCUMULATOR_LOW;
	if ((word & TEAK_OPCODE_MOV_DATA_IMM16_ACCUMULATOR_MASK) == 0xD4B8U)
		return TEAK_OP_MOV_DATA_IMM16_ACCUMULATOR;
	if ((word & TEAK_OPCODE_MOV_ACCUMULATOR_LOW_DATA_IMM16_MASK) == 0xD4BCU)
		return TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_IMM16;
	if ((word & TEAK_OPCODE_ALB_DATA_IMM8_MASK) == 0xE100U)
		return TEAK_OP_ALB_DATA_IMM8;
	if ((word & TEAK_OPCODE_ALB_RN_STEP_MASK) == 0x80E0U)
		return TEAK_OP_ALB_RN_STEP;
	if ((word & TEAK_OPCODE_ALB_REGISTER_MASK) == 0x81E0U)
		return TEAK_OP_ALB_REGISTER;
	switch (word & TEAK_OPCODE_ALU_IMM8_ACCUMULATOR_MASK) {
		case 0xC000:
		case 0xC200:
		case 0xC400:
		case 0xC600:
		case 0xCC00:
		case 0xCE00:
			return TEAK_OP_ALU_IMMEDIATE_ACCUMULATOR;
	}

	switch (word & TEAK_OPCODE_ALU_IMM16_ACCUMULATOR_MASK) {
		case 0x80C0:
		case 0x82C0:
		case 0x84C0:
		case 0x86C0:
		case 0x8CC0:
		case 0x8EC0:
			return TEAK_OP_ALU_IMMEDIATE_ACCUMULATOR;
	}

	switch (word & TEAK_OPCODE_ALU_DATA_IMM8_ACCUMULATOR_MASK) {
		case 0xA000:
		case 0xA200:
		case 0xA400:
		case 0xA600:
		case 0xAC00:
		case 0xAE00:
		case 0xB000:
		case 0xB200:
		case 0xB400:
		case 0xB600:
		case 0xB800:
		case 0xBA00:
		case 0xBC00:
		case 0xBE00:
			return TEAK_OP_ALU_DATA_IMM8_ACCUMULATOR;
	}

	switch (word & TEAK_OPCODE_ALU_DATA_IMM16_ACCUMULATOR_MASK) {
		case 0xD4F8:
		case 0xD4F9:
		case 0xD4FA:
		case 0xD4FB:
		case 0xD4FE:
		case 0xD4FF:
			return TEAK_OP_ALU_DATA_IMM16_ACCUMULATOR;
	}

	switch (word & TEAK_OPCODE_ALU_R7_OFFSET7_ACCUMULATOR_MASK) {
		case 0x4000:
		case 0x4200:
		case 0x4400:
		case 0x4600:
		case 0x4C00:
		case 0x4E00:
			return TEAK_OP_ALU_R7_OFFSET7_ACCUMULATOR;
	}

	switch (word & TEAK_OPCODE_ALU_R7_OFFSET16_ACCUMULATOR_MASK) {
		case 0xD4D8:
		case 0xD4D9:
		case 0xD4DA:
		case 0xD4DB:
		case 0xD4DE:
		case 0xD4DF:
			return TEAK_OP_ALU_R7_OFFSET16_ACCUMULATOR;
	}

	switch (word & TEAK_OPCODE_ALU_RN_STEP_ACCUMULATOR_MASK) {
		case 0x8080:
		case 0x8280:
		case 0x8480:
		case 0x8680:
		case 0x8880:
		case 0x8A80:
		case 0x8C80:
		case 0x8E80:
		case 0x9080:
		case 0x9280:
		case 0x9480:
		case 0x9680:
		case 0x9880:
		case 0x9A80:
		case 0x9C80:
		case 0x9E80:
			return TEAK_OP_ALU_RN_STEP_ACCUMULATOR;
	}

	switch (word & TEAK_OPCODE_ALU_REGISTER_ACCUMULATOR_MASK) {
		case 0x80A0:
		case 0x82A0:
		case 0x84A0:
		case 0x86A0:
		case 0x88A0:
		case 0x8AA0:
		case 0x8CA0:
		case 0x8EA0:
		case 0x90A0:
		case 0x92A0:
		case 0x94A0:
		case 0x96A0:
		case 0x98A0:
		case 0x9AA0:
		case 0x9CA0:
		case 0x9EA0:
			return TEAK_OP_ALU_REGISTER_ACCUMULATOR;
	}

	switch (word & TEAK_OPCODE_TEST_ACCUMULATOR_DATA_IMM8_MASK) {
		case 0xA800:
		case 0xAA00:
			return TEAK_OP_TEST_ACCUMULATOR_DATA_IMM8;
	}
	if ((word & TEAK_OPCODE_TSTB_IMM8_MASK) == 0xF000U)
		return TEAK_OP_TSTB_IMM8;
	if ((word & TEAK_OPCODE_TSTB_RN_STEP_MASK) == 0x9020U)
		return TEAK_OP_TSTB_RN_STEP;
	if ((word & TEAK_OPCODE_TSTB_REGISTER_MASK) == 0x9000U)
		return TEAK_OP_TSTB_REGISTER;
	if ((word & TEAK_OPCODE_LOAD_PAGE_MASK) == 0x0400U)
		return TEAK_OP_LOAD_PAGE;
	if ((word & TEAK_OPCODE_LOAD_STEP_MASK) == 0xDB80U)
		return TEAK_OP_LOAD_STEPI;
	if ((word & TEAK_OPCODE_LOAD_STEP_MASK) == 0xDF80U)
		return TEAK_OP_LOAD_STEPJ;
	if ((word & TEAK_OPCODE_LOAD_MOD_MASK) == 0x0200U)
		return TEAK_OP_LOAD_MODI;
	if ((word & TEAK_OPCODE_LOAD_MOD_MASK) == 0x0A00U)
		return TEAK_OP_LOAD_MODJ;
	if ((word & TEAK_OPCODE_LOAD_PRODUCT_SHIFT_MASK) == 0x4D80U)
		return TEAK_OP_LOAD_PRODUCT_SHIFT;
	if ((word & TEAK_OPCODE_SHIFT_CONDITIONAL_MASK) == 0xD280U)
		return TEAK_OP_SHIFT_CONDITIONAL;
	if ((word & TEAK_OPCODE_SHIFT_IMMEDIATE_MASK) == 0x9240U)
		return TEAK_OP_SHIFT_IMMEDIATE;
	if ((word & TEAK_OPCODE_MULTIPLY_IMMEDIATE_MASK) == 0x0800U)
		return TEAK_OP_MULTIPLY_IMMEDIATE;
	switch (word & TEAK_OPCODE_MULTIPLY_DATA_IMM8_MASK) {
		case 0xE000:
		case 0xE200:
		case 0xE400:
		case 0xE600:
			return TEAK_OP_MULTIPLY_DATA_IMM8;
	}
	if ((word & TEAK_OPCODE_MULTIPLY_R6_MASK) == 0x5EA0U) {
		uint8_t operation = word >> 1 & 7U;
		switch (operation) {
			case TEAK_MULTIPLY_MPY:
			case TEAK_MULTIPLY_MPYSU:
			case TEAK_MULTIPLY_MAC:
			case TEAK_MULTIPLY_MACUS:
			case TEAK_MULTIPLY_MAA:
			case TEAK_MULTIPLY_MACUU:
			case TEAK_MULTIPLY_MACSU:
			case TEAK_MULTIPLY_MAASU:
				return TEAK_OP_MULTIPLY_R6;
		}
	}
	if ((word & TEAK_OPCODE_MSU_DUAL_RN_MASK) == 0xD080U)
		return TEAK_OP_MULTIPLY_DUAL_RN;
	switch (word & TEAK_OPCODE_MULTIPLY_DUAL_RN_MASK) {
		case 0xD000:
		case 0xD100:
		case 0xD200:
		case 0xD300:
		case 0xD400:
		case 0xD500:
		case 0xD600:
		case 0xD700:
			return TEAK_OP_MULTIPLY_DUAL_RN;
	}
	if ((word & TEAK_OPCODE_MSU_RN_IMMEDIATE_MASK) == 0x90C0U)
		return TEAK_OP_MULTIPLY_RN_IMMEDIATE;
	switch (word & TEAK_OPCODE_MULTIPLY_MASK) {
		case 0x8000:
		case 0x8100:
		case 0x8200:
		case 0x8300:
		case 0x8400:
		case 0x8500:
		case 0x8600:
		case 0x8700:
			return TEAK_OP_MULTIPLY_RN_IMMEDIATE;

		case 0x8040:
		case 0x8140:
		case 0x8240:
		case 0x8340:
		case 0x8440:
		case 0x8540:
		case 0x8640:
		case 0x8740:
			return TEAK_OP_MULTIPLY_REGISTER;

		case 0x8020:
		case 0x8120:
		case 0x8220:
		case 0x8320:
		case 0x8420:
		case 0x8520:
		case 0x8620:
		case 0x8720:
			return TEAK_OP_MULTIPLY_RN_STEP;
	}
	if ((word & TEAK_OPCODE_MODIFY_RN_MASK) == 0x0080U)
		return TEAK_OP_MODIFY_RN;
	if ((word & TEAK_OPCODE_BLOCK_REPEAT_IMMEDIATE_MASK) == 0x5C00U)
		return TEAK_OP_BLOCK_REPEAT_IMMEDIATE;
	if ((word & TEAK_OPCODE_BLOCK_REPEAT_REGISTER_MASK) == 0x5D00U)
		return TEAK_OP_BLOCK_REPEAT_REGISTER;
	if ((word & TEAK_OPCODE_BRANCH_ABSOLUTE_MASK) == 0x4180U)
		return TEAK_OP_BRANCH_ABSOLUTE;
	if ((word & TEAK_OPCODE_BRANCH_RELATIVE_MASK) == 0x5000U)
		return TEAK_OP_BRANCH_RELATIVE;
	if ((word & TEAK_OPCODE_CALL_ABSOLUTE_MASK) == 0x41C0U)
		return TEAK_OP_CALL_ABSOLUTE;
	if ((word & TEAK_OPCODE_CALL_ACCUMULATOR_MASK) == 0xD480U)
		return TEAK_OP_CALL_ACCUMULATOR;
	if ((word & TEAK_OPCODE_CALL_RELATIVE_MASK) == 0x1000U)
		return TEAK_OP_CALL_RELATIVE;
	if ((word & TEAK_OPCODE_PUSH_REGISTER_MASK) == 0x5E40U)
		return TEAK_OP_PUSH_REGISTER;
	if ((word & TEAK_OPCODE_POP_REGISTER_MASK) == 0x5E60U)
		return TEAK_OP_POP_REGISTER;
	if ((word & TEAK_OPCODE_REPEAT_IMMEDIATE_MASK) == 0x0C00U)
		return TEAK_OP_REPEAT_IMMEDIATE;
	if ((word & TEAK_OPCODE_REPEAT_REGISTER_MASK) == 0x0D00U)
		return TEAK_OP_REPEAT_REGISTER;
	if ((word & TEAK_OPCODE_RETURN_MASK) == 0x4580U)
		return TEAK_OP_RETURN;
	if ((word & TEAK_OPCODE_RETURN_INTERRUPT_MASK) == 0x45C0U)
		return TEAK_OP_RETURN_INTERRUPT;
	if ((word & TEAK_OPCODE_RETURN_STACK_MASK) == 0x0900U)
		return TEAK_OP_RETURN_STACK;
	if ((word & TEAK_OPCODE_BREAK_MASK) == 0xD3C0U)
		return TEAK_OP_BREAK;
	if ((word & TEAK_OPCODE_NOP_MASK) == 0x0000U)
		return TEAK_OP_NOP;
	if ((word & TEAK_OPCODE_TRAP_MASK) == 0x0020U)
		return TEAK_OP_TRAP;
	if ((word & TEAK_OPCODE_INTERRUPT_ENABLE_MASK) == 0x4380U)
		return TEAK_OP_EINT;
	if ((word & TEAK_OPCODE_INTERRUPT_DISABLE_MASK) == 0x43C0U)
		return TEAK_OP_DINT;

	if ((word & TEAK_OPCODE_MODB_CLEAR_ACCUMULATOR_MASK) == 0x6F60U)
		return TEAK_OP_MODB3_ACCUMULATOR;

	switch (word & TEAK_OPCODE_MODIFY_ACCUMULATOR_MASK) {
		case 0x6700:
		case 0x6710:
		case 0x6720:
		case 0x6730:
		case 0x6740:
		case 0x6750:
		case 0x6760:
		case 0x6780:
		case 0x6790:
		case 0x67A0:
		case 0x67B0:
		case 0x67C0:
		case 0x67D0:
		case 0x67E0:
		case 0x67F0:
			return TEAK_OP_MODA4_ACCUMULATOR;

		case 0x6F00:
		case 0x6F10:
		case 0x6F20:
		case 0x6F30:
		case 0x6F40:
		case 0x6F50:
			return TEAK_OP_MODB3_ACCUMULATOR;
	}

	switch (word) {
		case 0xD780:
			return TEAK_OP_DELAYED_RETURN;
		case 0xD390:
			return TEAK_OP_CONTEXT_RESTORE;
	}
	return TEAK_OP_UNDEFINED;
}

static teak_alu_operation_t teak_decode_alu_operation(uint8_t operation) {
	switch (operation) {
		case 0:
			return TEAK_ALU_OR;
		case 1:
			return TEAK_ALU_AND;
		case 2:
			return TEAK_ALU_XOR;
		case 3:
			return TEAK_ALU_ADD;
		case 4:
			return TEAK_ALU_TST0;
		case 5:
			return TEAK_ALU_TST1;
		case 6:
			return TEAK_ALU_CMP;
		case 7:
			return TEAK_ALU_SUB;
		case 8:
			return TEAK_ALU_MSU;
		case 9:
			return TEAK_ALU_ADDH;
		case 10:
			return TEAK_ALU_ADDL;
		case 11:
			return TEAK_ALU_SUBH;
		case 12:
			return TEAK_ALU_SUBL;
		case 13:
			return TEAK_ALU_SQR;
		case 14:
			return TEAK_ALU_SQRA;
		case 15:
			return TEAK_ALU_CMPU;
		default:
			g_assert_not_reached();
	}
}

static teak_multiply_operation_t teak_decode_multiply2_operation(uint8_t operation) {
	switch (operation) {
		case 0:
			return TEAK_MULTIPLY_MPY;
		case 1:
			return TEAK_MULTIPLY_MAC;
		case 2:
			return TEAK_MULTIPLY_MAA;
		case 3:
			return TEAK_MULTIPLY_MACSU;
		default:
			g_assert_not_reached();
	}
}

static uint8_t teak_decode_accumulator_half_register(uint8_t encoded) {
	static const uint8_t registers[] = { 18, 16, 19, 17, 26, 28, 27, 29 };

	g_assert(encoded < ARRAY_SIZE(registers));
	return registers[encoded];
}

bool teak_decode(teak_tcg_core_t *core, uint32_t address, teak_insn_t *instruction) {
	memset(instruction, 0, sizeof(*instruction));
	instruction->address = address;
	instruction->word = teak_program_read(core, address);
	instruction->opcode = teak_decode_word(instruction->word);
	instruction->words = 1;

	switch (instruction->opcode) {
		case TEAK_OP_UNDEFINED:
		case TEAK_OP_NOP:
		case TEAK_OP_EINT:
		case TEAK_OP_DINT:
		case TEAK_OP_TRAP:
		case TEAK_OP_BREAK:
		case TEAK_OP_DELAYED_RETURN:
		case TEAK_OP_DELAYED_RETURN_INTERRUPT:
		case TEAK_OP_CONTEXT_STORE:
		case TEAK_OP_CONTEXT_RESTORE:
			break;

		case TEAK_OP_LOAD_PAGE:
			instruction->immediate = instruction->word & 0xFFU;
			break;

		case TEAK_OP_LOAD_STEPI:
		case TEAK_OP_LOAD_STEPJ:
			instruction->immediate = instruction->word & 0x7FU;
			break;

		case TEAK_OP_LOAD_MODI:
		case TEAK_OP_LOAD_MODJ:
			instruction->immediate = instruction->word & 0x1FFU;
			break;

		case TEAK_OP_LOAD_PRODUCT_SHIFT:
			instruction->immediate = instruction->word & 3U;
			break;

		case TEAK_OP_MOV_SHORT_REGISTER:
			instruction->immediate = (uint16_t) (int16_t) (int8_t) instruction->word;
			if ((instruction->word & TEAK_OPCODE_MOV_SHORT_ACCUMULATOR_HIGH_MASK) == 0x2500U) {
				instruction->register_code = 28 + (instruction->word >> 12 & 1U);
			} else if ((instruction->word & TEAK_OPCODE_MOV_SHORT_RN_MASK) == 0x2300U) {
				instruction->register_code = instruction->word >> 10 & 7U;
			} else {
				switch (instruction->word >> 8) {
					case 0x05:
						instruction->register_code = 31;
						break;
					case 0x29:
						instruction->register_code = 20;
						break;
					case 0x2D:
						instruction->register_code = 21;
						break;
					case 0x39:
						instruction->register_code = 22;
						break;
					case 0x3D:
						instruction->register_code = 23;
						break;
				}
			}
			break;

		case TEAK_OP_MOV_ACCUMULATOR_ACCUMULATOR:
			instruction->source_accumulator = instruction->word >> 10 & 3U;
			instruction->destination_accumulator = instruction->word >> 5 & 3U;
			break;

		case TEAK_OP_MOV_ACCUMULATOR_LOW_SPECIAL:
			if ((instruction->word & TEAK_OPCODE_MOV_ACCUMULATOR_LOW_ALIAS_MASK) == 0xD298U) {
				instruction->source_accumulator = instruction->word >> 10 & 3U;
				instruction->special_register = TEAK_SPECIAL_DVM;
			} else if ((instruction->word & TEAK_OPCODE_MOV_ACCUMULATOR_LOW_ALIAS_MASK) == 0xD2D8U) {
				instruction->source_accumulator = instruction->word >> 10 & 3U;
				instruction->special_register = TEAK_SPECIAL_X0;
			} else if ((instruction->word & TEAK_OPCODE_MOV_ACCUMULATOR_LOW_X1_MASK) == 0xD394U) {
				instruction->source_accumulator = instruction->word & 3U;
				instruction->special_register = TEAK_SPECIAL_X1;
			} else {
				instruction->source_accumulator = instruction->word & 3U;
				instruction->special_register = TEAK_SPECIAL_Y1;
			}
			break;

		case TEAK_OP_MOV_SPECIAL_ACCUMULATOR:
			if ((instruction->word & TEAK_OPCODE_MOV_X1_ACCUMULATOR_MASK) == 0x49C1U) {
				instruction->destination_accumulator = instruction->word >> 4 & 3U;
				instruction->special_register = TEAK_SPECIAL_X1;
				break;
			}
			if ((instruction->word & TEAK_OPCODE_MOV_Y1_ACCUMULATOR_MASK) == 0xD299U) {
				instruction->destination_accumulator = instruction->word >> 10 & 3U;
				instruction->special_register = TEAK_SPECIAL_Y1;
				break;
			}
			instruction->destination_accumulator = instruction->word >> 5 & 3U;
			switch (instruction->word & 3U) {
				case 0:
					instruction->special_register = TEAK_SPECIAL_REPC;
					break;
				case 1:
					instruction->special_register = TEAK_SPECIAL_DVM;
					break;
				case 2:
					instruction->special_register = TEAK_SPECIAL_ICR;
					break;
				case 3:
					instruction->special_register = TEAK_SPECIAL_X0;
					break;
			}
			break;

		case TEAK_OP_MOV_MIXP_REGISTER:
			instruction->special_register = TEAK_SPECIAL_MIXP;
			instruction->destination_register_code = instruction->word & 0x1FU;
			break;

		case TEAK_OP_MOV_REGISTER_MIXP:
			instruction->register_code = instruction->word & 0x1FU;
			instruction->special_register = TEAK_SPECIAL_MIXP;
			break;

		case TEAK_OP_MOV_REGISTER_ICR:
			instruction->register_code = instruction->word & 0x1FU;
			instruction->special_register = TEAK_SPECIAL_ICR;
			break;

		case TEAK_OP_SHIFT_CONDITIONAL:
			instruction->source_accumulator = instruction->word >> 10 & 3U;
			instruction->destination_accumulator = instruction->word >> 5 & 3U;
			instruction->condition = (teak_condition_t) (instruction->word & 0xFU);
			break;

		case TEAK_OP_SHIFT_IMMEDIATE:
			instruction->source_accumulator = instruction->word >> 10 & 3U;
			instruction->destination_accumulator = instruction->word >> 7 & 3U;
			instruction->shift = instruction->word & 0x3FU;
			if (instruction->shift & 0x20U)
				instruction->shift -= 0x40;
			break;

		case TEAK_OP_MULTIPLY_IMMEDIATE:
			instruction->immediate = instruction->word & 0xFFU;
			break;

		case TEAK_OP_MULTIPLY_DATA_IMM8:
			instruction->memory_address = instruction->word & 0xFFU;
			instruction->accumulator_index = instruction->word >> 11 & 1U;
			instruction->multiply_operation = teak_decode_multiply2_operation(instruction->word >> 9 & 3U);
			break;

		case TEAK_OP_MULTIPLY_DUAL_RN:
			instruction->address_register = instruction->word & 3U;
			instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
			instruction->y_address_register = 4U + (instruction->word >> 2 & 1U);
			instruction->y_step = (teak_step_t) (instruction->word >> 5 & 3U);
			instruction->accumulator_index = instruction->word >> 11 & 1U;
			if ((instruction->word & TEAK_OPCODE_MSU_DUAL_RN_MASK) == 0xD080U) {
				instruction->accumulator_index = instruction->word >> 8 & 1U;
				instruction->multiply_operation = TEAK_MULTIPLY_MSU;
			} else {
				instruction->multiply_operation = (teak_multiply_operation_t) (instruction->word >> 8 & 7U);
			}
			break;

		case TEAK_OP_MULTIPLY_RN_IMMEDIATE:
			instruction->words = 2;
			instruction->expansion = teak_program_read(core, (uint16_t) (address + 1));
			instruction->address_register = instruction->word & 7U;
			instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
			instruction->accumulator_index = instruction->word >> 11 & 1U;
			if ((instruction->word & TEAK_OPCODE_MSU_RN_IMMEDIATE_MASK) == 0x90C0U) {
				instruction->accumulator_index = instruction->word >> 8 & 1U;
				instruction->multiply_operation = TEAK_MULTIPLY_MSU;
			} else {
				instruction->multiply_operation = (teak_multiply_operation_t) (instruction->word >> 8 & 7U);
			}
			break;

		case TEAK_OP_MULTIPLY_R6:
			instruction->accumulator_index = instruction->word & 1U;
			instruction->multiply_operation = (teak_multiply_operation_t) (instruction->word >> 1 & 7U);
			break;

		case TEAK_OP_MULTIPLY_REGISTER:
			instruction->register_code = instruction->word & 0x1FU;
			instruction->accumulator_index = instruction->word >> 11 & 1U;
			instruction->multiply_operation = (teak_multiply_operation_t) (instruction->word >> 8 & 7U);
			break;

		case TEAK_OP_MULTIPLY_RN_STEP:
			instruction->address_register = instruction->word & 7U;
			instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
			instruction->accumulator_index = instruction->word >> 11 & 1U;
			instruction->multiply_operation = (teak_multiply_operation_t) (instruction->word >> 8 & 7U);
			break;

		case TEAK_OP_MODIFY_RN:
			instruction->address_register = instruction->word & 7U;
			instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
			instruction->disable_modulo = (instruction->word & 0x20U) != 0;
			break;

		case TEAK_OP_MOV_IMM_REGISTER:
			instruction->words = 2;
			instruction->expansion = teak_program_read(core, (uint16_t) (address + 1));
			instruction->register_code = instruction->word & 0x1FU;
			break;

		case TEAK_OP_MOV_IMM_B_ACCUMULATOR:
			instruction->words = 2;
			instruction->expansion = teak_program_read(core, (uint16_t) (address + 1));
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			break;

		case TEAK_OP_MOV_REGISTER_REGISTER:
			instruction->register_code = instruction->word & 0x1FU;
			instruction->destination_register_code = instruction->word >> 5 & 0x1FU;
			break;

		case TEAK_OP_MOV_REGISTER_B_ACCUMULATOR:
			instruction->register_code = instruction->word & 0x1FU;
			instruction->accumulator_index = instruction->word >> 5 & 1U;
			break;

		case TEAK_OP_PUSH_IMMEDIATE:
			instruction->words = 2;
			instruction->expansion = teak_program_read(core, (uint16_t) (address + 1));
			break;

		case TEAK_OP_PUSH_REGISTER:
		case TEAK_OP_POP_REGISTER:
		case TEAK_OP_MOV_STACK_REGISTER:
			instruction->register_code = instruction->word & 0x1FU;
			break;

		case TEAK_OP_MOV_IMM_ICR:
			instruction->immediate = instruction->word & 0x1FU;
			break;

		case TEAK_OP_MOV_IMM8_ACCUMULATOR_LOW:
			instruction->immediate = instruction->word & 0xFFU;
			instruction->accumulator_index = instruction->word >> 12 & 1U;
			break;

		case TEAK_OP_MOV_DATA_IMM16_ACCUMULATOR:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_IMM16:
			instruction->words = 2;
			instruction->expansion = teak_program_read(core, (uint16_t) (address + 1));
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			break;

		case TEAK_OP_MOVP_ACCUMULATOR_LOW_REGISTER:
			instruction->accumulator_index = instruction->word >> 5 & 1U;
			instruction->destination_register_code = instruction->word & 0x1FU;
			break;

		case TEAK_OP_MOVP_RN_RN:
			instruction->address_register = instruction->word & 7U;
			instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
			instruction->destination_register_code = instruction->word >> 5 & 3U;
			instruction->y_step = (teak_step_t) (instruction->word >> 7 & 3U);
			break;

		case TEAK_OP_MOVD_RN_RN:
			instruction->address_register = instruction->word & 3U;
			instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
			instruction->destination_register_code = 4U + (instruction->word >> 2 & 1U);
			instruction->y_step = (teak_step_t) (instruction->word >> 5 & 3U);
			break;

		case TEAK_OP_MOVS_REGISTER:
			instruction->register_code = instruction->word & 0x1FU;
			instruction->destination_accumulator = instruction->word >> 5 & 3U;
			break;

		case TEAK_OP_MOVS_RN_STEP:
			instruction->address_register = instruction->word & 7U;
			instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
			instruction->destination_accumulator = instruction->word >> 5 & 3U;
			break;

		case TEAK_OP_MOVS_DATA_IMM8:
			instruction->memory_address = instruction->word & 0xFFU;
			instruction->destination_accumulator = instruction->word >> 11 & 3U;
			break;

		case TEAK_OP_MOVS_R6:
			instruction->destination_accumulator = 2U + (instruction->word & 1U);
			break;

		case TEAK_OP_MOVSI_REGISTER:
			instruction->register_code = instruction->word >> 9 & 7U;
			instruction->destination_accumulator = instruction->word >> 5 & 3U;
			instruction->shift = instruction->word & 0x1FU;
			if ((instruction->shift & 0x10) != 0)
				instruction->shift -= 0x20;
			break;

		case TEAK_OP_MOVR_REGISTER:
			instruction->register_code = instruction->word & 0x1FU;
			instruction->destination_accumulator = 2U + (instruction->word >> 8 & 1U);
			break;

		case TEAK_OP_MOVR_RN_STEP:
			instruction->address_register = instruction->word & 7U;
			instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
			instruction->destination_accumulator = 2U + (instruction->word >> 8 & 1U);
			break;

		case TEAK_OP_MOVR_RN_HIGH: {
			static const uint8_t address_registers[] = { 0, 4, 2, 5 };
			static const teak_step_t steps[] = {
				TEAK_STEP_INCREASE,
				TEAK_STEP_DECREASE,
				TEAK_STEP_ZERO,
				TEAK_STEP_PLUS_STEP,
			};

			instruction->address_register = address_registers[instruction->word >> 3 & 3U];
			instruction->step = steps[instruction->word & 3U];
			instruction->destination_accumulator = instruction->word >> 8 & 3U;
			break;
		}

		case TEAK_OP_MOVR_B_ACCUMULATOR:
			instruction->source_accumulator = instruction->word >> 1 & 1U;
			instruction->destination_accumulator = 2U + (instruction->word & 1U);
			break;

		case TEAK_OP_MOVR_R6:
			instruction->destination_accumulator = 2U + (instruction->word >> 3 & 1U);
			break;

		case TEAK_OP_ALB_DATA_IMM8:
			instruction->words = 2;
			instruction->expansion = teak_program_read(core, (uint16_t) (address + 1));
			instruction->memory_address = instruction->word & 0xFFU;
			instruction->alb_operation = (teak_alb_operation_t) (instruction->word >> 9 & 7U);
			break;

		case TEAK_OP_ALB_RN_STEP:
			instruction->words = 2;
			instruction->expansion = teak_program_read(core, (uint16_t) (address + 1));
			instruction->address_register = instruction->word & 7U;
			instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
			instruction->alb_operation = (teak_alb_operation_t) (instruction->word >> 9 & 7U);
			break;

		case TEAK_OP_ALB_REGISTER:
			instruction->words = 2;
			instruction->expansion = teak_program_read(core, (uint16_t) (address + 1));
			instruction->register_code = instruction->word & 0x1FU;
			instruction->alb_operation = (teak_alb_operation_t) (instruction->word >> 9 & 7U);
			break;

		case TEAK_OP_REPEAT_IMMEDIATE:
			instruction->immediate = instruction->word & 0xFFU;
			break;

		case TEAK_OP_REPEAT_REGISTER:
			instruction->register_code = instruction->word & 0x1FU;
			break;

		case TEAK_OP_BLOCK_REPEAT_IMMEDIATE:
			instruction->words = 2;
			instruction->expansion = teak_program_read(core, (uint16_t) (address + 1));
			instruction->immediate = instruction->word & 0xFFU;
			instruction->branch_target = instruction->expansion;
			break;

		case TEAK_OP_BLOCK_REPEAT_REGISTER:
			instruction->words = 2;
			instruction->expansion = teak_program_read(core, (uint16_t) (address + 1));
			instruction->register_code = instruction->word & 0x1FU;
			instruction->branch_target = instruction->expansion;
			break;

		case TEAK_OP_MOV_DATA_IMM8_REGISTER:
			instruction->memory_address = instruction->word & 0xFFU;
			if ((instruction->word & TEAK_OPCODE_MOV_DATA_IMM8_SPECIAL_MASK) == 0x6D00U) {
				instruction->register_code = 31;
			} else if ((instruction->word & TEAK_OPCODE_MOV_DATA_IMM8_REGISTER_MASK) == 0x6200U) {
				instruction->register_code = teak_decode_accumulator_half_register(instruction->word >> 10 & 7U);
			} else {
				instruction->register_code = instruction->word >> 10 & 7U;
			}
			break;

		case TEAK_OP_MOV_DATA_IMM8_ACCUMULATOR:
			instruction->memory_address = instruction->word & 0xFFU;
			instruction->destination_accumulator = instruction->word >> 11 & 3U;
			break;

		case TEAK_OP_MOV_DATA_IMM8_ACCUMULATOR_HIGH_EU:
			instruction->memory_address = instruction->word & 0xFFU;
			instruction->accumulator_index = instruction->word >> 12 & 1U;
			break;

		case TEAK_OP_MOV_REGISTER_DATA_IMM8:
			instruction->memory_address = instruction->word & 0xFFU;
			if ((instruction->word & TEAK_OPCODE_MOV_DATA_IMM8_SPECIAL_MASK) == 0x7D00U) {
				instruction->register_code = 31;
			} else if ((instruction->word & TEAK_OPCODE_MOV_REGISTER_DATA_IMM8_MASK) == 0x3000U) {
				instruction->register_code = teak_decode_accumulator_half_register(instruction->word >> 9 & 7U);
			} else {
				instruction->register_code = instruction->word >> 9 & 7U;
			}
			break;

		case TEAK_OP_MOV_DATA_R7_OFFSET7_ACCUMULATOR:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET7:
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			instruction->memory_offset = instruction->word & 0x7FU;
			if ((instruction->memory_offset & 0x40) != 0)
				instruction->memory_offset -= 0x80;
			break;

		case TEAK_OP_MOV_DATA_R7_OFFSET16_ACCUMULATOR:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET16:
			instruction->words = 2;
			instruction->expansion = teak_program_read(core, (uint16_t) (address + 1));
			instruction->memory_offset = instruction->expansion;
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			break;

		case TEAK_OP_MOV_DATA_RN_STEP_REGISTER:
		case TEAK_OP_MOV_REGISTER_DATA_RN_STEP:
			instruction->register_code = instruction->word >> 5 & 0x1FU;
			instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
			instruction->address_register = instruction->word & 7U;
			break;

		case TEAK_OP_MOV_DATA_RN_STEP_B_ACCUMULATOR:
			instruction->address_register = instruction->word & 7U;
			instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			break;

		case TEAK_OP_ALU_IMMEDIATE_ACCUMULATOR: {
			bool signed_operand;
			if ((instruction->word & TEAK_OPCODE_ALU_IMM16_ACCUMULATOR_FAMILY_MASK) == 0x80C0U) {
				instruction->words = 2;
				instruction->expansion = teak_program_read(core, (uint16_t) (address + 1));
				instruction->immediate = instruction->expansion;
			} else {
				instruction->immediate = instruction->word & 0xFFU;
			}
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			instruction->alu_operation = teak_decode_alu_operation(instruction->word >> 9 & 7U);
			instruction->alu_operand = instruction->immediate;
			signed_operand = instruction->alu_operation == TEAK_ALU_ADD ||
				instruction->alu_operation == TEAK_ALU_CMP ||
				instruction->alu_operation == TEAK_ALU_SUB;
			if (signed_operand)
				instruction->alu_operand = (uint64_t) (int64_t) (int16_t) instruction->immediate;
			break;
		}

		case TEAK_OP_ALU_DATA_IMM8_ACCUMULATOR:
			instruction->memory_address = instruction->word & 0xFFU;
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			instruction->alu_operation = teak_decode_alu_operation(instruction->word >> 9 & 0xFU);
			break;

		case TEAK_OP_ALU_DATA_IMM16_ACCUMULATOR:
			instruction->words = 2;
			instruction->expansion = teak_program_read(core, (uint16_t) (address + 1));
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			instruction->alu_operation = teak_decode_alu_operation(instruction->word & 7U);
			break;

		case TEAK_OP_ALU_R7_OFFSET7_ACCUMULATOR:
			instruction->memory_offset = instruction->word & 0x7FU;
			if ((instruction->memory_offset & 0x40) != 0)
				instruction->memory_offset -= 0x80;
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			instruction->alu_operation = teak_decode_alu_operation(instruction->word >> 9 & 7U);
			break;

		case TEAK_OP_ALU_R7_OFFSET16_ACCUMULATOR:
			instruction->words = 2;
			instruction->expansion = teak_program_read(core, (uint16_t) (address + 1));
			instruction->memory_offset = instruction->expansion;
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			instruction->alu_operation = teak_decode_alu_operation(instruction->word & 7U);
			break;

		case TEAK_OP_ALU_RN_STEP_ACCUMULATOR:
			instruction->address_register = instruction->word & 7U;
			instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			instruction->alu_operation = teak_decode_alu_operation(instruction->word >> 9 & 0xFU);
			break;

		case TEAK_OP_ALU_REGISTER_ACCUMULATOR:
			instruction->register_code = instruction->word & 0x1FU;
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			instruction->alu_operation = teak_decode_alu_operation(instruction->word >> 9 & 0xFU);
			break;

		case TEAK_OP_TEST_ACCUMULATOR_DATA_IMM8:
			instruction->memory_address = instruction->word & 0xFFU;
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			switch (instruction->word & TEAK_OPCODE_TEST_ACCUMULATOR_DATA_IMM8_MASK) {
				case 0xA800:
					instruction->accumulator_test = TEAK_ACCUMULATOR_TST0;
					break;
				case 0xAA00:
					instruction->accumulator_test = TEAK_ACCUMULATOR_TST1;
					break;
			}
			break;

		case TEAK_OP_MODA4_ACCUMULATOR:
			instruction->accumulator_index = instruction->word >> 12 & 1U;
			instruction->moda_operation = (teak_moda_operation_t) (instruction->word >> 4 & 0xFU);
			instruction->condition = (teak_condition_t) (instruction->word & 0xFU);
			break;

		case TEAK_OP_MODB3_ACCUMULATOR:
			instruction->accumulator_index = instruction->word >> 12 & 1U;
			instruction->moda_operation = (teak_moda_operation_t) (instruction->word >> 4 & 7U);
			instruction->condition = (teak_condition_t) (instruction->word & 0xFU);
			break;

		case TEAK_OP_LIMIT_ACCUMULATOR:
			instruction->source_accumulator = instruction->word >> 5 & 1U;
			instruction->destination_accumulator = instruction->word >> 4 & 1U;
			break;

		case TEAK_OP_EXPONENT: {
			bool exponent_b = (instruction->word & TEAK_OPCODE_EXPONENT_B_SV_MASK) == 0x9460U ||
				(instruction->word & TEAK_OPCODE_EXPONENT_B_ACCUMULATOR_MASK) == 0x9060U;
			bool exponent_rn = (instruction->word & TEAK_OPCODE_EXPONENT_RN_SV_MASK) == 0x9C40U ||
				(instruction->word & TEAK_OPCODE_EXPONENT_RN_ACCUMULATOR_MASK) == 0x9840U;
			if (instruction->word == 0xD7C1U) {
				instruction->exponent_source = TEAK_EXPONENT_R6;
			} else if ((instruction->word & TEAK_OPCODE_EXPONENT_R6_ACCUMULATOR_MASK) == 0xD382U) {
				instruction->exponent_source = TEAK_EXPONENT_R6;
				instruction->write_accumulator = true;
				instruction->destination_accumulator = instruction->word >> 4 & 1U;
			} else if (exponent_b) {
				instruction->exponent_source = TEAK_EXPONENT_B_ACCUMULATOR;
				instruction->accumulator_index = instruction->word & 1U;
				instruction->write_accumulator = (instruction->word & 0x0400U) == 0;
				instruction->destination_accumulator = instruction->word >> 8 & 1U;
			} else if (exponent_rn) {
				instruction->exponent_source = TEAK_EXPONENT_RN_STEP;
				instruction->address_register = instruction->word & 7U;
				instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
				instruction->write_accumulator = (instruction->word & 0x0400U) == 0;
				instruction->destination_accumulator = instruction->word >> 8 & 1U;
			} else {
				instruction->exponent_source = TEAK_EXPONENT_REGISTER;
				instruction->register_code = instruction->word & 0x1FU;
				instruction->write_accumulator = (instruction->word & 0x0400U) == 0;
				instruction->destination_accumulator = instruction->word >> 8 & 1U;
			}
			break;
		}

		case TEAK_OP_DIVISION_STEP:
			instruction->memory_address = instruction->word & 0xFFU;
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			break;

		case TEAK_OP_NORMALIZE:
			instruction->address_register = instruction->word & 7U;
			instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			break;

		case TEAK_OP_SWAP_ACCUMULATORS:
			instruction->swap_operation = (teak_swap_operation_t) (instruction->word & 0xFU);
			break;

		case TEAK_OP_BANK_EXCHANGE:
			instruction->immediate = instruction->word & 0xFU;
			break;

		case TEAK_OP_MINIMUM_MAXIMUM: {
			bool data_minmax = (instruction->word & TEAK_OPCODE_MINMAX_DATA_MIN_MASK) == 0x47A0U ||
				(instruction->word & TEAK_OPCODE_MINMAX_DATA_MIN_MASK) == 0x47A4U;
			if (data_minmax) {
				instruction->memory_source = true;
				instruction->accumulator_index = instruction->word >> 3 & 1U;
				instruction->step = (teak_step_t) (instruction->word & 3U);
				instruction->minmax_operation = (instruction->word & 4U) != 0 ?
					TEAK_MINMAX_MIN_LT : TEAK_MINMAX_MIN_LE;
			} else {
				bool memory_source = (instruction->word & 0xFC00U) == 0x8000U;
				bool alias_memory_source = (instruction->word & 0xFC04U) == 0x8404U;
				bool alias_step = (instruction->word & 0xFC04U) == 0x8804U;
				bool minimum = (instruction->word & 0x0800U) != 0;
				uint16_t operation;
				if (alias_step) {
					operation = TEAK_MINMAX_MAX_GE;
					instruction->step = DSP_MINMAX_ALIAS_STEPS[instruction->word & 3U];
				} else {
					operation = minimum ? 2 : 0;
					operation += instruction->word >> 9 & 1U;
					instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
				}
				instruction->memory_source = memory_source || alias_memory_source;
				instruction->minmax_b_accumulator = alias_step;
				instruction->accumulator_index = instruction->word >> 8 & 1U;
				instruction->minmax_operation = (teak_minmax_operation_t) operation;
			}
			break;
		}

		case TEAK_OP_BRANCH_ABSOLUTE:
		case TEAK_OP_CALL_ABSOLUTE:
			instruction->words = 2;
			instruction->expansion = teak_program_read(core, (uint16_t) (address + 1));
			instruction->branch_target = instruction->expansion;
			instruction->condition = (teak_condition_t) (instruction->word & 0xFU);
			break;

		case TEAK_OP_BRANCH_RELATIVE:
		case TEAK_OP_CALL_RELATIVE: {
			int32_t relative_offset = instruction->word >> 4 & 0x7FU;
			if ((relative_offset & 0x40) != 0)
				relative_offset -= 0x80;
			instruction->branch_target = (uint16_t) ((int32_t) address + 1 + relative_offset);
			instruction->condition = (teak_condition_t) (instruction->word & 0xFU);
			break;
		}

		case TEAK_OP_CALL_ACCUMULATOR:
			instruction->accumulator_index = instruction->word >> 8 & 1U;
			break;

		case TEAK_OP_RETURN:
			instruction->condition = (teak_condition_t) (instruction->word & 0xFU);
			break;

		case TEAK_OP_RETURN_INTERRUPT:
			instruction->condition = (teak_condition_t) (instruction->word & 0xFU);
			instruction->context_switch = (instruction->word & 0x10U) != 0;
			break;

		case TEAK_OP_RETURN_STACK:
			instruction->immediate = instruction->word & 0xFFU;
			break;

		case TEAK_OP_TSTB_IMM8:
			instruction->memory_address = instruction->word & 0xFFU;
			instruction->bit_index = instruction->word >> 8 & 0xFU;
			break;

		case TEAK_OP_TSTB_RN_STEP:
			instruction->address_register = instruction->word & 7U;
			instruction->step = (teak_step_t) (instruction->word >> 3 & 3U);
			instruction->bit_index = instruction->word >> 8 & 0xFU;
			break;

		case TEAK_OP_TSTB_REGISTER:
			instruction->register_code = instruction->word & 0x1FU;
			instruction->bit_index = instruction->word >> 8 & 0xFU;
			break;
	}
	return instruction->opcode != TEAK_OP_UNDEFINED;
}
