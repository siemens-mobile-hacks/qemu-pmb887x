/*
 * WASM PORT FAST PATH (removable): P1, the C kernels of the hot GSM search
 * correlator loops (doc/wasm-fast-paths.md).  Not a translation unit of its
 * own: tcg.c includes it where the section stood, after
 * tcg_execute_prepared_block(), so the kernels see tcg.c's static helpers
 * and tcg.c's hooks see tcg_fast_discover(), tcg_fast_lookup(),
 * tcg_fast_invalidate(), tcg_fast_init() and tcg_execute_fast().
 *
 * To remove the fast path: delete this file, its #include in tcg.c, and the
 * hooks in tcg.c marked "WASM PORT FAST PATH (removable): P1" (the forward
 * declarations, the cache entry's fast/fast_epoch, the program-write
 * invalidations, the discovery in tcg_prepare_block(), the fast branches of
 * the chain helper and the slice runners), and core.h's fast_runs.
 */

/*
 * Hot DSP loops (the GSM search correlators) run as straight C instead of
 * chained TCG blocks. Each loop is a table of decoded instructions that must
 * match program memory exactly; the C semantics mirror the TCG emitters
 * instruction by instruction, and execution stops at an instruction boundary
 * on anything it does not model (non-direct memory, budget, exit requests),
 * leaving the rest to the translator.
 */
static const teak_insn_t tcg_fast_loop_35be[] = {
	{ .opcode = TEAK_OP_MOV_DATA_RN_STEP_REGISTER, .address = 0x35BE, .word = 0x1F08, .words = 1, .register_code = 24, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_REGISTER_DATA_RN_STEP, .address = 0x35BF, .word = 0x1B4C, .words = 1, .register_code = 26, .address_register = 4, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_DATA_RN_STEP_REGISTER, .address = 0x35C0, .word = 0x1F08, .words = 1, .register_code = 24, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_REGISTER_DATA_RN_STEP, .address = 0x35C1, .word = 0x1B4C, .words = 1, .register_code = 26, .address_register = 4, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MODA4_ACCUMULATOR, .address = 0x35C2, .word = 0x67C0, .words = 1, .moda_operation = TEAK_MODA_CLRR },
	{ .opcode = TEAK_OP_MODA4_ACCUMULATOR, .address = 0x35C3, .word = 0x77C0, .words = 1, .accumulator_index = 1, .moda_operation = TEAK_MODA_CLRR },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35C4, .word = 0xD022, .words = 1, .address_register = 2, .y_address_register = 4, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35C5, .word = 0xD22A, .words = 1, .address_register = 2, .y_address_register = 4, .multiply_operation = TEAK_MULTIPLY_MAC, .step = TEAK_STEP_INCREASE, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35C6, .word = 0xDA22, .words = 1, .address_register = 2, .y_address_register = 4, .accumulator_index = 1, .multiply_operation = TEAK_MULTIPLY_MAC, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35C7, .word = 0xD22A, .words = 1, .address_register = 2, .y_address_register = 4, .multiply_operation = TEAK_MULTIPLY_MAC, .step = TEAK_STEP_INCREASE, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35C8, .word = 0xDA22, .words = 1, .address_register = 2, .y_address_register = 4, .accumulator_index = 1, .multiply_operation = TEAK_MULTIPLY_MAC, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35C9, .word = 0xD22A, .words = 1, .address_register = 2, .y_address_register = 4, .multiply_operation = TEAK_MULTIPLY_MAC, .step = TEAK_STEP_INCREASE, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35CA, .word = 0xDA22, .words = 1, .address_register = 2, .y_address_register = 4, .accumulator_index = 1, .multiply_operation = TEAK_MULTIPLY_MAC, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35CB, .word = 0xD22A, .words = 1, .address_register = 2, .y_address_register = 4, .multiply_operation = TEAK_MULTIPLY_MAC, .step = TEAK_STEP_INCREASE, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35CC, .word = 0xDA22, .words = 1, .address_register = 2, .y_address_register = 4, .accumulator_index = 1, .multiply_operation = TEAK_MULTIPLY_MAC, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35CD, .word = 0xD22A, .words = 1, .address_register = 2, .y_address_register = 4, .multiply_operation = TEAK_MULTIPLY_MAC, .step = TEAK_STEP_INCREASE, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35CE, .word = 0xDA22, .words = 1, .address_register = 2, .y_address_register = 4, .accumulator_index = 1, .multiply_operation = TEAK_MULTIPLY_MAC, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35CF, .word = 0xD22A, .words = 1, .address_register = 2, .y_address_register = 4, .multiply_operation = TEAK_MULTIPLY_MAC, .step = TEAK_STEP_INCREASE, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35D0, .word = 0xDA22, .words = 1, .address_register = 2, .y_address_register = 4, .accumulator_index = 1, .multiply_operation = TEAK_MULTIPLY_MAC, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35D1, .word = 0xD22A, .words = 1, .address_register = 2, .y_address_register = 4, .multiply_operation = TEAK_MULTIPLY_MAC, .step = TEAK_STEP_INCREASE, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35D2, .word = 0xDA22, .words = 1, .address_register = 2, .y_address_register = 4, .accumulator_index = 1, .multiply_operation = TEAK_MULTIPLY_MAC, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35D3, .word = 0xD22A, .words = 1, .address_register = 2, .y_address_register = 4, .multiply_operation = TEAK_MULTIPLY_MAC, .step = TEAK_STEP_INCREASE, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35D4, .word = 0xDA22, .words = 1, .address_register = 2, .y_address_register = 4, .accumulator_index = 1, .multiply_operation = TEAK_MULTIPLY_MAC, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35D5, .word = 0xD22A, .words = 1, .address_register = 2, .y_address_register = 4, .multiply_operation = TEAK_MULTIPLY_MAC, .step = TEAK_STEP_INCREASE, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35D6, .word = 0xDA22, .words = 1, .address_register = 2, .y_address_register = 4, .accumulator_index = 1, .multiply_operation = TEAK_MULTIPLY_MAC, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35D7, .word = 0xD22A, .words = 1, .address_register = 2, .y_address_register = 4, .multiply_operation = TEAK_MULTIPLY_MAC, .step = TEAK_STEP_INCREASE, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35D8, .word = 0xDA22, .words = 1, .address_register = 2, .y_address_register = 4, .accumulator_index = 1, .multiply_operation = TEAK_MULTIPLY_MAC, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35D9, .word = 0xD22A, .words = 1, .address_register = 2, .y_address_register = 4, .multiply_operation = TEAK_MULTIPLY_MAC, .step = TEAK_STEP_INCREASE, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35DA, .word = 0xDA22, .words = 1, .address_register = 2, .y_address_register = 4, .accumulator_index = 1, .multiply_operation = TEAK_MULTIPLY_MAC, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35DB, .word = 0xD22A, .words = 1, .address_register = 2, .y_address_register = 4, .multiply_operation = TEAK_MULTIPLY_MAC, .step = TEAK_STEP_INCREASE, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35DC, .word = 0xDA22, .words = 1, .address_register = 2, .y_address_register = 4, .accumulator_index = 1, .multiply_operation = TEAK_MULTIPLY_MAC, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_DUAL_RN, .address = 0x35DD, .word = 0xD23A, .words = 1, .address_register = 2, .y_address_register = 4, .multiply_operation = TEAK_MULTIPLY_MAC, .step = TEAK_STEP_PLUS_STEP, .y_step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_ALU_REGISTER_ACCUMULATOR, .address = 0x35DE, .word = 0x87AB, .words = 1, .register_code = 11, .accumulator_index = 1, .alu_operation = TEAK_ALU_ADD },
	{ .opcode = TEAK_OP_MOV_REGISTER_DATA_RN_STEP, .address = 0x35DF, .word = 0x1B89, .words = 1, .register_code = 28, .address_register = 1, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_REGISTER_DATA_RN_STEP, .address = 0x35E0, .word = 0x1BA9, .words = 1, .register_code = 29, .address_register = 1, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_DATA_RN_STEP_REGISTER, .address = 0x35E1, .word = 0x1F08, .words = 1, .register_code = 24, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_REGISTER_DATA_RN_STEP, .address = 0x35E2, .word = 0x1B4C, .words = 1, .register_code = 26, .address_register = 4, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_DATA_RN_STEP_REGISTER, .address = 0x35E3, .word = 0x1F08, .words = 1, .register_code = 24, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_REGISTER_DATA_RN_STEP, .address = 0x35E4, .word = 0x1B4C, .words = 1, .register_code = 26, .address_register = 4, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MODIFY_RN, .address = 0x35E5, .word = 0x0095, .words = 1, .address_register = 5, .step = TEAK_STEP_DECREASE },
	{ .opcode = TEAK_OP_BRANCH_RELATIVE, .address = 0x35E6, .word = 0x557C, .branch_target = 0x35BE, .words = 1, .condition = TEAK_COND_NR },
};

