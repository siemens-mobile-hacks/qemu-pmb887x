#include "qemu/osdep.h"
#include "accel/tcg/tb-context.h"
#include "cpu.h"
#include "exec/tb-flush.h"
#include "exec/translation-block.h"
#include "tcg/tcg.h"
#include "tcg/tcg-op-common.h"

#include "hw/arm/pmb887x/dsp/tcg.h"
#include "hw/arm/pmb887x/dsp/core.h"

#define HELPER_H "hw/arm/pmb887x/dsp/tcg-helper.h"
#include "exec/helper-proto.h.inc"
#include "exec/helper-gen.h.inc"

#undef HELPER_H

#define TEAK_TCG_MAX_BLOCK_INSTRUCTIONS	64
#define TEAK_TCG_BLOCK_CACHE_ENTRIES	((size_t) UINT16_MAX + 1)
#define TEAK_ACCUMULATOR_BITS	36
#define TEAK_ACCUMULATOR_SIGN_BIT	(TEAK_ACCUMULATOR_BITS - 1)
#define TEAK_ACCUMULATOR_HOST_SHIFT	(64 - TEAK_ACCUMULATOR_BITS)
#define TEAK_ACCUMULATOR_MASK	0xFFFFFFFFFULL
#define TEAK_ACCUMULATOR_MAX	0x7FFFFFFFFULL
#define TEAK_ACCUMULATOR_SIGN	0x800000000ULL
#define TEAK_ACCUMULATOR_MIN	0xFFFFFFF800000000ULL
#define TEAK_ST1_RESERVED_READ_BITS	0x0300U
#define TEAK_ST2_RESERVED_READ_BITS	0x1000U
#define TEAK_BANK_CFGI	BIT(0)
#define TEAK_BANK_R4	BIT(1)
#define TEAK_BANK_R1	BIT(2)
#define TEAK_BANK_R0	BIT(3)
#define TEAK_INTERRUPT_NMI	3
#define TEAK_OPCODE_RETID	0xD7C0U
#define TEAK_TCG_COMPILE_RETRY	-3

typedef struct teak_tcg_block_t teak_tcg_block_t;
typedef struct teak_tcg_block_cache_entry_t teak_tcg_block_cache_entry_t;
typedef struct teak_tcg_swap_mapping_t teak_tcg_swap_mapping_t;
typedef void teak_tcg_emit_fn(void *opaque);

enum teak_tcg_accumulator_slot_t {
	TEAK_TCG_ACCUMULATOR_A0,
	TEAK_TCG_ACCUMULATOR_A1,
	TEAK_TCG_ACCUMULATOR_B0,
	TEAK_TCG_ACCUMULATOR_B1,
	TEAK_TCG_ACCUMULATOR_COUNT,
};

typedef enum teak_tcg_accumulator_slot_t teak_tcg_accumulator_slot_t;

struct teak_tcg_swap_mapping_t {
	teak_tcg_accumulator_slot_t destination_sources[TEAK_TCG_ACCUMULATOR_COUNT];
	teak_tcg_accumulator_slot_t flags_source;
};

struct teak_tcg_block_t {
	teak_insn_t instructions[TEAK_TCG_MAX_BLOCK_INSTRUCTIONS];
	uint8_t block_repeat_setup_level[TEAK_TCG_MAX_BLOCK_INSTRUCTIONS];
	uint32_t pc;
	uint16_t words;
	uint16_t instruction_count;
};

struct teak_tcg_block_cache_entry_t {
	teak_tcg_block_t block;
	TranslationBlock *tb;
	uint64_t cache_id;
	uint32_t block_repeat_end[TEAK_BLOCK_REPEAT_LEVELS];
	uint8_t block_repeat_level;
	teak_tcg_block_cache_entry_t *next;
};

static teak_tcg_block_cache_entry_t *tcg_block_cache[TEAK_TCG_BLOCK_CACHE_ENTRIES];
static TCGContext *tcg_block_cache_context;
static unsigned int tcg_block_cache_flush_count;
static TCGv_i32 tcg_memory_pc;
static TCGv_i32 tcg_memory_cycle;
static uint32_t tcg_memory_access;

static const uint16_t teak_interrupt_vectors[] = { 0x0006, 0x000E, 0x0016, 0x0004 };

typedef enum teak_tcg_data_space_t {
	TEAK_TCG_DATA_SPACE_ALL,
	TEAK_TCG_DATA_SPACE_XZ,
	TEAK_TCG_DATA_SPACE_Y,
} teak_tcg_data_space_t;

static TCGv_ptr tcg_emit_direct_data_pointer(TCGv_i32 address) {
	TCGv_i32 offset = tcg_temp_new_i32();
	TCGv_ptr base = tcg_temp_new_ptr();
	TCGv_ptr pointer = tcg_temp_new_ptr();

	tcg_gen_shli_i32(offset, address, 1);
	tcg_gen_ext_i32_ptr(pointer, offset);
	tcg_gen_ld_ptr(base, tcg_env, offsetof(teak_tcg_core_t, memory.direct_data));
	tcg_gen_add_ptr(pointer, pointer, base);
	return pointer;
}

static void tcg_emit_data_read(TCGv_i32 result, TCGv_i32 address, teak_tcg_data_space_t space) {
	TCGLabel *slow = gen_new_label();
	TCGLabel *done = gen_new_label();
	TCGLabel *zero = space == TEAK_TCG_DATA_SPACE_ALL ? NULL : gen_new_label();
	TCGv_i32 limit = tcg_temp_new_i32();
	uint32_t access = tcg_memory_access++;

	if (space != TEAK_TCG_DATA_SPACE_ALL) {
		TCGv_i32 y_space_base = tcg_temp_new_i32();
		TCGCond condition = space == TEAK_TCG_DATA_SPACE_XZ ? TCG_COND_GEU : TCG_COND_LTU;

		tcg_gen_ld16u_i32(y_space_base, tcg_env, offsetof(teak_tcg_core_t, memory.y_space_base));
		tcg_gen_brcond_i32(condition, address, y_space_base, zero);
	}

	tcg_gen_ld_i32(limit, tcg_env, offsetof(teak_tcg_core_t, memory.direct_data_read_size));
	tcg_gen_brcond_i32(TCG_COND_GEU, address, limit, slow);
	tcg_gen_ld16u_i32(result, tcg_emit_direct_data_pointer(address), 0);
	tcg_gen_br(done);

	gen_set_label(slow);
	switch (space) {
		case TEAK_TCG_DATA_SPACE_ALL:
			gen_helper_teak_tcg_data_read_at(result, tcg_env, address, tcg_memory_pc, tcg_memory_cycle,
				tcg_constant_i32(access));
			break;

		case TEAK_TCG_DATA_SPACE_XZ:
			gen_helper_teak_tcg_data_read_xz_at(result, tcg_env, address, tcg_memory_pc, tcg_memory_cycle,
				tcg_constant_i32(access));
			break;

		case TEAK_TCG_DATA_SPACE_Y:
			gen_helper_teak_tcg_data_read_y_at(result, tcg_env, address, tcg_memory_pc, tcg_memory_cycle,
				tcg_constant_i32(access));
			break;
	}
	tcg_gen_br(done);

	if (zero != NULL) {
		gen_set_label(zero);
		tcg_gen_movi_i32(result, 0);
	}
	gen_set_label(done);
}

static void tcg_emit_data_write(TCGv_i32 address, TCGv_i32 value) {
	TCGLabel *slow = gen_new_label();
	TCGLabel *done = gen_new_label();
	TCGv_i32 limit = tcg_temp_new_i32();
	uint32_t access = tcg_memory_access++;

	tcg_gen_ld_i32(limit, tcg_env, offsetof(teak_tcg_core_t, memory.direct_data_write_size));
	tcg_gen_brcond_i32(TCG_COND_GEU, address, limit, slow);
	tcg_gen_st16_i32(value, tcg_emit_direct_data_pointer(address), 0);
	tcg_gen_br(done);

	gen_set_label(slow);
	gen_helper_teak_tcg_data_write_at(tcg_env, address, value, tcg_memory_pc, tcg_memory_cycle,
		tcg_constant_i32(access));
	gen_set_label(done);
}

#define gen_helper_teak_tcg_data_read(result, env, address) \
	tcg_emit_data_read(result, address, TEAK_TCG_DATA_SPACE_ALL)
#define gen_helper_teak_tcg_data_read_xz(result, env, address) \
	tcg_emit_data_read(result, address, TEAK_TCG_DATA_SPACE_XZ)
#define gen_helper_teak_tcg_data_read_y(result, env, address) \
	tcg_emit_data_read(result, address, TEAK_TCG_DATA_SPACE_Y)
#define gen_helper_teak_tcg_data_write(env, address, value) tcg_emit_data_write(address, value)

static uint16_t tcg_pack_shadow_st0(const teak_state_t *state) {
	uint16_t limit = state->flm | state->fvl;
	uint16_t shadow = state->sat | (state->interrupt_mask & 3U) << 2 | state->fr << 4 | limit << 5;
	shadow |= state->fe << 6 | state->fc0 << 7 | state->fv << 8;
	shadow |= state->fn << 9 | state->fm << 10 | state->fz << 11;
	return shadow;
}

static uint16_t tcg_pack_shadow_st1(const teak_state_t *state) {
	return state->product_shift << 10;
}

static uint16_t tcg_pack_shadow_st2(const teak_state_t *state) {
	return state->modulo_enable | (state->interrupt_mask & 4U) << 4 | state->s << 7;
}

static void tcg_restore_shadow_registers(teak_state_t *state) {
	uint8_t limit = state->shadow_st0 >> 5 & 1U;
	state->sat = state->shadow_st0 & 1U;
	state->interrupt_mask = state->shadow_st0 >> 2 & 3U;
	state->interrupt_mask |= state->shadow_st2 >> 4 & 4U;
	state->fr = state->shadow_st0 >> 4 & 1U;
	state->flm = limit;
	state->fvl = limit;
	state->fe = state->shadow_st0 >> 6 & 1U;
	state->fc0 = state->shadow_st0 >> 7 & 1U;
	state->fv = state->shadow_st0 >> 8 & 1U;
	state->fn = state->shadow_st0 >> 9 & 1U;
	state->fm = state->shadow_st0 >> 10 & 1U;
	state->fz = state->shadow_st0 >> 11 & 1U;
	state->product_shift = state->shadow_st1 >> 10 & 3U;
	state->modulo_enable = state->shadow_st2 & 0x3FU;
	state->s = state->shadow_st2 >> 7 & 1U;
}

static void tcg_swap_context_registers(teak_state_t *state) {
	uint64_t accumulator = state->a[1];
	uint8_t page = state->page;
	state->a[1] = state->b[1];
	state->b[1] = accumulator;
	state->page = state->alternate_page;
	state->alternate_page = page;
}

static void tcg_set_accumulator_value_flags(teak_state_t *state, uint64_t value) {
	uint64_t canonical = value & TEAK_ACCUMULATOR_MASK;
	uint64_t signed32 = (uint64_t) (int64_t) (int32_t) canonical & TEAK_ACCUMULATOR_MASK;
	uint8_t bit31 = canonical >> 31 & 1U;
	uint8_t bit30 = canonical >> 30 & 1U;

	state->fz = canonical == 0;
	state->fm = canonical >> TEAK_ACCUMULATOR_SIGN_BIT & 1U;
	state->fe = canonical != signed32;
	state->fn = state->fz || (!state->fe && bit31 != bit30);
}

static uint64_t *tcg_accumulator(teak_state_t *state, uint8_t accumulator) {
	uint64_t *bank = accumulator < 2 ? state->b : state->a;
	return &bank[accumulator & 1U];
}

static int64_t tcg_sign_extend_accumulator(uint64_t value) {
	return (int64_t) (value << TEAK_ACCUMULATOR_HOST_SHIFT) >> TEAK_ACCUMULATOR_HOST_SHIFT;
}

static int64_t tcg_data_bus_saturate(teak_state_t *state, uint64_t value) {
	int64_t signed_value = tcg_sign_extend_accumulator(value);
	bool representable = signed_value >= INT32_MIN && signed_value <= INT32_MAX;
	if (state->sat || representable)
		return signed_value;
	state->flm = 1;
	return signed_value < 0 ? INT32_MIN : INT32_MAX;
}

static void tcg_set_accumulator_extension(uint64_t *accumulator, uint16_t extension) {
	uint64_t value = *accumulator & UINT32_MAX;
	value |= (uint64_t) (extension & 0xFU) << 32;
	*accumulator = (uint64_t) tcg_sign_extend_accumulator(value);
}

static uint16_t tcg_pack_st0(const teak_state_t *state) {
	uint16_t limit = state->flm | state->fvl;
	uint16_t value = state->sat | state->ie << 1 | (state->interrupt_mask & 3U) << 2 | state->fr << 4;
	value |= limit << 5 | state->fe << 6 | state->fc0 << 7 | state->fv << 8;
	value |= state->fn << 9 | state->fm << 10 | state->fz << 11;
	value |= (state->a[0] >> 32 & 0xFU) << 12;
	return value;
}

static uint16_t tcg_pack_st1(const teak_state_t *state) {
	return TEAK_ST1_RESERVED_READ_BITS | state->page | state->product_shift << 10 |
		(state->a[1] >> 32 & 0xFU) << 12;
}

static uint32_t tcg_pending_interrupts(const teak_state_t *state) {
	return qatomic_read(&state->pending_interrupts) | qatomic_read(&state->interrupt_lines);
}

static uint16_t tcg_pack_st2(const teak_state_t *state) {
	uint16_t value = TEAK_ST2_RESERVED_READ_BITS | state->modulo_enable |
		(state->interrupt_mask & 4U) << 4 | state->s << 7;
	uint32_t pending = tcg_pending_interrupts(state);
	value |= state->ou[0] << 8 | state->ou[1] << 9 | state->iu[0] << 10 | state->iu[1] << 11;
	value |= (pending >> 2 & 1U) << 13;
	value |= (pending & 1U) << 14;
	value |= (pending >> 1 & 1U) << 15;
	return value;
}

static uint16_t tcg_pack_icr(const teak_state_t *state) {
	return 0xFF00U | state->nonmaskable_context | state->interrupt_context << 1 | state->lp << 4 | state->bcn << 5;
}

static void tcg_unpack_st0(teak_state_t *state, uint16_t value) {
	uint8_t limit = value >> 5 & 1U;
	uint8_t interrupt_mask = state->interrupt_mask & 4U;
	state->sat = value & 1U;
	state->ie = value >> 1 & 1U;
	interrupt_mask |= value >> 2 & 3U;
	state->interrupt_mask = interrupt_mask;
	state->fr = value >> 4 & 1U;
	state->flm = limit;
	state->fvl = limit;
	state->fe = value >> 6 & 1U;
	state->fc0 = value >> 7 & 1U;
	state->fv = value >> 8 & 1U;
	state->fn = value >> 9 & 1U;
	state->fm = value >> 10 & 1U;
	state->fz = value >> 11 & 1U;
	tcg_set_accumulator_extension(&state->a[0], value >> 12);
}

static void tcg_unpack_st1(teak_state_t *state, uint16_t value) {
	state->page = value;
	state->product_shift = value >> 10 & 3U;
	tcg_set_accumulator_extension(&state->a[1], value >> 12);
}

static void tcg_unpack_st2(teak_state_t *state, uint16_t value) {
	uint8_t interrupt_mask = state->interrupt_mask & 3U;
	state->modulo_enable = value & 0x3FU;
	interrupt_mask |= value >> 4 & 4U;
	state->interrupt_mask = interrupt_mask;
	state->s = value >> 7 & 1U;
	state->ou[0] = value >> 8 & 1U;
	state->ou[1] = value >> 9 & 1U;
}

static void tcg_unpack_icr(teak_state_t *state, uint16_t value) {
	state->nonmaskable_context = value & 1U;
	state->interrupt_context = value >> 1 & 7U;
	if ((value & 0x10U) != 0) {
		state->lp = 0;
		state->bcn = 0;
	}
}

static uint16_t tcg_read_special_register(const teak_state_t *state, teak_special_register_t special_register) {
	switch (special_register) {
		case TEAK_SPECIAL_REPC:
			return state->repc;
		case TEAK_SPECIAL_DVM:
			return state->dvm;
		case TEAK_SPECIAL_ICR:
			return tcg_pack_icr(state);
		case TEAK_SPECIAL_X0:
			return state->x[0];
		case TEAK_SPECIAL_X1:
			return state->x[1];
		case TEAK_SPECIAL_Y1:
			return state->y[1];
		case TEAK_SPECIAL_MIXP:
			return state->mixp;
	}
	g_assert_not_reached();
}

static void tcg_write_special_register(teak_state_t *state, teak_special_register_t special_register, uint16_t value) {
	switch (special_register) {
		case TEAK_SPECIAL_REPC:
			state->repc = value;
			return;
		case TEAK_SPECIAL_DVM:
			state->dvm = value;
			return;
		case TEAK_SPECIAL_ICR:
			tcg_unpack_icr(state, value);
			return;
		case TEAK_SPECIAL_X0:
			state->x[0] = value;
			return;
		case TEAK_SPECIAL_X1:
			state->x[1] = value;
			return;
		case TEAK_SPECIAL_Y1:
			state->y[1] = value;
			return;
		case TEAK_SPECIAL_MIXP:
			state->mixp = value;
			return;
	}
	g_assert_not_reached();
}

static void tcg_context_store(teak_state_t *state) {
	state->shadow_st0 = tcg_pack_shadow_st0(state);
	state->shadow_st1 = tcg_pack_shadow_st1(state);
	state->shadow_st2 = tcg_pack_shadow_st2(state);
	tcg_swap_context_registers(state);
	tcg_set_accumulator_value_flags(state, state->a[1]);
}

static void tcg_context_restore(teak_state_t *state) {
	tcg_restore_shadow_registers(state);
	tcg_swap_context_registers(state);
}

void teak_tcg_request_interrupt(teak_tcg_core_t *core, uint8_t interrupt) {
	g_assert(interrupt <= TEAK_INTERRUPT_NMI);

	qatomic_or(&core->state.pending_interrupts, BIT(interrupt));
	qatomic_set(&core->state.interrupt_request, 1);
}

void teak_tcg_set_interrupt(teak_tcg_core_t *core, uint8_t interrupt, bool level) {
	g_assert(interrupt < TEAK_INTERRUPT_NMI);

	if (level) {
		qatomic_or(&core->state.pending_interrupts, BIT(interrupt));
		qatomic_set(&core->state.interrupt_request, 1);
	} else {
		qatomic_and(&core->state.pending_interrupts, (uint8_t) ~BIT(interrupt));
	}
}

void teak_tcg_update_irq_lines(teak_tcg_core_t *core, uint8_t lines) {
	lines &= 7U;
	qatomic_set(&core->state.interrupt_lines, lines);
	if (lines != 0)
		qatomic_set(&core->state.interrupt_request, 1);
}

void teak_tcg_request_exit(teak_tcg_core_t *core) {
	qatomic_set(&core->state.exit_request, 1);
}

bool teak_tcg_service_interrupt(teak_tcg_core_t *core) {
	teak_state_t *state = &core->state;
	uint32_t pending;
	uint8_t interrupt;
	bool context_switch;

	if (state->trap_active || state->nmi_active)
		return false;
	pending = tcg_pending_interrupts(state);
	if ((pending & BIT(TEAK_INTERRUPT_NMI)) != 0) {
		interrupt = TEAK_INTERRUPT_NMI;
		context_switch = state->nonmaskable_context;
		state->nmi_active = 1;
	} else {
		pending &= state->interrupt_mask;
		pending &= 7;
		if (!state->ie || pending == 0)
			return false;
		interrupt = (uint8_t) ctz32(pending);
		context_switch = (state->interrupt_context & BIT(interrupt)) != 0;
		qatomic_set(&state->maskable_interrupt_active, 1);
		state->ie = 0;
	}

	qatomic_and(&state->pending_interrupts, (uint8_t) ~BIT(interrupt));
	state->sp--;
	teak_data_write(core, state->sp, (uint16_t) state->pc);
	if (context_switch)
		tcg_context_store(state);
	state->pc = teak_interrupt_vectors[interrupt];
	state->exit_reason = TEAK_EXIT_INTERRUPT;
	return true;
}

static uint16_t tcg_read_register(teak_state_t *state, uint8_t register_code) {
	teak_tcg_core_t *core = container_of(state, teak_tcg_core_t, state);
	uint64_t accumulator;

	if (register_code < 6)
		return state->r[register_code];
	if (register_code == 6)
		return state->r[7];
	if (register_code == 7)
		return state->y[0];

	switch (register_code) {
		case 8:
			return tcg_pack_st0(state);
		case 9:
			return tcg_pack_st1(state);
		case 10:
			return tcg_pack_st2(state);
		case 12:
			return state->pc;
		case 13:
			return state->sp;
		case 14:
			return state->stepi | state->modi << 7;
		case 15:
			return state->stepj | state->modj << 7;
		case 20:
		case 21:
		case 22:
		case 23:
			if (core->memory.external.read == NULL)
				return 0;
			return core->memory.external.read(core->memory.external.opaque, register_code - 20);
		case 24:
		case 25:
			return state->a[register_code & 1U];
		case 30:
			if (state->lp) {
				g_assert(state->bcn != 0);
				return state->block_repeat_lc[state->bcn - 1];
			}
			return state->block_repeat_lc[0];
		case 31:
			return state->shift_value;
	}

	if (register_code >= 16 && register_code <= 19) {
		accumulator = state->b[register_code & 1U];
		return register_code < 18 ? accumulator >> 16 : accumulator;
	}

	g_assert(register_code >= 26 && register_code <= 29);
	accumulator = state->a[register_code & 1U];
	return register_code < 28 ? accumulator : accumulator >> 16;
}

static void tcg_write_accumulator_half(uint64_t *accumulator, uint16_t value, bool high) {
	uint64_t canonical = *accumulator & TEAK_ACCUMULATOR_MASK;
	if (high) {
		canonical &= ~0xFFFF0000ULL;
		canonical |= (uint64_t) value << 16;
	} else {
		canonical &= ~0xFFFFULL;
		canonical |= value;
	}
	*accumulator = (uint64_t) tcg_sign_extend_accumulator(canonical);
}

static void tcg_write_register(teak_state_t *state, uint8_t register_code, uint16_t value) {
	teak_tcg_core_t *core = container_of(state, teak_tcg_core_t, state);

	if (register_code < 6) {
		state->r[register_code] = value;
		return;
	}
	if (register_code == 6) {
		state->r[7] = value;
		return;
	}
	if (register_code == 7) {
		state->y[0] = value;
		return;
	}

	switch (register_code) {
		case 8:
			tcg_unpack_st0(state, value);
			return;
		case 9:
			tcg_unpack_st1(state, value);
			return;
		case 10:
			tcg_unpack_st2(state, value);
			return;
		case 11:
			state->product_extension[0] = value >> 15;
			state->p[0] = (state->p[0] & 0xFFFFU) | (uint32_t) value << 16;
			return;
		case 12:
			g_assert_not_reached();
		case 13:
			state->sp = value;
			return;
		case 14:
			state->stepi = value & 0x7FU;
			state->modi = value >> 7;
			return;
		case 15:
			state->stepj = value & 0x7FU;
			state->modj = value >> 7;
			return;
		case 20:
		case 21:
		case 22:
		case 23:
			if (core->memory.external.write != NULL)
				core->memory.external.write(core->memory.external.opaque, register_code - 20, value);
			return;
		case 30:
			if (state->lp) {
				g_assert(state->bcn != 0);
				state->block_repeat_lc[state->bcn - 1] = value;
			} else {
				state->block_repeat_lc[0] = value;
			}
			return;
		case 31:
			state->shift_value = value;
			return;
	}
	if (register_code == 24 || register_code == 25) {
		state->a[register_code & 1U] = (int16_t) value;
		tcg_set_accumulator_value_flags(state, state->a[register_code & 1U]);
		return;
	}

	if (register_code >= 16 && register_code <= 19) {
		tcg_write_accumulator_half(&state->b[register_code & 1U], value, register_code < 18);
		return;
	}

	g_assert(register_code >= 26 && register_code <= 29);
	tcg_write_accumulator_half(&state->a[register_code & 1U], value, register_code >= 28);
}

static void tcg_mov_write_register(teak_state_t *state, uint8_t register_code, uint16_t value) {
	uint64_t *accumulator;
	bool high;

	if (register_code >= 16 && register_code <= 19) {
		accumulator = &state->b[register_code & 1U];
		high = register_code < 18;
	} else if (register_code >= 26 && register_code <= 29) {
		accumulator = &state->a[register_code & 1U];
		high = register_code >= 28;
	} else {
		tcg_write_register(state, register_code, value);
		return;
	}

	if (high) {
		*accumulator = (int32_t) ((uint32_t) value << 16);
	} else {
		*accumulator = value;
	}
	tcg_set_accumulator_value_flags(state, *accumulator);
}

static uint16_t tcg_mov_read_register(teak_state_t *state, uint8_t register_code) {
	uint64_t accumulator;
	bool high;

	if (register_code >= 16 && register_code <= 19) {
		accumulator = state->b[register_code & 1U];
		high = register_code < 18;
	} else if (register_code >= 26 && register_code <= 29) {
		accumulator = state->a[register_code & 1U];
		high = register_code >= 28;
	} else {
		return tcg_read_register(state, register_code);
	}

	accumulator = tcg_data_bus_saturate(state, accumulator);
	return high ? accumulator >> 16 : accumulator;
}

static bool tcg_alb_modifies_operand(teak_alb_operation_t operation) {
	return operation <= TEAK_ALB_ADDV || operation == TEAK_ALB_SUBV;
}

static uint16_t tcg_alb_result(teak_state_t *state, teak_alb_operation_t operation, uint16_t value, uint16_t mask) {
	uint32_t wide_result;
	uint16_t result;

	switch (operation) {
		case TEAK_ALB_SET:
			result = value | mask;
			break;
		case TEAK_ALB_RST:
			result = value & ~mask;
			break;
		case TEAK_ALB_CHNG:
			result = value ^ mask;
			break;
		case TEAK_ALB_ADDV:
			wide_result = (uint32_t) value + mask;
			result = wide_result;
			state->fc0 = wide_result >> 16;
			break;
		case TEAK_ALB_TST0:
			state->fz = (value & mask) == 0;
			return value;
		case TEAK_ALB_TST1:
			state->fz = (value & mask) == mask;
			return value;
		case TEAK_ALB_CMPV:
		case TEAK_ALB_SUBV:
			wide_result = (uint32_t) value - mask;
			result = wide_result;
			state->fc0 = wide_result >> 16 != 0;
			break;
		default:
			g_assert_not_reached();
	}
	state->fz = result == 0;
	state->fm = result >> 15;
	return result;
}

static void tcg_synchronize_data_access(teak_tcg_core_t *core, uint32_t address, uint32_t cycle_offset, uint32_t access) {
	uint32_t cycles;

	if (address - core->memory.cycle_sensitive_base >= core->memory.cycle_sensitive_size)
		return;
	cycle_offset += core->batch_iterations * core->batch_block_cycles;
	if (!core->synchronization_valid) {
		cycles = cycle_offset;
	} else if (cycle_offset > core->synchronization_offset) {
		cycles = cycle_offset - core->synchronization_offset;
	} else if (access > core->synchronization_access) {
		cycles = 0;
	} else {
		cycles = 1;
	}
	core->synchronized_cycles += cycles;
	core->synchronization_offset = cycle_offset;
	core->synchronization_access = access;
	core->synchronization_valid = true;
	core->pending_cycles += cycles;
	if (core->pending_cycles != 0) {
		core->memory.advance_cycles(core->memory.cycle_opaque, core->pending_cycles);
		core->pending_cycles = 0;
	}
}

static bool tcg_direct_data_read(teak_tcg_core_t *core, uint32_t address, uint16_t *value) {
	if (core->memory.direct_data == NULL || address >= core->memory.direct_data_read_size)
		return false;

	*value = qatomic_read(&core->memory.direct_data[address]);
	return true;
}

static bool tcg_direct_data_write(teak_tcg_core_t *core, uint32_t address, uint16_t value) {
	if (core->memory.direct_data == NULL || address >= core->memory.direct_data_write_size)
		return false;

	qatomic_set(&core->memory.direct_data[address], value);
	return true;
}

uint32_t HELPER(teak_tcg_data_read_at)(void *opaque, uint32_t address, uint32_t pc, uint32_t cycle_offset, uint32_t access) {
	teak_state_t *state = opaque;
	teak_tcg_core_t *core = container_of(state, teak_tcg_core_t, state);
	uint16_t value;

	if (tcg_direct_data_read(core, address, &value))
		return value;

	state->trace_pc = pc;
	tcg_synchronize_data_access(core, address, cycle_offset, access);
	return teak_data_read(core, address);
}

uint32_t HELPER(teak_tcg_data_read_xz_at)(void *opaque, uint32_t address, uint32_t pc, uint32_t cycle_offset, uint32_t access) {
	teak_state_t *state = opaque;
	teak_tcg_core_t *core = container_of(state, teak_tcg_core_t, state);
	uint16_t value;

	state->trace_pc = pc;
	if (address >= core->memory.y_space_base)
		return 0;
	if (tcg_direct_data_read(core, address, &value))
		return value;
	tcg_synchronize_data_access(core, address, cycle_offset, access);
	return teak_data_read(core, address);
}

uint32_t HELPER(teak_tcg_data_read_y_at)(void *opaque, uint32_t address, uint32_t pc, uint32_t cycle_offset, uint32_t access) {
	teak_state_t *state = opaque;
	teak_tcg_core_t *core = container_of(state, teak_tcg_core_t, state);
	uint16_t value;

	state->trace_pc = pc;
	if (address < core->memory.y_space_base)
		return 0;
	if (tcg_direct_data_read(core, address, &value))
		return value;
	tcg_synchronize_data_access(core, address, cycle_offset, access);
	return teak_data_read(core, address);
}

void HELPER(teak_tcg_data_write_at)(void *opaque, uint32_t address, uint32_t value, uint32_t pc,
	uint32_t cycle_offset, uint32_t access
) {
	teak_state_t *state = opaque;
	teak_tcg_core_t *core = container_of(state, teak_tcg_core_t, state);

	if (tcg_direct_data_write(core, address, (uint16_t) value))
		return;

	state->trace_pc = pc;
	tcg_synchronize_data_access(core, address, cycle_offset, access);
	teak_data_write(core, address, (uint16_t) value);
}

uint32_t HELPER(teak_tcg_program_read)(void *opaque, uint32_t address) {
	teak_state_t *state = opaque;
	teak_tcg_core_t *core = container_of(state, teak_tcg_core_t, state);
	return teak_program_read(core, address);
}

void HELPER(teak_tcg_program_write)(void *opaque, uint32_t address, uint32_t value) {
	teak_state_t *state = opaque;
	teak_tcg_core_t *core = container_of(state, teak_tcg_core_t, state);
	uint16_t previous = teak_program_read(core, address);
	bool should_invalidate = true;

	teak_program_write(core, address, (uint16_t) value);
	if (teak_program_read(core, address) == previous)
		return;
	if (core->memory.program.should_invalidate != NULL)
		should_invalidate = core->memory.program.should_invalidate(core->memory.program.opaque, address);
	if (should_invalidate)
		teak_tcg_invalidate_program(core, address);
}

uint32_t HELPER(teak_tcg_register_read)(void *opaque, uint32_t register_code) {
	teak_state_t *state = opaque;
	return tcg_read_register(state, (uint8_t) register_code);
}

void HELPER(teak_tcg_register_write)(void *opaque, uint32_t register_code, uint32_t value) {
	teak_state_t *state = opaque;
	tcg_write_register(state, (uint8_t) register_code, (uint16_t) value);
}

uint32_t HELPER(teak_tcg_mov_register_read)(void *opaque, uint32_t register_code) {
	teak_state_t *state = opaque;
	return tcg_mov_read_register(state, (uint8_t) register_code);
}

void HELPER(teak_tcg_mov_register_write)(void *opaque, uint32_t register_code, uint32_t value) {
	teak_state_t *state = opaque;
	tcg_mov_write_register(state, (uint8_t) register_code, (uint16_t) value);
}

uint32_t HELPER(teak_tcg_special_register_read)(void *opaque, uint32_t special_register) {
	teak_state_t *state = opaque;
	return tcg_read_special_register(state, (teak_special_register_t) special_register);
}

void HELPER(teak_tcg_special_register_write)(void *opaque, uint32_t special_register, uint32_t value) {
	teak_state_t *state = opaque;
	tcg_write_special_register(state, (teak_special_register_t) special_register, (uint16_t) value);
}

uint32_t HELPER(teak_tcg_modulo_address)(void *opaque, uint32_t register_index, uint32_t address, uint32_t step) {
	teak_state_t *state = opaque;
	return teak_modulo_address(state, (uint8_t) register_index, (uint16_t) address, (int16_t) step);
}

void HELPER(teak_tcg_context_switch)(void *opaque, uint32_t restore) {
	teak_state_t *state = opaque;
	if (restore) {
		tcg_context_restore(state);
	} else {
		tcg_context_store(state);
	}
}

static void tcg_shift_bus(teak_state_t *state, uint64_t canonical, int16_t shift, uint8_t destination) {
	int64_t signed_value = tcg_sign_extend_accumulator(canonical);
	uint64_t result;
	uint8_t carry = 0;
	uint8_t overflow = 0;

	if (shift > 0) {
		uint16_t amount = shift;
		if (amount < TEAK_ACCUMULATOR_BITS) {
			result = canonical << amount & TEAK_ACCUMULATOR_MASK;
			carry = canonical >> (TEAK_ACCUMULATOR_BITS - amount) & 1U;
			overflow = tcg_sign_extend_accumulator(result) >> amount != signed_value;
		} else {
			result = 0;
			if (amount == TEAK_ACCUMULATOR_BITS)
				carry = canonical & 1U;
			overflow = canonical != 0;
		}
	} else if (shift < 0) {
		uint16_t amount = -(int32_t) shift;
		bool negative = canonical & TEAK_ACCUMULATOR_SIGN;

		if (amount < TEAK_ACCUMULATOR_BITS) {
			carry = canonical >> (amount - 1) & 1U;
			result = state->s ? canonical >> amount : (uint64_t) (signed_value >> amount) & TEAK_ACCUMULATOR_MASK;
		} else {
			if (amount == TEAK_ACCUMULATOR_BITS)
				carry = negative;
			result = !state->s && negative ? TEAK_ACCUMULATOR_MASK : 0;
		}
	} else {
		result = canonical;
	}

	state->fc0 = carry;
	if (!state->s) {
		state->fv = overflow;
		state->fvl |= overflow;
	}
	tcg_set_accumulator_value_flags(state, result);
	*tcg_accumulator(state, destination) = (uint64_t) tcg_sign_extend_accumulator(result);
}

void HELPER(teak_tcg_shift_accumulator)(void *opaque, uint32_t source, uint32_t destination) {
	teak_state_t *state = opaque;
	uint64_t canonical = *tcg_accumulator(state, source) & TEAK_ACCUMULATOR_MASK;
	tcg_shift_bus(state, canonical, (int16_t) state->shift_value, (uint8_t) destination);
}

void HELPER(teak_tcg_shift_value)(void *opaque, uint32_t source, uint32_t destination, uint32_t shift) {
	teak_state_t *state = opaque;
	uint64_t canonical = (uint64_t) (int64_t) (int16_t) source & TEAK_ACCUMULATOR_MASK;
	tcg_shift_bus(state, canonical, (int16_t) shift, (uint8_t) destination);
}

void HELPER(teak_tcg_alb_memory)(void *opaque, uint32_t address, uint32_t mask, uint32_t operation,
	uint32_t pc, uint32_t cycle_offset, uint32_t access
) {
	teak_state_t *state = opaque;
	teak_tcg_core_t *core = container_of(state, teak_tcg_core_t, state);
	teak_alb_operation_t alb_operation = (teak_alb_operation_t) operation;
	uint16_t value;
	uint16_t result;

	if (!tcg_direct_data_read(core, address, &value)) {
		state->trace_pc = pc;
		tcg_synchronize_data_access(core, address, cycle_offset, access);
		value = teak_data_read(core, address);
	}
	result = tcg_alb_result(state, alb_operation, value, mask);
	if (tcg_alb_modifies_operand(alb_operation) && !tcg_direct_data_write(core, address, result))
		teak_data_write(core, address, result);
}

void HELPER(teak_tcg_alb_register)(void *opaque, uint32_t register_code, uint32_t mask, uint32_t operation) {
	teak_state_t *state = opaque;
	uint16_t value = tcg_read_register(state, register_code);
	teak_alb_operation_t alb_operation = (teak_alb_operation_t) operation;
	uint16_t result = tcg_alb_result(state, alb_operation, value, mask);

	if (tcg_alb_modifies_operand(alb_operation))
		tcg_write_register(state, register_code, result);
}

#define HELPER_H "hw/arm/pmb887x/dsp/tcg-helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

static void tcg_request_tb_flush(void) {
	unsigned int flush_count = qatomic_read(&tb_ctx.tb_flush_count);

	g_assert(first_cpu != NULL);
	queue_tb_flush(first_cpu);
	while (qatomic_read(&tb_ctx.tb_flush_count) == flush_count)
		g_thread_yield();
}

static TranslationBlock *tcg_compile_block(uint32_t pc, uint16_t words, uint16_t instruction_count,
	teak_tcg_emit_fn *emit, void *opaque, int *error
) {
	TranslationBlock *tb;
	void *code;
	int code_size;

	*error = -1;
	qemu_thread_jit_write();
	tb = tcg_tb_alloc(tcg_ctx);
	if (tb == NULL) {
		tcg_request_tb_flush();
		*error = TEAK_TCG_COMPILE_RETRY;
		return NULL;
	}

	memset(tb, 0, sizeof(*tb));
	code = tcg_ctx->code_gen_ptr;
	tb->tc.ptr = tcg_splitwx_to_rx(code);
	tb->size = words;
	tb->icount = instruction_count;
	tcg_ctx->gen_tb = tb;
	code_size = sigsetjmp(tcg_ctx->jmp_trans, 0);
	if (code_size == 0) {
		tcg_func_start(tcg_ctx);
		emit(opaque);
		tcg_gen_exit_tb(NULL, 0);
		code_size = tcg_gen_code(tcg_ctx, tb, pc);
	}
	tcg_ctx->gen_tb = NULL;
	if (code_size == -1) {
		tcg_request_tb_flush();
		*error = TEAK_TCG_COMPILE_RETRY;
		return NULL;
	}
	if (code_size < 0) {
		*error = code_size;
		return NULL;
	}

	tb->tc.size = code_size;
	qatomic_set(&tcg_ctx->code_gen_ptr, (void *) ROUND_UP((uintptr_t) code + code_size, CODE_GEN_ALIGN));
	*error = 0;
	return tb;
}

static void tcg_clear_block_cache(void) {
	for (size_t i = 0; i < ARRAY_SIZE(tcg_block_cache); i++) {
		teak_tcg_block_cache_entry_t *entry = tcg_block_cache[i];

		while (entry != NULL) {
			teak_tcg_block_cache_entry_t *next = entry->next;
			g_free(entry);
			entry = next;
		}
		tcg_block_cache[i] = NULL;
	}
}

static void tcg_check_block_cache(void) {
	unsigned int flush_count = qatomic_read(&tb_ctx.tb_flush_count);
	if (tcg_block_cache_context == tcg_ctx && tcg_block_cache_flush_count == flush_count)
		return;
	tcg_clear_block_cache();
	tcg_block_cache_context = tcg_ctx;
	tcg_block_cache_flush_count = flush_count;
}

static bool tcg_cached_block_is_prefix(const teak_tcg_block_t *cached, const teak_tcg_block_t *block) {
	size_t instruction_bytes;

	if (cached->instruction_count >= block->instruction_count)
		return false;
	instruction_bytes = cached->instruction_count * sizeof(cached->instructions[0]);
	if (memcmp(cached->instructions, block->instructions, instruction_bytes) != 0)
		return false;
	return memcmp(cached->block_repeat_setup_level, block->block_repeat_setup_level,
		cached->instruction_count) == 0;
}

static TranslationBlock *tcg_find_cached_block(teak_tcg_core_t *core, teak_tcg_block_t *block) {
	teak_tcg_block_cache_entry_t *entry;
	teak_tcg_block_cache_entry_t *prefix = NULL;

	tcg_check_block_cache();
	entry = tcg_block_cache[block->pc];
	while (entry != NULL) {
		bool longer_prefix = prefix == NULL || entry->block.instruction_count > prefix->block.instruction_count;

		if (entry->cache_id != core->cache_id) {
			entry = entry->next;
			continue;
		}
		if (memcmp(&entry->block, block, sizeof(*block)) == 0)
			return entry->tb;
		if (longer_prefix && tcg_cached_block_is_prefix(&entry->block, block))
			prefix = entry;
		entry = entry->next;
	}
	if (prefix != NULL) {
		*block = prefix->block;
		return prefix->tb;
	}
	return NULL;
}

static teak_tcg_block_cache_entry_t *tcg_find_cached_entry_fast(teak_tcg_core_t *core) {
	teak_tcg_block_cache_entry_t *entry;
	uint8_t level = core->state.bcn;

	tcg_check_block_cache();
	entry = tcg_block_cache[core->state.pc];
	while (entry != NULL) {
		bool same_cache = entry->cache_id == core->cache_id;
		bool same_level = entry->block_repeat_level == level;
		if (same_cache && same_level) {
			bool same_repeat_end;

			if (level == 0) {
				core->cache_fast_hits++;
				return entry;
			}
			same_repeat_end = memcmp(entry->block_repeat_end, core->state.block_repeat_end,
				level * sizeof(entry->block_repeat_end[0])) == 0;
			if (same_repeat_end) {
				core->cache_fast_hits++;
				return entry;
			}
		}
		entry = entry->next;
	}
	return NULL;
}

static TranslationBlock *tcg_find_cached_block_fast(teak_tcg_core_t *core, teak_tcg_block_t *block) {
	teak_tcg_block_cache_entry_t *entry = tcg_find_cached_entry_fast(core);
	if (entry == NULL)
		return NULL;
	*block = entry->block;
	return entry->tb;
}

static void tcg_cache_block(const teak_tcg_core_t *core, const teak_tcg_block_t *block, TranslationBlock *tb) {
	teak_tcg_block_cache_entry_t *entry = g_new(teak_tcg_block_cache_entry_t, 1);
	entry->block = *block;
	entry->tb = tb;
	entry->cache_id = core->cache_id;
	memcpy(entry->block_repeat_end, core->state.block_repeat_end, sizeof(entry->block_repeat_end));
	entry->block_repeat_level = core->state.bcn;
	entry->next = tcg_block_cache[block->pc];
	tcg_block_cache[block->pc] = entry;
}

static bool tcg_block_contains_address(const teak_tcg_block_t *block, uint16_t address) {
	for (size_t i = 0; i < block->instruction_count; i++) {
		const teak_insn_t *instruction = &block->instructions[i];
		if ((uint16_t) instruction->address == address)
			return true;
		if (instruction->words == 2 && (uint16_t) (instruction->address + 1) == address)
			return true;
	}
	return false;
}

void teak_tcg_invalidate_program(teak_tcg_core_t *core, uint32_t address) {
	uint16_t program_address = (uint16_t) address;

	tcg_check_block_cache();
	for (size_t distance = 0; distance < TEAK_TCG_MAX_BLOCK_INSTRUCTIONS * 2; distance++) {
		uint16_t start = program_address - distance;
		teak_tcg_block_cache_entry_t **link = &tcg_block_cache[start];
		while (*link != NULL) {
			teak_tcg_block_cache_entry_t *entry = *link;
			bool contains_address = tcg_block_contains_address(&entry->block, program_address);
			bool matching_entry = entry->cache_id == core->cache_id && contains_address;
			if (matching_entry) {
				*link = entry->next;
				g_free(entry);
			} else {
				link = &entry->next;
			}
		}
	}
}

void teak_tcg_invalidate_program_range(teak_tcg_core_t *core, uint32_t address, size_t words) {
	uint16_t start = (uint16_t) address;

	tcg_check_block_cache();
	for (size_t i = 0; i < ARRAY_SIZE(tcg_block_cache); i++) {
		teak_tcg_block_cache_entry_t **link = &tcg_block_cache[i];
		while (*link != NULL) {
			teak_tcg_block_cache_entry_t *entry = *link;
			bool intersects = false;
			for (size_t j = 0; j < entry->block.instruction_count; j++) {
				const teak_insn_t *instruction = &entry->block.instructions[j];
				uint16_t offset = (uint16_t) instruction->address - start;
				intersects = offset < words || (instruction->words == 2 && (uint16_t) (offset + 1) < words);
				if (intersects)
					break;
			}

			if (entry->cache_id == core->cache_id && intersects) {
				*link = entry->next;
				g_free(entry);
			} else {
				link = &entry->next;
			}
		}
	}
}

void teak_tcg_invalidate_all(teak_tcg_core_t *core) {
	tcg_check_block_cache();
	for (size_t i = 0; i < ARRAY_SIZE(tcg_block_cache); i++) {
		teak_tcg_block_cache_entry_t **link = &tcg_block_cache[i];
		while (*link != NULL) {
			teak_tcg_block_cache_entry_t *entry = *link;
			if (entry->cache_id == core->cache_id) {
				*link = entry->next;
				g_free(entry);
			} else {
				link = &entry->next;
			}
		}
	}
}

static bool tcg_alu_uses_multiply(teak_alu_operation_t operation) {
	return operation == TEAK_ALU_MSU || operation == TEAK_ALU_SQR ||
		operation == TEAK_ALU_SQRA;
}

static bool tcg_can_read_mov_register(uint8_t register_code) {
	bool common = register_code <= 10;
	bool status = register_code >= 13 && register_code <= 15;
	if (common || status)
		return true;
	if (register_code >= 16 && register_code <= 23)
		return true;
	return register_code >= 26;
}

static bool tcg_can_translate(const teak_insn_t *instruction) {
	switch (instruction->opcode) {
		case TEAK_OP_NOP:
		case TEAK_OP_EINT:
		case TEAK_OP_DINT:
		case TEAK_OP_TRAP:
		case TEAK_OP_LOAD_PAGE:
		case TEAK_OP_LOAD_STEPI:
		case TEAK_OP_LOAD_STEPJ:
		case TEAK_OP_LOAD_MODI:
		case TEAK_OP_LOAD_MODJ:
		case TEAK_OP_LOAD_PRODUCT_SHIFT:
		case TEAK_OP_SHIFT_CONDITIONAL:
		case TEAK_OP_SHIFT_IMMEDIATE:
		case TEAK_OP_MULTIPLY_IMMEDIATE:
		case TEAK_OP_MULTIPLY_DATA_IMM8:
		case TEAK_OP_MULTIPLY_DUAL_RN:
		case TEAK_OP_MULTIPLY_R6:
		case TEAK_OP_TSTB_IMM8:
		case TEAK_OP_TSTB_RN_STEP:
		case TEAK_OP_TSTB_REGISTER:
		case TEAK_OP_MOV_DATA_IMM8_REGISTER:
		case TEAK_OP_MOV_DATA_IMM8_ACCUMULATOR:
		case TEAK_OP_MOV_DATA_IMM8_ACCUMULATOR_HIGH_EU:
		case TEAK_OP_MOV_REGISTER_DATA_IMM8:
		case TEAK_OP_MOV_DATA_R7_OFFSET7_ACCUMULATOR:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET7:
		case TEAK_OP_MOV_DATA_R7_OFFSET16_ACCUMULATOR:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET16:
		case TEAK_OP_MOV_IMM_B_ACCUMULATOR:
		case TEAK_OP_MOV_DATA_RN_STEP_B_ACCUMULATOR:
		case TEAK_OP_MOV_SHORT_REGISTER:
		case TEAK_OP_MOV_ACCUMULATOR_ACCUMULATOR:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_SPECIAL:
		case TEAK_OP_MOV_SPECIAL_ACCUMULATOR:
		case TEAK_OP_MOV_IMM_ICR:
		case TEAK_OP_MOV_IMM8_ACCUMULATOR_LOW:
		case TEAK_OP_MOV_DATA_IMM16_ACCUMULATOR:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_IMM16:
		case TEAK_OP_MOVP_RN_RN:
		case TEAK_OP_MOVD_RN_RN:
		case TEAK_OP_BRANCH_ABSOLUTE:
		case TEAK_OP_BRANCH_RELATIVE:
		case TEAK_OP_CALL_ABSOLUTE:
		case TEAK_OP_CALL_ACCUMULATOR:
		case TEAK_OP_CALL_RELATIVE:
		case TEAK_OP_RETURN:
		case TEAK_OP_RETURN_INTERRUPT:
		case TEAK_OP_RETURN_STACK:
		case TEAK_OP_DELAYED_RETURN:
		case TEAK_OP_DELAYED_RETURN_INTERRUPT:
		case TEAK_OP_CONTEXT_STORE:
		case TEAK_OP_CONTEXT_RESTORE:
		case TEAK_OP_PUSH_IMMEDIATE:
		case TEAK_OP_REPEAT_IMMEDIATE:
		case TEAK_OP_BLOCK_REPEAT_IMMEDIATE:
		case TEAK_OP_BREAK:
		case TEAK_OP_ALU_IMMEDIATE_ACCUMULATOR:
		case TEAK_OP_ALU_DATA_IMM8_ACCUMULATOR:
		case TEAK_OP_ALU_DATA_IMM16_ACCUMULATOR:
		case TEAK_OP_ALU_R7_OFFSET7_ACCUMULATOR:
		case TEAK_OP_ALU_R7_OFFSET16_ACCUMULATOR:
		case TEAK_OP_ALU_RN_STEP_ACCUMULATOR:
		case TEAK_OP_TEST_ACCUMULATOR_DATA_IMM8:
		case TEAK_OP_MODA4_ACCUMULATOR:
		case TEAK_OP_MODB3_ACCUMULATOR:
		case TEAK_OP_LIMIT_ACCUMULATOR:
			return true;

		case TEAK_OP_EXPONENT:
			return instruction->exponent_source != TEAK_EXPONENT_REGISTER ||
				instruction->register_code != 11;

		case TEAK_OP_DIVISION_STEP:
		case TEAK_OP_NORMALIZE:
		case TEAK_OP_SWAP_ACCUMULATORS:
		case TEAK_OP_BANK_EXCHANGE:
		case TEAK_OP_MINIMUM_MAXIMUM:
			return true;

		case TEAK_OP_ALB_DATA_IMM8:
			return true;

		case TEAK_OP_MOVP_ACCUMULATOR_LOW_REGISTER:
			return true;

		case TEAK_OP_MOVS_REGISTER:
			return instruction->register_code != 11;

		case TEAK_OP_MOVS_RN_STEP:
		case TEAK_OP_MOVS_DATA_IMM8:
		case TEAK_OP_MOVS_R6:
		case TEAK_OP_MOVSI_REGISTER:
		case TEAK_OP_MOVR_REGISTER:
		case TEAK_OP_MOVR_RN_STEP:
		case TEAK_OP_MOVR_RN_HIGH:
		case TEAK_OP_MOVR_B_ACCUMULATOR:
		case TEAK_OP_MOVR_R6:
			return true;

		case TEAK_OP_MODIFY_RN:
		case TEAK_OP_MULTIPLY_RN_IMMEDIATE:
		case TEAK_OP_MULTIPLY_RN_STEP:
			return instruction->address_register < 6;

		case TEAK_OP_ALB_RN_STEP:
			return instruction->address_register < 6;

		case TEAK_OP_ALB_REGISTER: {
			bool general_register = instruction->register_code != 11 && instruction->register_code != 12;
			bool accumulator = instruction->register_code == 24 || instruction->register_code == 25;
			return general_register && !accumulator;
		}

		case TEAK_OP_PUSH_REGISTER: {
			bool full_accumulator = instruction->register_code == 24 || instruction->register_code == 25;
			bool forbidden_register = instruction->register_code == 11 || instruction->register_code == 13;
			return !full_accumulator && !forbidden_register;
		}

		case TEAK_OP_MOV_STACK_REGISTER: {
			bool b_accumulator = instruction->register_code >= 16 && instruction->register_code <= 19;
			return instruction->register_code != 12 && !b_accumulator;
		}

		case TEAK_OP_POP_REGISTER:
			return instruction->register_code != 13;

		case TEAK_OP_MOV_REGISTER_REGISTER:
			if (instruction->register_code == 11)
				return instruction->destination_register_code == 24 || instruction->destination_register_code == 25;
			if (instruction->register_code == instruction->destination_register_code)
				return false;
			return instruction->register_code == 12 || tcg_can_read_mov_register(instruction->register_code);

		case TEAK_OP_MOV_REGISTER_B_ACCUMULATOR: {
			bool program_counter = instruction->register_code == 12;
			bool full_accumulator = instruction->register_code == 24 || instruction->register_code == 25;
			return program_counter || full_accumulator || tcg_can_read_mov_register(instruction->register_code);
		}

		case TEAK_OP_MOV_MIXP_REGISTER:
			return true;

		case TEAK_OP_MOV_REGISTER_MIXP:
		case TEAK_OP_MOV_REGISTER_ICR:
			return tcg_can_read_mov_register(instruction->register_code);

		case TEAK_OP_REPEAT_REGISTER:
		case TEAK_OP_BLOCK_REPEAT_REGISTER:
			return instruction->register_code != 11 && instruction->register_code != 12;

		case TEAK_OP_MULTIPLY_REGISTER:
			return tcg_can_read_mov_register(instruction->register_code);

		case TEAK_OP_MOV_IMM_REGISTER:
			switch (instruction->register_code) {
				case 0:
				case 1:
				case 2:
				case 3:
				case 4:
				case 5:
				case 6:
				case 7:
				case 8:
				case 9:
				case 10:
				case 12:
				case 13:
				case 14:
				case 15:
				case 16:
				case 17:
				case 18:
				case 19:
				case 20:
				case 21:
				case 22:
				case 23:
				case 24:
				case 25:
				case 26:
				case 27:
				case 28:
				case 29:
				case 30:
				case 31:
					return true;
				default:
					return false;
			}

		case TEAK_OP_MOV_DATA_RN_STEP_REGISTER: {
			bool basic_register = instruction->register_code <= 7;
			bool b_half = instruction->register_code >= 16 && instruction->register_code <= 19;
			bool external_register = instruction->register_code >= 20 && instruction->register_code <= 23;
			bool accumulator = instruction->register_code >= 24 && instruction->register_code <= 29;
			bool program_counter = instruction->register_code == 12;
			return basic_register || b_half || external_register || accumulator || program_counter;
		}

		case TEAK_OP_MOV_REGISTER_DATA_RN_STEP: {
			bool basic_register = instruction->register_code <= 7;
			bool b_half = instruction->register_code >= 16 && instruction->register_code <= 19;
			bool accumulator_half = instruction->register_code >= 26 && instruction->register_code <= 29;
			return basic_register || b_half || accumulator_half;
		}

		case TEAK_OP_ALU_REGISTER_ACCUMULATOR: {
			if (tcg_alu_uses_multiply(instruction->alu_operation))
				return tcg_can_read_mov_register(instruction->register_code);
			return true;
		}

		case TEAK_OP_UNDEFINED:
			return false;
	}
	g_assert_not_reached();
}

static bool tcg_is_repeat(const teak_insn_t *instruction) {
	return instruction->opcode == TEAK_OP_REPEAT_IMMEDIATE ||
		instruction->opcode == TEAK_OP_REPEAT_REGISTER;
}