static const teak_insn_t tcg_fast_loop_643f[] = {
	{ .opcode = TEAK_OP_MULTIPLY_RN_STEP, .address = 0x643F, .word = 0x802A, .words = 1, .address_register = 2, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_DATA_RN_STEP_REGISTER, .address = 0x6440, .word = 0x1F89, .words = 1, .register_code = 28, .address_register = 1, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_DATA_RN_STEP_REGISTER, .address = 0x6441, .word = 0x1FA9, .words = 1, .register_code = 29, .address_register = 1, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_ALU_RN_STEP_ACCUMULATOR, .address = 0x6442, .word = 0x9688, .words = 1, .alu_operation = TEAK_ALU_SUBH, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_ALU_RN_STEP_ACCUMULATOR, .address = 0x6443, .word = 0x9788, .words = 1, .accumulator_index = 1, .alu_operation = TEAK_ALU_SUBH, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_RN_STEP, .address = 0x6444, .word = 0x822A, .words = 1, .address_register = 2, .multiply_operation = TEAK_MULTIPLY_MAC, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_ALU_REGISTER_ACCUMULATOR, .address = 0x6445, .word = 0x87AB, .words = 1, .register_code = 11, .accumulator_index = 1, .alu_operation = TEAK_ALU_ADD },
	{ .opcode = TEAK_OP_MOV_REGISTER_DATA_RN_STEP, .address = 0x6446, .word = 0x1B8A, .words = 1, .register_code = 28, .address_register = 2, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_REGISTER_DATA_RN_STEP, .address = 0x6447, .word = 0x1BB2, .words = 1, .register_code = 29, .address_register = 2, .step = TEAK_STEP_DECREASE },
};

static const teak_insn_t tcg_fast_loop_6489[] = {
	{ .opcode = TEAK_OP_ALU_REGISTER_ACCUMULATOR, .address = 0x6489, .word = 0x9FA1, .words = 1, .register_code = 1, .accumulator_index = 1, .alu_operation = TEAK_ALU_CMPU },
	{ .opcode = TEAK_OP_BRANCH_RELATIVE, .address = 0x648A, .word = 0x5022, .branch_target = 0x648D, .words = 1, .condition = TEAK_COND_NEQ },
	{ .opcode = TEAK_OP_MOV_IMM_REGISTER, .address = 0x648B, .word = 0x5E01, .expansion = 0x01DC, .words = 2, .register_code = 1 },
	{ .opcode = TEAK_OP_MOV_DATA_RN_STEP_REGISTER, .address = 0x648D, .word = 0x1F08, .words = 1, .register_code = 24, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_REGISTER_DATA_RN_STEP, .address = 0x648E, .word = 0x1B49, .words = 1, .register_code = 26, .address_register = 1, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_DATA_RN_STEP_REGISTER, .address = 0x648F, .word = 0x1F08, .words = 1, .register_code = 24, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_REGISTER_DATA_RN_STEP, .address = 0x6490, .word = 0x1B49, .words = 1, .register_code = 26, .address_register = 1, .step = TEAK_STEP_INCREASE },
};

static const teak_insn_t tcg_fast_loop_64a9[] = {
	{ .opcode = TEAK_OP_MOV_REGISTER_B_ACCUMULATOR, .address = 0x64A9, .word = 0x5EDF, .words = 1, .register_code = 31 },
	{ .opcode = TEAK_OP_MOV_DATA_RN_STEP_REGISTER, .address = 0x64AA, .word = 0x1CE9, .words = 1, .register_code = 7, .address_register = 1, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_RN_STEP, .address = 0x64AB, .word = 0x8028, .words = 1, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_REGISTER_REGISTER, .address = 0x64AC, .word = 0x5B0B, .words = 1, .register_code = 11, .destination_register_code = 24 },
	{ .opcode = TEAK_OP_MOV_DATA_RN_STEP_REGISTER, .address = 0x64AD, .word = 0x1CE9, .words = 1, .register_code = 7, .address_register = 1, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MULTIPLY_RN_STEP, .address = 0x64AE, .word = 0x8028, .words = 1, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_ALU_REGISTER_ACCUMULATOR, .address = 0x64AF, .word = 0x86AB, .words = 1, .register_code = 11, .alu_operation = TEAK_ALU_ADD },
	{ .opcode = TEAK_OP_MODB3_ACCUMULATOR, .address = 0x64B0, .word = 0x6F04, .words = 1, .condition = TEAK_COND_GE },
	{ .opcode = TEAK_OP_ALU_REGISTER_ACCUMULATOR, .address = 0x64B1, .word = 0x87B2, .words = 1, .register_code = 18, .accumulator_index = 1, .alu_operation = TEAK_ALU_ADD },
	{ .opcode = TEAK_OP_ALU_RN_STEP_ACCUMULATOR, .address = 0x64B2, .word = 0x8F84, .words = 1, .address_register = 4, .accumulator_index = 1, .alu_operation = TEAK_ALU_SUB },
	{ .opcode = TEAK_OP_MOV_REGISTER_DATA_RN_STEP, .address = 0x64B3, .word = 0x1B6A, .words = 1, .register_code = 27, .address_register = 2, .step = TEAK_STEP_INCREASE },
	{ .opcode = TEAK_OP_MOV_REGISTER_DATA_RN_STEP, .address = 0x64B4, .word = 0x1A4C, .words = 1, .register_code = 18, .address_register = 4, .step = TEAK_STEP_INCREASE },
};


typedef struct teak_tcg_fast_loop_t {
	const teak_insn_t *instructions;
	size_t count;
	uint32_t (*run)(teak_tcg_core_t *core, uint32_t budget);
	bool block_repeat;
} teak_tcg_fast_loop_t;