static bool tcg_is_block_repeat(const teak_insn_t *instruction) {
	return instruction->opcode == TEAK_OP_BLOCK_REPEAT_IMMEDIATE ||
		instruction->opcode == TEAK_OP_BLOCK_REPEAT_REGISTER;
}

static bool tcg_is_delayed_return(const teak_insn_t *instruction) {
	return instruction->opcode == TEAK_OP_DELAYED_RETURN ||
		instruction->opcode == TEAK_OP_DELAYED_RETURN_INTERRUPT;
}

static uint8_t tcg_delayed_transfer_cycles(const teak_insn_t *instruction) {
	if (instruction->opcode == TEAK_OP_DELAYED_RETURN)
		return 2;
	if (instruction->opcode == TEAK_OP_DELAYED_RETURN_INTERRUPT)
		return 2;

	switch (instruction->opcode) {
		case TEAK_OP_MOV_IMM_REGISTER:
			return instruction->register_code == 12 ? 1 : 0;
		case TEAK_OP_MOV_REGISTER_REGISTER:
			return instruction->destination_register_code == 12 ? 2 : 0;
		case TEAK_OP_MOV_MIXP_REGISTER:
			return instruction->destination_register_code == 12 ? 2 : 0;
		case TEAK_OP_MOV_DATA_RN_STEP_REGISTER:
			return instruction->register_code == 12 ? 2 : 0;
		case TEAK_OP_MOVP_ACCUMULATOR_LOW_REGISTER:
			return instruction->destination_register_code == 12 ? 2 : 0;
		case TEAK_OP_POP_REGISTER:
			return instruction->register_code == 12 ? 2 : 0;
		default:
			return 0;
	}
}

static bool tcg_is_loop_control(const teak_insn_t *instruction) {
	if (tcg_delayed_transfer_cycles(instruction) != 0)
		return true;

	switch (instruction->opcode) {
		case TEAK_OP_BRANCH_ABSOLUTE:
		case TEAK_OP_BRANCH_RELATIVE:
		case TEAK_OP_CALL_ABSOLUTE:
		case TEAK_OP_CALL_ACCUMULATOR:
		case TEAK_OP_CALL_RELATIVE:
		case TEAK_OP_BLOCK_REPEAT_IMMEDIATE:
		case TEAK_OP_BLOCK_REPEAT_REGISTER:
		case TEAK_OP_BREAK:
		case TEAK_OP_REPEAT_IMMEDIATE:
		case TEAK_OP_REPEAT_REGISTER:
		case TEAK_OP_RETURN:
		case TEAK_OP_RETURN_INTERRUPT:
		case TEAK_OP_RETURN_STACK:
		case TEAK_OP_DELAYED_RETURN:
		case TEAK_OP_DELAYED_RETURN_INTERRUPT:
		case TEAK_OP_TRAP:
		case TEAK_OP_UNDEFINED:
			return true;

		default:
			return false;
	}
}

static bool tcg_can_repeat(const teak_insn_t *instruction) {
	if (instruction->words != 1)
		return false;
	if (tcg_is_loop_control(instruction))
		return false;
	return tcg_can_translate(instruction);
}

static uint8_t tcg_delay_slot_cycles(const teak_insn_t *instruction) {
	switch (instruction->opcode) {
		case TEAK_OP_NOP:
		case TEAK_OP_MODA4_ACCUMULATOR:
		case TEAK_OP_MODB3_ACCUMULATOR:
		case TEAK_OP_LIMIT_ACCUMULATOR:
		case TEAK_OP_EXPONENT:
		case TEAK_OP_DIVISION_STEP:
		case TEAK_OP_MINIMUM_MAXIMUM:
		case TEAK_OP_SWAP_ACCUMULATORS:
		case TEAK_OP_BANK_EXCHANGE:
		case TEAK_OP_CONTEXT_STORE:
		case TEAK_OP_CONTEXT_RESTORE:
		case TEAK_OP_LOAD_PRODUCT_SHIFT:
		case TEAK_OP_SHIFT_CONDITIONAL:
		case TEAK_OP_SHIFT_IMMEDIATE:
		case TEAK_OP_MULTIPLY_IMMEDIATE:
		case TEAK_OP_MULTIPLY_DATA_IMM8:
		case TEAK_OP_MULTIPLY_DUAL_RN:
		case TEAK_OP_MULTIPLY_R6:
		case TEAK_OP_MULTIPLY_REGISTER:
		case TEAK_OP_MULTIPLY_RN_STEP:
		case TEAK_OP_MOVS_REGISTER:
		case TEAK_OP_MOVS_RN_STEP:
		case TEAK_OP_MOVS_DATA_IMM8:
		case TEAK_OP_MOVS_R6:
		case TEAK_OP_MOVSI_REGISTER:
		case TEAK_OP_MOVR_REGISTER:
		case TEAK_OP_MOVR_RN_STEP:
		case TEAK_OP_MOVR_RN_HIGH:
		case TEAK_OP_MOVR_B_ACCUMULATOR:
		case TEAK_OP_MOVR_R6:
		case TEAK_OP_MOV_SHORT_REGISTER:
		case TEAK_OP_MOV_ACCUMULATOR_ACCUMULATOR:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_SPECIAL:
		case TEAK_OP_MOV_SPECIAL_ACCUMULATOR:
		case TEAK_OP_MOV_MIXP_REGISTER:
		case TEAK_OP_MOV_REGISTER_MIXP:
		case TEAK_OP_MOV_REGISTER_ICR:
		case TEAK_OP_MOV_REGISTER_B_ACCUMULATOR:
		case TEAK_OP_PUSH_REGISTER:
		case TEAK_OP_POP_REGISTER:
		case TEAK_OP_MOV_IMM8_ACCUMULATOR_LOW:
		case TEAK_OP_MOV_DATA_IMM8_REGISTER:
		case TEAK_OP_MOV_DATA_IMM8_ACCUMULATOR:
		case TEAK_OP_MOV_DATA_IMM8_ACCUMULATOR_HIGH_EU:
		case TEAK_OP_MOV_REGISTER_DATA_IMM8:
		case TEAK_OP_MOV_DATA_R7_OFFSET7_ACCUMULATOR:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET7:
			return 1;

		case TEAK_OP_NORMALIZE:
			return 2;

		case TEAK_OP_ALU_IMMEDIATE_ACCUMULATOR:
			return instruction->words;

		case TEAK_OP_ALU_DATA_IMM16_ACCUMULATOR:
		case TEAK_OP_ALU_R7_OFFSET16_ACCUMULATOR:
		case TEAK_OP_MOV_DATA_IMM16_ACCUMULATOR:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_IMM16:
		case TEAK_OP_MOV_DATA_R7_OFFSET16_ACCUMULATOR:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET16:
		case TEAK_OP_ALB_DATA_IMM8:
		case TEAK_OP_ALB_RN_STEP:
		case TEAK_OP_ALB_REGISTER:
		case TEAK_OP_MULTIPLY_RN_IMMEDIATE:
		case TEAK_OP_MOV_IMM_REGISTER:
		case TEAK_OP_MOV_IMM_B_ACCUMULATOR:
		case TEAK_OP_PUSH_IMMEDIATE:
			return 2;

		case TEAK_OP_MOVP_ACCUMULATOR_LOW_REGISTER:
		case TEAK_OP_MOVP_RN_RN:
			return 3;

		case TEAK_OP_MOVD_RN_RN:
			return 4;

		default:
			return 0;
	}
}

static TCGLabel *tcg_emit_condition_skip(teak_condition_t condition) {
	TCGv_i32 predicate = tcg_temp_new_i32();
	TCGv_i32 temporary = tcg_temp_new_i32();
	TCGLabel *skip = gen_new_label();

	switch (condition) {
		case TEAK_COND_EQ:
			tcg_gen_ld8u_i32(predicate, tcg_env, offsetof(teak_state_t, fz));
			break;
		case TEAK_COND_NEQ:
			tcg_gen_ld8u_i32(predicate, tcg_env, offsetof(teak_state_t, fz));
			tcg_gen_xori_i32(predicate, predicate, 1);
			break;
		case TEAK_COND_GT:
			tcg_gen_ld8u_i32(predicate, tcg_env, offsetof(teak_state_t, fz));
			tcg_gen_ld8u_i32(temporary, tcg_env, offsetof(teak_state_t, fm));
			tcg_gen_or_i32(predicate, predicate, temporary);
			tcg_gen_xori_i32(predicate, predicate, 1);
			break;
		case TEAK_COND_GE:
			tcg_gen_ld8u_i32(predicate, tcg_env, offsetof(teak_state_t, fm));
			tcg_gen_xori_i32(predicate, predicate, 1);
			break;
		case TEAK_COND_LT:
			tcg_gen_ld8u_i32(predicate, tcg_env, offsetof(teak_state_t, fm));
			break;
		case TEAK_COND_LE:
			tcg_gen_ld8u_i32(predicate, tcg_env, offsetof(teak_state_t, fm));
			tcg_gen_ld8u_i32(temporary, tcg_env, offsetof(teak_state_t, fz));
			tcg_gen_or_i32(predicate, predicate, temporary);
			break;
		case TEAK_COND_NN:
			tcg_gen_ld8u_i32(predicate, tcg_env, offsetof(teak_state_t, fn));
			tcg_gen_xori_i32(predicate, predicate, 1);
			break;
		case TEAK_COND_C:
			tcg_gen_ld8u_i32(predicate, tcg_env, offsetof(teak_state_t, fc0));
			break;
		case TEAK_COND_V:
			tcg_gen_ld8u_i32(predicate, tcg_env, offsetof(teak_state_t, fv));
			break;
		case TEAK_COND_E:
			tcg_gen_ld8u_i32(predicate, tcg_env, offsetof(teak_state_t, fe));
			break;
		case TEAK_COND_L:
			tcg_gen_ld8u_i32(predicate, tcg_env, offsetof(teak_state_t, flm));
			tcg_gen_ld8u_i32(temporary, tcg_env, offsetof(teak_state_t, fvl));
			tcg_gen_or_i32(predicate, predicate, temporary);
			break;
		case TEAK_COND_NR:
			tcg_gen_ld8u_i32(predicate, tcg_env, offsetof(teak_state_t, fr));
			tcg_gen_xori_i32(predicate, predicate, 1);
			break;
		case TEAK_COND_NIU0:
			tcg_gen_ld8u_i32(predicate, tcg_env, offsetof(teak_state_t, iu[0]));
			tcg_gen_xori_i32(predicate, predicate, 1);
			break;
		case TEAK_COND_IU0:
			tcg_gen_ld8u_i32(predicate, tcg_env, offsetof(teak_state_t, iu[0]));
			break;
		case TEAK_COND_IU1:
			tcg_gen_ld8u_i32(predicate, tcg_env, offsetof(teak_state_t, iu[1]));
			break;
		default:
			g_assert_not_reached();
	}
	tcg_gen_brcondi_i32(TCG_COND_EQ, predicate, 0, skip);
	return skip;
}

static size_t tcg_rn_old_offset(uint8_t register_code) {
	g_assert(register_code < 8);

	if (register_code < 6)
		return offsetof(teak_state_t, r) + register_code * sizeof(uint16_t);
	if (register_code == 6)
		return offsetof(teak_state_t, r) + 7 * sizeof(uint16_t);
	return offsetof(teak_state_t, y);
}

static void tcg_emit_push_pc(uint32_t return_address) {
	TCGv_i32 sp = tcg_temp_new_i32();
	TCGv_i32 cpc = tcg_temp_new_i32();
	TCGv_i32 low = tcg_constant_i32(return_address & 0xFFFFU);
	TCGv_i32 high = tcg_constant_i32(return_address >> 16);
	TCGLabel *done = gen_new_label();

	tcg_gen_ld16u_i32(sp, tcg_env, offsetof(teak_state_t, sp));
	tcg_gen_ld8u_i32(cpc, tcg_env, offsetof(teak_state_t, cpc));
	tcg_gen_subi_i32(sp, sp, 1);
	tcg_gen_andi_i32(sp, sp, 0xFFFFU);
	gen_helper_teak_tcg_data_write(tcg_env, sp, low);
	tcg_gen_brcondi_i32(TCG_COND_NE, cpc, 0, done);
	tcg_gen_subi_i32(sp, sp, 1);
	tcg_gen_andi_i32(sp, sp, 0xFFFFU);
	gen_helper_teak_tcg_data_write(tcg_env, sp, high);
	gen_set_label(done);
	tcg_gen_st16_i32(sp, tcg_env, offsetof(teak_state_t, sp));
}

static TCGv_i32 tcg_emit_pop_pc(uint16_t stack_adjust, bool update_stack) {
	TCGv_i32 sp = tcg_temp_new_i32();
	TCGv_i32 cpc = tcg_temp_new_i32();
	TCGv_i32 first = tcg_temp_new_i32();
	TCGv_i32 second = tcg_temp_new_i32();
	TCGv_i32 target = tcg_temp_new_i32();
	TCGLabel *two_words = gen_new_label();
	TCGLabel *done = gen_new_label();

	tcg_gen_ld16u_i32(sp, tcg_env, offsetof(teak_state_t, sp));
	tcg_gen_ld8u_i32(cpc, tcg_env, offsetof(teak_state_t, cpc));
	gen_helper_teak_tcg_data_read(first, tcg_env, sp);
	tcg_gen_addi_i32(sp, sp, 1);
	tcg_gen_andi_i32(sp, sp, 0xFFFFU);
	tcg_gen_brcondi_i32(TCG_COND_EQ, cpc, 0, two_words);
	tcg_gen_mov_i32(target, first);
	tcg_gen_br(done);
	gen_set_label(two_words);
	gen_helper_teak_tcg_data_read(second, tcg_env, sp);
	tcg_gen_addi_i32(sp, sp, 1);
	tcg_gen_andi_i32(sp, sp, 0xFFFFU);
	tcg_gen_shli_i32(target, first, 16);
	tcg_gen_or_i32(target, target, second);
	gen_set_label(done);
	if (stack_adjust != 0) {
		tcg_gen_addi_i32(sp, sp, stack_adjust);
		tcg_gen_andi_i32(sp, sp, 0xFFFFU);
	}
	if (update_stack)
		tcg_gen_st16_i32(sp, tcg_env, offsetof(teak_state_t, sp));
	return target;
}

static void tcg_emit_push_value(TCGv_i32 value) {
	TCGv_i32 sp = tcg_temp_new_i32();

	tcg_gen_ld16u_i32(sp, tcg_env, offsetof(teak_state_t, sp));
	tcg_gen_subi_i32(sp, sp, 1);
	tcg_gen_andi_i32(sp, sp, 0xFFFFU);
	gen_helper_teak_tcg_data_write(tcg_env, sp, value);
	tcg_gen_st16_i32(sp, tcg_env, offsetof(teak_state_t, sp));
}

static uint16_t tcg_instruction_end(const teak_insn_t *instruction) {
	return (uint16_t) (instruction->address + instruction->words);
}

static void tcg_emit_trap(const teak_insn_t *instruction) {
	uint16_t return_address = tcg_instruction_end(instruction);
	tcg_emit_push_value(tcg_constant_i32(return_address));
	tcg_gen_st16_i32(tcg_constant_i32(return_address), tcg_env, offsetof(teak_state_t, dvm));
	tcg_gen_st8_i32(tcg_constant_i32(1), tcg_env, offsetof(teak_state_t, trap_active));
	tcg_gen_st_i32(tcg_constant_i32(2), tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_st_i32(tcg_constant_i32(TEAK_EXIT_INTERRUPT), tcg_env,
		offsetof(teak_state_t, exit_reason));
}

static void tcg_emit_push_register(uint8_t register_code) {
	TCGv_i32 value = tcg_temp_new_i32();

	gen_helper_teak_tcg_mov_register_read(value, tcg_env, tcg_constant_i32(register_code));
	tcg_emit_push_value(value);
}

static TCGv_i32 tcg_emit_pop_value(void) {
	TCGv_i32 sp = tcg_temp_new_i32();
	TCGv_i32 value = tcg_temp_new_i32();

	tcg_gen_ld16u_i32(sp, tcg_env, offsetof(teak_state_t, sp));
	gen_helper_teak_tcg_data_read(value, tcg_env, sp);
	tcg_gen_addi_i32(sp, sp, 1);
	tcg_gen_andi_i32(sp, sp, 0xFFFFU);
	tcg_gen_st16_i32(sp, tcg_env, offsetof(teak_state_t, sp));
	return value;
}

static void tcg_emit_pop_register(uint8_t register_code) {
	TCGv_i32 value = tcg_emit_pop_value();
	bool accumulator = register_code >= 24 && register_code <= 29;
	if (accumulator) {
		gen_helper_teak_tcg_mov_register_write(tcg_env, tcg_constant_i32(register_code), value);
	} else {
		gen_helper_teak_tcg_register_write(tcg_env, tcg_constant_i32(register_code), value);
	}
}

static void tcg_emit_mov_stack_register(uint8_t register_code) {
	TCGv_i32 sp = tcg_temp_new_i32();
	TCGv_i32 value = tcg_temp_new_i32();

	tcg_gen_ld16u_i32(sp, tcg_env, offsetof(teak_state_t, sp));
	gen_helper_teak_tcg_data_read(value, tcg_env, sp);
	gen_helper_teak_tcg_mov_register_write(tcg_env, tcg_constant_i32(register_code), value);
}

static void tcg_emit_accumulator_value_flags(TCGv_i64 value);
static TCGv_i64 tcg_emit_shifted_product(void);
static size_t tcg_ab_offset(uint8_t accumulator);

static void tcg_emit_mov_register_register(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_temp_new_i32();

	if (instruction->destination_register_code == 12)
		return;
	if (instruction->register_code == 11) {
		size_t offset = offsetof(teak_state_t, a) + (instruction->destination_register_code - 24) * sizeof(uint64_t);
		TCGv_i64 product = tcg_emit_shifted_product();
		tcg_emit_accumulator_value_flags(product);
		tcg_gen_st_i64(product, tcg_env, offset);
		return;
	}
	if (instruction->register_code == 12) {
		tcg_gen_movi_i32(value, tcg_instruction_end(instruction));
	} else {
		gen_helper_teak_tcg_mov_register_read(value, tcg_env, tcg_constant_i32(instruction->register_code));
	}
	gen_helper_teak_tcg_mov_register_write(tcg_env,
		tcg_constant_i32(instruction->destination_register_code), value);
}

static void tcg_emit_mov_b_accumulator(uint8_t accumulator_index, TCGv_i32 value) {
	TCGv_i64 accumulator = tcg_temp_new_i64();

	tcg_gen_extu_i32_i64(accumulator, value);
	tcg_gen_ext16s_i64(accumulator, accumulator);
	tcg_emit_accumulator_value_flags(accumulator);
	tcg_gen_st_i64(accumulator, tcg_env,
		offsetof(teak_state_t, b) + accumulator_index * sizeof(uint64_t));
}

static void tcg_emit_mov_imm_b_accumulator(const teak_insn_t *instruction) {
	tcg_emit_mov_b_accumulator(instruction->accumulator_index, tcg_constant_i32(instruction->expansion));
}

static void tcg_emit_mov_register_b_accumulator(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_temp_new_i32();

	if (instruction->register_code == 24 || instruction->register_code == 25) {
		TCGv_i64 accumulator = tcg_temp_new_i64();
		size_t source_offset = offsetof(teak_state_t, a) + (instruction->register_code - 24) * sizeof(uint64_t);
		size_t destination_offset = offsetof(teak_state_t, b) + instruction->accumulator_index * sizeof(uint64_t);

		tcg_gen_ld_i64(accumulator, tcg_env, source_offset);
		tcg_emit_accumulator_value_flags(accumulator);
		tcg_gen_st_i64(accumulator, tcg_env, destination_offset);
		return;
	}
	if (instruction->register_code == 12) {
		tcg_gen_movi_i32(value, tcg_instruction_end(instruction));
	} else {
		gen_helper_teak_tcg_mov_register_read(value, tcg_env, tcg_constant_i32(instruction->register_code));
	}
	tcg_emit_mov_b_accumulator(instruction->accumulator_index, value);
}

static void tcg_emit_multiply(TCGv_i32 x, bool unsigned_x, bool unsigned_y) {
	TCGv_i32 x_factor = tcg_temp_new_i32();
	TCGv_i32 y_factor = tcg_temp_new_i32();
	TCGv_i32 y = tcg_temp_new_i32();
	TCGv_i32 product = tcg_temp_new_i32();
	TCGv_i32 extension = tcg_temp_new_i32();

	if (unsigned_x) {
		tcg_gen_ext16u_i32(x_factor, x);
	} else {
		tcg_gen_ext16s_i32(x_factor, x);
	}
	tcg_gen_ld16u_i32(y, tcg_env, offsetof(teak_state_t, y[0]));
	if (unsigned_y) {
		tcg_gen_ext16u_i32(y_factor, y);
	} else {
		tcg_gen_ext16s_i32(y_factor, y);
	}
	tcg_gen_mul_i32(product, x_factor, y_factor);
	if (unsigned_x && unsigned_y) {
		tcg_gen_movi_i32(extension, 0);
	} else {
		tcg_gen_shri_i32(extension, product, 31);
	}
	tcg_gen_st16_i32(x, tcg_env, offsetof(teak_state_t, x[0]));
	tcg_gen_st_i32(product, tcg_env, offsetof(teak_state_t, p[0]));
	tcg_gen_st8_i32(extension, tcg_env, offsetof(teak_state_t, product_extension[0]));
}

static void tcg_emit_multiply_operation(const teak_insn_t *instruction, TCGv_i32 x);

static void tcg_emit_multiply_immediate(const teak_insn_t *instruction) {
	int16_t immediate = (int8_t) instruction->immediate;
	tcg_emit_multiply_operation(instruction, tcg_constant_i32(immediate));
}

static void tcg_emit_multiply_register(const teak_insn_t *instruction) {
	TCGv_i32 x = tcg_temp_new_i32();

	gen_helper_teak_tcg_mov_register_read(x, tcg_env, tcg_constant_i32(instruction->register_code));
	tcg_emit_multiply_operation(instruction, x);
}

static void tcg_emit_multiply_r6(const teak_insn_t *instruction) {
	TCGv_i32 x = tcg_temp_new_i32();

	tcg_gen_ld16u_i32(x, tcg_env, offsetof(teak_state_t, r[6]));
	tcg_emit_multiply_operation(instruction, x);
}

static void tcg_emit_repeat(TCGv_i32 count) {
	tcg_gen_st16_i32(count, tcg_env, offsetof(teak_state_t, repc));
	tcg_gen_st8_i32(tcg_constant_i32(1), tcg_env, offsetof(teak_state_t, repeat_active));
}

static void tcg_emit_repeat_register(uint8_t register_code) {
	TCGv_i32 count = tcg_temp_new_i32();

	gen_helper_teak_tcg_register_read(count, tcg_env, tcg_constant_i32(register_code));
	tcg_emit_repeat(count);
}

static void tcg_emit_block_repeat(const teak_insn_t *instruction, TCGv_i32 count, uint8_t level) {
	g_assert(level < TEAK_BLOCK_REPEAT_LEVELS);

	tcg_gen_st_i32(tcg_constant_i32(tcg_instruction_end(instruction)), tcg_env,
		offsetof(teak_state_t, block_repeat_start) + level * sizeof(uint32_t));
	tcg_gen_st_i32(tcg_constant_i32(instruction->branch_target), tcg_env,
		offsetof(teak_state_t, block_repeat_end) + level * sizeof(uint32_t));
	tcg_gen_st16_i32(count, tcg_env,
		offsetof(teak_state_t, block_repeat_lc) + level * sizeof(uint16_t));
	tcg_gen_st8_i32(tcg_constant_i32(1), tcg_env, offsetof(teak_state_t, lp));
	tcg_gen_st8_i32(tcg_constant_i32(level + 1), tcg_env, offsetof(teak_state_t, bcn));
}

static void tcg_emit_break(void) {
	TCGv_i32 bcn = tcg_temp_new_i32();
	TCGv_i32 lp = tcg_temp_new_i32();

	tcg_gen_ld8u_i32(bcn, tcg_env, offsetof(teak_state_t, bcn));
	tcg_gen_subi_i32(bcn, bcn, 1);
	tcg_gen_st8_i32(bcn, tcg_env, offsetof(teak_state_t, bcn));
	tcg_gen_setcondi_i32(TCG_COND_NE, lp, bcn, 0);
	tcg_gen_st8_i32(lp, tcg_env, offsetof(teak_state_t, lp));
}

static void tcg_emit_control_transfer(const teak_insn_t *instruction, bool call) {
	uint16_t fallthrough = tcg_instruction_end(instruction);
	TCGv_i32 target_pc = tcg_constant_i32(instruction->branch_target);
	TCGv_i32 fallthrough_pc = tcg_constant_i32(fallthrough);
	TCGv_i32 branch_exit = tcg_constant_i32(TEAK_EXIT_BRANCH);
	TCGv_i32 no_exit = tcg_constant_i32(TEAK_EXIT_NONE);
	TCGLabel *not_taken;
	TCGLabel *done;

	if (instruction->condition == TEAK_COND_TRUE) {
		if (call)
			tcg_emit_push_pc(fallthrough);
		tcg_gen_st_i32(target_pc, tcg_env, offsetof(teak_state_t, pc));
		tcg_gen_st_i32(branch_exit, tcg_env, offsetof(teak_state_t, exit_reason));
		return;
	}

	not_taken = tcg_emit_condition_skip(instruction->condition);
	done = gen_new_label();
	if (call)
		tcg_emit_push_pc(fallthrough);
	tcg_gen_st_i32(target_pc, tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_st_i32(branch_exit, tcg_env, offsetof(teak_state_t, exit_reason));
	tcg_gen_br(done);
	gen_set_label(not_taken);
	tcg_gen_st_i32(fallthrough_pc, tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_st_i32(no_exit, tcg_env, offsetof(teak_state_t, exit_reason));
	gen_set_label(done);
}

static void tcg_emit_call_accumulator(const teak_insn_t *instruction) {
	TCGv_i64 accumulator = tcg_temp_new_i64();
	TCGv_i32 target = tcg_temp_new_i32();
	uint16_t return_address = tcg_instruction_end(instruction);
	size_t offset = offsetof(teak_state_t, a) + instruction->accumulator_index * sizeof(uint64_t);

	tcg_emit_push_pc(return_address);
	tcg_gen_ld_i64(accumulator, tcg_env, offset);
	tcg_gen_extrl_i64_i32(target, accumulator);
	tcg_gen_andi_i32(target, target, 0xFFFFU);
	tcg_gen_st_i32(target, tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_st_i32(tcg_constant_i32(TEAK_EXIT_BRANCH), tcg_env,
		offsetof(teak_state_t, exit_reason));
}

static void tcg_emit_interrupt_return_state(void) {
	TCGv_i32 trap_active = tcg_temp_new_i32();
	TCGv_i32 nmi_active = tcg_temp_new_i32();
	TCGLabel *done = gen_new_label();

	tcg_gen_ld8u_i32(trap_active, tcg_env, offsetof(teak_state_t, trap_active));
	tcg_gen_ld8u_i32(nmi_active, tcg_env, offsetof(teak_state_t, nmi_active));
	tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, trap_active));
	tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, nmi_active));
	tcg_gen_brcondi_i32(TCG_COND_NE, trap_active, 0, done);
	tcg_gen_st8_i32(tcg_constant_i32(1), tcg_env, offsetof(teak_state_t, ie));
	tcg_gen_brcondi_i32(TCG_COND_NE, nmi_active, 0, done);
	tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, maskable_interrupt_active));
	gen_set_label(done);
}