#define TEAK_FAST_INLINE static inline __attribute__((always_inline))

TEAK_FAST_INLINE int64_t tcg_fast_sign_extend(uint64_t value) {
	return (int64_t) (value << TEAK_ACCUMULATOR_HOST_SHIFT) >> TEAK_ACCUMULATOR_HOST_SHIFT;
}

TEAK_FAST_INLINE void tcg_fast_value_flags(teak_state_t *state, uint64_t value) {
	uint32_t zero = value == 0;
	uint32_t extension = value != (uint64_t) (int64_t) (int32_t) value;
	uint32_t top = (uint32_t) (value >> 30) & 3U;

	state->fz = zero;
	state->fm = (uint32_t) (value >> TEAK_ACCUMULATOR_SIGN_BIT) & 1U;
	state->fe = extension;
	state->fn = (((top ^ (top >> 1)) & 1U) & (extension ^ 1U)) | zero;
}

TEAK_FAST_INLINE void tcg_fast_add_sub_flags(teak_state_t *state, uint64_t left, uint64_t right, uint64_t result,
	bool subtract
) {
	uint64_t adjusted_right = subtract ? ~right : right;
	uint32_t overflow = (uint32_t) (((left ^ result) & ~(left ^ adjusted_right)) >> TEAK_ACCUMULATOR_SIGN_BIT) & 1U;

	state->fc0 = (uint32_t) (result >> TEAK_ACCUMULATOR_BITS) & 1U;
	state->fv = overflow;
	state->fvl |= overflow;
}

TEAK_FAST_INLINE uint64_t tcg_fast_saturate(uint64_t value, uint8_t disabled, uint8_t *latched) {
	uint32_t condition = (value != (uint64_t) (int64_t) (int32_t) value) & (disabled ^ 1U);

	*latched |= condition;
	if (condition == 0)
		return value;
	return (int64_t) value < 0 ? (uint64_t) (int64_t) INT32_MIN : INT32_MAX;
}

TEAK_FAST_INLINE void tcg_fast_alu_accumulator(teak_state_t *state, teak_alu_operation_t operation,
	uint8_t accumulator_index, uint64_t operand
) {
	uint64_t value = state->a[accumulator_index] & TEAK_ACCUMULATOR_MASK;
	uint64_t normalized_operand = operand & TEAK_ACCUMULATOR_MASK;
	uint64_t result;
	bool comparison = operation == TEAK_ALU_CMPU;
	bool saturating = false;

	switch (operation) {
		case TEAK_ALU_ADD:
			result = value + normalized_operand;
			tcg_fast_add_sub_flags(state, value, normalized_operand, result, false);
			saturating = true;
			break;
		case TEAK_ALU_CMPU:
		case TEAK_ALU_SUB:
		case TEAK_ALU_SUBH:
			result = value - normalized_operand;
			tcg_fast_add_sub_flags(state, value, normalized_operand, result, true);
			saturating = !comparison;
			break;
		default:
			g_assert_not_reached();
	}
	result = tcg_fast_sign_extend(result & TEAK_ACCUMULATOR_MASK);
	tcg_fast_value_flags(state, result);
	if (comparison)
		return;
	if (saturating)
		result = tcg_fast_saturate(result, state->sata, &state->flm);
	state->a[accumulator_index] = result;
}

TEAK_FAST_INLINE uint64_t tcg_fast_shifted_product(const teak_state_t *state) {
	uint64_t product = state->p[0] | (uint64_t) state->product_extension[0] * 0xF00000000ULL;
	uint32_t shift = state->product_shift;
	uint32_t nonzero = shift != 0;
	uint32_t right = shift == 1;

	product = tcg_fast_sign_extend(product);
	shift -= nonzero;
	return (uint64_t) ((int64_t) (product << (shift & 63U)) >> right);
}

TEAK_FAST_INLINE void tcg_fast_multiply(teak_state_t *state, uint16_t x) {
	uint16_t y = state->y[0];
	uint32_t product = (uint32_t) (int32_t) (int16_t) x * (uint32_t) (int32_t) (int16_t) y;

	state->x[0] = x;
	state->p[0] = product;
	state->product_extension[0] = product >> 31;
}

TEAK_FAST_INLINE void tcg_fast_multiply_operation(teak_state_t *state, const teak_insn_t *instruction, uint16_t x) {
	uint8_t index = instruction->accumulator_index;

	switch (instruction->multiply_operation) {
		case TEAK_MULTIPLY_MPY:
			break;
		case TEAK_MULTIPLY_MAC:
			tcg_fast_alu_accumulator(state, TEAK_ALU_ADD, index, tcg_fast_shifted_product(state));
			break;
		default:
			g_assert_not_reached();
	}
	tcg_fast_multiply(state, x);
}

TEAK_FAST_INLINE uint16_t tcg_fast_rn_next(const teak_state_t *state, uint8_t address_register, teak_step_t step_mode,
	bool disable_modulo, uint32_t address
) {
	uint32_t step;
	uint32_t updated;

	switch (step_mode) {
		case TEAK_STEP_ZERO:
			return address;
		case TEAK_STEP_INCREASE:
			step = 1;
			break;
		case TEAK_STEP_DECREASE:
			step = -1;
			break;
		default:
			step = (uint32_t) ((int32_t) ((uint32_t) (address_register < 4 ? state->stepi : state->stepj) << 25) >> 25);
			break;
	}
	if (address_register < TEAK_MODULO_REGISTERS && !disable_modulo) {
		const teak_modulo_step_t *modulo = &state->modulo_steps[address_register][step_mode - TEAK_STEP_INCREASE];
		uint32_t low = address & modulo->mask;

		updated = (low + step) & modulo->mask;
		if (low == modulo->boundary)
			updated = modulo->wrap_target;
		updated |= address & ~modulo->mask;
	} else {
		updated = address + step;
	}
	return updated;
}

TEAK_FAST_INLINE bool tcg_fast_read(const teak_data_map_t *map, teak_data_space_t space, uint32_t address,
	uint16_t *value
) {
	const uint16_t *page = map->read_pages[space][address >> TEAK_DATA_PAGE_SHIFT];

	if (page == NULL)
		return false;
	*value = page[address & (TEAK_DATA_PAGE_WORDS - 1)];
	return true;
}

/* Where tcg_emit_data_write() stores @address inline, or NULL when it calls the helper. */
TEAK_FAST_INLINE uint16_t *tcg_fast_write_pointer(const teak_data_map_t *map, uint32_t address) {
	uint16_t *page = map->write_pages[address >> TEAK_DATA_PAGE_SHIFT];

	return page == NULL ? NULL : page + (address & (TEAK_DATA_PAGE_WORDS - 1));
}

TEAK_FAST_INLINE bool tcg_fast_condition(const teak_state_t *state, teak_condition_t condition) {
	uint32_t predicate;

	switch (condition) {
		case TEAK_COND_TRUE:
			return true;
		case TEAK_COND_NEQ:
			predicate = state->fz ^ 1U;
			break;
		case TEAK_COND_GE:
			predicate = state->fm ^ 1U;
			break;
		case TEAK_COND_NR:
			predicate = state->fr ^ 1U;
			break;
		default:
			g_assert_not_reached();
	}
	return predicate != 0;
}

TEAK_FAST_INLINE uint16_t *tcg_fast_rn_old(teak_state_t *state, uint8_t register_code) {
	if (register_code < 6)
		return &state->r[register_code];
	if (register_code == 6)
		return &state->r[7];
	return &state->y[0];
}

TEAK_FAST_INLINE uint16_t tcg_fast_mov_read_register(teak_state_t *state, uint8_t register_code) {
	int64_t accumulator;
	bool high;

	if (register_code >= 16 && register_code <= 19) {
		accumulator = tcg_fast_sign_extend(state->b[register_code & 1U]);
		high = register_code < 18;
	} else if (register_code <= 7) {
		return *tcg_fast_rn_old(state, register_code);
	} else {
		g_assert(register_code == 31);
		return state->shift_value;
	}

	if (!state->sat && (accumulator < INT32_MIN || accumulator > INT32_MAX)) {
		state->flm = 1;
		accumulator = accumulator < 0 ? INT32_MIN : INT32_MAX;
	}
	return high ? (uint64_t) accumulator >> 16 : (uint64_t) accumulator;
}

/* tcg_complete_block_repeat() after @instruction, which ends a block when it ends the active body. */
TEAK_FAST_INLINE void tcg_fast_complete_block_repeat_level(teak_state_t *state, const teak_insn_t *instruction,
	const uint8_t level
) {
	uint16_t end = state->block_repeat_end[level];

	if ((uint16_t) (instruction->address + instruction->words - 1) != end || state->pc != (uint16_t) (end + 1))
		return;
	if (state->block_repeat_lc[level] != 0) {
		state->block_repeat_lc[level]--;
		if (state->block_repeat_start[level] != end || state->block_repeat_lc[level] != 0) {
			state->pc = state->block_repeat_start[level];
			return;
		}
	}
	state->bcn = (state->bcn - 1) & TEAK_BLOCK_REPEAT_COUNTER_MASK;
	state->lp = state->bcn != 0;
}

/* Constant indices keep the local state copy in registers. */
TEAK_FAST_INLINE void tcg_fast_complete_block_repeat(teak_state_t *state, const teak_insn_t *instruction) {
	QEMU_BUILD_BUG_ON(TEAK_BLOCK_REPEAT_LEVELS != 4);

	switch (state->bcn) {
		case 0:
		case 1:
			tcg_fast_complete_block_repeat_level(state, instruction, 0);
			break;
		case 2:
			tcg_fast_complete_block_repeat_level(state, instruction, 1);
			break;
		case 3:
			tcg_fast_complete_block_repeat_level(state, instruction, 2);
			break;
		default:
			tcg_fast_complete_block_repeat_level(state, instruction, 3);
			break;
	}
}

TEAK_FAST_INLINE uint64_t *tcg_fast_accumulator_register(teak_state_t *state, uint8_t register_code) {
	if (register_code >= 16 && register_code <= 19)
		return &state->b[register_code & 1U];
	return &state->a[register_code & 1U];
}

/*
 * Exactly the instructions the tables above hold: tcg_fast_execute() models
 * these and nothing else, so a table with any other fails verification.
 */
static bool tcg_fast_supported(const teak_insn_t *instruction) {
	uint8_t code = instruction->register_code;
	teak_alu_operation_t alu = instruction->alu_operation;

	switch (instruction->opcode) {
		case TEAK_OP_MOV_DATA_RN_STEP_REGISTER:
			return code == 7 || code == 24 || code == 28 || code == 29;
		case TEAK_OP_MOV_REGISTER_DATA_RN_STEP:
			return code == 18 || (code >= 26 && code <= 29);
		case TEAK_OP_MODA4_ACCUMULATOR:
			return instruction->moda_operation == TEAK_MODA_CLRR && instruction->condition == TEAK_COND_TRUE;
		case TEAK_OP_MODB3_ACCUMULATOR:
			return instruction->moda_operation == TEAK_MODA_SHR && instruction->condition == TEAK_COND_GE;
		case TEAK_OP_MULTIPLY_DUAL_RN:
		case TEAK_OP_MULTIPLY_RN_STEP:
			return instruction->multiply_operation == TEAK_MULTIPLY_MPY ||
				instruction->multiply_operation == TEAK_MULTIPLY_MAC;
		case TEAK_OP_ALU_RN_STEP_ACCUMULATOR:
			return alu == TEAK_ALU_SUB || alu == TEAK_ALU_SUBH;
		case TEAK_OP_ALU_REGISTER_ACCUMULATOR:
			return (alu == TEAK_ALU_ADD && (code == 11 || code == 18)) || (alu == TEAK_ALU_CMPU && code == 1);
		case TEAK_OP_MOV_REGISTER_B_ACCUMULATOR:
			return code == 31;
		case TEAK_OP_MOV_REGISTER_REGISTER:
			return code == 11 && instruction->destination_register_code == 24;
		case TEAK_OP_MODIFY_RN:
			return true;
		case TEAK_OP_BRANCH_RELATIVE:
			return instruction->condition == TEAK_COND_NEQ || instruction->condition == TEAK_COND_NR;
		case TEAK_OP_MOV_IMM_REGISTER:
			return code == 1;
		default:
			return false;
	}
}