static void tcg_emit_return(const teak_insn_t *instruction) {
	uint16_t fallthrough = tcg_instruction_end(instruction);
	bool enable_interrupt = instruction->opcode == TEAK_OP_RETURN_INTERRUPT;
	bool restore_context = enable_interrupt && instruction->context_switch;
	uint16_t stack_adjust = instruction->opcode == TEAK_OP_RETURN_STACK ? instruction->immediate : 0;
	TCGv_i32 fallthrough_pc = tcg_constant_i32(fallthrough);
	TCGv_i32 branch_exit = tcg_constant_i32(TEAK_EXIT_BRANCH);
	TCGv_i32 no_exit = tcg_constant_i32(TEAK_EXIT_NONE);
	TCGv_i32 target_pc;
	TCGLabel *not_taken;
	TCGLabel *done;

	if (instruction->condition == TEAK_COND_TRUE) {
		target_pc = tcg_emit_pop_pc(stack_adjust, true);
		tcg_gen_st_i32(target_pc, tcg_env, offsetof(teak_state_t, pc));
		if (enable_interrupt)
			tcg_emit_interrupt_return_state();
		if (restore_context)
			gen_helper_teak_tcg_context_switch(tcg_env, tcg_constant_i32(1));
		tcg_gen_st_i32(branch_exit, tcg_env, offsetof(teak_state_t, exit_reason));
		return;
	}

	not_taken = tcg_emit_condition_skip(instruction->condition);
	done = gen_new_label();
	target_pc = tcg_emit_pop_pc(stack_adjust, true);
	tcg_gen_st_i32(target_pc, tcg_env, offsetof(teak_state_t, pc));
	if (enable_interrupt)
		tcg_emit_interrupt_return_state();
	if (restore_context)
		gen_helper_teak_tcg_context_switch(tcg_env, tcg_constant_i32(1));
	tcg_gen_st_i32(branch_exit, tcg_env, offsetof(teak_state_t, exit_reason));
	tcg_gen_br(done);
	gen_set_label(not_taken);
	tcg_gen_st_i32(fallthrough_pc, tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_st_i32(no_exit, tcg_env, offsetof(teak_state_t, exit_reason));
	gen_set_label(done);
}

static void tcg_emit_arithmetic_flags(TCGv_i64 value, TCGCond carry_condition, uint64_t carry_value, uint64_t overflow_value) {
	TCGv_i64 flag64 = tcg_temp_new_i64();
	TCGv_i32 flag = tcg_temp_new_i32();
	TCGv_i32 temporary = tcg_temp_new_i32();

	tcg_gen_setcondi_i64(carry_condition, flag64, value, carry_value);
	tcg_gen_extrl_i64_i32(flag, flag64);
	tcg_gen_st8_i32(flag, tcg_env, offsetof(teak_state_t, fc0));

	tcg_gen_setcondi_i64(TCG_COND_EQ, flag64, value, overflow_value);
	tcg_gen_extrl_i64_i32(flag, flag64);
	tcg_gen_st8_i32(flag, tcg_env, offsetof(teak_state_t, fv));
	tcg_gen_ld8u_i32(temporary, tcg_env, offsetof(teak_state_t, fvl));
	tcg_gen_or_i32(temporary, temporary, flag);
	tcg_gen_st8_i32(temporary, tcg_env, offsetof(teak_state_t, fvl));
}

static void tcg_emit_rotate(teak_moda_operation_t operation, TCGv_i64 value, TCGv_i64 result) {
	TCGv_i64 old_carry64 = tcg_temp_new_i64();
	TCGv_i64 new_carry64 = tcg_temp_new_i64();
	TCGv_i32 old_carry = tcg_temp_new_i32();
	TCGv_i32 new_carry = tcg_temp_new_i32();

	tcg_gen_ld8u_i32(old_carry, tcg_env, offsetof(teak_state_t, fc0));
	tcg_gen_extu_i32_i64(old_carry64, old_carry);
	if (operation == TEAK_MODA_ROR) {
		tcg_gen_andi_i64(new_carry64, value, 1);
		tcg_gen_shri_i64(result, value, 1);
		tcg_gen_shli_i64(old_carry64, old_carry64, TEAK_ACCUMULATOR_SIGN_BIT);
	} else {
		g_assert(operation == TEAK_MODA_ROL);
		tcg_gen_shri_i64(new_carry64, value, TEAK_ACCUMULATOR_SIGN_BIT);
		tcg_gen_shli_i64(result, value, 1);
	}
	tcg_gen_or_i64(result, result, old_carry64);
	tcg_gen_extrl_i64_i32(new_carry, new_carry64);
	tcg_gen_andi_i32(new_carry, new_carry, 1);
	tcg_gen_st8_i32(new_carry, tcg_env, offsetof(teak_state_t, fc0));
}

static void tcg_emit_shift_left(TCGv_i64 value, TCGv_i64 result, uint32_t amount) {
	TCGv_i64 canonical = tcg_temp_new_i64();
	TCGv_i64 temporary64 = tcg_temp_new_i64();
	TCGv_i64 carry64 = tcg_temp_new_i64();
	TCGv_i64 overflow64 = tcg_temp_new_i64();
	TCGv_i32 shift_mode = tcg_temp_new_i32();
	TCGv_i32 carry = tcg_temp_new_i32();
	TCGv_i32 overflow = tcg_temp_new_i32();
	TCGv_i32 actual_overflow = tcg_temp_new_i32();
	TCGv_i32 arithmetic = tcg_temp_new_i32();
	TCGv_i32 latched_overflow = tcg_temp_new_i32();
	TCGv_i32 zero = tcg_constant_i32(0);

	tcg_gen_ld8u_i32(shift_mode, tcg_env, offsetof(teak_state_t, s));
	tcg_gen_shli_i64(canonical, value, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(canonical, canonical, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_shli_i64(result, value, amount);
	tcg_gen_shri_i64(carry64, result, TEAK_ACCUMULATOR_BITS);
	tcg_gen_shli_i64(temporary64, value, TEAK_ACCUMULATOR_HOST_SHIFT + amount);
	tcg_gen_sari_i64(temporary64, temporary64, TEAK_ACCUMULATOR_HOST_SHIFT + amount);
	tcg_gen_setcond_i64(TCG_COND_NE, overflow64, canonical, temporary64);
	tcg_gen_extrl_i64_i32(overflow, overflow64);

	tcg_gen_ld8u_i32(actual_overflow, tcg_env, offsetof(teak_state_t, fv));
	tcg_gen_movcond_i32(TCG_COND_EQ, actual_overflow, shift_mode, zero, overflow, actual_overflow);
	tcg_gen_st8_i32(actual_overflow, tcg_env, offsetof(teak_state_t, fv));

	tcg_gen_setcondi_i32(TCG_COND_EQ, arithmetic, shift_mode, 0);
	tcg_gen_and_i32(overflow, overflow, arithmetic);
	tcg_gen_ld8u_i32(latched_overflow, tcg_env, offsetof(teak_state_t, fvl));
	tcg_gen_or_i32(latched_overflow, latched_overflow, overflow);
	tcg_gen_st8_i32(latched_overflow, tcg_env, offsetof(teak_state_t, fvl));

	tcg_gen_extrl_i64_i32(carry, carry64);
	tcg_gen_andi_i32(carry, carry, 1);
	tcg_gen_st8_i32(carry, tcg_env, offsetof(teak_state_t, fc0));
}

static void tcg_emit_shift_right(TCGv_i64 value, TCGv_i64 result, uint32_t amount) {
	TCGv_i64 canonical = tcg_temp_new_i64();
	TCGv_i64 arithmetic_result = tcg_temp_new_i64();
	TCGv_i64 logical_result = tcg_temp_new_i64();
	TCGv_i64 carry64 = tcg_temp_new_i64();
	TCGv_i64 shift_mode64 = tcg_temp_new_i64();
	TCGv_i64 zero64 = tcg_constant_i64(0);
	TCGv_i32 shift_mode = tcg_temp_new_i32();
	TCGv_i32 carry = tcg_temp_new_i32();
	TCGv_i32 overflow = tcg_temp_new_i32();
	TCGv_i32 actual_overflow = tcg_temp_new_i32();
	TCGv_i32 zero = tcg_constant_i32(0);

	tcg_gen_ld8u_i32(shift_mode, tcg_env, offsetof(teak_state_t, s));
	tcg_gen_extu_i32_i64(shift_mode64, shift_mode);
	tcg_gen_shli_i64(canonical, value, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(canonical, canonical, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_shri_i64(carry64, value, amount - 1);
	tcg_gen_sari_i64(arithmetic_result, canonical, amount);
	tcg_gen_shri_i64(logical_result, value, amount);
	tcg_gen_movcond_i64(TCG_COND_EQ, result, shift_mode64, zero64, arithmetic_result, logical_result);

	tcg_gen_ld8u_i32(overflow, tcg_env, offsetof(teak_state_t, fv));
	tcg_gen_movcond_i32(TCG_COND_EQ, actual_overflow, shift_mode, zero, zero, overflow);
	tcg_gen_st8_i32(actual_overflow, tcg_env, offsetof(teak_state_t, fv));

	tcg_gen_extrl_i64_i32(carry, carry64);
	tcg_gen_andi_i32(carry, carry, 1);
	tcg_gen_st8_i32(carry, tcg_env, offsetof(teak_state_t, fc0));
}

static void tcg_emit_shift(teak_moda_operation_t operation, TCGv_i64 value, TCGv_i64 result) {
	bool left = operation == TEAK_MODA_SHL || operation == TEAK_MODA_SHL4;
	bool four_bits = operation == TEAK_MODA_SHR4 || operation == TEAK_MODA_SHL4;
	uint32_t amount = four_bits ? 4 : 1;

	if (left) {
		tcg_emit_shift_left(value, result, amount);
	} else {
		tcg_emit_shift_right(value, result, amount);
	}
}

static size_t tcg_ab_offset(uint8_t accumulator) {
	size_t bank_offset = accumulator < 2 ? offsetof(teak_state_t, b) : offsetof(teak_state_t, a);
	return bank_offset + (accumulator & 1U) * sizeof(uint64_t);
}

static TCGv_i32 tcg_emit_imm8_data_address(uint8_t memory_address);
static TCGv_i32 tcg_emit_rn_address(uint8_t address_register, teak_step_t step_mode, bool disable_modulo);
static void tcg_emit_modify_rn(const teak_insn_t *instruction);

static void tcg_emit_shift_zero(TCGv_i64 value, TCGv_i64 result) {
	TCGv_i32 shift_mode = tcg_temp_new_i32();
	TCGv_i32 overflow = tcg_temp_new_i32();
	TCGv_i32 zero = tcg_constant_i32(0);

	tcg_gen_mov_i64(result, value);
	tcg_gen_st8_i32(zero, tcg_env, offsetof(teak_state_t, fc0));
	tcg_gen_ld8u_i32(shift_mode, tcg_env, offsetof(teak_state_t, s));
	tcg_gen_ld8u_i32(overflow, tcg_env, offsetof(teak_state_t, fv));
	tcg_gen_movcond_i32(TCG_COND_EQ, overflow, shift_mode, zero, zero, overflow);
	tcg_gen_st8_i32(overflow, tcg_env, offsetof(teak_state_t, fv));
}

static void tcg_emit_shift_immediate(const teak_insn_t *instruction) {
	TCGv_i64 value = tcg_temp_new_i64();
	TCGv_i64 result = tcg_temp_new_i64();
	size_t source_offset = tcg_ab_offset(instruction->source_accumulator);
	size_t destination_offset = tcg_ab_offset(instruction->destination_accumulator);

	tcg_gen_ld_i64(value, tcg_env, source_offset);
	tcg_gen_andi_i64(value, value, TEAK_ACCUMULATOR_MASK);
	if (instruction->shift > 0) {
		tcg_emit_shift_left(value, result, instruction->shift);
	} else if (instruction->shift < 0) {
		tcg_emit_shift_right(value, result, -instruction->shift);
	} else {
		tcg_emit_shift_zero(value, result);
	}
	tcg_gen_andi_i64(result, result, TEAK_ACCUMULATOR_MASK);
	tcg_gen_shli_i64(result, result, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(result, result, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_emit_accumulator_value_flags(result);
	tcg_gen_st_i64(result, tcg_env, destination_offset);
}

static void tcg_emit_shift_conditional(const teak_insn_t *instruction) {
	TCGv_i32 source = tcg_constant_i32(instruction->source_accumulator);
	TCGv_i32 destination = tcg_constant_i32(instruction->destination_accumulator);
	TCGLabel *skip = NULL;

	if (instruction->condition != TEAK_COND_TRUE)
		skip = tcg_emit_condition_skip(instruction->condition);
	gen_helper_teak_tcg_shift_accumulator(tcg_env, source, destination);
	if (skip != NULL)
		gen_set_label(skip);
}

static void tcg_emit_accumulator_value_flags(TCGv_i64 value) {
	TCGv_i64 flag64 = tcg_temp_new_i64();
	TCGv_i64 temporary64 = tcg_temp_new_i64();
	TCGv_i32 flag = tcg_temp_new_i32();
	TCGv_i32 temporary = tcg_temp_new_i32();

	tcg_gen_setcondi_i64(TCG_COND_EQ, flag64, value, 0);
	tcg_gen_extrl_i64_i32(flag, flag64);
	tcg_gen_st8_i32(flag, tcg_env, offsetof(teak_state_t, fz));

	tcg_gen_shri_i64(flag64, value, TEAK_ACCUMULATOR_SIGN_BIT);
	tcg_gen_extrl_i64_i32(temporary, flag64);
	tcg_gen_andi_i32(temporary, temporary, 1);
	tcg_gen_st8_i32(temporary, tcg_env, offsetof(teak_state_t, fm));

	tcg_gen_ext32s_i64(temporary64, value);
	tcg_gen_setcond_i64(TCG_COND_NE, flag64, value, temporary64);
	tcg_gen_extrl_i64_i32(temporary, flag64);
	tcg_gen_st8_i32(temporary, tcg_env, offsetof(teak_state_t, fe));

	tcg_gen_shri_i64(flag64, value, 30);
	tcg_gen_extrl_i64_i32(temporary, flag64);
	tcg_gen_andi_i32(temporary, temporary, 3);
	tcg_gen_shri_i32(flag, temporary, 1);
	tcg_gen_xor_i32(temporary, temporary, flag);
	tcg_gen_andi_i32(temporary, temporary, 1);
	tcg_gen_ld8u_i32(flag, tcg_env, offsetof(teak_state_t, fe));
	tcg_gen_xori_i32(flag, flag, 1);
	tcg_gen_and_i32(temporary, temporary, flag);
	tcg_gen_ld8u_i32(flag, tcg_env, offsetof(teak_state_t, fz));
	tcg_gen_or_i32(temporary, temporary, flag);
	tcg_gen_st8_i32(temporary, tcg_env, offsetof(teak_state_t, fn));
}

static void tcg_emit_data_bus_saturation(TCGv_i64 value) {
	TCGv_i64 signed32 = tcg_temp_new_i64();
	TCGv_i64 saturated = tcg_temp_new_i64();
	TCGv_i64 condition64 = tcg_temp_new_i64();
	TCGv_i64 zero = tcg_constant_i64(0);
	TCGv_i64 minimum = tcg_constant_i64(INT32_MIN);
	TCGv_i64 maximum = tcg_constant_i64(INT32_MAX);
	TCGv_i32 condition = tcg_temp_new_i32();
	TCGv_i32 saturation_enabled = tcg_temp_new_i32();
	TCGv_i32 flm = tcg_temp_new_i32();

	tcg_gen_ext32s_i64(signed32, value);
	tcg_gen_setcond_i64(TCG_COND_NE, condition64, value, signed32);
	tcg_gen_extrl_i64_i32(condition, condition64);
	tcg_gen_ld8u_i32(saturation_enabled, tcg_env, offsetof(teak_state_t, sat));
	tcg_gen_xori_i32(saturation_enabled, saturation_enabled, 1);
	tcg_gen_and_i32(condition, condition, saturation_enabled);

	tcg_gen_movcond_i64(TCG_COND_LT, saturated, value, zero, minimum, maximum);
	tcg_gen_extu_i32_i64(condition64, condition);
	tcg_gen_movcond_i64(TCG_COND_NE, value, condition64, zero, saturated, value);

	tcg_gen_ld8u_i32(flm, tcg_env, offsetof(teak_state_t, flm));
	tcg_gen_or_i32(flm, flm, condition);
	tcg_gen_st8_i32(flm, tcg_env, offsetof(teak_state_t, flm));
}

static void tcg_emit_add_sub_flags(TCGv_i64 left, TCGv_i64 right, TCGv_i64 result, bool subtract) {
	TCGv_i64 adjusted_right = tcg_temp_new_i64();
	TCGv_i64 temporary = tcg_temp_new_i64();
	TCGv_i64 flag64 = tcg_temp_new_i64();
	TCGv_i32 flag = tcg_temp_new_i32();
	TCGv_i32 latched = tcg_temp_new_i32();

	tcg_gen_shri_i64(flag64, result, TEAK_ACCUMULATOR_BITS);
	tcg_gen_extrl_i64_i32(flag, flag64);
	tcg_gen_andi_i32(flag, flag, 1);
	tcg_gen_st8_i32(flag, tcg_env, offsetof(teak_state_t, fc0));

	if (subtract) {
		tcg_gen_not_i64(adjusted_right, right);
	} else {
		tcg_gen_mov_i64(adjusted_right, right);
	}
	tcg_gen_xor_i64(temporary, left, adjusted_right);
	tcg_gen_not_i64(temporary, temporary);
	tcg_gen_xor_i64(flag64, left, result);
	tcg_gen_and_i64(flag64, flag64, temporary);
	tcg_gen_shri_i64(flag64, flag64, TEAK_ACCUMULATOR_SIGN_BIT);
	tcg_gen_extrl_i64_i32(flag, flag64);
	tcg_gen_andi_i32(flag, flag, 1);
	tcg_gen_st8_i32(flag, tcg_env, offsetof(teak_state_t, fv));
	tcg_gen_ld8u_i32(latched, tcg_env, offsetof(teak_state_t, fvl));
	tcg_gen_or_i32(latched, latched, flag);
	tcg_gen_st8_i32(latched, tcg_env, offsetof(teak_state_t, fvl));
}

static void tcg_emit_accumulator_saturation(TCGv_i64 value) {
	TCGv_i64 signed32 = tcg_temp_new_i64();
	TCGv_i64 condition64 = tcg_temp_new_i64();
	TCGv_i64 saturated = tcg_temp_new_i64();
	TCGv_i64 zero = tcg_constant_i64(0);
	TCGv_i64 minimum = tcg_constant_i64((uint64_t) (int64_t) INT32_MIN);
	TCGv_i64 maximum = tcg_constant_i64(INT32_MAX);
	TCGv_i32 condition = tcg_temp_new_i32();
	TCGv_i32 saturation_disabled = tcg_temp_new_i32();
	TCGv_i32 flm = tcg_temp_new_i32();

	tcg_gen_ext32s_i64(signed32, value);
	tcg_gen_setcond_i64(TCG_COND_NE, condition64, value, signed32);
	tcg_gen_extrl_i64_i32(condition, condition64);
	tcg_gen_ld8u_i32(saturation_disabled, tcg_env, offsetof(teak_state_t, sata));
	tcg_gen_xori_i32(saturation_disabled, saturation_disabled, 1);
	tcg_gen_and_i32(condition, condition, saturation_disabled);

	tcg_gen_movcond_i64(TCG_COND_LT, saturated, value, zero, minimum, maximum);
	tcg_gen_extu_i32_i64(condition64, condition);
	tcg_gen_movcond_i64(TCG_COND_NE, value, condition64, zero, saturated, value);

	tcg_gen_ld8u_i32(flm, tcg_env, offsetof(teak_state_t, flm));
	tcg_gen_or_i32(flm, flm, condition);
	tcg_gen_st8_i32(flm, tcg_env, offsetof(teak_state_t, flm));
}

static void tcg_emit_alu_accumulator(const teak_insn_t *instruction, TCGv_i64 operand) {
	size_t accumulator_offset = offsetof(teak_state_t, a) + instruction->accumulator_index * sizeof(uint64_t);
	TCGv_i64 value = tcg_temp_new_i64();
	TCGv_i64 normalized_operand = tcg_temp_new_i64();
	TCGv_i64 result = tcg_temp_new_i64();
	bool addition = instruction->alu_operation == TEAK_ALU_ADD ||
		instruction->alu_operation == TEAK_ALU_ADDH || instruction->alu_operation == TEAK_ALU_ADDL;
	bool comparison = instruction->alu_operation == TEAK_ALU_CMP ||
		instruction->alu_operation == TEAK_ALU_CMPU;
	bool subtraction = instruction->alu_operation == TEAK_ALU_SUB ||
		instruction->alu_operation == TEAK_ALU_SUBH || instruction->alu_operation == TEAK_ALU_SUBL;
	bool saturating = addition || subtraction;

	tcg_gen_ld_i64(value, tcg_env, accumulator_offset);
	tcg_gen_andi_i64(value, value, TEAK_ACCUMULATOR_MASK);
	tcg_gen_andi_i64(normalized_operand, operand, TEAK_ACCUMULATOR_MASK);
	switch (instruction->alu_operation) {
		case TEAK_ALU_OR:
			tcg_gen_or_i64(result, value, normalized_operand);
			break;
		case TEAK_ALU_AND:
			tcg_gen_and_i64(result, value, normalized_operand);
			break;
		case TEAK_ALU_XOR:
			tcg_gen_xor_i64(result, value, normalized_operand);
			break;
		case TEAK_ALU_ADD:
		case TEAK_ALU_ADDH:
		case TEAK_ALU_ADDL:
			tcg_gen_add_i64(result, value, normalized_operand);
			tcg_emit_add_sub_flags(value, normalized_operand, result, false);
			break;
		case TEAK_ALU_CMP:
		case TEAK_ALU_CMPU:
		case TEAK_ALU_SUB:
		case TEAK_ALU_SUBH:
		case TEAK_ALU_SUBL:
			tcg_gen_sub_i64(result, value, normalized_operand);
			tcg_emit_add_sub_flags(value, normalized_operand, result, true);
			break;
		case TEAK_ALU_MSU:
		case TEAK_ALU_SQR:
		case TEAK_ALU_SQRA:
		case TEAK_ALU_TST0:
		case TEAK_ALU_TST1:
			g_assert_not_reached();
	}
	tcg_gen_andi_i64(result, result, TEAK_ACCUMULATOR_MASK);
	tcg_gen_shli_i64(result, result, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(result, result, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_emit_accumulator_value_flags(result);
	if (comparison)
		return;
	if (saturating)
		tcg_emit_accumulator_saturation(result);
	tcg_gen_st_i64(result, tcg_env, accumulator_offset);
}

static void tcg_emit_alu_immediate_accumulator(const teak_insn_t *instruction) {
	size_t accumulator_offset = offsetof(teak_state_t, a) + instruction->accumulator_index * sizeof(uint64_t);
	bool preserve_middle_byte = instruction->alu_operation == TEAK_ALU_AND && instruction->words == 1;
	if (!preserve_middle_byte) {
		tcg_emit_alu_accumulator(instruction, tcg_constant_i64(instruction->alu_operand));
		return;
	}

	TCGv_i64 preserved = tcg_temp_new_i64();
	TCGv_i64 result = tcg_temp_new_i64();
	tcg_gen_ld_i64(preserved, tcg_env, accumulator_offset);
	tcg_gen_andi_i64(preserved, preserved, 0xFF00U);
	tcg_emit_alu_accumulator(instruction, tcg_constant_i64(instruction->alu_operand));
	tcg_gen_ld_i64(result, tcg_env, accumulator_offset);
	tcg_gen_andi_i64(result, result, 0xFFFFFFFFFFFF00FFULL);
	tcg_gen_or_i64(result, result, preserved);
	tcg_gen_st_i64(result, tcg_env, accumulator_offset);
}

static void tcg_emit_modify_accumulator(const teak_insn_t *instruction) {
	bool b_accumulator = instruction->opcode == TEAK_OP_MODB3_ACCUMULATOR;
	size_t accumulator_offset = b_accumulator ? offsetof(teak_state_t, b) :
		offsetof(teak_state_t, a);
	TCGv_i64 value = tcg_temp_new_i64();
	TCGv_i64 result = tcg_temp_new_i64();
	accumulator_offset += instruction->accumulator_index * sizeof(uint64_t);

	tcg_gen_ld_i64(value, tcg_env, accumulator_offset);
	tcg_gen_andi_i64(value, value, TEAK_ACCUMULATOR_MASK);
	switch (instruction->moda_operation) {
		case TEAK_MODA_SHR:
		case TEAK_MODA_SHR4:
		case TEAK_MODA_SHL:
		case TEAK_MODA_SHL4:
			tcg_emit_shift(instruction->moda_operation, value, result);
			break;
		case TEAK_MODA_ROR:
		case TEAK_MODA_ROL:
			tcg_emit_rotate(instruction->moda_operation, value, result);
			break;
		case TEAK_MODA_CLR:
			tcg_gen_movi_i64(result, 0);
			break;
		case TEAK_MODA_NOT:
			tcg_gen_not_i64(result, value);
			break;
		case TEAK_MODA_NEG:
			tcg_gen_neg_i64(result, value);
			tcg_emit_arithmetic_flags(value, TCG_COND_NE, 0, TEAK_ACCUMULATOR_SIGN);
			break;
		case TEAK_MODA_RND:
			tcg_gen_addi_i64(result, value, 0x8000);
			tcg_emit_add_sub_flags(value, tcg_constant_i64(0x8000), result, false);
			break;
		case TEAK_MODA_PACR:
			value = tcg_emit_shifted_product();
			tcg_gen_andi_i64(value, value, TEAK_ACCUMULATOR_MASK);
			tcg_gen_addi_i64(result, value, 0x8000);
			tcg_emit_add_sub_flags(value, tcg_constant_i64(0x8000), result, false);
			break;
		case TEAK_MODA_CLRR:
			tcg_gen_movi_i64(result, 0x8000);
			break;
		case TEAK_MODA_INC:
			tcg_gen_addi_i64(result, value, 1);
			tcg_emit_arithmetic_flags(value, TCG_COND_EQ, TEAK_ACCUMULATOR_MASK, TEAK_ACCUMULATOR_MAX);
			break;
		case TEAK_MODA_DEC:
			tcg_gen_subi_i64(result, value, 1);
			tcg_emit_arithmetic_flags(value, TCG_COND_EQ, 0, TEAK_ACCUMULATOR_SIGN);
			break;
		case TEAK_MODA_COPY:
			tcg_gen_ld_i64(result, tcg_env,
				offsetof(teak_state_t, a) + (instruction->accumulator_index ^ 1U) * sizeof(uint64_t));
			break;
		default:
			g_assert_not_reached();
	}

	tcg_gen_andi_i64(result, result, TEAK_ACCUMULATOR_MASK);
	tcg_gen_shli_i64(result, result, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(result, result, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_emit_accumulator_value_flags(result);
	tcg_gen_st_i64(result, tcg_env, accumulator_offset);
}

static void tcg_emit_limit_accumulator(const teak_insn_t *instruction) {
	size_t source_offset = offsetof(teak_state_t, a) + instruction->source_accumulator * sizeof(uint64_t);
	size_t destination_offset = offsetof(teak_state_t, a) +
		instruction->destination_accumulator * sizeof(uint64_t);
	TCGv_i64 value = tcg_temp_new_i64();
	TCGv_i64 result = tcg_temp_new_i64();
	TCGv_i64 limited64 = tcg_temp_new_i64();
	TCGv_i32 limited = tcg_temp_new_i32();
	TCGv_i32 flm = tcg_temp_new_i32();

	tcg_gen_ld_i64(value, tcg_env, source_offset);
	tcg_gen_mov_i64(result, value);
	tcg_gen_movcond_i64(TCG_COND_GT, result, value, tcg_constant_i64(INT32_MAX),
		tcg_constant_i64(INT32_MAX), result);
	tcg_gen_movcond_i64(TCG_COND_LT, result, value, tcg_constant_i64((int64_t) INT32_MIN),
		tcg_constant_i64((int64_t) INT32_MIN), result);
	tcg_gen_setcond_i64(TCG_COND_NE, limited64, result, value);
	tcg_gen_extrl_i64_i32(limited, limited64);
	tcg_gen_ld8u_i32(flm, tcg_env, offsetof(teak_state_t, flm));
	tcg_gen_or_i32(flm, flm, limited);
	tcg_gen_st8_i32(flm, tcg_env, offsetof(teak_state_t, flm));
	tcg_emit_accumulator_value_flags(result);
	tcg_gen_st_i64(result, tcg_env, destination_offset);
}

static TCGv_i64 tcg_emit_exponent_16(TCGv_i32 value) {
	TCGv_i64 result = tcg_temp_new_i64();

	tcg_gen_ext16s_i32(value, value);
	tcg_gen_extu_i32_i64(result, value);
	tcg_gen_shli_i64(result, result, 16);
	return result;
}

static TCGv_i64 tcg_emit_exponent_source(const teak_insn_t *instruction) {
	TCGv_i32 value16;
	TCGv_i64 value;

	switch (instruction->exponent_source) {
		case TEAK_EXPONENT_REGISTER:
			if (instruction->register_code == 24 || instruction->register_code == 25) {
				value = tcg_temp_new_i64();
				tcg_gen_ld_i64(value, tcg_env,
					offsetof(teak_state_t, a) + (instruction->register_code & 1U) * sizeof(uint64_t));
				return value;
			}
			value16 = tcg_temp_new_i32();
			if (instruction->register_code == 12) {
				tcg_gen_movi_i32(value16, tcg_instruction_end(instruction));
			} else {
				gen_helper_teak_tcg_register_read(value16, tcg_env, tcg_constant_i32(instruction->register_code));
			}
			return tcg_emit_exponent_16(value16);

		case TEAK_EXPONENT_B_ACCUMULATOR:
			value = tcg_temp_new_i64();
			tcg_gen_ld_i64(value, tcg_env,
				offsetof(teak_state_t, b) + instruction->accumulator_index * sizeof(uint64_t));
			return value;

		case TEAK_EXPONENT_RN_STEP:
			value16 = tcg_temp_new_i32();
			gen_helper_teak_tcg_data_read(value16, tcg_env,
				tcg_emit_rn_address(instruction->address_register, instruction->step, false));
			return tcg_emit_exponent_16(value16);

		case TEAK_EXPONENT_R6:
			value16 = tcg_temp_new_i32();
			tcg_gen_ld16u_i32(value16, tcg_env, offsetof(teak_state_t, r[6]));
			return tcg_emit_exponent_16(value16);
	}
	g_assert_not_reached();
}

static void tcg_emit_exponent(const teak_insn_t *instruction) {
	TCGv_i64 value = tcg_emit_exponent_source(instruction);
	TCGv_i64 inverted = tcg_temp_new_i64();
	TCGv_i64 sign = tcg_temp_new_i64();
	TCGv_i64 normalized = tcg_temp_new_i64();
	TCGv_i64 exponent = tcg_temp_new_i64();

	tcg_gen_andi_i64(value, value, TEAK_ACCUMULATOR_MASK);
	tcg_gen_xori_i64(inverted, value, TEAK_ACCUMULATOR_MASK);
	tcg_gen_andi_i64(sign, value, TEAK_ACCUMULATOR_SIGN);
	tcg_gen_movcond_i64(TCG_COND_NE, normalized, sign, tcg_constant_i64(0), inverted, value);
	tcg_gen_clzi_i64(exponent, normalized, 64);
	tcg_gen_subi_i64(exponent, exponent, 33);
	tcg_gen_st16_i64(exponent, tcg_env, offsetof(teak_state_t, shift_value));
	if (instruction->write_accumulator)
		tcg_gen_st_i64(exponent, tcg_env,
			offsetof(teak_state_t, a) + instruction->destination_accumulator * sizeof(uint64_t));
}

static void tcg_emit_division_step(const teak_insn_t *instruction) {
	size_t accumulator_offset = offsetof(teak_state_t, a) + instruction->accumulator_index * sizeof(uint64_t);
	TCGv_i32 divisor32 = tcg_temp_new_i32();
	TCGv_i64 divisor = tcg_temp_new_i64();
	TCGv_i64 value = tcg_temp_new_i64();
	TCGv_i64 difference = tcg_temp_new_i64();
	TCGv_i64 negative_result = tcg_temp_new_i64();
	TCGv_i64 positive_result = tcg_temp_new_i64();
	TCGv_i64 result = tcg_temp_new_i64();

	gen_helper_teak_tcg_data_read(divisor32, tcg_env, tcg_emit_imm8_data_address(instruction->memory_address));
	tcg_gen_extu_i32_i64(divisor, divisor32);
	tcg_gen_shli_i64(divisor, divisor, 15);
	tcg_gen_ld_i64(value, tcg_env, accumulator_offset);
	tcg_gen_sub_i64(difference, value, divisor);
	tcg_gen_shli_i64(negative_result, value, 1);
	tcg_gen_shli_i64(positive_result, difference, 1);
	tcg_gen_addi_i64(positive_result, positive_result, 1);
	tcg_gen_movcond_i64(TCG_COND_LT, result, difference, tcg_constant_i64(0), negative_result, positive_result);
	tcg_gen_andi_i64(result, result, TEAK_ACCUMULATOR_MASK);
	tcg_gen_shli_i64(result, result, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(result, result, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_emit_accumulator_value_flags(result);
	tcg_gen_st_i64(result, tcg_env, accumulator_offset);
}

static void tcg_emit_normalize(const teak_insn_t *instruction) {
	size_t accumulator_offset = offsetof(teak_state_t, a) + instruction->accumulator_index * sizeof(uint64_t);
	TCGv_i32 normalized = tcg_temp_new_i32();
	TCGv_i64 value = tcg_temp_new_i64();
	TCGv_i64 result = tcg_temp_new_i64();
	TCGLabel *skip = gen_new_label();

	tcg_gen_ld8u_i32(normalized, tcg_env, offsetof(teak_state_t, fn));
	tcg_gen_brcondi_i32(TCG_COND_NE, normalized, 0, skip);
	tcg_gen_ld_i64(value, tcg_env, accumulator_offset);
	tcg_gen_andi_i64(value, value, TEAK_ACCUMULATOR_MASK);
	tcg_emit_shift_left(value, result, 1);
	tcg_gen_andi_i64(result, result, TEAK_ACCUMULATOR_MASK);
	tcg_gen_shli_i64(result, result, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(result, result, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_emit_accumulator_value_flags(result);
	tcg_gen_st_i64(result, tcg_env, accumulator_offset);
	tcg_emit_modify_rn(instruction);
	gen_set_label(skip);
}

static void tcg_emit_swap_accumulators(const teak_insn_t *instruction) {
	static const teak_tcg_swap_mapping_t mappings[TEAK_SWAP_OPERATION_COUNT] = {
		[TEAK_SWAP_A0_B0] = {
			.destination_sources = { TEAK_TCG_ACCUMULATOR_B0, TEAK_TCG_ACCUMULATOR_A1,
				TEAK_TCG_ACCUMULATOR_A0, TEAK_TCG_ACCUMULATOR_B1 },
			.flags_source = TEAK_TCG_ACCUMULATOR_B0,
		},
		[TEAK_SWAP_A0_B1] = {
			.destination_sources = { TEAK_TCG_ACCUMULATOR_B1, TEAK_TCG_ACCUMULATOR_A1,
				TEAK_TCG_ACCUMULATOR_B0, TEAK_TCG_ACCUMULATOR_A0 },
			.flags_source = TEAK_TCG_ACCUMULATOR_B1,
		},
		[TEAK_SWAP_A1_B0] = {
			.destination_sources = { TEAK_TCG_ACCUMULATOR_A0, TEAK_TCG_ACCUMULATOR_B0,
				TEAK_TCG_ACCUMULATOR_A1, TEAK_TCG_ACCUMULATOR_B1 },
			.flags_source = TEAK_TCG_ACCUMULATOR_B0,
		},
		[TEAK_SWAP_A1_B1] = {
			.destination_sources = { TEAK_TCG_ACCUMULATOR_A0, TEAK_TCG_ACCUMULATOR_B1,
				TEAK_TCG_ACCUMULATOR_B0, TEAK_TCG_ACCUMULATOR_A1 },
			.flags_source = TEAK_TCG_ACCUMULATOR_B1,
		},
		[TEAK_SWAP_A0_B0_A1_B1] = {
			.destination_sources = { TEAK_TCG_ACCUMULATOR_B0, TEAK_TCG_ACCUMULATOR_B1,
				TEAK_TCG_ACCUMULATOR_A0, TEAK_TCG_ACCUMULATOR_A1 },
			.flags_source = TEAK_TCG_ACCUMULATOR_B0,
		},
		[TEAK_SWAP_A0_B1_A1_B0] = {
			.destination_sources = { TEAK_TCG_ACCUMULATOR_B1, TEAK_TCG_ACCUMULATOR_B0,
				TEAK_TCG_ACCUMULATOR_A1, TEAK_TCG_ACCUMULATOR_A0 },
			.flags_source = TEAK_TCG_ACCUMULATOR_B1,
		},
		[TEAK_SWAP_A0_TO_B0_TO_A1] = {
			.destination_sources = { TEAK_TCG_ACCUMULATOR_A0, TEAK_TCG_ACCUMULATOR_B0,
				TEAK_TCG_ACCUMULATOR_A0, TEAK_TCG_ACCUMULATOR_B1 },
			.flags_source = TEAK_TCG_ACCUMULATOR_B0,
		},
		[TEAK_SWAP_A0_TO_B1_TO_A1] = {
			.destination_sources = { TEAK_TCG_ACCUMULATOR_A0, TEAK_TCG_ACCUMULATOR_B1,
				TEAK_TCG_ACCUMULATOR_B0, TEAK_TCG_ACCUMULATOR_A0 },
			.flags_source = TEAK_TCG_ACCUMULATOR_B1,
		},
		[TEAK_SWAP_A1_TO_B0_TO_A0] = {
			.destination_sources = { TEAK_TCG_ACCUMULATOR_B0, TEAK_TCG_ACCUMULATOR_A1,
				TEAK_TCG_ACCUMULATOR_A1, TEAK_TCG_ACCUMULATOR_B1 },
			.flags_source = TEAK_TCG_ACCUMULATOR_B0,
		},
		[TEAK_SWAP_A1_TO_B1_TO_A0] = {
			.destination_sources = { TEAK_TCG_ACCUMULATOR_B1, TEAK_TCG_ACCUMULATOR_A1,
				TEAK_TCG_ACCUMULATOR_B0, TEAK_TCG_ACCUMULATOR_A1 },
			.flags_source = TEAK_TCG_ACCUMULATOR_B1,
		},
		[TEAK_SWAP_B0_TO_A0_TO_B1] = {
			.destination_sources = { TEAK_TCG_ACCUMULATOR_B0, TEAK_TCG_ACCUMULATOR_A1,
				TEAK_TCG_ACCUMULATOR_B0, TEAK_TCG_ACCUMULATOR_A0 },
			.flags_source = TEAK_TCG_ACCUMULATOR_B0,
		},
		[TEAK_SWAP_B0_TO_A1_TO_B1] = {
			.destination_sources = { TEAK_TCG_ACCUMULATOR_A0, TEAK_TCG_ACCUMULATOR_B0,
				TEAK_TCG_ACCUMULATOR_B0, TEAK_TCG_ACCUMULATOR_A1 },
			.flags_source = TEAK_TCG_ACCUMULATOR_B0,
		},
		[TEAK_SWAP_B1_TO_A0_TO_B0] = {
			.destination_sources = { TEAK_TCG_ACCUMULATOR_B1, TEAK_TCG_ACCUMULATOR_A1,
				TEAK_TCG_ACCUMULATOR_A0, TEAK_TCG_ACCUMULATOR_B1 },
			.flags_source = TEAK_TCG_ACCUMULATOR_B1,
		},
		[TEAK_SWAP_B1_TO_A1_TO_B0] = {
			.destination_sources = { TEAK_TCG_ACCUMULATOR_A0, TEAK_TCG_ACCUMULATOR_B1,
				TEAK_TCG_ACCUMULATOR_A1, TEAK_TCG_ACCUMULATOR_B1 },
			.flags_source = TEAK_TCG_ACCUMULATOR_B1,
		},
	};
	static const size_t offsets[TEAK_TCG_ACCUMULATOR_COUNT] = {
		offsetof(teak_state_t, a),
		offsetof(teak_state_t, a) + sizeof(uint64_t),
		offsetof(teak_state_t, b),
		offsetof(teak_state_t, b) + sizeof(uint64_t),
	};
	const teak_tcg_swap_mapping_t *mapping = &mappings[instruction->swap_operation];
	TCGv_i64 values[TEAK_TCG_ACCUMULATOR_COUNT];

	for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
		values[i] = tcg_temp_new_i64();
		tcg_gen_ld_i64(values[i], tcg_env, offsets[i]);
	}
	for (size_t i = 0; i < ARRAY_SIZE(values); i++)
		tcg_gen_st_i64(values[mapping->destination_sources[i]], tcg_env, offsets[i]);
	tcg_emit_accumulator_value_flags(values[mapping->flags_source]);
}

static void tcg_emit_swap16(size_t left_offset, size_t right_offset) {
	TCGv_i32 left = tcg_temp_new_i32();
	TCGv_i32 right = tcg_temp_new_i32();

	tcg_gen_ld16u_i32(left, tcg_env, left_offset);
	tcg_gen_ld16u_i32(right, tcg_env, right_offset);
	tcg_gen_st16_i32(right, tcg_env, left_offset);
	tcg_gen_st16_i32(left, tcg_env, right_offset);
}

static void tcg_emit_swap8(size_t left_offset, size_t right_offset) {
	TCGv_i32 left = tcg_temp_new_i32();
	TCGv_i32 right = tcg_temp_new_i32();

	tcg_gen_ld8u_i32(left, tcg_env, left_offset);
	tcg_gen_ld8u_i32(right, tcg_env, right_offset);
	tcg_gen_st8_i32(right, tcg_env, left_offset);
	tcg_gen_st8_i32(left, tcg_env, right_offset);
}

static void tcg_emit_bank_exchange(const teak_insn_t *instruction) {
	if ((instruction->immediate & TEAK_BANK_CFGI) != 0) {
		tcg_emit_swap8(offsetof(teak_state_t, stepi), offsetof(teak_state_t, stepib));
		tcg_emit_swap16(offsetof(teak_state_t, modi), offsetof(teak_state_t, modib));
	}
	if ((instruction->immediate & TEAK_BANK_R4) != 0)
		tcg_emit_swap16(offsetof(teak_state_t, r) + 4 * sizeof(uint16_t),
			offsetof(teak_state_t, r4b));
	if ((instruction->immediate & TEAK_BANK_R1) != 0)
		tcg_emit_swap16(offsetof(teak_state_t, r) + sizeof(uint16_t),
			offsetof(teak_state_t, r1b));
	if ((instruction->immediate & TEAK_BANK_R0) != 0)
		tcg_emit_swap16(offsetof(teak_state_t, r), offsetof(teak_state_t, r0b));
}

static void tcg_emit_minimum_maximum(const teak_insn_t *instruction) {
	size_t accumulator_base = offsetof(teak_state_t, a);
	size_t accumulator_offset;
	size_t candidate_offset;
	TCGv_i32 pointer = tcg_emit_rn_address(0, instruction->step, false);
	TCGv_i32 candidate32 = tcg_temp_new_i32();
	TCGv_i32 predicate = tcg_temp_new_i32();
	TCGv_i32 mixp = tcg_temp_new_i32();
	TCGv_i64 candidate = tcg_temp_new_i64();
	TCGv_i64 current = tcg_temp_new_i64();
	TCGv_i64 predicate64 = tcg_temp_new_i64();
	TCGCond condition;

	if (instruction->minmax_b_accumulator)
		accumulator_base = offsetof(teak_state_t, b);
	accumulator_offset = accumulator_base + instruction->accumulator_index * sizeof(uint64_t);
	candidate_offset = accumulator_base + (instruction->accumulator_index ^ 1U) * sizeof(uint64_t);

	if (instruction->memory_source) {
		gen_helper_teak_tcg_data_read(candidate32, tcg_env, pointer);
		tcg_gen_extu_i32_i64(candidate, candidate32);
		tcg_gen_ext16s_i64(candidate, candidate);
	} else {
		tcg_gen_ld_i64(candidate, tcg_env, candidate_offset);
	}
	tcg_gen_ld_i64(current, tcg_env, accumulator_offset);
	switch (instruction->minmax_operation) {
		case TEAK_MINMAX_MAX_GE:
			condition = TCG_COND_GE;
			break;
		case TEAK_MINMAX_MAX_GT:
			condition = TCG_COND_GT;
			break;
		case TEAK_MINMAX_MIN_LE:
			condition = TCG_COND_LE;
			break;
		case TEAK_MINMAX_MIN_LT:
			condition = TCG_COND_LT;
			break;
		default:
			g_assert_not_reached();
	}
	tcg_gen_setcond_i64(condition, predicate64, candidate, current);
	tcg_gen_extrl_i64_i32(predicate, predicate64);
	tcg_gen_movcond_i64(TCG_COND_NE, current, predicate64, tcg_constant_i64(0), candidate, current);
	tcg_gen_st_i64(current, tcg_env, accumulator_offset);
	if (instruction->minmax_b_accumulator) {
		tcg_gen_st8_i32(predicate, tcg_env, offsetof(teak_state_t, fm));
		tcg_gen_st8_i32(predicate, tcg_env, offsetof(teak_state_t, fn));
		tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, fe));
		return;
	}

	tcg_gen_st8_i32(predicate, tcg_env, offsetof(teak_state_t, fm));
	tcg_gen_ld16u_i32(mixp, tcg_env, offsetof(teak_state_t, mixp));
	tcg_gen_movcond_i32(TCG_COND_NE, mixp, predicate, tcg_constant_i32(0), pointer, mixp);
	tcg_gen_st16_i32(mixp, tcg_env, offsetof(teak_state_t, mixp));
}

static size_t tcg_accumulator_register_offset(uint8_t register_code) {
	if (register_code >= 16 && register_code <= 19)
		return offsetof(teak_state_t, b) + (register_code & 1U) * sizeof(uint64_t);

	g_assert(register_code >= 24 && register_code <= 29);
	return offsetof(teak_state_t, a) + (register_code & 1U) * sizeof(uint64_t);
}

static bool tcg_alu_sign_extends_16(teak_alu_operation_t operation) {
	switch (operation) {
		case TEAK_ALU_OR:
		case TEAK_ALU_AND:
		case TEAK_ALU_XOR:
		case TEAK_ALU_TST0:
		case TEAK_ALU_TST1:
		case TEAK_ALU_ADDL:
		case TEAK_ALU_SUBL:
		case TEAK_ALU_CMPU:
			return false;

		case TEAK_ALU_ADD:
		case TEAK_ALU_CMP:
		case TEAK_ALU_SUB:
		case TEAK_ALU_MSU:
		case TEAK_ALU_ADDH:
		case TEAK_ALU_SUBH:
		case TEAK_ALU_SQR:
		case TEAK_ALU_SQRA:
			return true;
	}
	g_assert_not_reached();
}

static bool tcg_alu_shifts_operand(teak_alu_operation_t operation) {
	return operation == TEAK_ALU_ADDH || operation == TEAK_ALU_SUBH;
}

static TCGv_i64 tcg_emit_shifted_product(void) {
	TCGv_i32 shift = tcg_temp_new_i32();
	TCGv_i32 extension = tcg_temp_new_i32();
	TCGv_i64 product = tcg_temp_new_i64();
	TCGv_i64 extension64 = tcg_temp_new_i64();
	TCGv_i64 result = tcg_temp_new_i64();
	TCGLabel *shift_right = gen_new_label();
	TCGLabel *shift_left = gen_new_label();
	TCGLabel *shift_left_twice = gen_new_label();
	TCGLabel *done = gen_new_label();

	tcg_gen_ld32u_i64(product, tcg_env, offsetof(teak_state_t, p[0]));
	tcg_gen_ld8u_i32(extension, tcg_env, offsetof(teak_state_t, product_extension[0]));
	tcg_gen_extu_i32_i64(extension64, extension);
	tcg_gen_muli_i64(extension64, extension64, 0xF00000000ULL);
	tcg_gen_or_i64(product, product, extension64);
	tcg_gen_shli_i64(product, product, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(product, product, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_ld8u_i32(shift, tcg_env, offsetof(teak_state_t, product_shift));
	tcg_gen_brcondi_i32(TCG_COND_EQ, shift, 1, shift_right);
	tcg_gen_brcondi_i32(TCG_COND_EQ, shift, 2, shift_left);
	tcg_gen_brcondi_i32(TCG_COND_EQ, shift, 3, shift_left_twice);
	tcg_gen_mov_i64(result, product);
	tcg_gen_br(done);

	gen_set_label(shift_right);
	tcg_gen_sari_i64(result, product, 1);
	tcg_gen_br(done);

	gen_set_label(shift_left);
	tcg_gen_shli_i64(result, product, 1);
	tcg_gen_br(done);

	gen_set_label(shift_left_twice);
	tcg_gen_shli_i64(result, product, 2);
	gen_set_label(done);
	return result;
}

static TCGv_i64 tcg_emit_aligned_product(void) {
	TCGv_i64 product = tcg_emit_shifted_product();
	tcg_gen_shri_i64(product, product, 16);
	tcg_gen_shli_i64(product, product, 40);
	tcg_gen_sari_i64(product, product, 40);
	return product;
}

static void tcg_emit_multiply_operation(const teak_insn_t *instruction, TCGv_i32 x) {
	teak_insn_t accumulator = *instruction;
	bool unsigned_x;
	bool unsigned_y;

	switch (instruction->multiply_operation) {
		case TEAK_MULTIPLY_MPY:
			unsigned_x = false;
			unsigned_y = false;
			break;

		case TEAK_MULTIPLY_MPYSU:
			unsigned_x = true;
			unsigned_y = false;
			break;

		case TEAK_MULTIPLY_MAC:
			accumulator.alu_operation = TEAK_ALU_ADD;
			tcg_emit_alu_accumulator(&accumulator, tcg_emit_shifted_product());
			unsigned_x = false;
			unsigned_y = false;
			break;

		case TEAK_MULTIPLY_MACUS:
			accumulator.alu_operation = TEAK_ALU_ADD;
			tcg_emit_alu_accumulator(&accumulator, tcg_emit_shifted_product());
			unsigned_x = false;
			unsigned_y = true;
			break;

		case TEAK_MULTIPLY_MACSU:
			accumulator.alu_operation = TEAK_ALU_ADD;
			tcg_emit_alu_accumulator(&accumulator, tcg_emit_shifted_product());
			unsigned_x = true;
			unsigned_y = false;
			break;

		case TEAK_MULTIPLY_MAA:
			accumulator.alu_operation = TEAK_ALU_ADD;
			tcg_emit_alu_accumulator(&accumulator, tcg_emit_aligned_product());
			unsigned_x = false;
			unsigned_y = false;
			break;

		case TEAK_MULTIPLY_MACUU:
			accumulator.alu_operation = TEAK_ALU_ADD;
			tcg_emit_alu_accumulator(&accumulator, tcg_emit_shifted_product());
			unsigned_x = true;
			unsigned_y = true;
			break;

		case TEAK_MULTIPLY_MAASU:
			accumulator.alu_operation = TEAK_ALU_ADD;
			tcg_emit_alu_accumulator(&accumulator, tcg_emit_aligned_product());
			unsigned_x = true;
			unsigned_y = false;
			break;

		case TEAK_MULTIPLY_MSU:
			accumulator.alu_operation = TEAK_ALU_SUB;
			tcg_emit_alu_accumulator(&accumulator, tcg_emit_shifted_product());
			unsigned_x = false;
			unsigned_y = false;
			break;

		default:
			g_assert_not_reached();
	}
	tcg_emit_multiply(x, unsigned_x, unsigned_y);
}

static void tcg_emit_alu_multiply_operation(const teak_insn_t *instruction, TCGv_i32 operand) {
	teak_insn_t multiply = *instruction;

	switch (instruction->alu_operation) {
		case TEAK_ALU_MSU:
			multiply.multiply_operation = TEAK_MULTIPLY_MSU;
			break;

		case TEAK_ALU_SQR:
			multiply.multiply_operation = TEAK_MULTIPLY_MPY;
			break;

		case TEAK_ALU_SQRA:
			multiply.multiply_operation = TEAK_MULTIPLY_MAC;
			break;

		default:
			g_assert_not_reached();
	}

	if (instruction->alu_operation != TEAK_ALU_MSU)
		tcg_gen_st16_i32(operand, tcg_env, offsetof(teak_state_t, y[0]));
	tcg_emit_multiply_operation(&multiply, operand);
}

static void tcg_emit_accumulator_test(uint8_t accumulator_index, bool ones, TCGv_i32 operand) {
	size_t offset = offsetof(teak_state_t, a) + accumulator_index * sizeof(uint64_t);
	TCGv_i64 mask64 = tcg_temp_new_i64();
	TCGv_i32 mask = tcg_temp_new_i32();
	TCGv_i32 masked = tcg_temp_new_i32();
	TCGv_i32 predicate = tcg_temp_new_i32();

	tcg_gen_ld_i64(mask64, tcg_env, offset);
	tcg_gen_extrl_i64_i32(mask, mask64);
	tcg_gen_andi_i32(mask, mask, 0xFFFFU);
	tcg_gen_and_i32(masked, operand, mask);
	if (ones) {
		tcg_gen_setcond_i32(TCG_COND_EQ, predicate, masked, mask);
	} else {
		tcg_gen_setcondi_i32(TCG_COND_EQ, predicate, masked, 0);
	}
	tcg_gen_st8_i32(predicate, tcg_env, offsetof(teak_state_t, fz));
}

static void tcg_emit_alu_register_accumulator(const teak_insn_t *instruction) {
	TCGv_i32 operand32;
	TCGv_i64 operand;
	bool sign_extend = tcg_alu_sign_extends_16(instruction->alu_operation);
	bool shift_operand = tcg_alu_shifts_operand(instruction->alu_operation);
	bool multiply = tcg_alu_uses_multiply(instruction->alu_operation);
	bool test_zero = instruction->alu_operation == TEAK_ALU_TST0;
	bool test_one = instruction->alu_operation == TEAK_ALU_TST1;
	if (test_zero || test_one) {
		operand32 = tcg_temp_new_i32();
		gen_helper_teak_tcg_mov_register_read(operand32, tcg_env, tcg_constant_i32(instruction->register_code));
		tcg_emit_accumulator_test(instruction->accumulator_index, test_one, operand32);
		return;
	}

	if (multiply) {
		operand32 = tcg_temp_new_i32();
		gen_helper_teak_tcg_mov_register_read(operand32, tcg_env, tcg_constant_i32(instruction->register_code));
		tcg_emit_alu_multiply_operation(instruction, operand32);
		return;
	}

	if (instruction->register_code == 11) {
		operand = tcg_emit_shifted_product();
		tcg_emit_alu_accumulator(instruction, operand);
		return;
	}
	operand32 = tcg_temp_new_i32();
	operand = tcg_temp_new_i64();
	if (instruction->register_code >= 24 && instruction->register_code <= 25) {
		uint8_t accumulator_index = instruction->register_code - 24;
		size_t accumulator_offset = offsetof(teak_state_t, a) + accumulator_index * sizeof(uint64_t);

		tcg_gen_ld_i64(operand, tcg_env, accumulator_offset);
		tcg_gen_andi_i64(operand, operand, TEAK_ACCUMULATOR_MASK);
		tcg_emit_alu_accumulator(instruction, operand);
		return;
	}
	gen_helper_teak_tcg_mov_register_read(operand32, tcg_env, tcg_constant_i32(instruction->register_code));
	tcg_gen_andi_i32(operand32, operand32, 0xFFFFU);
	tcg_gen_extu_i32_i64(operand, operand32);
	if (sign_extend)
		tcg_gen_ext16s_i64(operand, operand);
	if (shift_operand)
		tcg_gen_shli_i64(operand, operand, 16);
	tcg_emit_alu_accumulator(instruction, operand);
}

static TCGv_i32 tcg_emit_imm8_data_address(uint8_t memory_address) {
	TCGv_i32 address = tcg_temp_new_i32();

	tcg_gen_ld8u_i32(address, tcg_env, offsetof(teak_state_t, page));
	tcg_gen_shli_i32(address, address, 8);
	tcg_gen_ori_i32(address, address, memory_address);
	return address;
}

static TCGv_i32 tcg_emit_r7_address(int16_t offset) {
	TCGv_i32 address = tcg_temp_new_i32();

	tcg_gen_ld16u_i32(address, tcg_env, offsetof(teak_state_t, r[7]));
	tcg_gen_addi_i32(address, address, offset);
	tcg_gen_andi_i32(address, address, 0xFFFFU);
	return address;
}

static void tcg_emit_alu_data_accumulator(const teak_insn_t *instruction, TCGv_i32 address) {
	TCGv_i32 operand32 = tcg_temp_new_i32();
	TCGv_i64 operand = tcg_temp_new_i64();
	bool sign_extend = tcg_alu_sign_extends_16(instruction->alu_operation);
	bool shift_operand = tcg_alu_shifts_operand(instruction->alu_operation);
	bool multiply = tcg_alu_uses_multiply(instruction->alu_operation);
	bool test_zero = instruction->alu_operation == TEAK_ALU_TST0;
	bool test_one = instruction->alu_operation == TEAK_ALU_TST1;

	gen_helper_teak_tcg_data_read(operand32, tcg_env, address);
	if (test_zero || test_one) {
		tcg_emit_accumulator_test(instruction->accumulator_index, test_one, operand32);
		return;
	}
	if (multiply) {
		tcg_emit_alu_multiply_operation(instruction, operand32);
		return;
	}
	tcg_gen_extu_i32_i64(operand, operand32);
	if (sign_extend)
		tcg_gen_ext16s_i64(operand, operand);
	if (shift_operand)
		tcg_gen_shli_i64(operand, operand, 16);
	tcg_emit_alu_accumulator(instruction, operand);
}

static void tcg_emit_alu_data_imm8_accumulator(const teak_insn_t *instruction) {
	tcg_emit_alu_data_accumulator(instruction, tcg_emit_imm8_data_address(instruction->memory_address));
}

static void tcg_emit_alu_data_imm16_accumulator(const teak_insn_t *instruction) {
	tcg_emit_alu_data_accumulator(instruction, tcg_constant_i32(instruction->expansion));
}

static void tcg_emit_alu_r7_offset7_accumulator(const teak_insn_t *instruction) {
	tcg_emit_alu_data_accumulator(instruction, tcg_emit_r7_address(instruction->memory_offset));
}

static void tcg_emit_alu_r7_offset16_accumulator(const teak_insn_t *instruction) {
	tcg_emit_alu_data_accumulator(instruction, tcg_emit_r7_address(instruction->memory_offset));
}

static TCGv_i32 tcg_emit_rn_address(uint8_t address_register, teak_step_t step_mode, bool disable_modulo) {
	size_t register_offset = offsetof(teak_state_t, r) + address_register * sizeof(uint16_t);
	TCGv_i32 address = tcg_temp_new_i32();

	tcg_gen_ld16u_i32(address, tcg_env, register_offset);
	if (step_mode == TEAK_STEP_ZERO)
		return address;

	TCGv_i32 updated = tcg_temp_new_i32();
	TCGv_i32 step = tcg_temp_new_i32();
	switch (step_mode) {
		case TEAK_STEP_INCREASE:
			tcg_gen_movi_i32(step, 1);
			break;

		case TEAK_STEP_DECREASE:
			tcg_gen_movi_i32(step, -1);
			break;

		case TEAK_STEP_PLUS_STEP: {
			size_t step_offset;

			if (address_register < 4) {
				step_offset = offsetof(teak_state_t, stepi);
			} else {
				step_offset = offsetof(teak_state_t, stepj);
			}
			tcg_gen_ld8u_i32(step, tcg_env, step_offset);
			tcg_gen_shli_i32(step, step, 25);
			tcg_gen_sari_i32(step, step, 25);
			break;
		}

		case TEAK_STEP_ZERO:
			g_assert_not_reached();
	}

	if (address_register < 6 && !disable_modulo) {
		TCGv_i32 modulo_enabled = tcg_temp_new_i32();
		TCGLabel *linear = gen_new_label();
		TCGLabel *done = gen_new_label();

		tcg_gen_ld8u_i32(modulo_enabled, tcg_env, offsetof(teak_state_t, modulo_enable));
		tcg_gen_andi_i32(modulo_enabled, modulo_enabled, BIT(address_register));
		tcg_gen_brcondi_i32(TCG_COND_EQ, modulo_enabled, 0, linear);
		gen_helper_teak_tcg_modulo_address(updated, tcg_env,
			tcg_constant_i32(address_register), address, step);
		tcg_gen_br(done);
		gen_set_label(linear);
		tcg_gen_add_i32(updated, address, step);
		gen_set_label(done);
	} else {
		tcg_gen_add_i32(updated, address, step);
	}
	tcg_gen_andi_i32(updated, updated, 0xFFFFU);
	tcg_gen_st16_i32(updated, tcg_env, register_offset);
	return address;
}

static void tcg_emit_modify_rn(const teak_insn_t *instruction) {
	size_t register_offset = offsetof(teak_state_t, r) + instruction->address_register * sizeof(uint16_t);
	TCGv_i32 value = tcg_temp_new_i32();

	tcg_emit_rn_address(instruction->address_register, instruction->step, instruction->disable_modulo);
	tcg_gen_ld16u_i32(value, tcg_env, register_offset);
	tcg_gen_setcondi_i32(TCG_COND_EQ, value, value, 0);
	tcg_gen_st8_i32(value, tcg_env, offsetof(teak_state_t, fr));
}

static void tcg_emit_multiply_rn_step(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_rn_address(instruction->address_register, instruction->step, instruction->disable_modulo);
	TCGv_i32 x = tcg_temp_new_i32();

	gen_helper_teak_tcg_data_read(x, tcg_env, address);
	tcg_emit_multiply_operation(instruction, x);
}

static void tcg_emit_multiply_data_imm8(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_imm8_data_address(instruction->memory_address);
	TCGv_i32 x = tcg_temp_new_i32();

	gen_helper_teak_tcg_data_read(x, tcg_env, address);
	tcg_emit_multiply_operation(instruction, x);
}

static void tcg_emit_multiply_rn_immediate(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_rn_address(instruction->address_register, instruction->step, instruction->disable_modulo);
	TCGv_i32 y = tcg_temp_new_i32();

	gen_helper_teak_tcg_data_read(y, tcg_env, address);
	tcg_gen_st16_i32(y, tcg_env, offsetof(teak_state_t, y[0]));
	tcg_emit_multiply_operation(instruction, tcg_constant_i32(instruction->expansion));
}

static void tcg_emit_multiply_dual_rn(const teak_insn_t *instruction) {
	TCGv_i32 y_address = tcg_emit_rn_address(instruction->y_address_register, instruction->y_step, false);
	TCGv_i32 x_address = tcg_emit_rn_address(instruction->address_register, instruction->step, false);
	TCGv_i32 y = tcg_temp_new_i32();
	TCGv_i32 x = tcg_temp_new_i32();

	gen_helper_teak_tcg_data_read_y(y, tcg_env, y_address);
	gen_helper_teak_tcg_data_read_xz(x, tcg_env, x_address);
	tcg_gen_st16_i32(y, tcg_env, offsetof(teak_state_t, y[0]));
	tcg_emit_multiply_operation(instruction, x);
}

static void tcg_emit_alu_rn_step_accumulator(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_rn_address(instruction->address_register, instruction->step, instruction->disable_modulo);

	tcg_emit_alu_data_accumulator(instruction, address);
}

static void tcg_emit_test_accumulator_data_imm8(const teak_insn_t *instruction) {
	size_t accumulator_offset = offsetof(teak_state_t, a) + instruction->accumulator_index * sizeof(uint64_t);
	TCGv_i32 address = tcg_emit_imm8_data_address(instruction->memory_address);
	TCGv_i64 accumulator64 = tcg_temp_new_i64();
	TCGv_i32 accumulator = tcg_temp_new_i32();
	TCGv_i32 operand = tcg_temp_new_i32();
	TCGv_i32 result = tcg_temp_new_i32();

	tcg_gen_ld_i64(accumulator64, tcg_env, accumulator_offset);
	tcg_gen_extrl_i64_i32(accumulator, accumulator64);
	tcg_gen_andi_i32(accumulator, accumulator, 0xFFFFU);
	gen_helper_teak_tcg_data_read(operand, tcg_env, address);
	if (instruction->accumulator_test == TEAK_ACCUMULATOR_TST1)
		tcg_gen_not_i32(operand, operand);
	tcg_gen_and_i32(result, accumulator, operand);
	tcg_gen_setcondi_i32(TCG_COND_EQ, result, result, 0);
	tcg_gen_st8_i32(result, tcg_env, offsetof(teak_state_t, fz));
}

static void tcg_emit_tstb_imm8(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_imm8_data_address(instruction->memory_address);
	TCGv_i32 value = tcg_temp_new_i32();

	gen_helper_teak_tcg_data_read(value, tcg_env, address);
	tcg_gen_shri_i32(value, value, instruction->bit_index);
	tcg_gen_andi_i32(value, value, 1);
	tcg_gen_st8_i32(value, tcg_env, offsetof(teak_state_t, fz));
}

static void tcg_emit_tstb_rn_step(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_rn_address(instruction->address_register, instruction->step, instruction->disable_modulo);
	TCGv_i32 value = tcg_temp_new_i32();

	gen_helper_teak_tcg_data_read(value, tcg_env, address);
	tcg_gen_shri_i32(value, value, instruction->bit_index);
	tcg_gen_andi_i32(value, value, 1);
	tcg_gen_st8_i32(value, tcg_env, offsetof(teak_state_t, fz));
}

static void tcg_emit_tstb_register(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_temp_new_i32();

	if (instruction->register_code == 24) {
		tcg_gen_ld16u_i32(value, tcg_env, offsetof(teak_state_t, r) + 6 * sizeof(uint16_t));
	} else {
		gen_helper_teak_tcg_register_read(value, tcg_env, tcg_constant_i32(instruction->register_code));
	}
	tcg_gen_shri_i32(value, value, instruction->bit_index);
	tcg_gen_andi_i32(value, value, 1);
	tcg_gen_st8_i32(value, tcg_env, offsetof(teak_state_t, fz));
}

static void tcg_emit_mov_data_imm8_register(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_imm8_data_address(instruction->memory_address);
	TCGv_i32 value = tcg_temp_new_i32();

	gen_helper_teak_tcg_data_read(value, tcg_env, address);
	gen_helper_teak_tcg_mov_register_write(tcg_env, tcg_constant_i32(instruction->register_code), value);
}

static void tcg_emit_mov_data_imm8_accumulator(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_imm8_data_address(instruction->memory_address);
	TCGv_i32 value = tcg_temp_new_i32();
	TCGv_i64 accumulator = tcg_temp_new_i64();

	gen_helper_teak_tcg_data_read(value, tcg_env, address);
	tcg_gen_extu_i32_i64(accumulator, value);
	tcg_gen_ext16s_i64(accumulator, accumulator);
	tcg_emit_accumulator_value_flags(accumulator);
	tcg_gen_st_i64(accumulator, tcg_env, tcg_ab_offset(instruction->destination_accumulator));
}

static void tcg_emit_mov_data_imm8_accumulator_high_eu(const teak_insn_t *instruction) {
	size_t offset = offsetof(teak_state_t, a) + instruction->accumulator_index * sizeof(uint64_t);
	TCGv_i32 address = tcg_emit_imm8_data_address(instruction->memory_address);
	TCGv_i32 value = tcg_temp_new_i32();
	TCGv_i64 accumulator = tcg_temp_new_i64();
	TCGv_i64 preserved = tcg_temp_new_i64();

	gen_helper_teak_tcg_data_read(value, tcg_env, address);
	tcg_gen_ld_i64(preserved, tcg_env, offset);
	tcg_gen_andi_i64(preserved, preserved, 0xF00000000ULL);
	tcg_gen_extu_i32_i64(accumulator, value);
	tcg_gen_shli_i64(accumulator, accumulator, 16);
	tcg_gen_or_i64(accumulator, accumulator, preserved);
	tcg_gen_shli_i64(accumulator, accumulator, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(accumulator, accumulator, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_emit_accumulator_value_flags(accumulator);
	tcg_gen_st_i64(accumulator, tcg_env, offset);
}

static void tcg_emit_mov_register_data_imm8(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_imm8_data_address(instruction->memory_address);
	TCGv_i32 value = tcg_temp_new_i32();

	gen_helper_teak_tcg_mov_register_read(value, tcg_env, tcg_constant_i32(instruction->register_code));
	gen_helper_teak_tcg_data_write(tcg_env, address, value);
}

static void tcg_emit_mov_data_r7_accumulator(const teak_insn_t *instruction) {
	size_t offset = offsetof(teak_state_t, a) + instruction->accumulator_index * sizeof(uint64_t);
	TCGv_i32 address = tcg_emit_r7_address(instruction->memory_offset);
	TCGv_i32 value = tcg_temp_new_i32();
	TCGv_i64 accumulator = tcg_temp_new_i64();

	gen_helper_teak_tcg_data_read(value, tcg_env, address);
	tcg_gen_extu_i32_i64(accumulator, value);
	tcg_gen_ext16s_i64(accumulator, accumulator);
	tcg_emit_accumulator_value_flags(accumulator);
	tcg_gen_st_i64(accumulator, tcg_env, offset);
}

static void tcg_emit_mov_accumulator_low_data_r7(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_r7_address(instruction->memory_offset);
	TCGv_i32 value = tcg_temp_new_i32();

	gen_helper_teak_tcg_mov_register_read(value, tcg_env, tcg_constant_i32(26 + instruction->accumulator_index));
	gen_helper_teak_tcg_data_write(tcg_env, address, value);
}

static void tcg_emit_mov_data_rn_step_register(const teak_insn_t *instruction) {
	if (instruction->register_code == 12)
		return;

	TCGv_i32 address = tcg_emit_rn_address(instruction->address_register, instruction->step, instruction->disable_modulo);
	TCGv_i32 value = tcg_temp_new_i32();
	size_t accumulator_offset;
	TCGv_i64 accumulator;
	bool full;
	bool high;

	gen_helper_teak_tcg_data_read(value, tcg_env, address);
	if (instruction->register_code <= 7) {
		tcg_gen_st16_i32(value, tcg_env, tcg_rn_old_offset(instruction->register_code));
		return;
	}
	if (instruction->register_code >= 20 && instruction->register_code <= 23) {
		gen_helper_teak_tcg_mov_register_write(tcg_env,
			tcg_constant_i32(instruction->register_code), value);
		return;
	}

	accumulator_offset = tcg_accumulator_register_offset(instruction->register_code);
	accumulator = tcg_temp_new_i64();
	full = instruction->register_code >= 24 && instruction->register_code <= 25;
	high = instruction->register_code <= 17 || instruction->register_code >= 28;

	tcg_gen_extu_i32_i64(accumulator, value);
	if (full) {
		tcg_gen_ext16s_i64(accumulator, accumulator);
	} else if (high) {
		tcg_gen_shli_i64(accumulator, accumulator, 16);
		tcg_gen_ext32s_i64(accumulator, accumulator);
	}
	tcg_emit_accumulator_value_flags(accumulator);
	tcg_gen_st_i64(accumulator, tcg_env, accumulator_offset);
}

static void tcg_emit_mov_register_data_rn_step(const teak_insn_t *instruction) {
	TCGv_i32 value;
	TCGv_i32 address;

	if (instruction->register_code <= 7) {
		value = tcg_temp_new_i32();
		tcg_gen_ld16u_i32(value, tcg_env, tcg_rn_old_offset(instruction->register_code));
	} else {
		size_t accumulator_offset = tcg_accumulator_register_offset(instruction->register_code);
		TCGv_i64 accumulator = tcg_temp_new_i64();
		bool high = instruction->register_code <= 17 || instruction->register_code >= 28;

		tcg_gen_ld_i64(accumulator, tcg_env, accumulator_offset);
		tcg_emit_data_bus_saturation(accumulator);
		if (high)
			tcg_gen_shri_i64(accumulator, accumulator, 16);
		value = tcg_temp_new_i32();
		tcg_gen_extrl_i64_i32(value, accumulator);
		tcg_gen_andi_i32(value, value, 0xFFFFU);
	}
	address = tcg_emit_rn_address(instruction->address_register, instruction->step, instruction->disable_modulo);
	gen_helper_teak_tcg_data_write(tcg_env, address, value);
}

static void tcg_emit_mov_data_rn_step_b_accumulator(const teak_insn_t *instruction) {
	size_t accumulator_offset = offsetof(teak_state_t, b) + instruction->accumulator_index * sizeof(uint64_t);
	TCGv_i32 address = tcg_emit_rn_address(instruction->address_register, instruction->step, instruction->disable_modulo);
	TCGv_i32 value = tcg_temp_new_i32();
	TCGv_i64 accumulator = tcg_temp_new_i64();

	gen_helper_teak_tcg_data_read(value, tcg_env, address);
	tcg_gen_extu_i32_i64(accumulator, value);
	tcg_gen_ext16s_i64(accumulator, accumulator);
	tcg_emit_accumulator_value_flags(accumulator);
	tcg_gen_st_i64(accumulator, tcg_env, accumulator_offset);
}

static void tcg_emit_accumulator_extension(size_t offset, uint16_t extension) {
	TCGv_i64 accumulator = tcg_temp_new_i64();

	tcg_gen_ld_i64(accumulator, tcg_env, offset);
	tcg_gen_andi_i64(accumulator, accumulator, UINT32_MAX);
	tcg_gen_ori_i64(accumulator, accumulator, (uint64_t) extension << 32);
	tcg_gen_shli_i64(accumulator, accumulator, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(accumulator, accumulator, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_st_i64(accumulator, tcg_env, offset);
}

static void tcg_emit_mov_imm_st0(uint16_t value) {
	TCGv_i32 interrupt_mask = tcg_temp_new_i32();
	uint8_t limit = value >> 5 & 1U;

	tcg_gen_st8_i32(tcg_constant_i32(value & 1U), tcg_env, offsetof(teak_state_t, sat));
	tcg_gen_st8_i32(tcg_constant_i32(value >> 1 & 1U), tcg_env, offsetof(teak_state_t, ie));
	tcg_gen_ld8u_i32(interrupt_mask, tcg_env, offsetof(teak_state_t, interrupt_mask));
	tcg_gen_andi_i32(interrupt_mask, interrupt_mask, 4);
	tcg_gen_ori_i32(interrupt_mask, interrupt_mask, value >> 2 & 3U);
	tcg_gen_st8_i32(interrupt_mask, tcg_env, offsetof(teak_state_t, interrupt_mask));
	tcg_gen_st8_i32(tcg_constant_i32(value >> 4 & 1U), tcg_env, offsetof(teak_state_t, fr));
	tcg_gen_st8_i32(tcg_constant_i32(limit), tcg_env, offsetof(teak_state_t, flm));
	tcg_gen_st8_i32(tcg_constant_i32(limit), tcg_env, offsetof(teak_state_t, fvl));
	tcg_gen_st8_i32(tcg_constant_i32(value >> 6 & 1U), tcg_env, offsetof(teak_state_t, fe));
	tcg_gen_st8_i32(tcg_constant_i32(value >> 7 & 1U), tcg_env, offsetof(teak_state_t, fc0));
	tcg_gen_st8_i32(tcg_constant_i32(value >> 8 & 1U), tcg_env, offsetof(teak_state_t, fv));
	tcg_gen_st8_i32(tcg_constant_i32(value >> 9 & 1U), tcg_env, offsetof(teak_state_t, fn));
	tcg_gen_st8_i32(tcg_constant_i32(value >> 10 & 1U), tcg_env, offsetof(teak_state_t, fm));
	tcg_gen_st8_i32(tcg_constant_i32(value >> 11 & 1U), tcg_env, offsetof(teak_state_t, fz));
	tcg_emit_accumulator_extension(offsetof(teak_state_t, a[0]), value >> 12);
}

static void tcg_emit_mov_imm_st1(uint16_t value) {
	tcg_gen_st8_i32(tcg_constant_i32(value & 0xFFU), tcg_env, offsetof(teak_state_t, page));
	tcg_gen_st8_i32(tcg_constant_i32(value >> 10 & 3U), tcg_env,
		offsetof(teak_state_t, product_shift));
	tcg_emit_accumulator_extension(offsetof(teak_state_t, a[1]), value >> 12);
}

static void tcg_emit_mov_imm_st2(uint16_t value) {
	TCGv_i32 interrupt_mask = tcg_temp_new_i32();

	tcg_gen_st8_i32(tcg_constant_i32(value & 0x3FU), tcg_env,
		offsetof(teak_state_t, modulo_enable));
	tcg_gen_ld8u_i32(interrupt_mask, tcg_env, offsetof(teak_state_t, interrupt_mask));
	tcg_gen_andi_i32(interrupt_mask, interrupt_mask, 3);
	tcg_gen_ori_i32(interrupt_mask, interrupt_mask, value >> 4 & 4U);
	tcg_gen_st8_i32(interrupt_mask, tcg_env, offsetof(teak_state_t, interrupt_mask));
	tcg_gen_st8_i32(tcg_constant_i32(value >> 7 & 1U), tcg_env, offsetof(teak_state_t, s));
	tcg_gen_st8_i32(tcg_constant_i32(value >> 8 & 1U), tcg_env, offsetof(teak_state_t, ou[0]));
	tcg_gen_st8_i32(tcg_constant_i32(value >> 9 & 1U), tcg_env, offsetof(teak_state_t, ou[1]));
}

static void tcg_emit_mov_imm_accumulator(uint8_t register_code, uint16_t value) {
	uint8_t accumulator_index = register_code & 1U;
	TCGv_i64 accumulator;

	if (register_code <= 25) {
		accumulator = tcg_constant_i64((int16_t) value);
	} else if (register_code <= 27) {
		accumulator = tcg_constant_i64(value);
	} else {
		accumulator = tcg_constant_i64((int64_t) (int16_t) value * 0x10000);
	}
	tcg_emit_accumulator_value_flags(accumulator);
	tcg_gen_st_i64(accumulator, tcg_env,
		offsetof(teak_state_t, a) + accumulator_index * sizeof(uint64_t));
}

static void tcg_emit_mov_imm_icr(uint16_t value) {
	tcg_gen_st8_i32(tcg_constant_i32(value & 1U), tcg_env,
		offsetof(teak_state_t, nonmaskable_context));
	tcg_gen_st8_i32(tcg_constant_i32(value >> 1 & 7U), tcg_env,
		offsetof(teak_state_t, interrupt_context));
	if ((value & 0x10U) != 0) {
		tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, lp));
		tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, bcn));
	}
}

static void tcg_emit_mov_data_imm16_accumulator(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_temp_new_i32();
	TCGv_i64 accumulator = tcg_temp_new_i64();
	size_t offset = offsetof(teak_state_t, a) + instruction->accumulator_index * sizeof(uint64_t);

	gen_helper_teak_tcg_data_read(value, tcg_env, tcg_constant_i32(instruction->expansion));
	tcg_gen_extu_i32_i64(accumulator, value);
	tcg_gen_ext16s_i64(accumulator, accumulator);
	tcg_emit_accumulator_value_flags(accumulator);
	tcg_gen_st_i64(accumulator, tcg_env, offset);
}

static void tcg_emit_mov_accumulator_low_data_imm16(const teak_insn_t *instruction) {
	TCGv_i64 accumulator = tcg_temp_new_i64();
	TCGv_i32 value = tcg_temp_new_i32();
	size_t offset = offsetof(teak_state_t, a) + instruction->accumulator_index * sizeof(uint64_t);

	tcg_gen_ld_i64(accumulator, tcg_env, offset);
	tcg_emit_data_bus_saturation(accumulator);
	tcg_gen_extrl_i64_i32(value, accumulator);
	tcg_gen_andi_i32(value, value, 0xFFFFU);
	gen_helper_teak_tcg_data_write(tcg_env, tcg_constant_i32(instruction->expansion), value);
}

static TCGv_i32 tcg_emit_movp_accumulator_low_read(const teak_insn_t *instruction) {
	size_t accumulator_offset = offsetof(teak_state_t, a) + instruction->accumulator_index * sizeof(uint64_t);
	TCGv_i64 accumulator = tcg_temp_new_i64();
	TCGv_i32 address = tcg_temp_new_i32();
	TCGv_i32 value = tcg_temp_new_i32();

	tcg_gen_ld_i64(accumulator, tcg_env, accumulator_offset);
	tcg_gen_extrl_i64_i32(address, accumulator);
	tcg_gen_andi_i32(address, address, 0xFFFFU);
	gen_helper_teak_tcg_program_read(value, tcg_env, address);
	return value;
}

static void tcg_emit_movp_accumulator_low_register(const teak_insn_t *instruction) {
	TCGv_i32 value;

	if (instruction->destination_register_code == 12)
		return;
	value = tcg_emit_movp_accumulator_low_read(instruction);
	gen_helper_teak_tcg_mov_register_write(tcg_env,
		tcg_constant_i32(instruction->destination_register_code), value);
}

static void tcg_emit_movp_rn_rn(const teak_insn_t *instruction) {
	TCGv_i32 source_address = tcg_emit_rn_address(instruction->address_register, instruction->step, false);
	TCGv_i32 destination_address = tcg_emit_rn_address(instruction->destination_register_code,
		instruction->y_step, false);
	TCGv_i32 value = tcg_temp_new_i32();

	gen_helper_teak_tcg_program_read(value, tcg_env, source_address);
	gen_helper_teak_tcg_data_write(tcg_env, destination_address, value);
}

static void tcg_emit_movd_rn_rn(const teak_insn_t *instruction) {
	TCGv_i32 source_address = tcg_emit_rn_address(instruction->address_register, instruction->step, false);
	TCGv_i32 destination_address = tcg_emit_rn_address(instruction->destination_register_code,
		instruction->y_step, false);
	TCGv_i32 value = tcg_temp_new_i32();

	gen_helper_teak_tcg_data_read(value, tcg_env, source_address);
	gen_helper_teak_tcg_program_write(tcg_env, destination_address, value);
}

static void tcg_emit_movs_result(const teak_insn_t *instruction, TCGv_i32 value, TCGv_i32 shift) {
	gen_helper_teak_tcg_shift_value(tcg_env, value,
		tcg_constant_i32(instruction->destination_accumulator), shift);
}

static void tcg_emit_movs_register(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_temp_new_i32();
	TCGv_i32 shift = tcg_temp_new_i32();

	gen_helper_teak_tcg_register_read(value, tcg_env, tcg_constant_i32(instruction->register_code));
	tcg_gen_ld16u_i32(shift, tcg_env, offsetof(teak_state_t, shift_value));
	tcg_emit_movs_result(instruction, value, shift);
}

static void tcg_emit_movs_rn_step(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_rn_address(instruction->address_register, instruction->step, false);
	TCGv_i32 value = tcg_temp_new_i32();
	TCGv_i32 shift = tcg_temp_new_i32();

	gen_helper_teak_tcg_data_read(value, tcg_env, address);
	tcg_gen_ld16u_i32(shift, tcg_env, offsetof(teak_state_t, shift_value));
	tcg_emit_movs_result(instruction, value, shift);
}

static void tcg_emit_movs_data_imm8(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_temp_new_i32();
	TCGv_i32 shift = tcg_temp_new_i32();

	gen_helper_teak_tcg_data_read(value, tcg_env, tcg_emit_imm8_data_address(instruction->memory_address));
	tcg_gen_ld16u_i32(shift, tcg_env, offsetof(teak_state_t, shift_value));
	tcg_emit_movs_result(instruction, value, shift);
}

static void tcg_emit_movs_r6(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_temp_new_i32();
	TCGv_i32 shift = tcg_temp_new_i32();

	tcg_gen_ld16u_i32(value, tcg_env, offsetof(teak_state_t, r) + 6 * sizeof(uint16_t));
	tcg_gen_ld16u_i32(shift, tcg_env, offsetof(teak_state_t, shift_value));
	tcg_emit_movs_result(instruction, value, shift);
}

static void tcg_emit_movsi_register(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_temp_new_i32();

	tcg_gen_ld16u_i32(value, tcg_env, tcg_rn_old_offset(instruction->register_code));
	tcg_emit_movs_result(instruction, value, tcg_constant_i32(instruction->shift));
}

static void tcg_emit_movr_full(const teak_insn_t *instruction, TCGv_i64 value) {
	TCGv_i64 result = tcg_temp_new_i64();
	bool high_memory_operand = instruction->opcode == TEAK_OP_MOVR_RN_HIGH;
	bool b_destination = high_memory_operand && instruction->destination_accumulator < 2;

	tcg_gen_andi_i64(value, value, TEAK_ACCUMULATOR_MASK);
	tcg_gen_addi_i64(result, value, 0x8000);
	tcg_emit_add_sub_flags(value, tcg_constant_i64(0x8000), result, false);
	tcg_gen_andi_i64(result, result, TEAK_ACCUMULATOR_MASK);
	tcg_gen_shli_i64(result, result, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(result, result, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_emit_accumulator_value_flags(result);
	if (b_destination) {
		TCGv_i64 b1 = tcg_temp_new_i64();

		/* TeakLite I routes both B destinations from B1 while flags use the rounded operand. */
		tcg_gen_ld_i64(b1, tcg_env, offsetof(teak_state_t, b[1]));
		tcg_gen_st_i64(b1, tcg_env, tcg_ab_offset(instruction->destination_accumulator));
		return;
	}
	tcg_gen_st_i64(result, tcg_env, tcg_ab_offset(instruction->destination_accumulator));
}

static void tcg_emit_movr_16(const teak_insn_t *instruction, TCGv_i32 value) {
	TCGv_i32 result = tcg_temp_new_i32();
	TCGv_i32 carry = tcg_temp_new_i32();
	TCGv_i64 accumulator = tcg_temp_new_i64();

	tcg_gen_addi_i32(result, value, 0x8000);
	tcg_gen_shri_i32(carry, result, 16);
	tcg_gen_andi_i32(carry, carry, 1);
	tcg_gen_st8_i32(carry, tcg_env, offsetof(teak_state_t, fc0));
	tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, fv));
	tcg_gen_andi_i32(result, result, 0xFFFFU);
	tcg_gen_extu_i32_i64(accumulator, result);
	tcg_emit_accumulator_value_flags(accumulator);
	tcg_gen_st_i64(accumulator, tcg_env, tcg_ab_offset(instruction->destination_accumulator));
}

static void tcg_emit_movr_register(const teak_insn_t *instruction) {
	if (instruction->register_code == 11) {
		tcg_emit_movr_full(instruction, tcg_emit_shifted_product());
		return;
	}
	if (instruction->register_code == 24 || instruction->register_code == 25) {
		TCGv_i64 value = tcg_temp_new_i64();

		tcg_gen_ld_i64(value, tcg_env,
			offsetof(teak_state_t, a) + (instruction->register_code & 1U) * sizeof(uint64_t));
		tcg_emit_movr_full(instruction, value);
		return;
	}

	TCGv_i32 value = tcg_temp_new_i32();
	gen_helper_teak_tcg_register_read(value, tcg_env, tcg_constant_i32(instruction->register_code));
	tcg_emit_movr_16(instruction, value);
}

static void tcg_emit_movr_rn_step(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_rn_address(instruction->address_register, instruction->step, false);
	TCGv_i32 value = tcg_temp_new_i32();

	gen_helper_teak_tcg_data_read(value, tcg_env, address);
	tcg_emit_movr_16(instruction, value);
}

static void tcg_emit_movr_rn_high(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_rn_address(instruction->address_register, instruction->step, false);
	TCGv_i32 value32 = tcg_temp_new_i32();
	TCGv_i64 value = tcg_temp_new_i64();

	gen_helper_teak_tcg_data_read(value32, tcg_env, address);
	tcg_gen_extu_i32_i64(value, value32);
	tcg_gen_ext16s_i64(value, value);
	tcg_gen_shli_i64(value, value, 16);
	tcg_emit_movr_full(instruction, value);
}

static void tcg_emit_movr_b_accumulator(const teak_insn_t *instruction) {
	TCGv_i64 value = tcg_temp_new_i64();

	tcg_gen_ld_i64(value, tcg_env,
		offsetof(teak_state_t, b) + instruction->source_accumulator * sizeof(uint64_t));
	tcg_emit_movr_full(instruction, value);
}

static void tcg_emit_movr_r6(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_temp_new_i32();

	tcg_gen_ld16u_i32(value, tcg_env, offsetof(teak_state_t, r) + 6 * sizeof(uint16_t));
	tcg_emit_movr_16(instruction, value);
}

static void tcg_emit_alb_memory(const teak_insn_t *instruction, TCGv_i32 address) {
	TCGv_i32 mask = tcg_constant_i32(instruction->expansion);
	TCGv_i32 operation = tcg_constant_i32(instruction->alb_operation);

	gen_helper_teak_tcg_alb_memory(tcg_env, address, mask, operation, tcg_memory_pc,
		tcg_memory_cycle, tcg_constant_i32(tcg_memory_access++));
}

static void tcg_emit_alb_data_imm8(const teak_insn_t *instruction) {
	tcg_emit_alb_memory(instruction, tcg_emit_imm8_data_address(instruction->memory_address));
}

static void tcg_emit_alb_rn_step(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_rn_address(instruction->address_register, instruction->step, false);

	tcg_emit_alb_memory(instruction, address);
}

static void tcg_emit_alb_register(const teak_insn_t *instruction) {
	TCGv_i32 register_code = tcg_constant_i32(instruction->register_code);
	TCGv_i32 mask = tcg_constant_i32(instruction->expansion);
	TCGv_i32 operation = tcg_constant_i32(instruction->alb_operation);

	gen_helper_teak_tcg_alb_register(tcg_env, register_code, mask, operation);
}

static void tcg_emit_mov_imm_register(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_constant_i32(instruction->expansion);

	switch (instruction->register_code) {
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
			tcg_gen_st16_i32(value, tcg_env,
				offsetof(teak_state_t, r) + instruction->register_code * sizeof(uint16_t));
			break;

		case 6:
			tcg_gen_st16_i32(value, tcg_env, offsetof(teak_state_t, r[7]));
			break;

		case 7:
			tcg_gen_st16_i32(value, tcg_env, offsetof(teak_state_t, y[0]));
			break;

		case 8:
			tcg_emit_mov_imm_st0(instruction->expansion);
			break;

		case 9:
			tcg_emit_mov_imm_st1(instruction->expansion);
			break;

		case 10:
			tcg_emit_mov_imm_st2(instruction->expansion);
			break;

		case 12:
			break;

		case 13:
			tcg_gen_st16_i32(value, tcg_env, offsetof(teak_state_t, sp));
			break;

		case 14:
			tcg_gen_st8_i32(tcg_constant_i32(instruction->expansion & 0x7FU), tcg_env,
				offsetof(teak_state_t, stepi));
			tcg_gen_st16_i32(tcg_constant_i32(instruction->expansion >> 7), tcg_env,
				offsetof(teak_state_t, modi));
			break;

		case 15:
			tcg_gen_st8_i32(tcg_constant_i32(instruction->expansion & 0x7FU), tcg_env,
				offsetof(teak_state_t, stepj));
			tcg_gen_st16_i32(tcg_constant_i32(instruction->expansion >> 7), tcg_env,
				offsetof(teak_state_t, modj));
			break;

		case 16:
		case 17:
		case 18:
		case 19:
			gen_helper_teak_tcg_mov_register_write(tcg_env, tcg_constant_i32(instruction->register_code), value);
			break;

		case 20:
		case 21:
		case 22:
		case 23:
			tcg_gen_st16_i32(value, tcg_env,
				offsetof(teak_state_t, extension) + (instruction->register_code - 20) * sizeof(uint16_t));
			break;

		case 24:
		case 25:
		case 26:
		case 27:
		case 28:
		case 29:
			tcg_emit_mov_imm_accumulator(instruction->register_code, instruction->expansion);
			break;

		case 30:
			gen_helper_teak_tcg_mov_register_write(tcg_env, tcg_constant_i32(instruction->register_code), value);
			break;

		case 31:
			tcg_gen_st16_i32(value, tcg_env, offsetof(teak_state_t, shift_value));
			break;

		default:
			g_assert_not_reached();
	}
}

static void tcg_emit_mov_short_register(const teak_insn_t *instruction) {
	gen_helper_teak_tcg_mov_register_write(tcg_env, tcg_constant_i32(instruction->register_code),
		tcg_constant_i32(instruction->immediate));
}

static void tcg_emit_mov_accumulator_accumulator(const teak_insn_t *instruction) {
	TCGv_i64 value = tcg_temp_new_i64();

	tcg_gen_ld_i64(value, tcg_env, tcg_ab_offset(instruction->source_accumulator));
	tcg_emit_accumulator_value_flags(value);
	tcg_gen_st_i64(value, tcg_env, tcg_ab_offset(instruction->destination_accumulator));
}

static void tcg_emit_mov_accumulator_low_special(const teak_insn_t *instruction) {
	static const uint8_t accumulator_low_registers[] = { 18, 19, 26, 27 };
	TCGv_i32 value = tcg_temp_new_i32();

	gen_helper_teak_tcg_mov_register_read(value, tcg_env,
		tcg_constant_i32(accumulator_low_registers[instruction->source_accumulator]));
	gen_helper_teak_tcg_special_register_write(tcg_env,
		tcg_constant_i32(instruction->special_register), value);
}

static void tcg_emit_mov_special_accumulator(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_temp_new_i32();
	TCGv_i64 accumulator = tcg_temp_new_i64();

	gen_helper_teak_tcg_special_register_read(value, tcg_env, tcg_constant_i32(instruction->special_register));
	tcg_gen_extu_i32_i64(accumulator, value);
	tcg_gen_ext16s_i64(accumulator, accumulator);
	tcg_emit_accumulator_value_flags(accumulator);
	tcg_gen_st_i64(accumulator, tcg_env, tcg_ab_offset(instruction->destination_accumulator));
}

static void tcg_emit_mov_mixp_register(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_temp_new_i32();

	if (instruction->destination_register_code == 12)
		return;
	gen_helper_teak_tcg_special_register_read(value, tcg_env, tcg_constant_i32(TEAK_SPECIAL_MIXP));
	gen_helper_teak_tcg_mov_register_write(tcg_env,
		tcg_constant_i32(instruction->destination_register_code), value);
}

static void tcg_emit_mov_register_special(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_temp_new_i32();

	gen_helper_teak_tcg_mov_register_read(value, tcg_env, tcg_constant_i32(instruction->register_code));
	gen_helper_teak_tcg_special_register_write(tcg_env,
		tcg_constant_i32(instruction->special_register), value);
}

static void tcg_emit_instruction(teak_insn_t *instruction, uint8_t block_repeat_level) {
	uint16_t next_pc = tcg_instruction_end(instruction);

	switch (instruction->opcode) {
		case TEAK_OP_NOP:
			break;
		case TEAK_OP_EINT:
			tcg_gen_st8_i32(tcg_constant_i32(1), tcg_env, offsetof(teak_state_t, ie));
			break;
		case TEAK_OP_DINT:
			tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, ie));
			break;
		case TEAK_OP_CONTEXT_STORE:
			gen_helper_teak_tcg_context_switch(tcg_env, tcg_constant_i32(0));
			break;
		case TEAK_OP_CONTEXT_RESTORE:
			gen_helper_teak_tcg_context_switch(tcg_env, tcg_constant_i32(1));
			break;
		case TEAK_OP_LOAD_PAGE:
			tcg_gen_st8_i32(tcg_constant_i32(instruction->immediate), tcg_env, offsetof(teak_state_t, page));
			break;
		case TEAK_OP_LOAD_STEPI:
			tcg_gen_st8_i32(tcg_constant_i32(instruction->immediate), tcg_env, offsetof(teak_state_t, stepi));
			break;
		case TEAK_OP_LOAD_STEPJ:
			tcg_gen_st8_i32(tcg_constant_i32(instruction->immediate), tcg_env, offsetof(teak_state_t, stepj));
			break;
		case TEAK_OP_LOAD_MODI:
			tcg_gen_st16_i32(tcg_constant_i32(instruction->immediate), tcg_env, offsetof(teak_state_t, modi));
			break;
		case TEAK_OP_LOAD_MODJ:
			tcg_gen_st16_i32(tcg_constant_i32(instruction->immediate), tcg_env, offsetof(teak_state_t, modj));
			break;
		case TEAK_OP_LOAD_PRODUCT_SHIFT:
			tcg_gen_st8_i32(tcg_constant_i32(instruction->immediate), tcg_env, offsetof(teak_state_t, product_shift));
			break;
		case TEAK_OP_SHIFT_IMMEDIATE:
			tcg_emit_shift_immediate(instruction);
			break;
		case TEAK_OP_SHIFT_CONDITIONAL:
			tcg_emit_shift_conditional(instruction);
			break;
		case TEAK_OP_MULTIPLY_IMMEDIATE:
			tcg_emit_multiply_immediate(instruction);
			break;
		case TEAK_OP_MULTIPLY_DATA_IMM8:
			tcg_emit_multiply_data_imm8(instruction);
			break;
		case TEAK_OP_MULTIPLY_DUAL_RN:
			tcg_emit_multiply_dual_rn(instruction);
			break;
		case TEAK_OP_MULTIPLY_RN_IMMEDIATE:
			tcg_emit_multiply_rn_immediate(instruction);
			break;
		case TEAK_OP_MULTIPLY_R6:
			tcg_emit_multiply_r6(instruction);
			break;
		case TEAK_OP_MULTIPLY_REGISTER:
			tcg_emit_multiply_register(instruction);
			break;
		case TEAK_OP_MULTIPLY_RN_STEP:
			tcg_emit_multiply_rn_step(instruction);
			break;
		case TEAK_OP_MODIFY_RN:
			tcg_emit_modify_rn(instruction);
			break;
		case TEAK_OP_TSTB_IMM8:
			tcg_emit_tstb_imm8(instruction);
			break;
		case TEAK_OP_TSTB_RN_STEP:
			tcg_emit_tstb_rn_step(instruction);
			break;
		case TEAK_OP_TSTB_REGISTER:
			tcg_emit_tstb_register(instruction);
			break;
		case TEAK_OP_MOV_DATA_IMM8_REGISTER:
			tcg_emit_mov_data_imm8_register(instruction);
			break;
		case TEAK_OP_MOV_DATA_IMM8_ACCUMULATOR:
			tcg_emit_mov_data_imm8_accumulator(instruction);
			break;
		case TEAK_OP_MOV_DATA_IMM8_ACCUMULATOR_HIGH_EU:
			tcg_emit_mov_data_imm8_accumulator_high_eu(instruction);
			break;
		case TEAK_OP_MOV_REGISTER_DATA_IMM8:
			tcg_emit_mov_register_data_imm8(instruction);
			break;
		case TEAK_OP_MOV_DATA_R7_OFFSET7_ACCUMULATOR:
		case TEAK_OP_MOV_DATA_R7_OFFSET16_ACCUMULATOR:
			tcg_emit_mov_data_r7_accumulator(instruction);
			break;
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET7:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET16:
			tcg_emit_mov_accumulator_low_data_r7(instruction);
			break;
		case TEAK_OP_MOV_DATA_RN_STEP_REGISTER:
			tcg_emit_mov_data_rn_step_register(instruction);
			break;
		case TEAK_OP_MOV_REGISTER_DATA_RN_STEP:
			tcg_emit_mov_register_data_rn_step(instruction);
			break;
		case TEAK_OP_MOV_DATA_RN_STEP_B_ACCUMULATOR:
			tcg_emit_mov_data_rn_step_b_accumulator(instruction);
			break;
		case TEAK_OP_MOV_IMM_ICR:
			tcg_emit_mov_imm_icr(instruction->immediate);
			break;
		case TEAK_OP_MOV_IMM8_ACCUMULATOR_LOW:
			tcg_emit_mov_imm_accumulator(26 + instruction->accumulator_index, instruction->immediate);
			break;
		case TEAK_OP_MOV_DATA_IMM16_ACCUMULATOR:
			tcg_emit_mov_data_imm16_accumulator(instruction);
			break;
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_IMM16:
			tcg_emit_mov_accumulator_low_data_imm16(instruction);
			break;
		case TEAK_OP_MOVP_ACCUMULATOR_LOW_REGISTER:
			tcg_emit_movp_accumulator_low_register(instruction);
			break;
		case TEAK_OP_MOVP_RN_RN:
			tcg_emit_movp_rn_rn(instruction);
			break;
		case TEAK_OP_MOVD_RN_RN:
			tcg_emit_movd_rn_rn(instruction);
			break;
		case TEAK_OP_MOVS_REGISTER:
			tcg_emit_movs_register(instruction);
			break;
		case TEAK_OP_MOVS_RN_STEP:
			tcg_emit_movs_rn_step(instruction);
			break;
		case TEAK_OP_MOVS_DATA_IMM8:
			tcg_emit_movs_data_imm8(instruction);
			break;
		case TEAK_OP_MOVS_R6:
			tcg_emit_movs_r6(instruction);
			break;
		case TEAK_OP_MOVSI_REGISTER:
			tcg_emit_movsi_register(instruction);
			break;
		case TEAK_OP_MOVR_REGISTER:
			tcg_emit_movr_register(instruction);
			break;
		case TEAK_OP_MOVR_RN_STEP:
			tcg_emit_movr_rn_step(instruction);
			break;
		case TEAK_OP_MOVR_RN_HIGH:
			tcg_emit_movr_rn_high(instruction);
			break;
		case TEAK_OP_MOVR_B_ACCUMULATOR:
			tcg_emit_movr_b_accumulator(instruction);
			break;
		case TEAK_OP_MOVR_R6:
			tcg_emit_movr_r6(instruction);
			break;
		case TEAK_OP_ALB_DATA_IMM8:
			tcg_emit_alb_data_imm8(instruction);
			break;
		case TEAK_OP_ALB_RN_STEP:
			tcg_emit_alb_rn_step(instruction);
			break;
		case TEAK_OP_ALB_REGISTER:
			tcg_emit_alb_register(instruction);
			break;
		case TEAK_OP_ALU_IMMEDIATE_ACCUMULATOR:
			tcg_emit_alu_immediate_accumulator(instruction);
			break;
		case TEAK_OP_ALU_DATA_IMM8_ACCUMULATOR:
			tcg_emit_alu_data_imm8_accumulator(instruction);
			break;
		case TEAK_OP_ALU_DATA_IMM16_ACCUMULATOR:
			tcg_emit_alu_data_imm16_accumulator(instruction);
			break;
		case TEAK_OP_ALU_R7_OFFSET7_ACCUMULATOR:
			tcg_emit_alu_r7_offset7_accumulator(instruction);
			break;
		case TEAK_OP_ALU_R7_OFFSET16_ACCUMULATOR:
			tcg_emit_alu_r7_offset16_accumulator(instruction);
			break;
		case TEAK_OP_ALU_RN_STEP_ACCUMULATOR:
			tcg_emit_alu_rn_step_accumulator(instruction);
			break;
		case TEAK_OP_ALU_REGISTER_ACCUMULATOR:
			tcg_emit_alu_register_accumulator(instruction);
			break;
		case TEAK_OP_TEST_ACCUMULATOR_DATA_IMM8:
			tcg_emit_test_accumulator_data_imm8(instruction);
			break;
		case TEAK_OP_MOV_IMM_REGISTER:
			tcg_emit_mov_imm_register(instruction);
			break;
		case TEAK_OP_MOV_IMM_B_ACCUMULATOR:
			tcg_emit_mov_imm_b_accumulator(instruction);
			break;
		case TEAK_OP_MOV_SHORT_REGISTER:
			tcg_emit_mov_short_register(instruction);
			break;
		case TEAK_OP_MOV_ACCUMULATOR_ACCUMULATOR:
			tcg_emit_mov_accumulator_accumulator(instruction);
			break;
		case TEAK_OP_MOV_ACCUMULATOR_LOW_SPECIAL:
			tcg_emit_mov_accumulator_low_special(instruction);
			break;
		case TEAK_OP_MOV_SPECIAL_ACCUMULATOR:
			tcg_emit_mov_special_accumulator(instruction);
			break;
		case TEAK_OP_MOV_MIXP_REGISTER:
			tcg_emit_mov_mixp_register(instruction);
			break;
		case TEAK_OP_MOV_REGISTER_MIXP:
		case TEAK_OP_MOV_REGISTER_ICR:
			tcg_emit_mov_register_special(instruction);
			break;
		case TEAK_OP_MODA4_ACCUMULATOR:
		case TEAK_OP_MODB3_ACCUMULATOR: {
			TCGLabel *skip = NULL;
			if (instruction->condition != TEAK_COND_TRUE)
				skip = tcg_emit_condition_skip(instruction->condition);
			tcg_emit_modify_accumulator(instruction);
			if (skip != NULL)
				gen_set_label(skip);
			break;
		}
		case TEAK_OP_LIMIT_ACCUMULATOR:
			tcg_emit_limit_accumulator(instruction);
			break;
		case TEAK_OP_EXPONENT:
			tcg_emit_exponent(instruction);
			break;
		case TEAK_OP_DIVISION_STEP:
			tcg_emit_division_step(instruction);
			break;
		case TEAK_OP_NORMALIZE:
			tcg_emit_normalize(instruction);
			break;
		case TEAK_OP_SWAP_ACCUMULATORS:
			tcg_emit_swap_accumulators(instruction);
			break;
		case TEAK_OP_BANK_EXCHANGE:
			tcg_emit_bank_exchange(instruction);
			break;
		case TEAK_OP_TRAP:
			tcg_emit_trap(instruction);
			return;
		case TEAK_OP_MINIMUM_MAXIMUM:
			tcg_emit_minimum_maximum(instruction);
			break;
		case TEAK_OP_PUSH_IMMEDIATE:
			tcg_emit_push_value(tcg_constant_i32(instruction->expansion));
			break;
		case TEAK_OP_PUSH_REGISTER:
			tcg_emit_push_register(instruction->register_code);
			break;
		case TEAK_OP_POP_REGISTER:
			if (instruction->register_code != 12)
				tcg_emit_pop_register(instruction->register_code);
			break;
		case TEAK_OP_REPEAT_IMMEDIATE:
			tcg_emit_repeat(tcg_constant_i32(instruction->immediate));
			break;
		case TEAK_OP_REPEAT_REGISTER:
			tcg_emit_repeat_register(instruction->register_code);
			break;
		case TEAK_OP_BLOCK_REPEAT_IMMEDIATE:
			tcg_emit_block_repeat(instruction, tcg_constant_i32(instruction->immediate), block_repeat_level);
			break;
		case TEAK_OP_BLOCK_REPEAT_REGISTER: {
			TCGv_i32 count = tcg_temp_new_i32();

			gen_helper_teak_tcg_register_read(count, tcg_env, tcg_constant_i32(instruction->register_code));
			tcg_emit_block_repeat(instruction, count, block_repeat_level);
			break;
		}
		case TEAK_OP_BREAK:
			tcg_emit_break();
			break;
		case TEAK_OP_MOV_STACK_REGISTER:
			tcg_emit_mov_stack_register(instruction->register_code);
			break;
		case TEAK_OP_MOV_REGISTER_REGISTER:
			tcg_emit_mov_register_register(instruction);
			break;
		case TEAK_OP_MOV_REGISTER_B_ACCUMULATOR:
			tcg_emit_mov_register_b_accumulator(instruction);
			break;
		case TEAK_OP_BRANCH_ABSOLUTE:
		case TEAK_OP_BRANCH_RELATIVE:
			tcg_emit_control_transfer(instruction, false);
			return;
		case TEAK_OP_CALL_ABSOLUTE:
		case TEAK_OP_CALL_RELATIVE:
			tcg_emit_control_transfer(instruction, true);
			return;
		case TEAK_OP_CALL_ACCUMULATOR:
			tcg_emit_call_accumulator(instruction);
			return;
		case TEAK_OP_RETURN:
		case TEAK_OP_RETURN_INTERRUPT:
		case TEAK_OP_RETURN_STACK:
			tcg_emit_return(instruction);
			return;
		case TEAK_OP_DELAYED_RETURN:
		case TEAK_OP_DELAYED_RETURN_INTERRUPT:
			break;
		default:
			g_assert_not_reached();
	}
	tcg_gen_st_i32(tcg_constant_i32(next_pc), tcg_env, offsetof(teak_state_t, pc));
}

static bool tcg_block_repeat_setup_valid(const teak_tcg_core_t *core, const teak_insn_t *instruction) {
	uint8_t level = core->state.bcn;
	if (level == TEAK_BLOCK_REPEAT_LEVELS)
		return false;
	if (instruction->branch_target < instruction->address + instruction->words)
		return false;
	if (level != 0 && instruction->branch_target >= core->state.block_repeat_end[level - 1])
		return false;
	if (instruction->opcode == TEAK_OP_BLOCK_REPEAT_REGISTER) {
		bool full_accumulator = instruction->register_code == 24 || instruction->register_code == 25;
		bool nested_lc = level != 0 && instruction->register_code == 30;
		if (full_accumulator || nested_lc)
			return false;
	}
	for (size_t i = 0; i < level; i++) {
		if (instruction->branch_target == core->state.block_repeat_end[i])
			return false;
	}
	return true;
}

static bool tcg_active_block_repeat_instruction_valid(const teak_tcg_core_t *core, const teak_insn_t *instruction) {
	uint32_t active_end = core->state.block_repeat_end[core->state.bcn - 1];
	uint32_t instruction_end = instruction->address + instruction->words - 1;
	if (instruction_end > active_end)
		return false;
	if (instruction->opcode == TEAK_OP_BREAK && instruction_end == active_end)
		return false;
	if (tcg_is_repeat(instruction) && instruction->address + 1 >= active_end)
		return false;
	if (tcg_is_block_repeat(instruction) && instruction->address + 1 >= active_end)
		return false;
	return true;
}

static bool tcg_may_write_data(const teak_insn_t *instruction) {
	switch (instruction->opcode) {
		case TEAK_OP_ALB_DATA_IMM8:
		case TEAK_OP_ALB_RN_STEP:
		case TEAK_OP_MOV_REGISTER_DATA_IMM8:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET7:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET16:
		case TEAK_OP_MOV_REGISTER_DATA_RN_STEP:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_IMM16:
		case TEAK_OP_MOVP_RN_RN:
		case TEAK_OP_PUSH_IMMEDIATE:
		case TEAK_OP_PUSH_REGISTER:
			return true;

		default:
			return false;
	}
}

static bool tcg_block_can_batch(const teak_tcg_block_t *block) {
	const teak_insn_t *instruction;

	if (block->instruction_count == 0)
		return false;

	for (size_t i = 0; i < block->instruction_count; i++)
		if (tcg_may_write_data(&block->instructions[i]))
			return false;

	instruction = &block->instructions[block->instruction_count - 1];
	if (instruction->opcode != TEAK_OP_BRANCH_ABSOLUTE && instruction->opcode != TEAK_OP_BRANCH_RELATIVE)
		return false;
	return instruction->branch_target == block->pc;
}

static bool tcg_block_has_dynamic_cycles(const teak_tcg_block_t *block) {
	const teak_insn_t *instruction = &block->instructions[0];

	return instruction->opcode == TEAK_OP_REPEAT_IMMEDIATE || instruction->opcode == TEAK_OP_REPEAT_REGISTER;
}

static bool tcg_decode_block(teak_tcg_core_t *core, teak_tcg_block_t *block, size_t max_instructions) {
	uint16_t address;
	uint8_t delayed_transfer_cycles = 0;
	bool repeat_pending = false;

	memset(block, 0, sizeof(*block));
	memset(block->block_repeat_setup_level, UINT8_MAX, sizeof(block->block_repeat_setup_level));
	block->pc = core->state.pc;
	address = block->pc;

	while (block->instruction_count < ARRAY_SIZE(block->instructions)) {
		uint16_t instruction_index = block->instruction_count;
		teak_insn_t *instruction = &block->instructions[instruction_index];
		uint32_t instruction_end;
		uint8_t transfer_cycles;
		uint8_t delay_slot_cycles;
		bool limit_reached = instruction_index >= max_instructions && !repeat_pending &&
			delayed_transfer_cycles == 0;
		if (limit_reached)
			break;

		if (!teak_decode(core, address, instruction)) {
			core->translation_error_address = address;
			core->translation_error = TEAK_TRANSLATION_ERROR_DECODE;
			break;
		}
		if (!tcg_can_translate(instruction)) {
			core->translation_error_address = instruction->address;
			core->translation_error = TEAK_TRANSLATION_ERROR_UNSUPPORTED;
			break;
		}
		bool invalid_repeat_body = core->state.bcn != 0 &&
			!tcg_active_block_repeat_instruction_valid(core, instruction);
		if (invalid_repeat_body) {
			core->translation_error_address = instruction->address;
			core->translation_error = TEAK_TRANSLATION_ERROR_BLOCK_REPEAT_BODY;
			return false;
		}
		bool invalid_break = instruction->opcode == TEAK_OP_BREAK && core->state.bcn == 0;
		if (invalid_break) {
			core->translation_error_address = instruction->address;
			core->translation_error = TEAK_TRANSLATION_ERROR_BREAK;
			return false;
		}
		bool nested_repeat = tcg_is_repeat(instruction) && instruction_index != 0;
		bool nested_block_repeat = tcg_is_block_repeat(instruction) && instruction_index != 0;
		if (nested_repeat)
			break;
		if (nested_block_repeat)
			break;
		if (repeat_pending && !tcg_can_repeat(instruction)) {
			core->translation_error_address = instruction->address;
			core->translation_error = TEAK_TRANSLATION_ERROR_REPEAT;
			return false;
		}
		bool invalid_block_repeat = tcg_is_block_repeat(instruction) &&
			!tcg_block_repeat_setup_valid(core, instruction);
		if (invalid_block_repeat) {
			core->translation_error_address = instruction->address;
			core->translation_error = TEAK_TRANSLATION_ERROR_BLOCK_REPEAT_SETUP;
			return false;
		}
		transfer_cycles = tcg_delayed_transfer_cycles(instruction);
		if (transfer_cycles != 0 && instruction_index != 0)
			break;
		delay_slot_cycles = tcg_delay_slot_cycles(instruction);
		bool complete_delayed_transfer = delayed_transfer_cycles != 0 &&
			delay_slot_cycles > delayed_transfer_cycles && block->instruction_count > 1;
		if (complete_delayed_transfer)
			return true;
		if (delayed_transfer_cycles != 0 && delay_slot_cycles == 0) {
			core->translation_error_address = instruction->address;
			core->translation_error = TEAK_TRANSLATION_ERROR_DELAY_SLOT;
			return false;
		}
		if (delayed_transfer_cycles != 0 && delay_slot_cycles > delayed_transfer_cycles) {
			core->translation_error_address = instruction->address;
			core->translation_error = TEAK_TRANSLATION_ERROR_DELAY_SLOT;
			return false;
		}

		block->instruction_count++;
		block->words += instruction->words;
		address += instruction->words;
		instruction_end = instruction->address + instruction->words - 1;

		if (delayed_transfer_cycles != 0) {
			delayed_transfer_cycles -= delay_slot_cycles;
			if (delayed_transfer_cycles == 0)
				return true;
		}
		if (repeat_pending)
			return true;
		if (tcg_is_repeat(instruction)) {
			repeat_pending = true;
			continue;
		}
		if (transfer_cycles != 0) {
			delayed_transfer_cycles = transfer_cycles;
			continue;
		}
		if (tcg_is_block_repeat(instruction)) {
			block->block_repeat_setup_level[instruction_index] = core->state.bcn;
			return true;
		}
		if (instruction->opcode == TEAK_OP_BREAK)
			return true;
		if (core->state.bcn != 0 && instruction_end == core->state.block_repeat_end[core->state.bcn - 1])
			return true;
		if (instruction->opcode == TEAK_OP_MOVD_RN_RN || tcg_is_loop_control(instruction))
			return true;
	}

	if (block->instruction_count == 0)
		return false;
	if (repeat_pending) {
		core->translation_error_address = address;
		core->translation_error = TEAK_TRANSLATION_ERROR_REPEAT;
		return false;
	}
	if (delayed_transfer_cycles != 0) {
		core->translation_error_address = address;
		core->translation_error = TEAK_TRANSLATION_ERROR_DELAY_SLOT;
		return false;
	}
	return true;
}

static void tcg_emit_repeat_end(TCGLabel *start) {
	TCGv_i32 repc = tcg_temp_new_i32();
	TCGLabel *done = gen_new_label();

	tcg_gen_ld16u_i32(repc, tcg_env, offsetof(teak_state_t, repc));
	tcg_gen_brcondi_i32(TCG_COND_EQ, repc, 0, done);
	tcg_gen_subi_i32(repc, repc, 1);
	tcg_gen_st16_i32(repc, tcg_env, offsetof(teak_state_t, repc));
	tcg_gen_br(start);
	gen_set_label(done);
	tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, repeat_active));
}

static TCGv_i32 tcg_emit_delayed_transfer_target(const teak_insn_t *instruction) {
	TCGv_i32 target;

	if (tcg_is_delayed_return(instruction)) {
		target = tcg_emit_pop_pc(0, true);
		/* Reserved RETID encodings observed on hardware resume at the stacked PC. */
		if (instruction->word == TEAK_OPCODE_RETID)
			tcg_gen_addi_i32(target, target, 1);
		return target;
	}

	switch (instruction->opcode) {
		case TEAK_OP_MOV_IMM_REGISTER:
			return tcg_constant_i32(instruction->expansion);
		case TEAK_OP_MOV_REGISTER_REGISTER:
			target = tcg_temp_new_i32();
			gen_helper_teak_tcg_mov_register_read(target, tcg_env, tcg_constant_i32(instruction->register_code));
			return target;
		case TEAK_OP_MOV_MIXP_REGISTER:
			target = tcg_temp_new_i32();
			gen_helper_teak_tcg_special_register_read(target, tcg_env, tcg_constant_i32(TEAK_SPECIAL_MIXP));
			return target;
		case TEAK_OP_MOV_DATA_RN_STEP_REGISTER:
			target = tcg_temp_new_i32();
			gen_helper_teak_tcg_data_read(target, tcg_env,
				tcg_emit_rn_address(instruction->address_register, instruction->step, false));
			return target;
		case TEAK_OP_MOVP_ACCUMULATOR_LOW_REGISTER:
			return tcg_emit_movp_accumulator_low_read(instruction);
		case TEAK_OP_POP_REGISTER:
			return tcg_emit_pop_value();
		default:
			g_assert_not_reached();
	}
}

static void tcg_emit_block_batch(const teak_tcg_block_t *block, TCGLabel *loop, TCGLabel *exit) {
	TCGv_i32 block_repeat_level = tcg_temp_new_i32();
	TCGv_i32 cycles_remaining = tcg_temp_new_i32();
	TCGv_i32 exit_reason = tcg_temp_new_i32();
	TCGv_i32 exit_request = tcg_temp_new_i32();
	TCGv_i32 interrupt_request = tcg_temp_new_i32();
	TCGv_i32 iterations = tcg_temp_new_i32();
	TCGv_i32 interrupt_lines = tcg_temp_new_i32();
	TCGv_i32 pending_interrupts = tcg_temp_new_i32();
	TCGv_i32 pc = tcg_temp_new_i32();

	tcg_gen_ld_i32(iterations, tcg_env, offsetof(teak_tcg_core_t, batch_iterations));
	tcg_gen_addi_i32(iterations, iterations, 1);
	tcg_gen_st_i32(iterations, tcg_env, offsetof(teak_tcg_core_t, batch_iterations));

	tcg_gen_ld_i32(exit_request, tcg_env, offsetof(teak_state_t, exit_request));
	tcg_gen_brcondi_i32(TCG_COND_NE, exit_request, 0, exit);
	tcg_gen_ld_i32(interrupt_request, tcg_env, offsetof(teak_state_t, interrupt_request));
	tcg_gen_brcondi_i32(TCG_COND_NE, interrupt_request, 0, exit);
	tcg_gen_ld_i32(pending_interrupts, tcg_env, offsetof(teak_state_t, pending_interrupts));
	tcg_gen_ld_i32(interrupt_lines, tcg_env, offsetof(teak_state_t, interrupt_lines));
	tcg_gen_or_i32(pending_interrupts, pending_interrupts, interrupt_lines);
	tcg_gen_brcondi_i32(TCG_COND_NE, pending_interrupts, 0, exit);
	tcg_gen_ld_i32(pc, tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_brcondi_i32(TCG_COND_NE, pc, block->pc, exit);
	tcg_gen_ld8u_i32(block_repeat_level, tcg_env, offsetof(teak_state_t, bcn));
	tcg_gen_brcondi_i32(TCG_COND_NE, block_repeat_level, 0, exit);
	tcg_gen_ld_i32(exit_reason, tcg_env, offsetof(teak_state_t, exit_reason));
	tcg_gen_brcondi_i32(TCG_COND_NE, exit_reason, TEAK_EXIT_BRANCH, exit);

	tcg_gen_ld_i32(cycles_remaining, tcg_env, offsetof(teak_tcg_core_t, batch_cycles_remaining));
	tcg_gen_subi_i32(cycles_remaining, cycles_remaining, block->instruction_count);
	tcg_gen_st_i32(cycles_remaining, tcg_env, offsetof(teak_tcg_core_t, batch_cycles_remaining));
	tcg_gen_brcondi_i32(TCG_COND_LTU, cycles_remaining, block->instruction_count, exit);

	tcg_gen_st_i32(tcg_constant_i32(TEAK_EXIT_NONE), tcg_env, offsetof(teak_state_t, exit_reason));
	tcg_gen_br(loop);
}

static void tcg_emit_block_chain(const teak_tcg_block_t *block, bool batched_loop) {
	TCGv_i32 block_count = tcg_temp_new_i32();
	TCGv_i32 block_cycles = tcg_temp_new_i32();
	TCGv_ptr next = tcg_temp_new_ptr();
	TCGLabel *done = gen_new_label();

	tcg_gen_ld_i32(block_cycles, tcg_env, offsetof(teak_tcg_core_t, batch_block_cycles));
	if (batched_loop) {
		tcg_gen_ld_i32(block_count, tcg_env, offsetof(teak_tcg_core_t, batch_iterations));
		tcg_gen_mul_i32(block_cycles, block_cycles, block_count);
	} else {
		tcg_gen_movi_i32(block_count, 1);
	}
	gen_helper_teak_tcg_chain(next, tcg_env, block_cycles, block_count, tcg_constant_i32(block->pc));
	tcg_gen_brcondi_ptr(TCG_COND_EQ, next, 0, done);
	tcg_gen_goto_ptr(next);
	gen_set_label(done);
}

static void tcg_emit_block(void *opaque) {
	teak_tcg_block_t *block = opaque;
	TCGv_i32 delayed_transfer_target = NULL;
	TCGLabel *exit = gen_new_label();
	TCGLabel *repeat = NULL;
	TCGLabel *loop = NULL;
	bool delayed_transfer_interrupt = false;
	bool repeat_pending = false;
	bool can_batch = tcg_block_can_batch(block);

	tcg_gen_st_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_tcg_core_t, batch_iterations));
	if (!tcg_block_has_dynamic_cycles(block))
		tcg_gen_st_i32(tcg_constant_i32(block->instruction_count), tcg_env,
			offsetof(teak_tcg_core_t, batch_block_cycles));
	if (can_batch) {
		loop = gen_new_label();
		gen_set_label(loop);
	}

	for (size_t i = 0; i < block->instruction_count; i++) {
		teak_insn_t *instruction = &block->instructions[i];
		uint8_t setup_level = block->block_repeat_setup_level[i];

		tcg_memory_pc = tcg_constant_i32(instruction->address);
		tcg_memory_cycle = tcg_constant_i32(i);
		tcg_memory_access = 0;
		if (repeat_pending)
			gen_set_label(repeat);
		tcg_gen_insn_start(instruction->address, 0, 0);
		if (tcg_delayed_transfer_cycles(instruction) != 0) {
			delayed_transfer_target = tcg_emit_delayed_transfer_target(instruction);
			delayed_transfer_interrupt = instruction->opcode == TEAK_OP_DELAYED_RETURN_INTERRUPT;
		}
		tcg_emit_instruction(instruction, setup_level);
		if (tcg_is_repeat(instruction)) {
			repeat = gen_new_label();
			repeat_pending = true;
			continue;
		}
		if (repeat_pending) {
			tcg_emit_repeat_end(repeat);
			repeat_pending = false;
		}
		if (delayed_transfer_target != NULL && i + 1 == block->instruction_count) {
			tcg_gen_st_i32(delayed_transfer_target, tcg_env, offsetof(teak_state_t, pc));
			if (delayed_transfer_interrupt)
				tcg_emit_interrupt_return_state();
			tcg_gen_st_i32(tcg_constant_i32(TEAK_EXIT_BRANCH), tcg_env,
				offsetof(teak_state_t, exit_reason));
		}
		if (tcg_may_write_data(instruction)) {
			TCGv_i32 exit_request = tcg_temp_new_i32();
			TCGv_i32 interrupt_request = tcg_temp_new_i32();

			tcg_gen_ld_i32(exit_request, tcg_env, offsetof(teak_state_t, exit_request));
			tcg_gen_brcondi_i32(TCG_COND_NE, exit_request, 0, exit);
			tcg_gen_ld_i32(interrupt_request, tcg_env, offsetof(teak_state_t, interrupt_request));
			tcg_gen_brcondi_i32(TCG_COND_NE, interrupt_request, 0, exit);
		}
	}
	if (can_batch)
		tcg_emit_block_batch(block, loop, exit);
	gen_set_label(exit);
	tcg_emit_block_chain(block, can_batch);
}

static void tcg_complete_block_repeat(teak_state_t *state) {
	uint8_t level;
	uint16_t next;

	if (!state->lp || state->bcn == 0)
		return;

	level = state->bcn - 1;
	next = (uint16_t) (state->block_repeat_end[level] + 1);
	if (state->pc != next)
		return;
	if (state->block_repeat_lc[level] != 0) {
		state->block_repeat_lc[level]--;
		state->pc = state->block_repeat_start[level];
		return;
	}
	state->bcn = level;
	state->lp = level != 0;
}

static uint32_t tcg_block_cycles(teak_tcg_core_t *core, const teak_tcg_block_t *block) {
	const teak_insn_t *instruction = &block->instructions[0];
	uint32_t cycles = block->instruction_count;

	if (instruction->opcode == TEAK_OP_REPEAT_IMMEDIATE)
		cycles += instruction->immediate;
	if (instruction->opcode == TEAK_OP_REPEAT_REGISTER)
		cycles += tcg_read_register(&core->state, instruction->register_code);
	return cycles;
}

static TranslationBlock *tcg_prepare_block(teak_tcg_core_t *core, teak_tcg_block_t *block) {
	TranslationBlock *tb;
	size_t max_instructions = TEAK_TCG_MAX_BLOCK_INSTRUCTIONS;
	int compile_error;

	tb = tcg_find_cached_block_fast(core, block);

	while (tb == NULL) {
		if (!tcg_decode_block(core, block, max_instructions))
			return NULL;

		tb = tcg_find_cached_block(core, block);
		if (tb != NULL) {
			core->cache_decoded_hits++;
			break;
		}

		tb = tcg_compile_block(block->pc, block->words, block->instruction_count, tcg_emit_block,
			block, &compile_error);
		if (tb != NULL) {
			tcg_cache_block(core, block, tb);
			core->cache_compiles++;
			break;
		}

		if (compile_error == TEAK_TCG_COMPILE_RETRY) {
			tcg_check_block_cache();
			continue;
		}

		if (compile_error != -2 || block->instruction_count == 1) {
			core->translation_error = TEAK_TRANSLATION_ERROR_COMPILE;
			return NULL;
		}
		max_instructions = MAX((size_t) 1, block->instruction_count / 2);
	}
	return tb;
}

static void tcg_precompile_enqueue(uint16_t *queue, bool *queued, size_t *tail, uint32_t address) {
	uint16_t pc = (uint16_t) address;

	if (queued[pc])
		return;
	queued[pc] = true;
	queue[(*tail)++] = pc;
}

size_t teak_tcg_precompile_entry(teak_tcg_core_t *core, uint32_t entry) {
	uint16_t *queue = g_new(uint16_t, (size_t) TEAK_PROGRAM_ADDRESS_MASK + 1);
	bool *queued = g_new0(bool, (size_t) TEAK_PROGRAM_ADDRESS_MASK + 1);
	size_t head = 0;
	size_t tail = 0;
	size_t blocks = 0;

	uint32_t saved_pc = core->state.pc;
	uint32_t saved_error_address = core->translation_error_address;
	teak_translation_error_t saved_error = core->translation_error;
	uint8_t saved_bcn = core->state.bcn;
	uint8_t saved_lp = core->state.lp;

	tcg_precompile_enqueue(queue, queued, &tail, entry);
	for (size_t i = 0; i < ARRAY_SIZE(teak_interrupt_vectors); i++)
		tcg_precompile_enqueue(queue, queued, &tail, teak_interrupt_vectors[i]);

	while (head < tail) {
		teak_tcg_block_t block;

		core->state.pc = queue[head++];
		core->state.bcn = 0;
		core->state.lp = 0;
		TranslationBlock *tb = tcg_prepare_block(core, &block);
		if (tb == NULL)
			continue;

		blocks++;
		const teak_insn_t *last = &block.instructions[block.instruction_count - 1];
		uint16_t fallthrough = (uint16_t) (last->address + last->words);

		switch (last->opcode) {
			case TEAK_OP_BRANCH_ABSOLUTE:
			case TEAK_OP_BRANCH_RELATIVE:
				tcg_precompile_enqueue(queue, queued, &tail, last->branch_target);
				if (last->condition != TEAK_COND_TRUE)
					tcg_precompile_enqueue(queue, queued, &tail, fallthrough);
				break;

			case TEAK_OP_CALL_ABSOLUTE:
			case TEAK_OP_CALL_RELATIVE:
				tcg_precompile_enqueue(queue, queued, &tail, last->branch_target);
				tcg_precompile_enqueue(queue, queued, &tail, fallthrough);
				break;

			case TEAK_OP_CALL_ACCUMULATOR:
				tcg_precompile_enqueue(queue, queued, &tail, fallthrough);
				break;

			case TEAK_OP_RETURN:
			case TEAK_OP_RETURN_INTERRUPT:
				if (last->condition != TEAK_COND_TRUE)
					tcg_precompile_enqueue(queue, queued, &tail, fallthrough);
				break;

			case TEAK_OP_RETURN_STACK:
			case TEAK_OP_DELAYED_RETURN:
			case TEAK_OP_DELAYED_RETURN_INTERRUPT:
				break;

			case TEAK_OP_TRAP:
				tcg_precompile_enqueue(queue, queued, &tail, 2);
				tcg_precompile_enqueue(queue, queued, &tail, fallthrough);
				break;

			default:
				tcg_precompile_enqueue(queue, queued, &tail, fallthrough);
				break;
		}
	}

	core->state.pc = saved_pc;
	core->state.bcn = saved_bcn;
	core->state.lp = saved_lp;
	core->translation_error_address = saved_error_address;
	core->translation_error = saved_error;

	g_free(queued);
	g_free(queue);
	return blocks;
}

static void tcg_complete_block_cycles(teak_tcg_core_t *core, uint32_t block_cycles) {
	assert(core->synchronized_cycles <= block_cycles);
	core->pending_cycles += block_cycles - core->synchronized_cycles;
}

static void tcg_flush_cycles(teak_tcg_core_t *core) {
	if (core->memory.advance_cycles != NULL && core->pending_cycles != 0)
		core->memory.advance_cycles(core->memory.cycle_opaque, core->pending_cycles);
	core->pending_cycles = 0;
}

void *HELPER(teak_tcg_chain)(void *opaque, uint32_t block_cycles, uint32_t block_count, uint32_t block_pc) {
	teak_state_t *state = opaque;
	teak_tcg_core_t *core = container_of(state, teak_tcg_core_t, state);
	teak_tcg_block_cache_entry_t *entry;

	g_assert(block_cycles != 0);
	g_assert(block_count != 0);

	tcg_complete_block_cycles(core, block_cycles);
	core->last_block_cycles += block_cycles;
	core->last_block_count += block_count;
	if (state->lp && state->bcn != 0)
		tcg_complete_block_repeat(state);

	core->synchronized_cycles = 0;
	core->synchronization_offset = 0;
	core->synchronization_access = 0;
	core->synchronization_valid = false;

	if (qatomic_read(&state->exit_request) != 0) {
		core->chain_exit_pc = block_pc;
		core->chain_exit_stops++;
		return NULL;
	}
	if (qatomic_read(&state->interrupt_request) != 0)
		return NULL;
	if (tcg_pending_interrupts(state) != 0 && teak_tcg_service_interrupt(core))
		core->chain_interrupts++;
	if (core->last_block_cycles >= core->chain_cycle_limit) {
		core->chain_budget_stops++;
		return NULL;
	}

	entry = tcg_find_cached_entry_fast(core);
	if (entry == NULL || tcg_block_has_dynamic_cycles(&entry->block)) {
		core->chain_cache_stops++;
		return NULL;
	}
	if (core->chain_cycle_limit - core->last_block_cycles < entry->block.instruction_count) {
		core->chain_budget_stops++;
		return NULL;
	}

	core->batch_cycles_remaining = core->chain_cycle_limit - core->last_block_cycles;
	state->exit_reason = TEAK_EXIT_NONE;
	core->chain_links++;
	return (void *) entry->tb->tc.ptr;
}

static bool tcg_execute_prepared_block(
	teak_tcg_core_t *core,
	const teak_tcg_block_t *block,
	TranslationBlock *tb,
	size_t max_cycles
) {
	uint32_t block_cycles = tcg_block_cycles(core, block);
	uintptr_t exit;

	core->synchronized_cycles = 0;
	core->synchronization_offset = 0;
	core->synchronization_access = 0;
	core->synchronization_valid = false;
	core->batch_iterations = 0;
	core->batch_block_cycles = block_cycles;
	core->batch_cycles_remaining = MAX(max_cycles, (size_t) block_cycles);
	core->chain_cycle_limit = core->last_block_cycles + core->batch_cycles_remaining;

	qemu_thread_jit_execute();
	exit = tcg_qemu_tb_exec((CPUArchState *) &core->state, tb->tc.ptr);
	core->jit_entries++;

	core->state.pc &= TEAK_PROGRAM_ADDRESS_MASK;

	if (exit != 0) {
		core->translation_error = TEAK_TRANSLATION_ERROR_EXECUTION;
		return false;
	}
	return true;
}

bool teak_tcg_execute_block(teak_tcg_core_t *core) {
	teak_tcg_block_t block;
	TranslationBlock *tb;
	uint32_t block_cycles;

	core->translation_error_address = core->state.pc;
	core->translation_error = TEAK_TRANSLATION_ERROR_NONE;
	core->last_block_cycles = 0;
	core->last_block_count = 0;
	core->pending_cycles = 0;

	tb = tcg_prepare_block(core, &block);
	if (tb == NULL)
		return false;

	block_cycles = tcg_block_cycles(core, &block);
	if (!tcg_execute_prepared_block(core, &block, tb, block_cycles)) {
		tcg_flush_cycles(core);
		return false;
	}

	tcg_flush_cycles(core);
	return true;
}

static bool tcg_execute_slice_block(teak_tcg_core_t *core, const teak_tcg_block_t *block, TranslationBlock *tb, size_t max_cycles) {
	bool stable_block = core->state.bcn == 0;

	do {
		bool continue_block = stable_block;
		size_t remaining_cycles = max_cycles - core->last_block_cycles;

		if (!tcg_execute_prepared_block(core, block, tb, remaining_cycles))
			return false;

		if (qatomic_xchg(&core->state.exit_request, 0) != 0)
			break;

		bool interrupt_requested = qatomic_xchg(&core->state.interrupt_request, 0) != 0;
		bool interrupt_pending = interrupt_requested || tcg_pending_interrupts(&core->state) != 0;
		if (interrupt_pending && teak_tcg_service_interrupt(core))
			core->chain_interrupts++;

		if (core->state.pc != block->pc)
			continue_block = false;
		if (core->state.bcn != 0)
			continue_block = false;
		if (core->state.exit_reason != TEAK_EXIT_BRANCH)
			continue_block = false;

		if (!continue_block)
			break;

		core->state.exit_reason = TEAK_EXIT_NONE;
	} while (core->last_block_cycles < max_cycles);
	return true;
}

static bool tcg_execute_cached_slice(teak_tcg_core_t *core, teak_tcg_block_cache_entry_t *entry, size_t max_cycles) {
	while (true) {
		teak_tcg_block_cache_entry_t *next;
		size_t remaining_cycles = max_cycles - core->last_block_cycles;

		if (!tcg_execute_prepared_block(core, &entry->block, entry->tb, remaining_cycles))
			return false;

		if (qatomic_xchg(&core->state.exit_request, 0) != 0)
			break;

		bool interrupt_requested = qatomic_xchg(&core->state.interrupt_request, 0) != 0;
		bool interrupt_pending = interrupt_requested || tcg_pending_interrupts(&core->state) != 0;
		if (interrupt_pending && teak_tcg_service_interrupt(core))
			core->chain_interrupts++;

		if (core->last_block_cycles >= max_cycles)
			break;

		next = tcg_find_cached_entry_fast(core);
		if (next == NULL)
			break;
		entry = next;
		core->state.exit_reason = TEAK_EXIT_NONE;
	}
	return true;
}

static bool __attribute__((noinline))
tcg_execute_slice_slow(teak_tcg_core_t *core, size_t max_cycles) {
	teak_tcg_block_t block;
	TranslationBlock *tb = tcg_prepare_block(core, &block);

	if (tb == NULL)
		return false;
	return tcg_execute_slice_block(core, &block, tb, max_cycles);
}

bool teak_tcg_execute_slice(teak_tcg_core_t *core, size_t max_cycles) {
	teak_tcg_block_cache_entry_t *entry;
	bool success;

	assert(max_cycles != 0);

	core->translation_error_address = core->state.pc;
	core->translation_error = TEAK_TRANSLATION_ERROR_NONE;
	core->last_block_cycles = 0;
	core->last_block_count = 0;
	core->pending_cycles = 0;

	if (qatomic_xchg(&core->state.exit_request, 0) != 0)
		return true;

	entry = tcg_find_cached_entry_fast(core);
	if (entry == NULL) {
		success = tcg_execute_slice_slow(core, max_cycles);
	} else {
		success = tcg_execute_cached_slice(core, entry, max_cycles);
	}

	tcg_flush_cycles(core);
	return success;
}