TEAK_FAST_INLINE bool tcg_fast_execute(teak_state_t *state, const teak_data_map_t *map,
	const teak_insn_t *instruction
) {
	uint16_t next_pc = tcg_instruction_end(instruction);
	uint8_t address_register = instruction->address_register;

	switch (instruction->opcode) {
		case TEAK_OP_MOV_DATA_RN_STEP_REGISTER: {
			uint8_t code = instruction->register_code;
			uint32_t address = state->r[address_register];
			uint16_t value;

			if (!tcg_fast_read(map, TEAK_DATA_SPACE_ALL, address, &value))
				return false;
			state->r[address_register] = tcg_fast_rn_next(state, address_register, instruction->step,
				instruction->disable_modulo, address);
			if (code <= 7) {
				*tcg_fast_rn_old(state, code) = value;
			} else {
				uint64_t accumulator = value;

				if (code == 24 || code == 25)
					accumulator = (uint64_t) (int64_t) (int16_t) value;
				else if (code <= 17 || code >= 28)
					accumulator = (uint64_t) (int64_t) (int32_t) ((uint32_t) value << 16);
				tcg_fast_value_flags(state, accumulator);
				*tcg_fast_accumulator_register(state, code) = accumulator;
			}
			break;
		}

		case TEAK_OP_MOV_REGISTER_DATA_RN_STEP: {
			uint8_t code = instruction->register_code;
			uint8_t flm = state->flm;
			uint32_t address;
			uint16_t *pointer;
			uint16_t value;

			if ((code >= 16 && code <= 19) || (code >= 26 && code <= 29)) {
				uint64_t accumulator = tcg_fast_saturate(*tcg_fast_accumulator_register(state, code), state->sat, &flm);

				if (code <= 17 || code >= 28)
					accumulator >>= 16;
				value = accumulator;
			} else {
				g_assert_not_reached();
			}
			address = state->r[address_register];
			pointer = tcg_fast_write_pointer(map, address);
			if (pointer == NULL)
				return false;
			state->flm = flm;
			state->r[address_register] = tcg_fast_rn_next(state, address_register, instruction->step,
				instruction->disable_modulo, address);
			*pointer = value;
			break;
		}

		case TEAK_OP_MODA4_ACCUMULATOR:
		case TEAK_OP_MODB3_ACCUMULATOR: {
			uint64_t *accumulator;
			uint64_t value;
			uint64_t result;

			if (!tcg_fast_condition(state, instruction->condition))
				break;
			accumulator = instruction->opcode == TEAK_OP_MODB3_ACCUMULATOR ? &state->b[instruction->accumulator_index] :
				&state->a[instruction->accumulator_index];
			value = *accumulator & TEAK_ACCUMULATOR_MASK;
			switch (instruction->moda_operation) {
				case TEAK_MODA_CLRR:
					result = 0x8000;
					break;
				case TEAK_MODA_SHR:
					result = state->s == 0 ? (uint64_t) (tcg_fast_sign_extend(value) >> 1) : value >> 1;
					if (state->s == 0)
						state->fv = 0;
					state->fc0 = (uint32_t) value & 1U;
					break;
				default:
					g_assert_not_reached();
			}
			result = tcg_fast_sign_extend(result & TEAK_ACCUMULATOR_MASK);
			tcg_fast_value_flags(state, result);
			*accumulator = result;
			break;
		}

		case TEAK_OP_MULTIPLY_DUAL_RN: {
			uint8_t y_register = instruction->y_address_register;
			uint32_t y_address = state->r[y_register];
			uint16_t y_next = tcg_fast_rn_next(state, y_register, instruction->y_step, false, y_address);
			uint32_t x_address = address_register == y_register ? y_next : state->r[address_register];
			uint16_t x_next = tcg_fast_rn_next(state, address_register, instruction->step, false, x_address);
			uint16_t x;
			uint16_t y;

			if (!tcg_fast_read(map, TEAK_DATA_SPACE_Y, y_address, &y) ||
				!tcg_fast_read(map, TEAK_DATA_SPACE_XZ, x_address, &x))
				return false;
			state->r[y_register] = y_next;
			state->r[address_register] = x_next;
			state->y[0] = y;
			tcg_fast_multiply_operation(state, instruction, x);
			break;
		}

		case TEAK_OP_MULTIPLY_RN_STEP: {
			uint32_t address = state->r[address_register];
			uint16_t x;

			if (!tcg_fast_read(map, TEAK_DATA_SPACE_ALL, address, &x))
				return false;
			state->r[address_register] = tcg_fast_rn_next(state, address_register, instruction->step,
				instruction->disable_modulo, address);
			tcg_fast_multiply_operation(state, instruction, x);
			break;
		}

		case TEAK_OP_ALU_RN_STEP_ACCUMULATOR:
		case TEAK_OP_ALU_REGISTER_ACCUMULATOR: {
			teak_alu_operation_t operation = instruction->alu_operation;
			uint8_t code = instruction->register_code;
			uint64_t operand;
			uint16_t value;

			if (instruction->opcode == TEAK_OP_ALU_RN_STEP_ACCUMULATOR) {
				uint32_t address = state->r[address_register];

				if (!tcg_fast_read(map, TEAK_DATA_SPACE_ALL, address, &value))
					return false;
				state->r[address_register] = tcg_fast_rn_next(state, address_register, instruction->step,
					instruction->disable_modulo, address);
			} else if (code == 11) {
				tcg_fast_alu_accumulator(state, operation, instruction->accumulator_index,
					tcg_fast_shifted_product(state));
				break;
			} else {
				value = tcg_fast_mov_read_register(state, code);
			}
			operand = value;
			if (tcg_alu_sign_extends_16(operation))
				operand = (uint64_t) (int64_t) (int16_t) value;
			if (tcg_alu_shifts_operand(operation))
				operand <<= 16;
			tcg_fast_alu_accumulator(state, operation, instruction->accumulator_index, operand);
			break;
		}

		case TEAK_OP_MOV_REGISTER_B_ACCUMULATOR: {
			uint16_t value = tcg_fast_mov_read_register(state, instruction->register_code);
			uint64_t accumulator = (uint64_t) (int64_t) (int16_t) value;

			tcg_fast_value_flags(state, accumulator);
			state->b[instruction->accumulator_index] = accumulator;
			break;
		}

		case TEAK_OP_MOV_REGISTER_REGISTER: {
			uint8_t code = instruction->register_code;
			uint8_t destination = instruction->destination_register_code;

			if (code == 11) {
				uint64_t product = tcg_fast_shifted_product(state);

				tcg_fast_value_flags(state, product);
				state->a[destination - 24] = product;
				break;
			}
			g_assert_not_reached();
		}

		case TEAK_OP_MODIFY_RN: {
			uint16_t updated = tcg_fast_rn_next(state, address_register, instruction->step, instruction->disable_modulo,
				state->r[address_register]);

			state->r[address_register] = updated;
			state->fr = updated == 0;
			break;
		}

		case TEAK_OP_BRANCH_RELATIVE:
			if (tcg_fast_condition(state, instruction->condition)) {
				state->pc = instruction->branch_target;
				state->exit_reason = TEAK_EXIT_BRANCH;
			} else {
				state->pc = next_pc;
				state->exit_reason = TEAK_EXIT_NONE;
			}
			return true;

		case TEAK_OP_MOV_IMM_REGISTER:
			*tcg_fast_rn_old(state, instruction->register_code) = instruction->expansion;
			break;

		default:
			g_assert_not_reached();
	}
	state->pc = next_pc;
	return true;
}

#if defined(__clang__)
#define TEAK_FAST_UNROLL _Pragma("clang loop unroll(full)")
#else
#define TEAK_FAST_UNROLL _Pragma("GCC unroll 64")
#endif

TEAK_FAST_INLINE bool tcg_fast_is_branch(const teak_insn_t *instruction) {
	return instruction->opcode == TEAK_OP_BRANCH_ABSOLUTE || instruction->opcode == TEAK_OP_BRANCH_RELATIVE;
}

/* The per-instruction loop of the fast runners, from @cycles of @budget. */
TEAK_FAST_INLINE uint32_t tcg_fast_steps(teak_tcg_core_t *core, teak_state_t *state, const teak_data_map_t *map,
	uint32_t budget, uint32_t cycles, const teak_insn_t *instructions, size_t count, bool stop_at_head
) {
	bool progressed;

	do {
		progressed = false;
		TEAK_FAST_UNROLL
		for (size_t i = 0; i < count; i++) {
			if (state->pc != instructions[i].address)
				continue;
			if (cycles >= budget || !tcg_fast_execute(state, map, &instructions[i]))
				return cycles;
			cycles++;
			progressed = true;
			if (state->lp)
				tcg_fast_complete_block_repeat(state, &instructions[i]);
			if (instructions[i].opcode == TEAK_OP_MOV_REGISTER_DATA_RN_STEP &&
				(qatomic_read(&core->state.exit_request) | qatomic_read(&core->state.interrupt_request)) != 0)
				return cycles;
			if (stop_at_head && state->pc == instructions[0].address)
				return cycles;
		}
	} while (progressed);
	return cycles;
}

TEAK_FAST_INLINE uint32_t tcg_fast_run(teak_tcg_core_t *core, uint32_t budget,
	const teak_insn_t *instructions, size_t count
) {
	teak_state_t state = core->state;
	const teak_data_map_t *map = &core->data_map;
	uint32_t cycles = 0;

	/*
	 * A block-repeat loop is the innermost repeat, so only its last instruction
	 * or a branch can complete it: whole iterations check the budget once and
	 * the repeat only there.
	 */
	while (state.pc == instructions[0].address && budget - cycles >= count) {
		TEAK_FAST_UNROLL
		for (size_t i = 0; i < count; i++) {
			if (state.pc != instructions[i].address)
				continue;
			if (!tcg_fast_execute(&state, map, &instructions[i]))
				goto out;
			cycles++;
			if ((i == count - 1 || tcg_fast_is_branch(&instructions[i])) && state.lp)
				tcg_fast_complete_block_repeat(&state, &instructions[i]);
			if (instructions[i].opcode == TEAK_OP_MOV_REGISTER_DATA_RN_STEP &&
				(qatomic_read(&core->state.exit_request) | qatomic_read(&core->state.interrupt_request)) != 0)
				goto out;
		}
	}

	cycles = tcg_fast_steps(core, &state, map, budget, cycles, instructions, count, false);

out:
	state.pending_interrupts = qatomic_read(&core->state.pending_interrupts);
	state.interrupt_lines = qatomic_read(&core->state.interrupt_lines);
	state.interrupt_request = qatomic_read(&core->state.interrupt_request);
	state.exit_request = qatomic_read(&core->state.exit_request);
	core->state = state;
	return cycles;
}

#define TEAK_FAST_LOOP(name, repeat) { tcg_fast_loop_##name, ARRAY_SIZE(tcg_fast_loop_##name), tcg_fast_run_##name, repeat }
#define TEAK_FAST_RUNNER(name) \
	static uint32_t tcg_fast_run_##name(teak_tcg_core_t *core, uint32_t budget) { \
		return tcg_fast_run(core, budget, tcg_fast_loop_##name, ARRAY_SIZE(tcg_fast_loop_##name)); \
	}

/* How many post-increments of @address_register stay linear from @address. */
TEAK_FAST_INLINE uint32_t tcg_fast_linear(const teak_state_t *state, uint8_t address_register, uint32_t address) {
	const teak_modulo_step_t *modulo = &state->modulo_steps[address_register][TEAK_STEP_INCREASE - 1];
	uint32_t low = address & modulo->mask;
	uint32_t linear = modulo->mask - low;

	if (modulo->boundary >= low && modulo->boundary - low < linear)
		linear = modulo->boundary - low;
	return linear;
}

/*
 * One 35BE iteration on guest memory itself, in the order of its
 * instructions, for the correlator's usual shape: r0, r1 and r2 linear, the r4
 * accesses in one flat ring (or linear), saturation off.  No operand
 * copies; false leaves the state alone for the per-instruction runner.
 */
TEAK_FAST_INLINE bool tcg_fast_iterate_35be_flat(teak_state_t *state, const teak_data_map_t *map,
	uint32_t y_space_base
) {
	enum { MULTIPLIES = 26, X_READS = 13, R4_ACCESSES = 30 };
	const teak_modulo_step_t *modulo = &state->modulo_steps[4][TEAK_STEP_INCREASE - 1];
	uint32_t r0 = state->r[0], r1 = state->r[1], r2 = state->r[2], r4 = state->r[4];
	uint32_t low = r4 & modulo->mask;
	uint32_t base, length, index;
	const uint16_t *source, *x_words, *ring_read, *y_words;
	uint16_t *ring, *r1_store[2];
	uint32_t before;
	uint32_t shift = state->product_shift;
	uint32_t right = shift == 1;
	int64_t sum[2] = { 0x8000, 0x8000 };
	uint16_t x = 0, y = 0, value;
	uint32_t product = 0;
	uint64_t stored;

	if (!state->sata || shift > 2)
		return false;
	if (tcg_fast_linear(state, 0, r0) < 4 || tcg_fast_linear(state, 1, r1) < 2 ||
		tcg_fast_linear(state, 2, r2) < X_READS || r2 + X_READS > y_space_base)
		return false;
	if (tcg_fast_linear(state, 4, r4) >= R4_ACCESSES) {
		base = r4;
		length = UINT32_MAX;
		index = 0;
		if (((r4 ^ (r4 + R4_ACCESSES - 1)) >> TEAK_DATA_PAGE_SHIFT) != 0 || r4 + 2 < y_space_base)
			return false;
	} else {
		if (low < modulo->wrap_target || low > modulo->boundary)
			return false;
		base = (r4 & ~modulo->mask) | modulo->wrap_target;
		length = modulo->boundary - modulo->wrap_target + 1;
		index = low - modulo->wrap_target;
		if (length < MULTIPLIES || ((base ^ (base + length - 1)) >> TEAK_DATA_PAGE_SHIFT) != 0 ||
			base < y_space_base)
			return false;
	}
	if ((((r0 ^ (r0 + 3)) | (r2 ^ (r2 + X_READS - 1))) >> TEAK_DATA_PAGE_SHIFT) != 0)
		return false;
	source = map->read_pages[TEAK_DATA_SPACE_ALL][r0 >> TEAK_DATA_PAGE_SHIFT];
	x_words = map->read_pages[TEAK_DATA_SPACE_ALL][r2 >> TEAK_DATA_PAGE_SHIFT];
	ring_read = map->read_pages[TEAK_DATA_SPACE_ALL][base >> TEAK_DATA_PAGE_SHIFT];
	ring = map->write_pages[base >> TEAK_DATA_PAGE_SHIFT];
	r1_store[0] = tcg_fast_write_pointer(map, r1);
	r1_store[1] = tcg_fast_write_pointer(map, r1 + 1);
	/* the ring is read where it is stored */
	if (source == NULL || x_words == NULL || ring == NULL || ring != ring_read || r1_store[0] == NULL ||
		r1_store[1] == NULL)
		return false;
	source += r0 & (TEAK_DATA_PAGE_WORDS - 1);
	x_words += r2 & (TEAK_DATA_PAGE_WORDS - 1);
	ring += base & (TEAK_DATA_PAGE_WORDS - 1);

#define TEAK_35BE_RING_NEXT()	(index = index + 1 == length ? 0 : index + 1)
	ring[index] = source[0];
	TEAK_35BE_RING_NEXT();
	ring[index] = source[1];
	TEAK_35BE_RING_NEXT();

	/*
	 * The 26 Y reads wrap at most once (the ring holds them all), so each
	 * finds its word by itself: stepping the ring index would chain them.
	 */
	y_words = ring + index;
	before = length - index;
	if (shift == 0) {
		TEAK_FAST_UNROLL
		for (uint32_t i = 0; i < MULTIPLIES; i++) {
			x = x_words[i / 2];
			y = i < before ? y_words[i] : ring[i - before];
			product = (uint32_t) ((int32_t) (int16_t) x * (int32_t) (int16_t) y);
			if (i < MULTIPLIES - 1)
				sum[i & 1] += (int64_t) (int32_t) product;
		}
	} else {
		shift -= 1;
		TEAK_FAST_UNROLL
		for (uint32_t i = 0; i < MULTIPLIES; i++) {
			x = x_words[i / 2];
			y = i < before ? y_words[i] : ring[i - before];
			product = (uint32_t) ((int32_t) (int16_t) x * (int32_t) (int16_t) y);
			if (i < MULTIPLIES - 1)
				sum[i & 1] += (int64_t) ((uint64_t) (int64_t) (int32_t) product << shift) >> right;
		}
	}
	index += MULTIPLIES;
	if (index >= length)
		index -= length;
	state->x[0] = x;
	state->y[0] = y;
	state->p[0] = product;
	state->product_extension[0] = product >> 31;
	state->a[0] = sum[0];
	state->a[1] = sum[1];
	tcg_fast_alu_accumulator(state, TEAK_ALU_ADD, 1, tcg_fast_shifted_product(state));
	stored = tcg_fast_saturate(state->a[0], state->sat, &state->flm);
	*r1_store[0] = stored >> 16;
	stored = tcg_fast_saturate(state->a[1], state->sat, &state->flm);
	*r1_store[1] = stored >> 16;

	value = source[2];
	ring[index] = value;
	TEAK_35BE_RING_NEXT();
	value = source[3];
	state->a[0] = (uint64_t) (int64_t) (int16_t) value;
	tcg_fast_value_flags(state, state->a[0]);
	ring[index] = value;
	TEAK_35BE_RING_NEXT();
#undef TEAK_35BE_RING_NEXT

	state->r[0] = r0 + 4;
	state->r[1] = r1 + 2;
	state->r[2] = tcg_fast_rn_next(state, 2, TEAK_STEP_PLUS_STEP, false, r2 + X_READS - 1);
	state->r[4] = length == UINT32_MAX ? r4 + R4_ACCESSES : base + index;
	return true;
}

/*
 * Whole iterations of the 35BE correlator in its usual shape
 * (tcg_fast_iterate_35be_flat()): the budget and the exit flags are checked
 * once at the loop head, and only the flags that survive to the branch are
 * computed.  An iteration the checks or the flat iteration refuse is left to
 * the per-instruction runner.
 */
static uint32_t tcg_fast_iterate_35be(teak_tcg_core_t *core, uint32_t budget) {
	enum { ITERATION_CYCLES = ARRAY_SIZE(tcg_fast_loop_35be) };
	teak_state_t state = core->state;
	const teak_data_map_t *map = &core->data_map;
	uint32_t y_space_base = core->memory.y_space_base;
	uint32_t cycles = 0;

	QEMU_BUILD_BUG_ON(ITERATION_CYCLES != 41);
	/*
	 * A block repeat completes after the instruction at its end; iterations
	 * run 35BE..35E6.
	 */
	while (state.pc == 0x35BE && budget - cycles >= ITERATION_CYCLES &&
		!(state.lp &&
			(uint16_t) (state.block_repeat_end[teak_block_repeat_slot(state.bcn)] + 1 - 0x35BE) <= 0x35E7 - 0x35BE) &&
		(qatomic_read(&core->state.exit_request) | qatomic_read(&core->state.interrupt_request)) == 0) {
		if (!tcg_fast_iterate_35be_flat(&state, map, y_space_base))
			break;
		state.r[5] = tcg_fast_rn_next(&state, 5, TEAK_STEP_DECREASE, false, state.r[5]);
		state.fr = state.r[5] == 0;
		if (state.fr == 0) {
			state.pc = 0x35BE;
			state.exit_reason = TEAK_EXIT_BRANCH;
		} else {
			state.pc = 0x35E7;
			state.exit_reason = TEAK_EXIT_NONE;
		}
		cycles += ITERATION_CYCLES;
	}

	if (cycles != 0) {
		state.pending_interrupts = qatomic_read(&core->state.pending_interrupts);
		state.interrupt_lines = qatomic_read(&core->state.interrupt_lines);
		state.interrupt_request = qatomic_read(&core->state.interrupt_request);
		state.exit_request = qatomic_read(&core->state.exit_request);
		core->state = state;
	}
	return cycles;
}

/* A run of a few instructions: on core->state itself, without the copies of tcg_fast_run(). */
static uint32_t tcg_fast_step_35be(teak_tcg_core_t *core, uint32_t budget) {
	return tcg_fast_steps(core, &core->state, &core->data_map, budget, 0, tcg_fast_loop_35be,
		ARRAY_SIZE(tcg_fast_loop_35be), true);
}

/*
 * The per-instruction runner takes ~1 % of the 35BE cycles.  Called through
 * a volatile pointer, neither LLVM nor Binaryen inlines it: in one function
 * with the iterations, its instruction switch cost TurboFan the registers of
 * the iteration loop (SL65 video: the kernel 24.4 -> ~20 % of the vCPU).
 */
static uint32_t (*volatile tcg_fast_step_35be_fn)(teak_tcg_core_t *core, uint32_t budget) = tcg_fast_step_35be;

static uint32_t tcg_fast_run_35be(teak_tcg_core_t *core, uint32_t budget) {
	uint32_t cycles = 0;

	for (;;) {
		uint32_t run;

		cycles += tcg_fast_iterate_35be(core, budget - cycles);
		run = tcg_fast_step_35be_fn(core, budget - cycles);
		cycles += run;
		if (run == 0 || core->state.pc != 0x35BE)
			return cycles;
	}
}

TEAK_FAST_RUNNER(643f)
TEAK_FAST_RUNNER(6489)
TEAK_FAST_RUNNER(64a9)

static const teak_tcg_fast_loop_t tcg_fast_loops[] = {
	TEAK_FAST_LOOP(35be, false),
	TEAK_FAST_LOOP(643f, true),
	TEAK_FAST_LOOP(6489, true),
	TEAK_FAST_LOOP(64a9, true),
};

/* A loop at the addresses of its table (delta 0), or where another firmware placed the same code. */
typedef struct teak_tcg_fast_site_t {
	const teak_tcg_fast_loop_t *loop;
	uint32_t delta;
	int8_t verified;
} teak_tcg_fast_site_t;

/* Bumped whenever tcg_fast_map changes. */
static uint32_t tcg_fast_map_epoch = 1;
static teak_tcg_fast_site_t tcg_fast_sites[32];
static size_t tcg_fast_site_count;
static uint64_t tcg_fast_cache_id;
static uint32_t tcg_fast_program_context;
static bool tcg_fast_ready;

static bool tcg_fast_same_instruction(const teak_insn_t *left, const teak_insn_t *right) {
	return left->opcode == right->opcode && left->address == right->address && left->word == right->word &&
		left->expansion == right->expansion && left->immediate == right->immediate &&
		left->memory_offset == right->memory_offset && left->branch_target == right->branch_target &&
		left->alu_operand == right->alu_operand && left->words == right->words &&
		left->register_code == right->register_code &&
		left->destination_register_code == right->destination_register_code &&
		left->address_register == right->address_register && left->y_address_register == right->y_address_register &&
		left->accumulator_index == right->accumulator_index &&
		left->source_accumulator == right->source_accumulator &&
		left->destination_accumulator == right->destination_accumulator &&
		left->memory_address == right->memory_address && left->bit_index == right->bit_index &&
		left->accumulator_test == right->accumulator_test && left->alu_operation == right->alu_operation &&
		left->moda_operation == right->moda_operation && left->exponent_source == right->exponent_source &&
		left->minmax_operation == right->minmax_operation && left->write_accumulator == right->write_accumulator &&
		left->memory_source == right->memory_source &&
		left->alb_operation == right->alb_operation && left->multiply_operation == right->multiply_operation &&
		left->condition == right->condition && left->step == right->step && left->y_step == right->y_step &&
		left->swap_operation == right->swap_operation && left->special_register == right->special_register &&
		left->shift == right->shift && left->disable_modulo == right->disable_modulo &&
		left->context_switch == right->context_switch;
}

static bool tcg_fast_verify(teak_tcg_core_t *core, const teak_tcg_fast_site_t *site) {
	const teak_tcg_fast_loop_t *loop = site->loop;

	for (size_t i = 0; i < loop->count; i++) {
		teak_insn_t expected = loop->instructions[i];
		teak_insn_t instruction;

		expected.address += site->delta;
		if (expected.branch_target != 0)
			expected.branch_target += site->delta;
		memset(&instruction, 0, sizeof(instruction));
		if (!teak_decode(core, expected.address, &instruction) || !tcg_fast_same_instruction(&instruction, &expected))
			return false;
	}
	return true;
}

static void tcg_fast_invalidate(void) {
	for (size_t i = 0; i < tcg_fast_site_count; i++)
		tcg_fast_sites[i].verified = 0;
}

static const teak_tcg_fast_site_t *tcg_fast_lookup(teak_tcg_core_t *core) {
	const teak_tcg_fast_site_t *site;
	const teak_tcg_fast_loop_t *loop;
	const teak_state_t *state = &core->state;
	uint8_t index = tcg_fast_map[state->pc & TEAK_PROGRAM_ADDRESS_MASK];
	uint32_t first;
	uint32_t last;
	bool inside_repeat;

	if (index == 0)
		return NULL;
	if (tcg_fast_cache_id != core->cache_id || tcg_fast_program_context != core->program_context) {
		tcg_fast_cache_id = core->cache_id;
		tcg_fast_program_context = core->program_context;
		tcg_fast_invalidate();
	}
	site = &tcg_fast_sites[index - 1];
	loop = site->loop;
	if (site->verified == 0)
		tcg_fast_sites[index - 1].verified = tcg_fast_verify(core, site) ? 1 : -1;
	if (site->verified < 0 || state->repeat_active || state->fetch_split_pending)
		return NULL;

	first = loop->instructions[0].address + site->delta;
	last = loop->instructions[loop->count - 1].address + site->delta;
	inside_repeat = state->lp;
	if (loop->block_repeat) {
		uint8_t level = teak_block_repeat_slot(state->bcn);

		if (!inside_repeat || state->bcn == 0 || state->bcn > TEAK_BLOCK_REPEAT_LEVELS || first == last ||
			state->block_repeat_start[level] != first || state->block_repeat_end[level] != last)
			return NULL;
	} else if (inside_repeat) {
		uint32_t end = state->block_repeat_end[teak_block_repeat_slot(state->bcn)];

		if (end >= first && end <= last + loop->instructions[loop->count - 1].words - 1)
			return NULL;
	}
	return site;
}

static void tcg_fast_init(void) {
	if (tcg_fast_ready)
		return;
	tcg_fast_ready = true;
	QEMU_BUILD_BUG_ON(ARRAY_SIZE(tcg_fast_loops) > ARRAY_SIZE(tcg_fast_sites));
	for (size_t i = 0; i < ARRAY_SIZE(tcg_fast_loops); i++) {
		tcg_fast_sites[i].loop = &tcg_fast_loops[i];
		for (size_t j = 0; j < tcg_fast_loops[i].count; j++) {
			g_assert(tcg_fast_supported(&tcg_fast_loops[i].instructions[j]));
			tcg_fast_map[tcg_fast_loops[i].instructions[j].address] = i + 1;
		}
	}
	tcg_fast_map_epoch++;
	tcg_block_cache_generation++;
	tcg_fast_site_count = ARRAY_SIZE(tcg_fast_loops);
}

static bool tcg_fast_words_at(teak_tcg_core_t *core, const teak_tcg_fast_loop_t *loop, uint32_t delta) {
	for (size_t i = 0; i < loop->count; i++) {
		const teak_insn_t *instruction = &loop->instructions[i];
		uint32_t address = instruction->address + delta;

		if (teak_program_read(core, address & TEAK_PROGRAM_ADDRESS_MASK) != instruction->word ||
			(instruction->words == 2 &&
				teak_program_read(core, (address + 1) & TEAK_PROGRAM_ADDRESS_MASK) != instruction->expansion))
			return false;
	}
	return true;
}

/* Maps a loop the firmware placed elsewhere when a block compiled at @pc starts its code. */
static void tcg_fast_discover(teak_tcg_core_t *core, uint32_t pc) {
	/* a new program is precompiled before its first slice */
	tcg_fast_init();
	for (size_t i = 0; i < ARRAY_SIZE(tcg_fast_loops); i++) {
		const teak_tcg_fast_loop_t *loop = &tcg_fast_loops[i];
		uint32_t delta = pc - loop->instructions[0].address;
		size_t index;

		if (!tcg_fast_words_at(core, loop, delta))
			continue;
		for (index = 0; index < tcg_fast_site_count; index++)
			if (tcg_fast_sites[index].loop == loop && tcg_fast_sites[index].delta == delta)
				break;
		if (index == ARRAY_SIZE(tcg_fast_sites))
			return;
		if (index == tcg_fast_site_count)
			tcg_fast_site_count++;
		tcg_fast_sites[index] = (teak_tcg_fast_site_t) { .loop = loop, .delta = delta };
		for (size_t k = 0; k < loop->count; k++)
			tcg_fast_map[(loop->instructions[k].address + delta) & TEAK_PROGRAM_ADDRESS_MASK] = index + 1;
		/* tcg_cache_block() bumped tcg_block_cache_generation just before */
		tcg_fast_map_epoch++;
		return;
	}
}

/* Moves the program addresses in @state by @delta. */
static void tcg_fast_relocate(teak_state_t *state, uint32_t delta) {
	state->pc += delta;
	state->trace_pc += delta;
	state->suspended_repeat_pc += delta;
	for (size_t i = 0; i < TEAK_BLOCK_REPEAT_LEVELS; i++) {
		state->block_repeat_start[i] += delta;
		state->block_repeat_end[i] += delta;
	}
}

/* Runs the loop in the addresses of its table. */
static uint32_t tcg_fast_site_run(teak_tcg_core_t *core, const teak_tcg_fast_site_t *site, uint32_t budget) {
	uint32_t cycles;

	if (site->delta == 0)
		return site->loop->run(core, budget);
	tcg_fast_relocate(&core->state, -site->delta);
	cycles = site->loop->run(core, budget);
	tcg_fast_relocate(&core->state, site->delta);
	return cycles;
}

static bool tcg_execute_fast(teak_tcg_core_t *core, const teak_tcg_fast_site_t *site, size_t max_cycles) {
	uint32_t cycles = tcg_fast_site_run(core, site, max_cycles - core->last_block_cycles);

	if (cycles == 0)
		return false;
	core->last_block_cycles += cycles;
	core->fast_runs++;
	return true;
}
