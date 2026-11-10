#include "qemu/osdep.h"
#include "accel/tcg/tb-context.h"
#include "cpu.h"
#include "exec/tb-flush.h"
#include "exec/translation-block.h"
#include "tcg/tcg.h"
#include "tcg/tcg-internal.h"
#include "tcg/tcg-op-common.h"
#include "tcg/perf.h"

#include "hw/arm/pmb887x/dsp/tcg.h"
#include "hw/arm/pmb887x/dsp/core.h"

#define HELPER_H "hw/arm/pmb887x/dsp/tcg-helper.h"
#include "exec/helper-proto.h.inc"
#include "exec/helper-gen.h.inc"

#undef HELPER_H

#define TEAK_TCG_MAX_BLOCK_INSTRUCTIONS	64
#define TEAK_TCG_BLOCK_CACHE_ENTRIES	((size_t) UINT16_MAX + 1)
#define TEAK_TCG_CHAIN_EXIT_LINKS	2
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
#define TEAK_TCG_COMPILE_RETRY	-3
#define TEAK_TCG_COMPILE_MODE_WRITE	-4
#define TEAK_TCG_CALL_NO_FLAG_READS	BIT(0)
#define TEAK_TCG_CALL_NO_MODE_WRITES	BIT(1)
#define TEAK_TCG_PRECOMPILE_VALUES	128

typedef struct teak_tcg_block_t teak_tcg_block_t;
typedef struct teak_tcg_block_cache_entry_t teak_tcg_block_cache_entry_t;
typedef struct teak_tcg_chain_link_t teak_tcg_chain_link_t;
typedef struct teak_tcg_chain_exit_t teak_tcg_chain_exit_t;
typedef struct teak_tcg_mode_t teak_tcg_mode_t;
typedef struct teak_tcg_swap_mapping_t teak_tcg_swap_mapping_t;
typedef struct teak_tcg_precompile_node_t teak_tcg_precompile_node_t;
typedef struct teak_tcg_precompile_state_t teak_tcg_precompile_state_t;
typedef struct teak_tcg_precompile_value_t teak_tcg_precompile_value_t;
typedef void teak_tcg_emit_fn(void *opaque);

/*
 * Precompilation uses bounded value propagation to discover indirect-call
 * targets. The result is only a cache hint: unknown values stay
 * on the normal decode-and-compile path, and compiled code is still subject to
 * the regular program-memory invalidation rules.
 */
typedef enum teak_tcg_precompile_value_kind_t {
	TEAK_TCG_PRECOMPILE_UNKNOWN,
	TEAK_TCG_PRECOMPILE_RANGE,
	TEAK_TCG_PRECOMPILE_SET,
} teak_tcg_precompile_value_kind_t;

typedef enum teak_tcg_precompile_flags_source_t {
	TEAK_TCG_PRECOMPILE_FLAGS_UNKNOWN,
	TEAK_TCG_PRECOMPILE_FLAGS_A0,
	TEAK_TCG_PRECOMPILE_FLAGS_A1,
} teak_tcg_precompile_flags_source_t;

struct teak_tcg_precompile_value_t {
	teak_tcg_precompile_value_kind_t kind;
	int32_t minimum;
	int32_t maximum;
	uint16_t values[TEAK_TCG_PRECOMPILE_VALUES];
	uint8_t count;
};

struct teak_tcg_precompile_state_t {
	teak_tcg_precompile_value_t a[2];
	teak_tcg_precompile_value_t r[8];
	teak_tcg_precompile_flags_source_t flags_source;
	int32_t comparison;
};

struct teak_tcg_precompile_node_t {
	teak_tcg_precompile_state_t state;
	uint16_t pc;
	bool queued;
	bool reached;
};

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
	uint32_t pc;
	uint16_t words;
	uint16_t instruction_count;
	bool repeat_resume;
	/* bcn of the block-repeat whose whole body this block is, else 0 */
	uint8_t repeat_body_level;
	bool ends_block_repeat;
	bool breaks_at_block_repeat_end;
	bool splits_block_repeat_end;
	bool splits_delayed_return_slot;
	bool fetch_split;
	uint32_t fetch_split_target;
	uint8_t block_repeat_slot;
	uint16_t block_repeat_start;
	teak_insn_t block_repeat_loop_instruction;
	uint8_t block_repeat_setup_level[TEAK_TCG_MAX_BLOCK_INSTRUCTIONS];
	teak_insn_t instructions[TEAK_TCG_MAX_BLOCK_INSTRUCTIONS];
};

struct teak_tcg_mode_t {
	bool specialized;
	uint8_t product_shift;
	uint8_t modulo_enable;
	uint8_t sat;
	uint8_t sata;
};

struct teak_tcg_block_cache_entry_t {
	teak_tcg_block_cache_entry_t *next;
	uint64_t cache_id;
	uint32_t program_context;
	uint32_t block_repeat_end[TEAK_BLOCK_REPEAT_LEVELS];
	uint8_t block_repeat_level;
	bool block_repeat_active;
	bool repeat_resume;
	/* the decode depends on no block-repeat state but repeat_resume */
	bool level_free;
	/* what a chain link reads, next to the lookup keys */
	bool dynamic_cycles;
	/* tcg_fast_map[pc] != 0, as of fast_epoch */
	bool fast;
	uint32_t fast_epoch;
	const void *code;
#ifdef CONFIG_TCG_WASM64
	/* the tagged table index of a landed tb, as of evict_epoch */
	uintptr_t target;
	uint32_t evict_epoch;
#endif
	/* list heads chained to from this entry, as of succ_epoch */
	teak_tcg_block_cache_entry_t *succ[2];
	uint32_t succ_epoch;
	teak_tcg_mode_t mode;
	TranslationBlock *tb;
	teak_tcg_block_t block;
};

struct teak_tcg_chain_link_t {
	uint64_t cache_generation;
	const void *code;
	uint32_t pc;
	uint32_t program_context;
	uint32_t block_repeat_end;
	uint32_t instruction_count;
	uint8_t block_repeat_active;
	uint8_t block_repeat_level;
	teak_tcg_mode_t mode;
#ifdef CONFIG_TCG_WASM64
	/* code is a tagged table index, valid until a batch eviction */
	uint32_t evict_epoch;
#endif
};

struct teak_tcg_chain_exit_t {
	teak_tcg_mode_t source_mode;
	uint8_t next_link;
	teak_tcg_chain_link_t links[TEAK_TCG_CHAIN_EXIT_LINKS];
};

static teak_tcg_block_cache_entry_t *tcg_block_cache[TEAK_TCG_BLOCK_CACHE_ENTRIES];
static uint64_t tcg_block_cache_generation = 1;
/*
 * Bumped whenever an entry is added or freed; never 0. Unlike the generation,
 * it survives program context switches (tens of thousands a second on S75).
 */
static uint32_t tcg_block_cache_epoch = 1;
static GPtrArray *tcg_chain_exits;
static uint8_t tcg_borrowed_word_addresses[TEAK_TCG_BLOCK_CACHE_ENTRIES / 8];
static TCGContext *tcg_block_cache_context;
static unsigned int tcg_block_cache_flush_count;
static TCGv_i32 tcg_memory_pc;
static TCGv_i32 tcg_memory_cycle;
static uint32_t tcg_memory_access;
static GHashTable *tcg_call_effects;
static teak_tcg_mode_t tcg_block_mode;

static const size_t tcg_flag_offsets[] = {
	offsetof(teak_state_t, fz),
	offsetof(teak_state_t, fm),
	offsetof(teak_state_t, fn),
	offsetof(teak_state_t, fv),
	offsetof(teak_state_t, fe),
	offsetof(teak_state_t, fc0),
	offsetof(teak_state_t, fc1),
	offsetof(teak_state_t, flm),
	offsetof(teak_state_t, fvl),
	offsetof(teak_state_t, fr),
};

static const size_t tcg_mode_offsets[] = {
	offsetof(teak_state_t, product_shift),
	offsetof(teak_state_t, modulo_enable),
	offsetof(teak_state_t, sat),
	offsetof(teak_state_t, sata),
};

#define TEAK_TCG_ALL_FLAGS	((uint16_t) (BIT(ARRAY_SIZE(tcg_flag_offsets)) - 1))

static const uint16_t teak_interrupt_vectors[] = { 0x0006, 0x000E, 0x0016, 0x0004 };

static void tcg_mark_call(uint32_t effects) {
	g_hash_table_insert(tcg_call_effects, tcg_last_op(), GUINT_TO_POINTER(effects));
}

/*
 * Translator temps are TCG_TB temps, which TCG never frees, so a block runs
 * out of them long before it runs out of instructions. Only the delayed
 * transfer target outlives the instruction that computes it, so every other
 * temp is handed back at each instruction boundary and reused by the next one.
 */
#define TEAK_TCG_POOL_TEMPS	256

typedef struct teak_tcg_temp_pool_t {
	TCGTemp *free[TCG_TYPE_COUNT][TEAK_TCG_POOL_TEMPS];
	TCGTemp *used[TCG_TYPE_COUNT][TEAK_TCG_POOL_TEMPS];
	unsigned free_count[TCG_TYPE_COUNT];
	unsigned used_count[TCG_TYPE_COUNT];
} teak_tcg_temp_pool_t;

static teak_tcg_temp_pool_t tcg_temp_pool;

static void tcg_temp_pool_reset(void) {
	memset(tcg_temp_pool.free_count, 0, sizeof(tcg_temp_pool.free_count));
	memset(tcg_temp_pool.used_count, 0, sizeof(tcg_temp_pool.used_count));
}

static void tcg_temp_pool_release(void) {
	for (int type = 0; type < TCG_TYPE_COUNT; type++) {
		while (tcg_temp_pool.used_count[type] != 0 && tcg_temp_pool.free_count[type] < TEAK_TCG_POOL_TEMPS) {
			tcg_temp_pool.free[type][tcg_temp_pool.free_count[type]++] =
				tcg_temp_pool.used[type][--tcg_temp_pool.used_count[type]];
		}
		tcg_temp_pool.used_count[type] = 0;
	}
}

static void tcg_temp_pool_pin(TCGTemp *temp) {
	unsigned *count = &tcg_temp_pool.used_count[temp->base_type];
	TCGTemp **used = tcg_temp_pool.used[temp->base_type];

	for (unsigned i = 0; i < *count; i++) {
		if (used[i] == temp) {
			used[i] = used[--*count];
			return;
		}
	}
}

static TCGTemp *tcg_temp_pool_get(TCGType type, TCGTemp *(*allocate)(void)) {
	TCGTemp *temp;

	if (tcg_temp_pool.free_count[type] != 0)
		temp = tcg_temp_pool.free[type][--tcg_temp_pool.free_count[type]];
	else
		temp = allocate();
	if (tcg_temp_pool.used_count[type] < TEAK_TCG_POOL_TEMPS)
		tcg_temp_pool.used[type][tcg_temp_pool.used_count[type]++] = temp;
	return temp;
}

static TCGTemp *tcg_temp_allocate_i32(void) {
	return tcgv_i32_temp((tcg_temp_new_i32)());
}

static TCGTemp *tcg_temp_allocate_i64(void) {
	return tcgv_i64_temp((tcg_temp_new_i64)());
}

static TCGv_i32 tcg_pool_temp_new_i32(void) {
	return temp_tcgv_i32(tcg_temp_pool_get(TCG_TYPE_I32, tcg_temp_allocate_i32));
}

static TCGv_i64 tcg_pool_temp_new_i64(void) {
	return temp_tcgv_i64(tcg_temp_pool_get(TCG_TYPE_I64, tcg_temp_allocate_i64));
}

static TCGv_ptr tcg_pool_temp_new_ptr(void) {
	QEMU_BUILD_BUG_ON(TCG_TYPE_PTR != TCG_TYPE_I64);
	return temp_tcgv_ptr(tcg_temp_pool_get(TCG_TYPE_I64, tcg_temp_allocate_i64));
}

#define tcg_temp_new_i32 tcg_pool_temp_new_i32
#define tcg_temp_new_i64 tcg_pool_temp_new_i64
#define tcg_temp_new_ptr tcg_pool_temp_new_ptr

static void tcg_fast_invalidate(void);
static void tcg_fast_discover(teak_tcg_core_t *core, uint32_t pc);
typedef struct teak_tcg_fast_site_t teak_tcg_fast_site_t;
static const teak_tcg_fast_site_t *tcg_fast_lookup(teak_tcg_core_t *core);
static uint8_t tcg_fast_map[TEAK_PROGRAM_ADDRESS_MASK + 1];
static uint32_t tcg_fast_map_epoch;
static uint32_t tcg_call_get_effects(const TCGOp *op) {
	return GPOINTER_TO_UINT(g_hash_table_lookup(tcg_call_effects, op));
}

static bool tcg_is_status_register(uint8_t register_code) {
	return register_code >= 8 && register_code <= 10;
}

static uint32_t tcg_register_write_effects(uint8_t register_code) {
	return tcg_is_status_register(register_code) ? 0 : TEAK_TCG_CALL_NO_MODE_WRITES;
}

static teak_tcg_mode_t tcg_mode_from_state(const teak_state_t *state) {
	return (teak_tcg_mode_t) {
		.specialized = true,
		.product_shift = state->product_shift,
		.modulo_enable = state->modulo_enable,
		.sat = state->sat,
		.sata = state->sata,
	};
}

static bool tcg_mode_is_compatible(teak_tcg_mode_t block_mode, teak_tcg_mode_t mode) {
	if (!block_mode.specialized)
		return true;
	return block_mode.product_shift == mode.product_shift && block_mode.modulo_enable == mode.modulo_enable &&
		block_mode.sat == mode.sat && block_mode.sata == mode.sata;
}

static TCGv_ptr tcg_emit_data_page(TCGv_i32 address, size_t table_offset) {
	TCGv_i32 entry_offset = tcg_temp_new_i32();
	TCGv_ptr entry = tcg_temp_new_ptr();
	TCGv_ptr page = tcg_temp_new_ptr();

	tcg_gen_shri_i32(entry_offset, address, TEAK_DATA_PAGE_SHIFT);
	tcg_gen_muli_i32(entry_offset, entry_offset, sizeof(const uint16_t *));
	tcg_gen_ext_i32_ptr(entry, entry_offset);
	tcg_gen_add_ptr(entry, entry, tcg_env);
	tcg_gen_ld_ptr(page, entry, table_offset);
	return page;
}

static TCGv_ptr tcg_emit_data_word_pointer(TCGv_ptr page, TCGv_i32 address) {
	TCGv_i32 word_offset = tcg_temp_new_i32();
	TCGv_ptr pointer = tcg_temp_new_ptr();

	tcg_gen_andi_i32(word_offset, address, TEAK_DATA_PAGE_WORDS - 1);
	tcg_gen_muli_i32(word_offset, word_offset, sizeof(uint16_t));
	tcg_gen_ext_i32_ptr(pointer, word_offset);
	tcg_gen_add_ptr(pointer, pointer, page);
	return pointer;
}

static void tcg_emit_data_read(TCGv_i32 result, TCGv_i32 address, teak_data_space_t space) {
	TCGLabel *slow = gen_new_label();
	TCGLabel *done = gen_new_label();
	uint32_t access = tcg_memory_access++;
	TCGv_ptr page = tcg_emit_data_page(address, offsetof(teak_tcg_core_t, data_map.read_pages[space]));

	tcg_gen_brcondi_ptr(TCG_COND_EQ, page, 0, slow);
	tcg_gen_ld16u_i32(result, tcg_emit_data_word_pointer(page, address), 0);
	tcg_gen_br(done);

	gen_set_label(slow);
	gen_helper_teak_tcg_data_read_at(result, tcg_env, address, tcg_memory_pc, tcg_memory_cycle,
		tcg_constant_i32(access));
	tcg_mark_call(TEAK_TCG_CALL_NO_FLAG_READS | TEAK_TCG_CALL_NO_MODE_WRITES);
	gen_set_label(done);
}

static void tcg_emit_data_write(TCGv_i32 address, TCGv_i32 value) {
	TCGLabel *slow = gen_new_label();
	TCGLabel *done = gen_new_label();
	uint32_t access = tcg_memory_access++;
	TCGv_ptr page = tcg_emit_data_page(address, offsetof(teak_tcg_core_t, data_map.write_pages));

	tcg_gen_brcondi_ptr(TCG_COND_EQ, page, 0, slow);
	tcg_gen_st16_i32(value, tcg_emit_data_word_pointer(page, address), 0);
	tcg_gen_br(done);

	gen_set_label(slow);
	gen_helper_teak_tcg_data_write_at(tcg_env, address, value, tcg_memory_pc, tcg_memory_cycle,
		tcg_constant_i32(access));
	tcg_mark_call(TEAK_TCG_CALL_NO_FLAG_READS | TEAK_TCG_CALL_NO_MODE_WRITES);
	gen_set_label(done);
}

#define gen_helper_teak_tcg_data_read(result, env, address) \
	tcg_emit_data_read(result, address, TEAK_DATA_SPACE_ALL)
#define gen_helper_teak_tcg_data_read_xz(result, env, address) \
	tcg_emit_data_read(result, address, TEAK_DATA_SPACE_XZ)
#define gen_helper_teak_tcg_data_read_y(result, env, address) \
	tcg_emit_data_read(result, address, TEAK_DATA_SPACE_Y)
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
	teak_update_modulo_steps(state);
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
	teak_update_modulo_steps(state);
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
	if (state->fetch_split_pending)
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

	if (state->repeat_active) {
		state->suspended_repeat_pc = state->pc;
		state->suspended_repc = state->repc;
		state->repeat_suspended = 1;
		state->repeat_active = 0;
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
		case 11:
			/* ph is write-only (TeakLite 4.41, 4.2.2 d); reading REG p yields a1[15:0]. */
			return state->a[1];
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
			if (state->lp)
				return state->block_repeat_lc[teak_block_repeat_slot(state->bcn)];
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
			teak_update_modulo_steps(state);
			return;
		case 15:
			state->stepj = value & 0x7FU;
			state->modj = value >> 7;
			teak_update_modulo_steps(state);
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
				state->block_repeat_lc[teak_block_repeat_slot(state->bcn)] = value;
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
	if (address - core->memory.cycle_sensitive_base >= core->memory.cycle_sensitive_size)
		return;

	uint32_t cycle = core->last_block_cycles + cycle_offset + core->batch_iterations * core->batch_block_cycles;
	bool same_block = core->synchronization_valid && core->synchronization_block_start == core->last_block_cycles;
	uint32_t cycles;

	if (!same_block) {
		assert(core->advanced_cycles <= cycle);
		cycles = cycle - core->advanced_cycles;
	} else if (cycle > core->synchronization_cycle) {
		cycles = cycle - core->synchronization_cycle;
	} else if (access > core->synchronization_access) {
		cycles = 0;
	} else {
		cycles = 1;
	}
	core->advanced_cycles += cycles;
	core->synchronization_cycle = cycle;
	core->synchronization_block_start = core->last_block_cycles;
	core->synchronization_access = access;
	core->synchronization_valid = true;
	if (cycles != 0)
		core->memory.advance_cycles(core->memory.cycle_opaque, cycles);
}

static void tcg_add_data_wait_cycles(teak_tcg_core_t *core, uint32_t address) {
	if (address - core->memory.wait_state_base >= core->memory.wait_state_size)
		return;
	core->wait_cycles += core->memory.wait_state_cycles;
	core->batch_cycles_remaining -= MIN(core->batch_cycles_remaining, core->memory.wait_state_cycles);
}

static bool tcg_mapped_data_read(teak_tcg_core_t *core, uint32_t address, uint16_t *value) {
	const uint16_t *page = core->data_map.read_pages[TEAK_DATA_SPACE_ALL][address / TEAK_DATA_PAGE_WORDS];

	if (page == NULL)
		return false;
	*value = qatomic_read(&page[address % TEAK_DATA_PAGE_WORDS]);
	return true;
}

static bool tcg_mapped_data_write(teak_tcg_core_t *core, uint32_t address, uint16_t value) {
	uint16_t *page = core->data_map.write_pages[address / TEAK_DATA_PAGE_WORDS];

	if (page == NULL)
		return false;
	qatomic_set(&page[address % TEAK_DATA_PAGE_WORDS], value);
	return true;
}

uint32_t HELPER(teak_tcg_data_read_at)(void *opaque, uint32_t address, uint32_t pc, uint32_t cycle_offset, uint32_t access) {
	teak_state_t *state = opaque;
	teak_tcg_core_t *core = container_of(state, teak_tcg_core_t, state);

	state->trace_pc = pc;
	tcg_synchronize_data_access(core, address, cycle_offset, access);
	tcg_add_data_wait_cycles(core, address);
	return teak_data_read(core, address);
}

void HELPER(teak_tcg_data_write_at)(void *opaque, uint32_t address, uint32_t value, uint32_t pc,
	uint32_t cycle_offset, uint32_t access
) {
	teak_state_t *state = opaque;
	teak_tcg_core_t *core = container_of(state, teak_tcg_core_t, state);

	state->trace_pc = pc;
	tcg_synchronize_data_access(core, address, cycle_offset, access);
	tcg_add_data_wait_cycles(core, address);
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

void HELPER(teak_tcg_update_modulo_steps)(void *opaque) {
	teak_update_modulo_steps(opaque);
}

void HELPER(teak_tcg_context_switch)(void *opaque, uint32_t restore) {
	teak_state_t *state = opaque;
	if (restore) {
		tcg_context_restore(state);
	} else {
		tcg_context_store(state);
	}
}

/* Registers that read and write as a plain state field, whichever mov or ALU form touches them. */
static ptrdiff_t tcg_plain_register_offset(uint8_t register_code) {
	if (register_code < 6)
		return offsetof(teak_state_t, r) + register_code * sizeof(uint16_t);
	if (register_code == 6)
		return offsetof(teak_state_t, r) + 7 * sizeof(uint16_t);
	if (register_code == 7)
		return offsetof(teak_state_t, y);
	if (register_code == 13)
		return offsetof(teak_state_t, sp);
	return -1;
}

static void tcg_emit_data_bus_saturation(TCGv_i64 value);

/*
 * Reads that need no helper beyond a plain field: an accumulator half (through
 * the data bus saturation for a mov, as tcg_mov_read_register does) and the
 * step/modulo packs. Returns false for any other register.
 */
static bool tcg_emit_inline_register_read(TCGv_i32 value, uint8_t register_code, bool mov) {
	ptrdiff_t offset;
	bool high;

	if (register_code == 14 || register_code == 15) {
		TCGv_i32 modulo = tcg_temp_new_i32();

		tcg_gen_ld8u_i32(value, tcg_env, register_code == 14 ? offsetof(teak_state_t, stepi) :
			offsetof(teak_state_t, stepj));
		tcg_gen_ld16u_i32(modulo, tcg_env, register_code == 14 ? offsetof(teak_state_t, modi) :
			offsetof(teak_state_t, modj));
		tcg_gen_shli_i32(modulo, modulo, 7);
		tcg_gen_or_i32(value, value, modulo);
		tcg_gen_ext16u_i32(value, value);
		return true;
	}
	if (register_code >= 16 && register_code <= 19) {
		offset = offsetof(teak_state_t, b) + (register_code & 1U) * sizeof(uint64_t);
		high = register_code < 18;
	} else if (register_code >= 26 && register_code <= 29) {
		offset = offsetof(teak_state_t, a) + (register_code & 1U) * sizeof(uint64_t);
		high = register_code >= 28;
	} else {
		return false;
	}

	TCGv_i64 accumulator = tcg_temp_new_i64();

	tcg_gen_ld_i64(accumulator, tcg_env, offset);
	if (mov) {
		tcg_gen_shli_i64(accumulator, accumulator, TEAK_ACCUMULATOR_HOST_SHIFT);
		tcg_gen_sari_i64(accumulator, accumulator, TEAK_ACCUMULATOR_HOST_SHIFT);
		tcg_emit_data_bus_saturation(accumulator);
	}
	if (high)
		tcg_gen_shri_i64(accumulator, accumulator, 16);
	tcg_gen_extrl_i64_i32(value, accumulator);
	tcg_gen_ext16u_i32(value, value);
	return true;
}

static void tcg_emit_register_load(TCGv_i32 value, uint8_t register_code) {
	ptrdiff_t offset = tcg_plain_register_offset(register_code);

	if (offset >= 0)
		tcg_gen_ld16u_i32(value, tcg_env, offset);
	else if (!tcg_emit_inline_register_read(value, register_code, false)) {
		gen_helper_teak_tcg_register_read(value, tcg_env, tcg_constant_i32(register_code));
		tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
	}
}

static void tcg_emit_register_write(uint8_t register_code, TCGv_i32 value) {
	ptrdiff_t offset = tcg_plain_register_offset(register_code);

	if (offset >= 0)
		tcg_gen_st16_i32(value, tcg_env, offset);
	else {
		gen_helper_teak_tcg_register_write(tcg_env, tcg_constant_i32(register_code), value);
		tcg_mark_call(tcg_register_write_effects(register_code));
	}
}

static void tcg_emit_mov_register_load(TCGv_i32 value, uint8_t register_code) {
	ptrdiff_t offset = tcg_plain_register_offset(register_code);

	if (offset >= 0)
		tcg_gen_ld16u_i32(value, tcg_env, offset);
	else if (!tcg_emit_inline_register_read(value, register_code, true)) {
		gen_helper_teak_tcg_mov_register_read(value, tcg_env, tcg_constant_i32(register_code));
		tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
	}
}

static void tcg_emit_accumulator_value_flags(TCGv_i64 value);

/* tcg_mov_write_register() of an accumulator half: the whole accumulator is replaced. */
static bool tcg_emit_inline_mov_accumulator_write(uint8_t register_code, TCGv_i32 value) {
	ptrdiff_t offset;
	bool high;

	if (register_code >= 16 && register_code <= 19) {
		offset = offsetof(teak_state_t, b) + (register_code & 1U) * sizeof(uint64_t);
		high = register_code < 18;
	} else if (register_code >= 26 && register_code <= 29) {
		offset = offsetof(teak_state_t, a) + (register_code & 1U) * sizeof(uint64_t);
		high = register_code >= 28;
	} else {
		return false;
	}

	TCGv_i32 half = tcg_temp_new_i32();
	TCGv_i64 accumulator = tcg_temp_new_i64();

	if (high) {
		tcg_gen_shli_i32(half, value, 16);
		tcg_gen_ext_i32_i64(accumulator, half);
	} else {
		tcg_gen_ext16u_i32(half, value);
		tcg_gen_extu_i32_i64(accumulator, half);
	}
	tcg_gen_st_i64(accumulator, tcg_env, offset);
	tcg_emit_accumulator_value_flags(accumulator);
	return true;
}

static void tcg_emit_mov_register_write(uint8_t register_code, TCGv_i32 value) {
	ptrdiff_t offset = tcg_plain_register_offset(register_code);

	if (offset >= 0)
		tcg_gen_st16_i32(value, tcg_env, offset);
	else if (!tcg_emit_inline_mov_accumulator_write(register_code, value)) {
		gen_helper_teak_tcg_mov_register_write(tcg_env, tcg_constant_i32(register_code), value);
		tcg_mark_call(tcg_register_write_effects(register_code));
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

	if (!tcg_mapped_data_read(core, address, &value)) {
		state->trace_pc = pc;
		tcg_synchronize_data_access(core, address, cycle_offset, access);
		value = teak_data_read(core, address);
	}
	result = tcg_alb_result(state, alb_operation, value, mask);
	if (tcg_alb_modifies_operand(alb_operation) && !tcg_mapped_data_write(core, address, result))
		teak_data_write(core, address, result);
}

static uint16_t tcg_shifted_product_low(const teak_state_t *state) {
	switch (state->product_shift) {
		case 1:
			return state->p[0] >> 1;
		case 2:
			return state->p[0] << 1;
		case 3:
			return state->p[0] << 2;
	}
	return state->p[0];
}

/*
 * The datasheet forbids aX as an ALB operand (TeakLite 4.41, set/rst/chng/addv/subv/cmpv). Hardware operates on the
 * whole 36-bit aX with the mask sign-extended, takes C from bit 36 and Z and M from the low 16 bits; tst0 and tst1
 * test the 16-bit mask.
 */
static void tcg_alb_accumulator(teak_state_t *state, teak_alb_operation_t operation, uint8_t index, uint16_t mask) {
	uint64_t value = state->a[index] & TEAK_ACCUMULATOR_MASK;
	uint64_t operand = (uint64_t) (int64_t) (int16_t) mask & TEAK_ACCUMULATOR_MASK;
	uint64_t result;

	switch (operation) {
		case TEAK_ALB_SET:
			result = value | operand;
			break;
		case TEAK_ALB_RST:
			result = value & ~operand;
			break;
		case TEAK_ALB_CHNG:
			result = value ^ operand;
			break;
		case TEAK_ALB_ADDV:
			result = value + operand;
			state->fc0 = (result >> TEAK_ACCUMULATOR_BITS) & 1U;
			break;
		case TEAK_ALB_CMPV:
		case TEAK_ALB_SUBV:
			result = value - operand;
			state->fc0 = (result >> TEAK_ACCUMULATOR_BITS) & 1U;
			break;
		case TEAK_ALB_TST0:
			state->fz = (value & mask) == 0;
			return;
		case TEAK_ALB_TST1:
			state->fz = (value & mask) == mask;
			return;
		default:
			g_assert_not_reached();
	}
	state->fz = (result & 0xFFFFU) == 0;
	state->fm = (result >> 15) & 1U;
	if (operation != TEAK_ALB_CMPV)
		state->a[index] = (uint64_t) tcg_sign_extend_accumulator(result);
}

uint32_t HELPER(teak_tcg_alb_value)(void *opaque, uint32_t value, uint32_t mask, uint32_t operation) {
	return tcg_alb_result(opaque, (teak_alb_operation_t) operation, value, mask);
}

void HELPER(teak_tcg_alb_register)(void *opaque, uint32_t register_code, uint32_t mask, uint32_t operation) {
	teak_state_t *state = opaque;
	teak_alb_operation_t alb_operation = (teak_alb_operation_t) operation;

	if (register_code == 24 || register_code == 25) {
		tcg_alb_accumulator(state, alb_operation, register_code & 1U, mask);
		return;
	}

	uint16_t value = register_code == 11 ? tcg_shifted_product_low(state) : tcg_read_register(state, register_code);
	uint16_t result = tcg_alb_result(state, alb_operation, value, mask);

	if (tcg_alb_modifies_operand(alb_operation))
		tcg_write_register(state, register_code, result);
}

#define HELPER_H "hw/arm/pmb887x/dsp/tcg-helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

static void tcg_request_tb_flush(void) {
	g_assert(first_cpu != NULL);
	queue_tb_flush(first_cpu);
}

/*
 * Every instruction stores the registers it writes, its flags and the next
 * pc to the state, and a run of MACs overwrites them on every step. Drop the
 * state stores that every path overwrites before anything can observe them;
 * TCG's liveness then drops the computations that fed them.
 *
 * Observers are explicit loads from env, every exit from the block, and any
 * helper call except the data accessors. Those reach peripherals and the
 * runtime, which read nothing of the state but trace_pc, and the accessors
 * set that themselves before they use it.
 */
#define TCG_STATE_WORDS	DIV_ROUND_UP(sizeof(teak_state_t), 64)

typedef struct tcg_state_bytes_t {
	uint64_t word[TCG_STATE_WORDS];
} tcg_state_bytes_t;

static unsigned tcg_env_access_size(TCGOp *op) {
	switch (op->opc) {
		case INDEX_op_ld8u:
		case INDEX_op_ld8s:
		case INDEX_op_st8:
			return 1;
		case INDEX_op_ld16u:
		case INDEX_op_ld16s:
		case INDEX_op_st16:
			return 2;
		case INDEX_op_ld32u:
		case INDEX_op_ld32s:
		case INDEX_op_st32:
			return 4;
		case INDEX_op_ld:
		case INDEX_op_st:
			return TCGOP_TYPE(op) == TCG_TYPE_I32 ? 4 : 8;
		default:
			return 0;
	}
}

static uint16_t tcg_env_field_mask(const TCGOp *op, size_t size, const size_t *field_offsets, size_t field_count) {
	if (arg_temp(op->args[1]) != tcgv_ptr_temp(tcg_env))
		return 0;

	size_t offset = op->args[2];
	uint16_t mask = 0;
	for (size_t i = 0; i < field_count; i++) {
		if (field_offsets[i] >= offset && field_offsets[i] < offset + size)
			mask |= BIT(i);
	}
	return mask;
}

static uint16_t tcg_env_flag_mask(const TCGOp *op, size_t size) {
	return tcg_env_field_mask(op, size, tcg_flag_offsets, ARRAY_SIZE(tcg_flag_offsets));
}

static size_t tcg_store_size(const TCGOp *op) {
	switch (op->opc) {
		case INDEX_op_st8:
			return 1;
		case INDEX_op_st16:
			return 2;
		case INDEX_op_st32:
			return 4;
		case INDEX_op_st:
			return tcg_type_size(TCGOP_TYPE(op));
		default:
			return 0;
	}
}

static bool tcg_block_writes_mode(void) {
	TCGOp *op;

	QTAILQ_FOREACH(op, &tcg_ctx->ops, link) {
		if (op->opc == INDEX_op_call && (tcg_call_get_effects(op) & TEAK_TCG_CALL_NO_MODE_WRITES) == 0)
			return true;

		size_t size = tcg_store_size(op);
		if (size != 0 && tcg_env_field_mask(op, size, tcg_mode_offsets, ARRAY_SIZE(tcg_mode_offsets)) != 0)
			return true;
	}
	return false;
}

static void tcg_remove_dead_flag_stores(void) {
	g_autofree uint16_t *label_live_flags = g_new(uint16_t, tcg_ctx->nb_labels);
	uint16_t live_flags = TEAK_TCG_ALL_FLAGS;
	TCGOp *op;
	TCGOp *previous;

	for (int i = 0; i < tcg_ctx->nb_labels; i++)
		label_live_flags[i] = TEAK_TCG_ALL_FLAGS;

	QTAILQ_FOREACH_REVERSE_SAFE(op, &tcg_ctx->ops, link, previous) {
		switch (op->opc) {
			case INDEX_op_set_label:
				label_live_flags[arg_label(op->args[0])->id] = live_flags;
				break;
			case INDEX_op_br:
				live_flags = label_live_flags[arg_label(op->args[0])->id];
				break;
			case INDEX_op_call:
				if ((tcg_call_get_effects(op) & TEAK_TCG_CALL_NO_FLAG_READS) == 0)
					live_flags = TEAK_TCG_ALL_FLAGS;
				break;
			case INDEX_op_ld8u:
			case INDEX_op_ld8s:
				live_flags |= tcg_env_flag_mask(op, 1);
				break;
			case INDEX_op_ld16u:
			case INDEX_op_ld16s:
				live_flags |= tcg_env_flag_mask(op, 2);
				break;
			case INDEX_op_ld32u:
			case INDEX_op_ld32s:
				live_flags |= tcg_env_flag_mask(op, 4);
				break;
			case INDEX_op_ld:
				live_flags |= tcg_env_flag_mask(op, tcg_type_size(TCGOP_TYPE(op)));
				break;
			case INDEX_op_st8: {
				uint16_t stored_flags = tcg_env_flag_mask(op, 1);

				if (stored_flags != 0 && (live_flags & stored_flags) == 0) {
					tcg_op_remove(tcg_ctx, op);
					break;
				}
				live_flags &= ~stored_flags;
				break;
			}
			default: {
				const TCGOpDef *def = &tcg_op_defs[op->opc];

				if ((def->flags & TCG_OPF_BB_EXIT) != 0)
					live_flags = TEAK_TCG_ALL_FLAGS;
				if ((def->flags & TCG_OPF_COND_BRANCH) != 0)
					live_flags |= label_live_flags[arg_label(op->args[def->nb_args - 1])->id];
				break;
			}
		}
	}
}

static bool tcg_env_store(TCGOp *op) {
	return op->opc == INDEX_op_st8 || op->opc == INDEX_op_st16 ||
		op->opc == INDEX_op_st32 || op->opc == INDEX_op_st;
}

/* The state bytes @op loads or stores; false if it accesses none. */
static bool tcg_state_access(TCGOp *op, tcg_state_bytes_t *bytes) {
	unsigned size = tcg_env_access_size(op);
	bool any = false;

	if (size == 0 || arg_temp(op->args[1]) != tcgv_ptr_temp(tcg_env))
		return false;
	memset(bytes, 0, sizeof(*bytes));
	for (unsigned i = 0; i < size; i++) {
		intptr_t byte = op->args[2] + i;

		if (byte >= 0 && byte < (intptr_t) sizeof(teak_state_t)) {
			bytes->word[byte / 64] |= BIT_ULL(byte % 64);
			any = true;
		}
	}
	return any;
}

static bool tcg_call_hides_state(TCGOp *op) {
	void *func = tcg_call_func(op);

	return func == (void *) helper_teak_tcg_data_read_at ||
		func == (void *) helper_teak_tcg_data_write_at;
}

/* Backward liveness of the state bytes at one op; label_live holds the
 * state at every label, taken from the previous sweep. */
static void tcg_state_live_before(TCGOp *op, tcg_state_bytes_t *live, const tcg_state_bytes_t *label_live) {
	tcg_state_bytes_t bytes;

	switch (op->opc) {
		case INDEX_op_br:
			*live = label_live[arg_label(op->args[0])->id];
			return;
		case INDEX_op_brcond:
			for (unsigned i = 0; i < TCG_STATE_WORDS; i++)
				live->word[i] |= label_live[arg_label(op->args[3])->id].word[i];
			return;
		case INDEX_op_exit_tb:
		case INDEX_op_goto_tb:
		case INDEX_op_goto_ptr:
			memset(live, 0xff, sizeof(*live));
			return;
		case INDEX_op_call:
			if (!tcg_call_hides_state(op))
				memset(live, 0xff, sizeof(*live));
			return;
		default:
			break;
	}
	if (!tcg_state_access(op, &bytes))
		return;
	for (unsigned i = 0; i < TCG_STATE_WORDS; i++) {
		if (tcg_env_store(op))
			live->word[i] &= ~bytes.word[i];
		else
			live->word[i] |= bytes.word[i];
	}
}

static void tcg_remove_dead_state_stores(TCGContext *s) {
	g_autofree tcg_state_bytes_t *label_live = g_new0(tcg_state_bytes_t, s->nb_labels);
	tcg_state_bytes_t live;
	TCGOp *op, *prev;
	bool changed;

	/* Labels start dead and only grow, so loops converge. */
	do {
		changed = false;
		memset(&live, 0xff, sizeof(live));
		QTAILQ_FOREACH_REVERSE(op, &s->ops, link) {
			if (op->opc == INDEX_op_set_label) {
				tcg_state_bytes_t *in = &label_live[arg_label(op->args[0])->id];

				for (unsigned i = 0; i < TCG_STATE_WORDS; i++) {
					if ((in->word[i] | live.word[i]) != in->word[i]) {
						in->word[i] |= live.word[i];
						changed = true;
					}
				}
				live = *in;
				continue;
			}
			tcg_state_live_before(op, &live, label_live);
		}
	} while (changed);

	memset(&live, 0xff, sizeof(live));
	QTAILQ_FOREACH_REVERSE_SAFE(op, &s->ops, link, prev) {
		tcg_state_bytes_t bytes;

		if (op->opc == INDEX_op_set_label) {
			live = label_live[arg_label(op->args[0])->id];
			continue;
		}
		if (tcg_env_store(op) && tcg_state_access(op, &bytes)) {
			bool dead = true;

			for (unsigned i = 0; i < TCG_STATE_WORDS; i++)
				dead &= (bytes.word[i] & live.word[i]) == 0;
			if (dead) {
				tcg_op_remove(s, op);
				continue;
			}
		}
		tcg_state_live_before(op, &live, label_live);
	}
}

/*
 * TCG keeps a TEMP_TB temp in memory at every branch, since it cannot see
 * whether the target reads it, and the temp pool reuses a few TEMP_TB temps
 * for every instruction: almost every value the block computes was stored
 * to the frame and never read. A TB-wide liveness over the labels finds the
 * temps no label is entered with a value of, which TCG may then treat as
 * TEMP_EBB, dead at every label.
 */
static void tcg_demote_label_dead_temps(TCGContext *s) {
	unsigned words = DIV_ROUND_UP(s->nb_temps, 64);
	g_autofree uint64_t *label_live = g_new0(uint64_t, (size_t) s->nb_labels * words);
	g_autofree uint64_t *live = g_new0(uint64_t, words);
	g_autofree uint64_t *crossing = g_new0(uint64_t, words);
	TCGOp *op;
	bool changed;

	/* Labels start dead and only grow, so loops converge. */
	do {
		changed = false;
		memset(live, 0, words * sizeof(uint64_t));
		QTAILQ_FOREACH_REVERSE(op, &s->ops, link) {
			const TCGOpDef *def = &tcg_op_defs[op->opc];
			unsigned nb_oargs, nb_iargs;
			uint64_t *target = NULL;

			switch (op->opc) {
				case INDEX_op_set_label: {
					uint64_t *in = label_live + (size_t) arg_label(op->args[0])->id * words;
					for (unsigned i = 0; i < words; i++) {
						if ((in[i] | live[i]) != in[i]) {
							in[i] |= live[i];
							changed = true;
						}
					}
					continue;
				}
				case INDEX_op_br:
					memcpy(live, label_live + (size_t) arg_label(op->args[0])->id * words, words * sizeof(uint64_t));
					continue;
				case INDEX_op_brcond:
					target = label_live + (size_t) arg_label(op->args[3])->id * words;
					break;
				case INDEX_op_discard:
					live[temp_idx(arg_temp(op->args[0])) / 64] &= ~BIT_ULL(temp_idx(arg_temp(op->args[0])) % 64);
					continue;
				case INDEX_op_call:
					nb_oargs = TCGOP_CALLO(op);
					nb_iargs = TCGOP_CALLI(op);
					goto operands;
				default:
					break;
			}
			if (def->flags & TCG_OPF_BB_EXIT)
				memset(live, 0, words * sizeof(uint64_t));
			else if (def->flags & TCG_OPF_BB_END && target == NULL)
				g_assert_not_reached();
			nb_oargs = def->nb_oargs;
			nb_iargs = def->nb_iargs;
		operands:
			if (target != NULL) {
				for (unsigned i = 0; i < words; i++)
					live[i] |= target[i];
			}
			for (unsigned i = 0; i < nb_oargs; i++) {
				size_t index = temp_idx(arg_temp(op->args[i]));
				live[index / 64] &= ~BIT_ULL(index % 64);
			}
			for (unsigned i = nb_oargs; i < nb_oargs + nb_iargs; i++) {
				size_t index = temp_idx(arg_temp(op->args[i]));
				live[index / 64] |= BIT_ULL(index % 64);
			}
		}
	} while (changed);

	for (size_t label = 0; label < s->nb_labels; label++) {
		for (unsigned i = 0; i < words; i++)
			crossing[i] |= label_live[label * words + i];
	}
	for (int i = s->nb_globals; i < s->nb_temps; i++) {
		if (s->temps[i].kind == TEMP_TB && !(crossing[i / 64] & BIT_ULL(i % 64)))
			s->temps[i].kind = TEMP_EBB;
	}
}

static TranslationBlock *tcg_compile_block(uint32_t pc, uint16_t words, uint16_t instruction_count,
	teak_tcg_emit_fn *emit, void *opaque, int *error
) {
	TranslationBlock *tb;
	void *code;
	int code_size;
	bool next_region = false;

	if (tcg_call_effects == NULL)
		tcg_call_effects = g_hash_table_new(NULL, NULL);

	*error = -1;
	qemu_thread_jit_write();
retry:
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
		g_hash_table_remove_all(tcg_call_effects);
		emit(opaque);
		tcg_gen_exit_tb(NULL, 0);
		if (tcg_block_mode.specialized && tcg_block_writes_mode()) {
			code_size = TEAK_TCG_COMPILE_MODE_WRITE;
		} else {
			tcg_remove_dead_flag_stores();
			tcg_remove_dead_state_stores(tcg_ctx);
			tcg_demote_label_dead_temps(tcg_ctx);
			code_size = tcg_gen_code(tcg_ctx, tb, pc);
		}
	}
	tcg_ctx->gen_tb = NULL;
	if (code_size == -1 && !next_region) {
		/*
		 * Past the highwater of the current region, not of the whole buffer:
		 * give up the rest of it, and tcg_tb_alloc moves on to the next one,
		 * failing above (with the flush) only when every region is used.
		 */
		next_region = true;
		qatomic_set(&tcg_ctx->code_gen_ptr, tcg_ctx->code_gen_highwater);
		goto retry;
	}
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
	perf_report_named(tb->tc.ptr, code_size, "dsp-0x%05" PRIx32, pc);
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
	memset(tcg_borrowed_word_addresses, 0, sizeof(tcg_borrowed_word_addresses));
	if (tcg_chain_exits != NULL)
		g_ptr_array_set_size(tcg_chain_exits, 0);
	tcg_block_cache_generation++;
	tcg_block_cache_epoch++;
}

static void tcg_remove_cached_entry(teak_tcg_block_cache_entry_t **link) {
	teak_tcg_block_cache_entry_t *entry = *link;

	*link = entry->next;
	g_free(entry);
	tcg_block_cache_generation++;
	tcg_block_cache_epoch++;
}

static bool tcg_is_block_repeat(const teak_insn_t *instruction);

#ifdef CONFIG_TCG_WASM64
static uint32_t tcg_block_cache_evict_epoch;

/* An evicted batch took its blocks' code with it: they are compiled again. */
static void tcg_drop_evicted_blocks(void) {
	if (tcg_block_cache_evict_epoch == w64_evict_epoch)
		return;
	tcg_block_cache_evict_epoch = w64_evict_epoch;
	for (size_t i = 0; i < ARRAY_SIZE(tcg_block_cache); i++) {
		teak_tcg_block_cache_entry_t **link = &tcg_block_cache[i];
		while (*link != NULL) {
			if (w64_tc_evicted((*link)->code))
				tcg_remove_cached_entry(link);
			else
				link = &(*link)->next;
		}
	}
}
#endif

static void tcg_check_block_cache(void) {
	unsigned int flush_count = qatomic_read(&tb_ctx.tb_flush_count);
	if (tcg_block_cache_context == tcg_ctx && tcg_block_cache_flush_count == flush_count) {
#ifdef CONFIG_TCG_WASM64
		tcg_drop_evicted_blocks();
#endif
		return;
	}
	tcg_clear_block_cache();
	tcg_block_cache_context = tcg_ctx;
	tcg_block_cache_flush_count = flush_count;
}

static bool tcg_cached_block_is_prefix(const teak_tcg_block_t *cached, const teak_tcg_block_t *block) {
	size_t instruction_bytes;

	if (cached->instruction_count >= block->instruction_count)
		return false;
	if (cached->repeat_resume != block->repeat_resume)
		return false;
	if (cached->ends_block_repeat || cached->splits_block_repeat_end || cached->splits_delayed_return_slot)
		return false;
	if (cached->fetch_split)
		return false;
	if (cached->instructions[cached->instruction_count - 1].opcode == TEAK_OP_MOVD_RN_RN)
		return false;
	instruction_bytes = cached->instruction_count * sizeof(cached->instructions[0]);
	if (memcmp(cached->instructions, block->instructions, instruction_bytes) != 0)
		return false;
	return memcmp(cached->block_repeat_setup_level, block->block_repeat_setup_level,
		cached->instruction_count) == 0;
}

static const teak_tcg_block_cache_entry_t *tcg_find_cached_block(teak_tcg_core_t *core, teak_tcg_block_t *block) {
	teak_tcg_mode_t mode = tcg_mode_from_state(&core->state);
	teak_tcg_block_cache_entry_t *entry;
	teak_tcg_block_cache_entry_t *prefix = NULL;

	tcg_check_block_cache();
	entry = tcg_block_cache[block->pc];
	while (entry != NULL) {
		bool longer_prefix = prefix == NULL || entry->block.instruction_count > prefix->block.instruction_count;
		bool same_context = entry->cache_id == core->cache_id && entry->program_context == core->program_context;

		if (!same_context || !tcg_mode_is_compatible(entry->mode, mode)) {
			entry = entry->next;
			continue;
		}
		if (memcmp(&entry->block, block, sizeof(*block)) == 0)
			return entry;
		if (longer_prefix && tcg_cached_block_is_prefix(&entry->block, block))
			prefix = entry;
		entry = entry->next;
	}
	if (prefix != NULL)
		*block = prefix->block;
	return prefix;
}

static uint8_t tcg_active_block_repeat_slots(const teak_state_t *state) {
	return state->lp ? teak_block_repeat_slot(state->bcn) + 1 : 0;
}

static bool tcg_same_block_repeat_ends(const uint32_t *cached_ends, const uint32_t *current_ends, uint8_t slots) {
	for (uint8_t i = 0; i < slots; i++) {
		if (cached_ends[i] != current_ends[i])
			return false;
	}
	return true;
}

static bool tcg_block_overlaps(const teak_tcg_block_t *block, uint32_t start, uint32_t end) {
	return block->pc <= end && block->pc + block->words - 1 >= start;
}

static inline QEMU_ALWAYS_INLINE bool tcg_cached_entry_matches(const teak_tcg_core_t *core,
	const teak_tcg_block_cache_entry_t *entry) {
	uint8_t level = core->state.bcn;
	bool same_cache = entry->cache_id == core->cache_id;
	bool same_context = entry->program_context == core->program_context;
	bool same_repeat = entry->repeat_resume == core->state.repeat_active;
	bool same_fetch = entry->block.fetch_split == core->state.fetch_split_pending &&
		(!entry->block.fetch_split || entry->block.fetch_split_target == core->state.fetch_split_target);

	if (!same_cache || !same_context || !same_repeat || !same_fetch)
		return false;
	if (entry->mode.specialized &&
		!tcg_mode_is_compatible(entry->mode, tcg_mode_from_state(&core->state)))
		return false;
	/*
	 * A level-free block outside the active repeat body decodes the
	 * same at any nesting: the routines a loop body calls are reached
	 * under every loop in the program.
	 */
	if (entry->level_free) {
		if (!core->state.lp)
			return true;
		uint8_t slot = teak_block_repeat_slot(level);
		if (!tcg_block_overlaps(&entry->block, core->state.block_repeat_start[slot],
				core->state.block_repeat_end[slot]))
			return true;
	}
	bool same_level = entry->block_repeat_level == level;
	bool same_active = entry->block_repeat_active == core->state.lp;
	bool same_split = !entry->block.splits_block_repeat_end ||
		entry->block.block_repeat_start == core->state.block_repeat_start[entry->block.block_repeat_slot];
	return same_level && same_active && same_split &&
		tcg_same_block_repeat_ends(entry->block_repeat_end, core->state.block_repeat_end,
			tcg_active_block_repeat_slots(&core->state));
}

static inline QEMU_ALWAYS_INLINE teak_tcg_block_cache_entry_t *tcg_find_cached_entry_fast(teak_tcg_core_t *core) {
	teak_tcg_block_cache_entry_t *entry;

	entry = tcg_block_cache[core->state.pc];
	while (entry != NULL) {
		if (tcg_cached_entry_matches(core, entry)) {
			core->cache_fast_hits++;
			return entry;
		}
		entry = entry->next;
	}
	return NULL;
}

/*
 * tcg_find_cached_entry_fast() for a chain link, first through the list
 * heads the previous link's entry went to.  A head that matches is what the
 * walk returns as long as no entry was added or freed since.
 */
static inline QEMU_ALWAYS_INLINE teak_tcg_block_cache_entry_t *tcg_find_chained_entry(teak_tcg_core_t *core) {
	teak_tcg_block_cache_entry_t *prev = core->chain_prev;
	teak_tcg_block_cache_entry_t *entry;
	uint32_t pc = core->state.pc;

	if (prev != NULL && prev->succ_epoch == tcg_block_cache_epoch) {
		for (size_t i = 0; i < ARRAY_SIZE(prev->succ); i++) {
			entry = prev->succ[i];
			if (entry != NULL && entry->block.pc == pc && tcg_cached_entry_matches(core, entry)) {
				core->cache_fast_hits++;
				return entry;
			}
		}
	}
	entry = tcg_find_cached_entry_fast(core);
	if (prev != NULL && entry != NULL && tcg_block_cache[pc] == entry) {
		if (prev->succ_epoch != tcg_block_cache_epoch) {
			prev->succ[0] = NULL;
			prev->succ[1] = NULL;
			prev->succ_epoch = tcg_block_cache_epoch;
		}
		prev->succ[1] = prev->succ[0];
		prev->succ[0] = entry;
	}
	return entry;
}

static TranslationBlock *tcg_find_cached_block_fast(teak_tcg_core_t *core, teak_tcg_block_t *block) {
	teak_tcg_block_cache_entry_t *entry = tcg_find_cached_entry_fast(core);
	if (entry == NULL)
		return NULL;
	*block = entry->block;
	return entry->tb;
}

static bool tcg_block_has_dynamic_cycles(const teak_tcg_block_t *block);

static void tcg_cache_block(
	const teak_tcg_core_t *core,
	const teak_tcg_block_t *block,
	TranslationBlock *tb,
	teak_tcg_mode_t mode
) {
	teak_tcg_block_cache_entry_t *entry = g_new(teak_tcg_block_cache_entry_t, 1);
	entry->block = *block;
	entry->tb = tb;
	entry->code = tb->tc.ptr;
	entry->dynamic_cycles = tcg_block_has_dynamic_cycles(block);
	entry->fast_epoch = 0;
#ifdef CONFIG_TCG_WASM64
	entry->evict_epoch = w64_evict_epoch - 1;
#endif
	entry->cache_id = core->cache_id;
	entry->program_context = core->program_context;
	memcpy(entry->block_repeat_end, core->state.block_repeat_end, sizeof(entry->block_repeat_end));
	entry->block_repeat_level = core->state.bcn;
	entry->block_repeat_active = core->state.lp;
	entry->repeat_resume = block->repeat_resume;
	/*
	 * tcg_decode_block() reads the block-repeat state only for a repeat
	 * setup, a break, and instructions inside the active body, and the
	 * emitted code takes it from the setup levels it recorded.
	 */
	uint8_t slot = teak_block_repeat_slot(core->state.bcn);
	entry->level_free = !core->state.lp ||
		!tcg_block_overlaps(block, core->state.block_repeat_start[slot], core->state.block_repeat_end[slot]);
	if (block->ends_block_repeat || block->breaks_at_block_repeat_end || block->splits_block_repeat_end)
		entry->level_free = false;
	for (size_t i = 0; i < block->instruction_count && entry->level_free; i++) {
		const teak_insn_t *instruction = &block->instructions[i];
		if (tcg_is_block_repeat(instruction) || instruction->opcode == TEAK_OP_BREAK)
			entry->level_free = false;
	}
	entry->succ_epoch = 0;
	entry->mode = mode;
	entry->next = tcg_block_cache[block->pc];
	tcg_block_cache[block->pc] = entry;
	tcg_block_cache_generation++;
	tcg_block_cache_epoch++;
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

static void tcg_mark_borrowed_word(uint16_t address) {
	tcg_borrowed_word_addresses[address >> 3] |= BIT(address & 7U);
}

static bool tcg_block_borrows_word(const teak_tcg_block_t *block, uint16_t address, size_t words) {
	bool loop_start = block->splits_block_repeat_end && (uint16_t) (block->block_repeat_start - address) < words;
	bool return_address = block->fetch_split && (uint16_t) (block->fetch_split_target - address) < words;
	return loop_start || return_address;
}

static void tcg_invalidate_borrowed_word(teak_tcg_core_t *core, uint16_t address) {
	uint8_t bit = BIT(address & 7U);

	if ((tcg_borrowed_word_addresses[address >> 3] & bit) == 0)
		return;
	tcg_borrowed_word_addresses[address >> 3] &= ~bit;
	for (size_t i = 0; i < ARRAY_SIZE(tcg_block_cache); i++) {
		teak_tcg_block_cache_entry_t **link = &tcg_block_cache[i];
		while (*link != NULL) {
			teak_tcg_block_cache_entry_t *entry = *link;
			if (tcg_block_borrows_word(&entry->block, address, 1)) {
				tcg_remove_cached_entry(link);
				core->chain_prev = NULL;
			} else {
				link = &entry->next;
			}
		}
	}
}

void teak_tcg_invalidate_program(teak_tcg_core_t *core, uint32_t address) {
	uint16_t program_address = (uint16_t) address;

	tcg_fast_invalidate();
	tcg_check_block_cache();
	for (size_t distance = 0; distance < TEAK_TCG_MAX_BLOCK_INSTRUCTIONS * 2; distance++) {
		uint16_t start = program_address - distance;
		teak_tcg_block_cache_entry_t **link = &tcg_block_cache[start];
		while (*link != NULL) {
			teak_tcg_block_cache_entry_t *entry = *link;
			bool contains_address = tcg_block_contains_address(&entry->block, program_address);
			bool matching_entry = entry->cache_id == core->cache_id && contains_address;
			if (matching_entry) {
				tcg_remove_cached_entry(link);
				core->chain_prev = NULL;
			} else {
				link = &entry->next;
			}
		}
	}
	tcg_invalidate_borrowed_word(core, program_address);
}

void teak_tcg_invalidate_program_range(teak_tcg_core_t *core, uint32_t address, size_t words) {
	uint16_t start = (uint16_t) address;

	tcg_fast_invalidate();
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
			bool borrowed_word_written = tcg_block_borrows_word(&entry->block, start, words);

			if (entry->cache_id == core->cache_id && (intersects || borrowed_word_written)) {
				tcg_remove_cached_entry(link);
				core->chain_prev = NULL;
			} else {
				link = &entry->next;
			}
		}
	}
}

void teak_tcg_invalidate_all(teak_tcg_core_t *core) {
	tcg_fast_invalidate();
	tcg_check_block_cache();
	for (size_t i = 0; i < ARRAY_SIZE(tcg_block_cache); i++) {
		teak_tcg_block_cache_entry_t **link = &tcg_block_cache[i];
		while (*link != NULL) {
			teak_tcg_block_cache_entry_t *entry = *link;
			if (entry->cache_id == core->cache_id) {
				tcg_remove_cached_entry(link);
				core->chain_prev = NULL;
			} else {
				link = &entry->next;
			}
		}
	}
}

void teak_tcg_set_program_context(teak_tcg_core_t *core, uint32_t context) {
	core->program_context = context;
}

static bool tcg_alu_uses_multiply(teak_alu_operation_t operation) {
	return operation == TEAK_ALU_MSU || operation == TEAK_ALU_SQR ||
		operation == TEAK_ALU_SQRA;
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
		case TEAK_OP_MOV_STACK_REGISTER:
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
		case TEAK_OP_ALB_REGISTER:
			return instruction->register_code == 12 && tcg_alb_modifies_operand(instruction->alb_operation);

		default:
			return false;
	}
}

static bool tcg_can_repeat(const teak_insn_t *instruction) {
	if (instruction->words != 1)
		return false;
	return instruction->opcode == TEAK_OP_BREAK || !tcg_is_loop_control(instruction);
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
		case TEAK_OP_MULTIPLY_REGISTER:
		case TEAK_OP_MULTIPLY_RN_STEP:
		case TEAK_OP_MOVS_REGISTER:
		case TEAK_OP_MOVS_RN_STEP:
		case TEAK_OP_MOVS_DATA_IMM8:
		case TEAK_OP_MOVSI_REGISTER:
		case TEAK_OP_MOVR_REGISTER:
		case TEAK_OP_MOVR_RN_STEP:
		case TEAK_OP_MOVR_RN_HIGH:
		case TEAK_OP_MOVR_B_ACCUMULATOR:
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
		case TEAK_OP_MOV_DATA_RN_STEP_REGISTER:
		case TEAK_OP_MOV_REGISTER_DATA_RN_STEP:
		case TEAK_OP_MOV_DATA_RN_STEP_B_ACCUMULATOR:
		case TEAK_OP_MOV_REGISTER_REGISTER:
		case TEAK_OP_MOV_STACK_REGISTER:
		case TEAK_OP_MOV_DATA_R7_OFFSET7_ACCUMULATOR:
		case TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET7:
		case TEAK_OP_ALU_REGISTER_ACCUMULATOR:
		case TEAK_OP_ALU_RN_STEP_ACCUMULATOR:
		case TEAK_OP_ALU_R7_OFFSET7_ACCUMULATOR:
		case TEAK_OP_ALU_DATA_IMM8_ACCUMULATOR:
		case TEAK_OP_TEST_ACCUMULATOR_DATA_IMM8:
		case TEAK_OP_TSTB_IMM8:
		case TEAK_OP_TSTB_REGISTER:
		case TEAK_OP_TSTB_RN_STEP:
		case TEAK_OP_MODIFY_RN:
		case TEAK_OP_LOAD_MODI:
		case TEAK_OP_LOAD_MODJ:
		case TEAK_OP_LOAD_STEPI:
		case TEAK_OP_LOAD_STEPJ:
		case TEAK_OP_BREAK:
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

static TCGv_i32 tcg_emit_register_read(const teak_insn_t *instruction, uint8_t register_code) {
	TCGv_i32 value = tcg_temp_new_i32();

	if (register_code == 12) {
		tcg_gen_movi_i32(value, tcg_instruction_end(instruction));
	} else {
		tcg_emit_register_load(value, register_code);
	}
	return value;
}

static TCGv_i32 tcg_emit_mov_register_read(const teak_insn_t *instruction, uint8_t register_code) {
	TCGv_i32 value = tcg_temp_new_i32();

	if (register_code == 12) {
		tcg_gen_movi_i32(value, tcg_instruction_end(instruction));
	} else {
		tcg_emit_mov_register_load(value, register_code);
	}
	return value;
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

static void tcg_emit_push_register(const teak_insn_t *instruction) {
	tcg_emit_push_value(tcg_emit_mov_register_read(instruction, instruction->register_code));
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

	/* pop sp is not allowed (TeakLite 4.41, pop <2>); hardware only post-increments sp. */
	if (register_code == 13)
		return;
	bool accumulator = (register_code >= 16 && register_code <= 19) || (register_code >= 24 && register_code <= 29);
	if (accumulator) {
		tcg_emit_mov_register_write(register_code, value);
	} else {
		tcg_emit_register_write(register_code, value);
	}
}

static TCGv_i32 tcg_emit_stack_top_read(void) {
	TCGv_i32 sp = tcg_temp_new_i32();
	TCGv_i32 value = tcg_temp_new_i32();

	tcg_gen_ld16u_i32(sp, tcg_env, offsetof(teak_state_t, sp));
	gen_helper_teak_tcg_data_read(value, tcg_env, sp);
	return value;
}

static void tcg_emit_mov_stack_register(uint8_t register_code) {
	tcg_emit_mov_register_write(register_code, tcg_emit_stack_top_read());
}

static void tcg_emit_accumulator_value_flags(TCGv_i64 value);
static size_t tcg_accumulator_register_offset(uint8_t register_code);
static TCGv_i64 tcg_emit_shifted_product(void);
static void tcg_emit_accumulator_test(uint8_t accumulator_index, bool ones, TCGv_i32 operand);
static size_t tcg_ab_offset(uint8_t accumulator);

static void tcg_emit_mov_register_register(const teak_insn_t *instruction) {
	uint8_t source = instruction->register_code;
	uint8_t destination = instruction->destination_register_code;
	bool a_destination = destination >= 24 && destination <= 29;
	bool b_destination = destination >= 16 && destination <= 19;

	if (destination == 12)
		return;
	if ((source == 24 || source == 25) && (a_destination || b_destination)) {
		TCGv_i64 accumulator = tcg_temp_new_i64();

		tcg_gen_ld_i64(accumulator, tcg_env, tcg_accumulator_register_offset(source));
		tcg_emit_accumulator_value_flags(accumulator);
		tcg_gen_st_i64(accumulator, tcg_env, tcg_accumulator_register_offset(destination));
		return;
	}
	if (source == 11 && a_destination) {
		TCGv_i64 product = tcg_emit_shifted_product();

		tcg_emit_accumulator_value_flags(product);
		tcg_gen_st_i64(product, tcg_env, tcg_accumulator_register_offset(destination));
		return;
	}
	tcg_emit_mov_register_write(destination, tcg_emit_mov_register_read(instruction, source));
	/* mov p,bX: REG p reads a1[15:0] (see tcg_read_register); hardware sets the flags from the shifted p. */
	if (source == 11 && b_destination)
		tcg_emit_accumulator_value_flags(tcg_emit_shifted_product());
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
	if (instruction->register_code == 24 || instruction->register_code == 25) {
		TCGv_i64 accumulator = tcg_temp_new_i64();
		size_t source_offset = offsetof(teak_state_t, a) + (instruction->register_code - 24) * sizeof(uint64_t);
		size_t destination_offset = offsetof(teak_state_t, b) + instruction->accumulator_index * sizeof(uint64_t);

		tcg_gen_ld_i64(accumulator, tcg_env, source_offset);
		tcg_emit_accumulator_value_flags(accumulator);
		tcg_gen_st_i64(accumulator, tcg_env, destination_offset);
		return;
	}
	tcg_emit_mov_b_accumulator(instruction->accumulator_index,
		tcg_emit_mov_register_read(instruction, instruction->register_code));
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
	tcg_emit_multiply_operation(instruction, tcg_emit_mov_register_read(instruction, instruction->register_code));
}

static void tcg_emit_repeat(TCGv_i32 count) {
	tcg_gen_st16_i32(count, tcg_env, offsetof(teak_state_t, repc));
	tcg_gen_st8_i32(tcg_constant_i32(1), tcg_env, offsetof(teak_state_t, repeat_active));
}

static void tcg_emit_repeat_register(const teak_insn_t *instruction) {
	tcg_emit_repeat(tcg_emit_register_read(instruction, instruction->register_code));
}

static void tcg_emit_block_repeat(const teak_insn_t *instruction, TCGv_i32 count, uint8_t level) {
	uint8_t bcn = (level + 1) & TEAK_BLOCK_REPEAT_COUNTER_MASK;
	uint8_t slot = teak_block_repeat_slot(bcn);

	bool stores_addresses = bcn != 0 && bcn <= TEAK_BLOCK_REPEAT_LEVELS;
	if (stores_addresses) {
		tcg_gen_st_i32(tcg_constant_i32(tcg_instruction_end(instruction)), tcg_env,
			offsetof(teak_state_t, block_repeat_start) + slot * sizeof(uint32_t));
		tcg_gen_st_i32(tcg_constant_i32(instruction->branch_target), tcg_env,
			offsetof(teak_state_t, block_repeat_end) + slot * sizeof(uint32_t));
	}
	tcg_gen_st16_i32(count, tcg_env,
		offsetof(teak_state_t, block_repeat_lc) + slot * sizeof(uint16_t));
	tcg_gen_st8_i32(tcg_constant_i32(1), tcg_env, offsetof(teak_state_t, lp));
	tcg_gen_st8_i32(tcg_constant_i32(bcn), tcg_env, offsetof(teak_state_t, bcn));
}

static void tcg_emit_block_repeat_register(const teak_insn_t *instruction, uint8_t level) {
	tcg_emit_block_repeat(instruction, tcg_emit_register_read(instruction, instruction->register_code), level);
}

static void tcg_emit_break(void) {
	TCGv_i32 bcn = tcg_temp_new_i32();
	TCGv_i32 lp = tcg_temp_new_i32();
	TCGLabel *inactive = gen_new_label();

	tcg_gen_ld8u_i32(lp, tcg_env, offsetof(teak_state_t, lp));
	tcg_gen_brcondi_i32(TCG_COND_EQ, lp, 0, inactive);
	tcg_gen_ld8u_i32(bcn, tcg_env, offsetof(teak_state_t, bcn));
	tcg_gen_subi_i32(bcn, bcn, 1);
	tcg_gen_andi_i32(bcn, bcn, TEAK_BLOCK_REPEAT_COUNTER_MASK);
	tcg_gen_st8_i32(bcn, tcg_env, offsetof(teak_state_t, bcn));
	tcg_gen_setcondi_i32(TCG_COND_NE, lp, bcn, 0);
	tcg_gen_st8_i32(lp, tcg_env, offsetof(teak_state_t, lp));
	gen_set_label(inactive);
}

/*
 * break cannot be the last instruction (TeakLite 4.41, break <1>). Hardware first completes the loop iteration (loop
 * back, or pop when lc is 0) and then pops one more level for the break.
 */
static void tcg_emit_break_at_block_repeat_end(const teak_insn_t *instruction) {
	tcg_gen_st8_i32(tcg_constant_i32(1), tcg_env, offsetof(teak_state_t, block_repeat_break_pending));
	tcg_gen_st_i32(tcg_constant_i32(tcg_instruction_end(instruction)), tcg_env, offsetof(teak_state_t, pc));
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

static void tcg_emit_repeat_resume(TCGv_i32 return_pc) {
	TCGv_i32 repeat_suspended = tcg_temp_new_i32();
	TCGv_i32 suspended_repeat_pc = tcg_temp_new_i32();
	TCGv_i32 repc = tcg_temp_new_i32();
	TCGLabel *done = gen_new_label();

	tcg_gen_ld8u_i32(repeat_suspended, tcg_env, offsetof(teak_state_t, repeat_suspended));
	tcg_gen_brcondi_i32(TCG_COND_EQ, repeat_suspended, 0, done);
	tcg_gen_ld_i32(suspended_repeat_pc, tcg_env, offsetof(teak_state_t, suspended_repeat_pc));
	tcg_gen_brcond_i32(TCG_COND_NE, return_pc, suspended_repeat_pc, done);

	tcg_gen_ld16u_i32(repc, tcg_env, offsetof(teak_state_t, suspended_repc));
	tcg_gen_st16_i32(repc, tcg_env, offsetof(teak_state_t, repc));
	tcg_gen_st8_i32(tcg_constant_i32(1), tcg_env, offsetof(teak_state_t, repeat_active));
	tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, repeat_suspended));
	gen_set_label(done);
}

static void tcg_emit_interrupt_return_state(TCGv_i32 return_pc) {
	TCGv_i32 trap_active = tcg_temp_new_i32();
	TCGv_i32 nmi_active = tcg_temp_new_i32();
	TCGLabel *done = gen_new_label();

	tcg_emit_repeat_resume(return_pc);

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
			tcg_emit_interrupt_return_state(target_pc);
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
		tcg_emit_interrupt_return_state(target_pc);
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

/*
 * tcg_shift_bus() by the shift value register for |shift| < 40, without
 * branches: the left and right forms are both computed (each is the identity
 * with no carry or overflow at amount 0) and the sign picks one.  Longer
 * shifts take the helper.
 */
static void tcg_emit_shift_by_value(uint8_t source, uint8_t destination) {
	TCGLabel *slow = gen_new_label();
	TCGLabel *done = gen_new_label();
	TCGv_i32 shift32 = tcg_temp_new_i32();
	TCGv_i32 bound = tcg_temp_new_i32();
	TCGv_i64 zero = tcg_constant_i64(0);

	tcg_gen_ld16s_i32(shift32, tcg_env, offsetof(teak_state_t, shift_value));
	tcg_gen_addi_i32(bound, shift32, TEAK_ACCUMULATOR_BITS - 1);
	tcg_gen_brcondi_i32(TCG_COND_GTU, bound, 2 * (TEAK_ACCUMULATOR_BITS - 1), slow);

	TCGv_i64 shift = tcg_temp_new_i64();
	TCGv_i64 left = tcg_temp_new_i64();
	TCGv_i64 right = tcg_temp_new_i64();
	TCGv_i64 canonical = tcg_temp_new_i64();
	TCGv_i64 signed_value = tcg_temp_new_i64();
	TCGv_i64 left_result = tcg_temp_new_i64();
	TCGv_i64 right_result = tcg_temp_new_i64();
	TCGv_i64 arithmetic = tcg_temp_new_i64();
	TCGv_i64 carry64 = tcg_temp_new_i64();
	TCGv_i64 temporary64 = tcg_temp_new_i64();
	TCGv_i64 shift_mode64 = tcg_temp_new_i64();
	TCGv_i32 shift_mode = tcg_temp_new_i32();
	TCGv_i32 carry = tcg_temp_new_i32();
	TCGv_i32 overflow = tcg_temp_new_i32();
	TCGv_i32 actual_overflow = tcg_temp_new_i32();
	TCGv_i32 latched_overflow = tcg_temp_new_i32();
	TCGv_i32 zero32 = tcg_constant_i32(0);

	tcg_gen_ext_i32_i64(shift, shift32);
	tcg_gen_movcond_i64(TCG_COND_GT, left, shift, zero, shift, zero);
	tcg_gen_neg_i64(right, shift);
	tcg_gen_movcond_i64(TCG_COND_GT, right, right, zero, right, zero);
	tcg_gen_ld_i64(canonical, tcg_env, tcg_ab_offset(source));
	tcg_gen_andi_i64(canonical, canonical, TEAK_ACCUMULATOR_MASK);
	tcg_gen_shli_i64(signed_value, canonical, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(signed_value, signed_value, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_ld8u_i32(shift_mode, tcg_env, offsetof(teak_state_t, s));
	tcg_gen_extu_i32_i64(shift_mode64, shift_mode);

	tcg_gen_shl_i64(left_result, canonical, left);
	tcg_gen_andi_i64(left_result, left_result, TEAK_ACCUMULATOR_MASK);
	tcg_gen_subfi_i64(temporary64, TEAK_ACCUMULATOR_BITS, left);
	tcg_gen_shr_i64(carry64, canonical, temporary64);
	tcg_gen_shli_i64(temporary64, left_result, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(temporary64, temporary64, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sar_i64(temporary64, temporary64, left);
	tcg_gen_setcond_i64(TCG_COND_NE, temporary64, temporary64, signed_value);
	tcg_gen_extrl_i64_i32(overflow, temporary64);

	tcg_gen_shli_i64(temporary64, canonical, 1);
	tcg_gen_shr_i64(temporary64, temporary64, right);
	tcg_gen_or_i64(carry64, carry64, temporary64);
	tcg_gen_shr_i64(right_result, canonical, right);
	tcg_gen_sar_i64(arithmetic, signed_value, right);
	tcg_gen_andi_i64(arithmetic, arithmetic, TEAK_ACCUMULATOR_MASK);
	tcg_gen_movcond_i64(TCG_COND_EQ, right_result, shift_mode64, zero, arithmetic, right_result);
	tcg_gen_movcond_i64(TCG_COND_GT, left_result, shift, zero, left_result, right_result);

	tcg_gen_extrl_i64_i32(carry, carry64);
	tcg_gen_andi_i32(carry, carry, 1);
	tcg_gen_st8_i32(carry, tcg_env, offsetof(teak_state_t, fc0));
	tcg_gen_ld8u_i32(actual_overflow, tcg_env, offsetof(teak_state_t, fv));
	tcg_gen_movcond_i32(TCG_COND_EQ, actual_overflow, shift_mode, zero32, overflow, actual_overflow);
	tcg_gen_st8_i32(actual_overflow, tcg_env, offsetof(teak_state_t, fv));
	tcg_gen_movcond_i32(TCG_COND_EQ, overflow, shift_mode, zero32, overflow, zero32);
	tcg_gen_ld8u_i32(latched_overflow, tcg_env, offsetof(teak_state_t, fvl));
	tcg_gen_or_i32(latched_overflow, latched_overflow, overflow);
	tcg_gen_st8_i32(latched_overflow, tcg_env, offsetof(teak_state_t, fvl));

	tcg_gen_shli_i64(left_result, left_result, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(left_result, left_result, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_emit_accumulator_value_flags(left_result);
	tcg_gen_st_i64(left_result, tcg_env, tcg_ab_offset(destination));
	tcg_gen_br(done);

	gen_set_label(slow);
	gen_helper_teak_tcg_shift_accumulator(tcg_env, tcg_constant_i32(source), tcg_constant_i32(destination));
	tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
	gen_set_label(done);
}

static void tcg_emit_shift_conditional(const teak_insn_t *instruction) {
	TCGLabel *skip = NULL;

	if (instruction->condition != TEAK_COND_TRUE)
		skip = tcg_emit_condition_skip(instruction->condition);
	tcg_emit_shift_by_value(instruction->source_accumulator, instruction->destination_accumulator);
	if (skip != NULL)
		gen_set_label(skip);
}

static void tcg_emit_accumulator_value_flags(TCGv_i64 value) {
	TCGv_i64 flag64 = tcg_temp_new_i64();
	TCGv_i64 signed32 = tcg_temp_new_i64();
	TCGv_i32 zero = tcg_temp_new_i32();
	TCGv_i32 extension = tcg_temp_new_i32();
	TCGv_i32 bits = tcg_temp_new_i32();
	TCGv_i32 normalized = tcg_temp_new_i32();

	tcg_gen_setcondi_i64(TCG_COND_EQ, flag64, value, 0);
	tcg_gen_extrl_i64_i32(zero, flag64);
	tcg_gen_st8_i32(zero, tcg_env, offsetof(teak_state_t, fz));

	tcg_gen_shri_i64(flag64, value, TEAK_ACCUMULATOR_SIGN_BIT);
	tcg_gen_extrl_i64_i32(bits, flag64);
	tcg_gen_andi_i32(bits, bits, 1);
	tcg_gen_st8_i32(bits, tcg_env, offsetof(teak_state_t, fm));

	tcg_gen_ext32s_i64(signed32, value);
	tcg_gen_setcond_i64(TCG_COND_NE, flag64, value, signed32);
	tcg_gen_extrl_i64_i32(extension, flag64);
	tcg_gen_st8_i32(extension, tcg_env, offsetof(teak_state_t, fe));

	tcg_gen_shri_i64(flag64, value, 30);
	tcg_gen_extrl_i64_i32(bits, flag64);
	tcg_gen_andi_i32(bits, bits, 3);
	tcg_gen_shri_i32(normalized, bits, 1);
	tcg_gen_xor_i32(normalized, normalized, bits);
	tcg_gen_andi_i32(normalized, normalized, 1);
	tcg_gen_andc_i32(normalized, normalized, extension);
	tcg_gen_or_i32(normalized, normalized, zero);
	tcg_gen_st8_i32(normalized, tcg_env, offsetof(teak_state_t, fn));
}

static void tcg_emit_saturation(TCGv_i64 value, size_t disable_offset, bool block_disable) {
	if (tcg_block_mode.specialized && block_disable)
		return;

	TCGv_i64 signed32 = tcg_temp_new_i64();
	TCGv_i64 saturated = tcg_temp_new_i64();
	TCGv_i64 condition64 = tcg_temp_new_i64();
	TCGv_i64 zero = tcg_constant_i64(0);
	TCGv_i64 minimum = tcg_constant_i64(INT32_MIN);
	TCGv_i64 maximum = tcg_constant_i64(INT32_MAX);
	TCGv_i32 condition = tcg_temp_new_i32();
	TCGv_i32 flm = tcg_temp_new_i32();

	tcg_gen_ext32s_i64(signed32, value);
	tcg_gen_setcond_i64(TCG_COND_NE, condition64, value, signed32);
	tcg_gen_extrl_i64_i32(condition, condition64);
	if (!tcg_block_mode.specialized) {
		TCGv_i32 enabled = tcg_temp_new_i32();

		tcg_gen_ld8u_i32(enabled, tcg_env, disable_offset);
		tcg_gen_xori_i32(enabled, enabled, 1);
		tcg_gen_and_i32(condition, condition, enabled);
	}

	tcg_gen_movcond_i64(TCG_COND_LT, saturated, value, zero, minimum, maximum);
	tcg_gen_extu_i32_i64(condition64, condition);
	tcg_gen_movcond_i64(TCG_COND_NE, value, condition64, zero, saturated, value);

	tcg_gen_ld8u_i32(flm, tcg_env, offsetof(teak_state_t, flm));
	tcg_gen_or_i32(flm, flm, condition);
	tcg_gen_st8_i32(flm, tcg_env, offsetof(teak_state_t, flm));
}

static void tcg_emit_data_bus_saturation(TCGv_i64 value) {
	tcg_emit_saturation(value, offsetof(teak_state_t, sat), tcg_block_mode.sat);
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
	tcg_emit_saturation(value, offsetof(teak_state_t, sata), tcg_block_mode.sata);
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
	bool test_zero = instruction->alu_operation == TEAK_ALU_TST0;
	bool test_one = instruction->alu_operation == TEAK_ALU_TST1;
	if (test_zero || test_one) {
		tcg_emit_accumulator_test(instruction->accumulator_index, test_one, tcg_constant_i32(instruction->immediate));
		return;
	}

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
	/* moda 0111 and modb 111 are reserved (TeakLite 4.41, moda, modb); hardware writes an ALU latch left unmodeled. */
	if (instruction->moda_operation == TEAK_MODA_RESERVED)
		return;

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
			return tcg_emit_exponent_16(tcg_emit_register_read(instruction, instruction->register_code));

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
		gen_helper_teak_tcg_update_modulo_steps(tcg_env);
		tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
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
	size_t accumulator_offset = offsetof(teak_state_t, a) + instruction->accumulator_index * sizeof(uint64_t);
	size_t candidate_offset = offsetof(teak_state_t, a) + (instruction->accumulator_index ^ 1U) * sizeof(uint64_t);
	TCGv_i32 candidate32 = tcg_temp_new_i32();
	TCGv_i32 predicate = tcg_temp_new_i32();
	TCGv_i32 mixp = tcg_temp_new_i32();
	TCGv_i64 candidate = tcg_temp_new_i64();
	TCGv_i64 current = tcg_temp_new_i64();
	TCGv_i64 predicate64 = tcg_temp_new_i64();
	TCGCond condition;

	TCGv_i32 r0 = tcg_temp_new_i32();
	tcg_gen_ld16u_i32(r0, tcg_env, offsetof(teak_state_t, r[0]));
	TCGv_i32 pointer = tcg_emit_rn_address(instruction->address_register, instruction->step, false);
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
	tcg_gen_st8_i32(predicate, tcg_env, offsetof(teak_state_t, fm));
	tcg_gen_ld16u_i32(mixp, tcg_env, offsetof(teak_state_t, mixp));
	tcg_gen_movcond_i32(TCG_COND_NE, mixp, predicate, tcg_constant_i32(0), r0, mixp);
	tcg_gen_st16_i32(mixp, tcg_env, offsetof(teak_state_t, mixp));
}

static bool tcg_is_accumulator_register(uint8_t register_code) {
	bool b_register = register_code >= 16 && register_code <= 19;
	bool a_register = register_code >= 24 && register_code <= 29;
	return b_register || a_register;
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

static void tcg_emit_product_shift(TCGv_i64 result, TCGv_i64 product, uint8_t shift) {
	switch (shift) {
		case 0:
			tcg_gen_mov_i64(result, product);
			break;
		case 1:
			tcg_gen_sari_i64(result, product, 1);
			break;
		case 2:
			tcg_gen_shli_i64(result, product, 1);
			break;
		case 3:
			tcg_gen_shli_i64(result, product, 2);
			break;
		default:
			g_assert_not_reached();
	}
}

static TCGv_i64 tcg_emit_shifted_product(void) {
	TCGv_i32 extension = tcg_temp_new_i32();
	TCGv_i64 product = tcg_temp_new_i64();
	TCGv_i64 extension64 = tcg_temp_new_i64();
	TCGv_i64 result = tcg_temp_new_i64();

	tcg_gen_ld32u_i64(product, tcg_env, offsetof(teak_state_t, p[0]));
	tcg_gen_ld8u_i32(extension, tcg_env, offsetof(teak_state_t, product_extension[0]));
	tcg_gen_extu_i32_i64(extension64, extension);
	tcg_gen_muli_i64(extension64, extension64, 0xF00000000ULL);
	tcg_gen_or_i64(product, product, extension64);
	tcg_gen_shli_i64(product, product, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(product, product, TEAK_ACCUMULATOR_HOST_SHIFT);
	if (tcg_block_mode.specialized) {
		tcg_emit_product_shift(result, product, tcg_block_mode.product_shift);
		return result;
	}

	TCGv_i32 shift = tcg_temp_new_i32();
	TCGv_i32 nonzero = tcg_temp_new_i32();
	TCGv_i32 right = tcg_temp_new_i32();
	TCGv_i64 left64 = tcg_temp_new_i64();
	TCGv_i64 right64 = tcg_temp_new_i64();

	/* Product shift modes 0..3 are: none, >> 1, << 1, << 2. */
	tcg_gen_ld8u_i32(shift, tcg_env, offsetof(teak_state_t, product_shift));
	tcg_gen_setcondi_i32(TCG_COND_NE, nonzero, shift, 0);
	tcg_gen_setcondi_i32(TCG_COND_EQ, right, shift, 1);
	tcg_gen_sub_i32(shift, shift, nonzero);
	tcg_gen_extu_i32_i64(left64, shift);
	tcg_gen_extu_i32_i64(right64, right);
	tcg_gen_shl_i64(result, product, left64);
	tcg_gen_sar_i64(result, result, right64);
	return result;
}

static TCGv_i64 tcg_emit_aligned_product(void) {
	TCGv_i64 product = tcg_emit_shifted_product();
	tcg_gen_shri_i64(product, product, 16);
	tcg_gen_shli_i64(product, product, 40);
	tcg_gen_sari_i64(product, product, 40);
	return product;
}

/*
 * REG cannot be aX in mac, msu or sqra (TeakLite 4.41, mac <3>, msu <3>, sqra <2>); hardware then adds or subtracts
 * the whole aX instead of the product.
 */
static TCGv_i64 tcg_emit_multiply_addend(const teak_insn_t *instruction, bool aligned) {
	bool register_operand = instruction->opcode == TEAK_OP_MULTIPLY_REGISTER ||
		instruction->opcode == TEAK_OP_ALU_REGISTER_ACCUMULATOR;
	bool accumulator_register = instruction->register_code == 24 || instruction->register_code == 25;
	bool accumulator_operand = register_operand && accumulator_register;

	if (accumulator_operand) {
		TCGv_i64 accumulator = tcg_temp_new_i64();

		tcg_gen_ld_i64(accumulator, tcg_env,
			offsetof(teak_state_t, a) + (instruction->register_code & 1U) * sizeof(uint64_t));
		return accumulator;
	}
	return aligned ? tcg_emit_aligned_product() : tcg_emit_shifted_product();
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
			tcg_emit_alu_accumulator(&accumulator, tcg_emit_multiply_addend(instruction, false));
			unsigned_x = false;
			unsigned_y = false;
			break;

		case TEAK_MULTIPLY_MACUS:
			accumulator.alu_operation = TEAK_ALU_ADD;
			tcg_emit_alu_accumulator(&accumulator, tcg_emit_multiply_addend(instruction, false));
			unsigned_x = false;
			unsigned_y = true;
			break;

		case TEAK_MULTIPLY_MACSU:
			accumulator.alu_operation = TEAK_ALU_ADD;
			tcg_emit_alu_accumulator(&accumulator, tcg_emit_multiply_addend(instruction, false));
			unsigned_x = true;
			unsigned_y = false;
			break;

		case TEAK_MULTIPLY_MAA:
			accumulator.alu_operation = TEAK_ALU_ADD;
			tcg_emit_alu_accumulator(&accumulator, tcg_emit_multiply_addend(instruction, true));
			unsigned_x = false;
			unsigned_y = false;
			break;

		case TEAK_MULTIPLY_MACUU:
			accumulator.alu_operation = TEAK_ALU_ADD;
			tcg_emit_alu_accumulator(&accumulator, tcg_emit_multiply_addend(instruction, false));
			unsigned_x = true;
			unsigned_y = true;
			break;

		case TEAK_MULTIPLY_MAASU:
			accumulator.alu_operation = TEAK_ALU_ADD;
			tcg_emit_alu_accumulator(&accumulator, tcg_emit_multiply_addend(instruction, true));
			unsigned_x = true;
			unsigned_y = false;
			break;

		case TEAK_MULTIPLY_MSU:
			accumulator.alu_operation = TEAK_ALU_SUB;
			tcg_emit_alu_accumulator(&accumulator, tcg_emit_multiply_addend(instruction, false));
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

/*
 * tst0/tst1 forbid p (TeakLite 4.41, tst0, tst1). Hardware applies 4.2.2 e and f: p is the shifted p
 * sign-extended to 36 bits, and the 16-bit aXl mask is sign-extended to 36 bits.
 */
static void tcg_emit_accumulator_product_test(uint8_t accumulator_index, bool ones) {
	size_t offset = offsetof(teak_state_t, a) + accumulator_index * sizeof(uint64_t);
	TCGv_i64 product = tcg_emit_shifted_product();
	TCGv_i64 mask = tcg_temp_new_i64();
	TCGv_i64 predicate = tcg_temp_new_i64();

	tcg_gen_ld_i64(mask, tcg_env, offset);
	tcg_gen_ext16s_i64(mask, mask);
	tcg_gen_andi_i64(mask, mask, TEAK_ACCUMULATOR_MASK);
	tcg_gen_and_i64(product, product, mask);
	if (ones) {
		tcg_gen_setcond_i64(TCG_COND_EQ, predicate, product, mask);
	} else {
		tcg_gen_setcondi_i64(TCG_COND_EQ, predicate, product, 0);
	}
	tcg_gen_st8_i64(predicate, tcg_env, offsetof(teak_state_t, fz));
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
		if (instruction->register_code == 11) {
			tcg_emit_accumulator_product_test(instruction->accumulator_index, test_one);
			return;
		}
		operand32 = tcg_emit_mov_register_read(instruction, instruction->register_code);
		tcg_emit_accumulator_test(instruction->accumulator_index, test_one, operand32);
		return;
	}

	if (multiply) {
		operand32 = tcg_emit_mov_register_read(instruction, instruction->register_code);
		tcg_emit_alu_multiply_operation(instruction, operand32);
		return;
	}

	if (instruction->register_code == 11) {
		operand = tcg_emit_shifted_product();
		tcg_emit_alu_accumulator(instruction, operand);
		return;
	}
	operand = tcg_temp_new_i64();
	if (instruction->register_code >= 24 && instruction->register_code <= 25) {
		uint8_t accumulator_index = instruction->register_code - 24;
		size_t accumulator_offset = offsetof(teak_state_t, a) + accumulator_index * sizeof(uint64_t);

		tcg_gen_ld_i64(operand, tcg_env, accumulator_offset);
		tcg_gen_andi_i64(operand, operand, TEAK_ACCUMULATOR_MASK);
		tcg_emit_alu_accumulator(instruction, operand);
		return;
	}
	operand32 = tcg_emit_mov_register_read(instruction, instruction->register_code);
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

static void tcg_emit_modulo_address(TCGv_i32 updated, TCGv_i32 address, TCGv_i32 step, uint8_t address_register,
	teak_step_t step_mode
) {
	size_t entry_offset = offsetof(teak_state_t, modulo_steps) +
		(address_register * TEAK_MODULO_STEP_MODES + step_mode - TEAK_STEP_INCREASE) * sizeof(teak_modulo_step_t);
	TCGv_i32 mask = tcg_temp_new_i32();
	TCGv_i32 boundary = tcg_temp_new_i32();
	TCGv_i32 wrap_target = tcg_temp_new_i32();
	TCGv_i32 low = tcg_temp_new_i32();

	tcg_gen_ld_i32(mask, tcg_env, entry_offset + offsetof(teak_modulo_step_t, mask));
	tcg_gen_ld_i32(boundary, tcg_env, entry_offset + offsetof(teak_modulo_step_t, boundary));
	tcg_gen_ld_i32(wrap_target, tcg_env, entry_offset + offsetof(teak_modulo_step_t, wrap_target));

	tcg_gen_and_i32(low, address, mask);
	tcg_gen_add_i32(updated, low, step);
	tcg_gen_and_i32(updated, updated, mask);
	tcg_gen_movcond_i32(TCG_COND_EQ, updated, low, boundary, wrap_target, updated);
	tcg_gen_andc_i32(low, address, mask);
	tcg_gen_or_i32(updated, updated, low);
}

static bool tcg_may_use_modulo(uint8_t address_register) {
	if (address_register >= TEAK_MODULO_REGISTERS)
		return false;
	return !tcg_block_mode.specialized || (tcg_block_mode.modulo_enable & BIT(address_register)) != 0;
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

	if (!disable_modulo && tcg_may_use_modulo(address_register)) {
		tcg_emit_modulo_address(updated, address, step, address_register, step_mode);
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
	TCGv_i32 value;

	/* tstb p: hardware tests the shifted p (TeakLite 4.41, 4.2.2 e), like the other p source operands. */
	if (instruction->register_code == 11) {
		value = tcg_temp_new_i32();
		tcg_gen_extrl_i64_i32(value, tcg_emit_shifted_product());
	} else if (instruction->register_code == 24) {
		value = tcg_temp_new_i32();
		tcg_gen_ld16u_i32(value, tcg_env, offsetof(teak_state_t, r) + 6 * sizeof(uint16_t));
	} else {
		value = tcg_emit_register_read(instruction, instruction->register_code);
	}
	tcg_gen_shri_i32(value, value, instruction->bit_index);
	tcg_gen_andi_i32(value, value, 1);
	tcg_gen_st8_i32(value, tcg_env, offsetof(teak_state_t, fz));
}

static void tcg_emit_mov_data_imm8_register(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_imm8_data_address(instruction->memory_address);
	TCGv_i32 value = tcg_temp_new_i32();

	gen_helper_teak_tcg_data_read(value, tcg_env, address);
	tcg_emit_mov_register_write(instruction->register_code, value);
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
	TCGv_i32 value = tcg_emit_mov_register_read(instruction, instruction->register_code);

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

	tcg_emit_mov_register_load(value, 26 + instruction->accumulator_index);
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
	if (instruction->register_code == 31) {
		tcg_gen_st16_i32(value, tcg_env, offsetof(teak_state_t, shift_value));
		return;
	}
	if (!tcg_is_accumulator_register(instruction->register_code)) {
		tcg_emit_mov_register_write(instruction->register_code, value);
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
	bool full_accumulator = instruction->register_code == 24 || instruction->register_code == 25;
	bool accumulator_half = tcg_is_accumulator_register(instruction->register_code) && !full_accumulator;
	TCGv_i32 value;

	if (instruction->register_code <= 7) {
		value = tcg_temp_new_i32();
		tcg_gen_ld16u_i32(value, tcg_env, tcg_rn_old_offset(instruction->register_code));
	} else if (!accumulator_half) {
		value = tcg_emit_mov_register_read(instruction, instruction->register_code);
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
	TCGv_i32 address = tcg_emit_rn_address(instruction->address_register, instruction->step,
		instruction->disable_modulo);
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
	gen_helper_teak_tcg_update_modulo_steps(tcg_env);
	tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
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
	tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
	return value;
}

static void tcg_emit_movp_accumulator_low_register(const teak_insn_t *instruction) {
	TCGv_i32 value;

	if (instruction->destination_register_code == 12)
		return;
	value = tcg_emit_movp_accumulator_low_read(instruction);
	tcg_emit_mov_register_write(instruction->destination_register_code, value);
}

static void tcg_emit_movp_rn_rn(const teak_insn_t *instruction) {
	TCGv_i32 source_address = tcg_emit_rn_address(instruction->address_register, instruction->step, false);
	TCGv_i32 destination_address = tcg_emit_rn_address(instruction->destination_register_code,
		instruction->y_step, false);
	TCGv_i32 value = tcg_temp_new_i32();

	gen_helper_teak_tcg_program_read(value, tcg_env, source_address);
	tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
	gen_helper_teak_tcg_data_write(tcg_env, destination_address, value);
}

static void tcg_emit_movd_rn_rn(const teak_insn_t *instruction) {
	TCGv_i32 source_address = tcg_emit_rn_address(instruction->address_register, instruction->step, false);
	TCGv_i32 destination_address = tcg_emit_rn_address(instruction->destination_register_code,
		instruction->y_step, false);
	TCGv_i32 value = tcg_temp_new_i32();

	gen_helper_teak_tcg_data_read(value, tcg_env, source_address);
	gen_helper_teak_tcg_program_write(tcg_env, destination_address, value);
	tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
}

static void tcg_emit_movs_result(const teak_insn_t *instruction, TCGv_i32 value, TCGv_i32 shift) {
	gen_helper_teak_tcg_shift_value(tcg_env, value,
		tcg_constant_i32(instruction->destination_accumulator), shift);
	tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
}

static void tcg_emit_movs_register(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_emit_register_read(instruction, instruction->register_code);
	TCGv_i32 shift = tcg_temp_new_i32();

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

	tcg_emit_movr_16(instruction, tcg_emit_register_read(instruction, instruction->register_code));
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

static void tcg_emit_alb_memory(const teak_insn_t *instruction, TCGv_i32 address) {
	TCGv_i32 mask = tcg_constant_i32(instruction->expansion);
	TCGv_i32 operation = tcg_constant_i32(instruction->alb_operation);

	gen_helper_teak_tcg_alb_memory(tcg_env, address, mask, operation, tcg_memory_pc,
		tcg_memory_cycle, tcg_constant_i32(tcg_memory_access++));
	tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
}

static void tcg_emit_alb_data_imm8(const teak_insn_t *instruction) {
	tcg_emit_alb_memory(instruction, tcg_emit_imm8_data_address(instruction->memory_address));
}

static void tcg_emit_alb_rn_step(const teak_insn_t *instruction) {
	TCGv_i32 address = tcg_emit_rn_address(instruction->address_register, instruction->step, false);

	tcg_emit_alb_memory(instruction, address);
}

/* tcg_alb_result() on a plain register or an accumulator half, whose writes have no side effects. */
static bool tcg_emit_inline_alb_register(const teak_insn_t *instruction) {
	uint8_t register_code = instruction->register_code;
	teak_alb_operation_t operation = instruction->alb_operation;
	uint16_t mask = instruction->expansion;
	ptrdiff_t offset = tcg_plain_register_offset(register_code);
	bool accumulator_half = (register_code >= 16 && register_code <= 19) ||
		(register_code >= 26 && register_code <= 29);

	if (offset < 0 && !accumulator_half)
		return false;

	TCGv_i32 value = tcg_temp_new_i32();
	TCGv_i32 result = tcg_temp_new_i32();
	TCGv_i32 flag = tcg_temp_new_i32();

	if (offset >= 0)
		tcg_gen_ld16u_i32(value, tcg_env, offset);
	else
		tcg_emit_inline_register_read(value, register_code, false);

	switch (operation) {
		case TEAK_ALB_SET:
			tcg_gen_ori_i32(result, value, mask);
			break;
		case TEAK_ALB_RST:
			tcg_gen_andi_i32(result, value, (uint16_t) ~mask);
			break;
		case TEAK_ALB_CHNG:
			tcg_gen_xori_i32(result, value, mask);
			break;
		case TEAK_ALB_ADDV:
			tcg_gen_addi_i32(result, value, mask);
			tcg_gen_shri_i32(flag, result, 16);
			tcg_gen_st8_i32(flag, tcg_env, offsetof(teak_state_t, fc0));
			tcg_gen_ext16u_i32(result, result);
			break;
		case TEAK_ALB_TST0:
		case TEAK_ALB_TST1:
			tcg_gen_andi_i32(flag, value, mask);
			tcg_gen_setcondi_i32(TCG_COND_EQ, flag, flag, operation == TEAK_ALB_TST0 ? 0 : mask);
			tcg_gen_st8_i32(flag, tcg_env, offsetof(teak_state_t, fz));
			return true;
		case TEAK_ALB_CMPV:
		case TEAK_ALB_SUBV:
			tcg_gen_setcondi_i32(TCG_COND_LTU, flag, value, mask);
			tcg_gen_st8_i32(flag, tcg_env, offsetof(teak_state_t, fc0));
			tcg_gen_subi_i32(result, value, mask);
			tcg_gen_ext16u_i32(result, result);
			break;
		default:
			g_assert_not_reached();
	}
	tcg_gen_setcondi_i32(TCG_COND_EQ, flag, result, 0);
	tcg_gen_st8_i32(flag, tcg_env, offsetof(teak_state_t, fz));
	tcg_gen_shri_i32(flag, result, 15);
	tcg_gen_st8_i32(flag, tcg_env, offsetof(teak_state_t, fm));
	if (!tcg_alb_modifies_operand(operation))
		return true;

	if (offset >= 0) {
		tcg_gen_st16_i32(result, tcg_env, offset);
		return true;
	}

	/* tcg_write_accumulator_half() */
	bool high = register_code < 18 || register_code >= 28;
	ptrdiff_t accumulator_offset = (register_code < 20 ? offsetof(teak_state_t, b) : offsetof(teak_state_t, a)) +
		(register_code & 1U) * sizeof(uint64_t);
	TCGv_i64 accumulator = tcg_temp_new_i64();
	TCGv_i64 half = tcg_temp_new_i64();

	tcg_gen_ld_i64(accumulator, tcg_env, accumulator_offset);
	tcg_gen_extu_i32_i64(half, result);
	if (high) {
		tcg_gen_andi_i64(accumulator, accumulator, ~0xFFFF0000ULL);
		tcg_gen_shli_i64(half, half, 16);
	} else {
		tcg_gen_andi_i64(accumulator, accumulator, ~0xFFFFULL);
	}
	tcg_gen_or_i64(accumulator, accumulator, half);
	tcg_gen_shli_i64(accumulator, accumulator, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_sari_i64(accumulator, accumulator, TEAK_ACCUMULATOR_HOST_SHIFT);
	tcg_gen_st_i64(accumulator, tcg_env, accumulator_offset);
	return true;
}

/* set, rst, chng, addv and subv cannot use pc (TeakLite 4.41, set <1>); hardware continues at the result. */
static void tcg_emit_alb_pc(const teak_insn_t *instruction) {
	uint16_t next_pc = tcg_instruction_end(instruction);
	TCGv_i32 result = tcg_temp_new_i32();

	gen_helper_teak_tcg_alb_value(result, tcg_env, tcg_constant_i32(next_pc),
		tcg_constant_i32(instruction->expansion), tcg_constant_i32(instruction->alb_operation));
	tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
	if (!tcg_alb_modifies_operand(instruction->alb_operation))
		return;
	tcg_gen_st_i32(result, tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_st_i32(tcg_constant_i32(TEAK_EXIT_BRANCH), tcg_env, offsetof(teak_state_t, exit_reason));
}

static void tcg_emit_alb_register(const teak_insn_t *instruction) {
	if (tcg_emit_inline_alb_register(instruction))
		return;

	TCGv_i32 register_code = tcg_constant_i32(instruction->register_code);
	TCGv_i32 mask = tcg_constant_i32(instruction->expansion);
	TCGv_i32 operation = tcg_constant_i32(instruction->alb_operation);

	gen_helper_teak_tcg_alb_register(tcg_env, register_code, mask, operation);
	tcg_mark_call(tcg_register_write_effects(instruction->register_code));
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

		case 11:
			gen_helper_teak_tcg_register_write(tcg_env, tcg_constant_i32(instruction->register_code), value);
			tcg_mark_call(tcg_register_write_effects(instruction->register_code));
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
			gen_helper_teak_tcg_update_modulo_steps(tcg_env);
			tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
			break;

		case 15:
			tcg_gen_st8_i32(tcg_constant_i32(instruction->expansion & 0x7FU), tcg_env,
				offsetof(teak_state_t, stepj));
			tcg_gen_st16_i32(tcg_constant_i32(instruction->expansion >> 7), tcg_env,
				offsetof(teak_state_t, modj));
			gen_helper_teak_tcg_update_modulo_steps(tcg_env);
			tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
			break;

		case 16:
		case 17:
		case 18:
		case 19:
			tcg_emit_mov_register_write(instruction->register_code, value);
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
			tcg_mark_call(tcg_register_write_effects(instruction->register_code));
			break;

		case 31:
			tcg_gen_st16_i32(value, tcg_env, offsetof(teak_state_t, shift_value));
			break;

		default:
			g_assert_not_reached();
	}
}

static void tcg_emit_mov_short_register(const teak_insn_t *instruction) {
	tcg_emit_mov_register_write(instruction->register_code, tcg_constant_i32(instruction->immediate));
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

	tcg_emit_mov_register_load(value, accumulator_low_registers[instruction->source_accumulator]);
	gen_helper_teak_tcg_special_register_write(tcg_env,
		tcg_constant_i32(instruction->special_register), value);
	tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
}

static void tcg_emit_mov_special_accumulator(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_temp_new_i32();
	TCGv_i64 accumulator = tcg_temp_new_i64();

	gen_helper_teak_tcg_special_register_read(value, tcg_env, tcg_constant_i32(instruction->special_register));
	tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
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
	tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
	tcg_emit_mov_register_write(instruction->destination_register_code, value);
}

static void tcg_emit_mov_register_special(const teak_insn_t *instruction) {
	TCGv_i32 value = tcg_emit_mov_register_read(instruction, instruction->register_code);

	gen_helper_teak_tcg_special_register_write(tcg_env,
		tcg_constant_i32(instruction->special_register), value);
	tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
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
			tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
			break;
		case TEAK_OP_CONTEXT_RESTORE:
			gen_helper_teak_tcg_context_switch(tcg_env, tcg_constant_i32(1));
			break;
		case TEAK_OP_LOAD_PAGE:
			tcg_gen_st8_i32(tcg_constant_i32(instruction->immediate), tcg_env, offsetof(teak_state_t, page));
			break;
		case TEAK_OP_LOAD_STEPI:
			tcg_gen_st8_i32(tcg_constant_i32(instruction->immediate), tcg_env, offsetof(teak_state_t, stepi));
			gen_helper_teak_tcg_update_modulo_steps(tcg_env);
			tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
			break;
		case TEAK_OP_LOAD_STEPJ:
			tcg_gen_st8_i32(tcg_constant_i32(instruction->immediate), tcg_env, offsetof(teak_state_t, stepj));
			gen_helper_teak_tcg_update_modulo_steps(tcg_env);
			tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
			break;
		case TEAK_OP_LOAD_MODI:
			tcg_gen_st16_i32(tcg_constant_i32(instruction->immediate), tcg_env, offsetof(teak_state_t, modi));
			gen_helper_teak_tcg_update_modulo_steps(tcg_env);
			tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
			break;
		case TEAK_OP_LOAD_MODJ:
			tcg_gen_st16_i32(tcg_constant_i32(instruction->immediate), tcg_env, offsetof(teak_state_t, modj));
			gen_helper_teak_tcg_update_modulo_steps(tcg_env);
			tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
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
		case TEAK_OP_ALB_DATA_IMM8:
			tcg_emit_alb_data_imm8(instruction);
			break;
		case TEAK_OP_ALB_RN_STEP:
			tcg_emit_alb_rn_step(instruction);
			break;
		case TEAK_OP_ALB_REGISTER:
			if (instruction->register_code != 12) {
				tcg_emit_alb_register(instruction);
				break;
			}
			tcg_emit_alb_pc(instruction);
			if (tcg_alb_modifies_operand(instruction->alb_operation))
				return;
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
			tcg_emit_push_register(instruction);
			break;
		case TEAK_OP_POP_REGISTER:
			if (instruction->register_code != 12)
				tcg_emit_pop_register(instruction->register_code);
			break;
		case TEAK_OP_REPEAT_IMMEDIATE:
			tcg_emit_repeat(tcg_constant_i32(instruction->immediate));
			break;
		case TEAK_OP_REPEAT_REGISTER:
			tcg_emit_repeat_register(instruction);
			break;
		case TEAK_OP_BLOCK_REPEAT_IMMEDIATE:
			tcg_emit_block_repeat(instruction, tcg_constant_i32(instruction->immediate), block_repeat_level);
			break;
		case TEAK_OP_BLOCK_REPEAT_REGISTER:
			tcg_emit_block_repeat_register(instruction, block_repeat_level);
			break;
		case TEAK_OP_BREAK:
			tcg_emit_break();
			break;
		case TEAK_OP_MOV_STACK_REGISTER:
			if (instruction->register_code != 12)
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
	if (instruction->opcode != TEAK_OP_BLOCK_REPEAT_REGISTER)
		return true;

	bool nested_lc = core->state.lp && instruction->register_code == 30;
	return !nested_lc;
}

static uint32_t tcg_active_block_repeat_end(const teak_state_t *state) {
	return state->block_repeat_end[teak_block_repeat_slot(state->bcn)];
}

static bool tcg_active_block_repeat_instruction_valid(const teak_tcg_core_t *core, const teak_insn_t *instruction) {
	uint8_t slot = teak_block_repeat_slot(core->state.bcn);
	uint32_t active_start = core->state.block_repeat_start[slot];
	uint32_t active_end = core->state.block_repeat_end[slot];

	if (instruction->address < active_start || instruction->address > active_end)
		return true;

	if (tcg_is_repeat(instruction) && instruction->address + 1 >= active_end)
		return false;
	if (tcg_is_block_repeat(instruction) && instruction->address + 1 >= active_end)
		return false;
	return true;
}

static bool tcg_is_interrupt_status_write(const teak_insn_t *instruction) {
	uint8_t register_code;

	switch (instruction->opcode) {
		case TEAK_OP_EINT:
			return true;
		case TEAK_OP_MOV_REGISTER_REGISTER:
		case TEAK_OP_MOVP_ACCUMULATOR_LOW_REGISTER:
		case TEAK_OP_MOV_MIXP_REGISTER:
			register_code = instruction->destination_register_code;
			break;
		case TEAK_OP_MOV_IMM_REGISTER:
		case TEAK_OP_MOV_DATA_IMM8_REGISTER:
		case TEAK_OP_MOV_DATA_RN_STEP_REGISTER:
		case TEAK_OP_MOV_STACK_REGISTER:
		case TEAK_OP_POP_REGISTER:
			register_code = instruction->register_code;
			break;
		case TEAK_OP_ALB_REGISTER:
			if (!tcg_alb_modifies_operand(instruction->alb_operation))
				return false;
			register_code = instruction->register_code;
			break;
		default:
			return false;
	}
	return register_code == 8 || register_code == 10;
}

/* Interrupts are sampled this many instructions after eint or an st0/st2 write (TeakLite 4.41, Table 5B). */
static uint8_t tcg_interrupt_status_delay(const teak_insn_t *instruction) {
	switch (instruction->opcode) {
		case TEAK_OP_EINT:
			return 0;
		case TEAK_OP_MOV_IMM_REGISTER:
			return 1;
		default:
			return 2;
	}
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
	if (block->fetch_split || block->ends_block_repeat || block->splits_block_repeat_end ||
		block->splits_delayed_return_slot)
		return false;

	instruction = &block->instructions[block->instruction_count - 1];
	if (instruction->opcode != TEAK_OP_BRANCH_ABSOLUTE && instruction->opcode != TEAK_OP_BRANCH_RELATIVE)
		return false;
	return instruction->branch_target == block->pc;
}

static bool tcg_block_has_dynamic_cycles(const teak_tcg_block_t *block) {
	return block->repeat_resume || tcg_is_repeat(&block->instructions[0]);
}

/*
 * The last block repeat address must be the second word of a two-word instruction (TeakLite 4.41, bkrep <3>). When
 * it is the first word, hardware still loops and fetches the second word from the loop start.
 */
static bool tcg_is_split_block_repeat_end(const teak_tcg_core_t *core, const teak_insn_t *instruction) {
	if (!core->state.lp || instruction->words != 2)
		return false;
	return instruction->address == tcg_active_block_repeat_end(&core->state);
}

static void tcg_decode_block_repeat_split(
	teak_tcg_core_t *core,
	teak_tcg_block_t *block,
	const teak_insn_t *instruction
) {
	uint8_t slot = teak_block_repeat_slot(core->state.bcn);
	uint16_t start = core->state.block_repeat_start[slot];

	bool decoded = teak_decode_with_expansion_address(core, instruction->address, start,
		&block->block_repeat_loop_instruction);

	g_assert(decoded);
	block->splits_block_repeat_end = true;
	block->block_repeat_slot = slot;
	block->block_repeat_start = start;
	tcg_mark_borrowed_word(start);
}

static bool tcg_decode_fetch_split(teak_tcg_core_t *core, teak_tcg_block_t *block) {
	uint16_t target = core->state.fetch_split_target;
	teak_insn_t *instruction = &block->instructions[0];

	if (!teak_decode_with_expansion_address(core, block->pc, target, instruction)) {
		core->translation_error_address = block->pc;
		core->translation_error = TEAK_TRANSLATION_ERROR_DECODE;
		return false;
	}
	block->instruction_count = 1;
	block->words = instruction->words;
	block->fetch_split = true;
	block->fetch_split_target = target;
	tcg_mark_borrowed_word(target);
	return true;
}

static bool tcg_decode_block(teak_tcg_core_t *core, teak_tcg_block_t *block, size_t max_instructions) {
	uint16_t address;
	uint8_t delayed_transfer_cycles = 0;
	uint8_t interrupt_sample_delay = 0;
	bool repeat_pending = core->state.repeat_active;
	bool movd_next_fetched = false;

	memset(block, 0, sizeof(*block));
	memset(block->block_repeat_setup_level, UINT8_MAX, sizeof(block->block_repeat_setup_level));
	block->pc = core->state.pc;
	block->repeat_resume = repeat_pending;
	if (core->state.fetch_split_pending)
		return tcg_decode_fetch_split(core, block);
	address = block->pc;

	while (block->instruction_count < ARRAY_SIZE(block->instructions)) {
		uint16_t instruction_index = block->instruction_count;
		teak_insn_t *instruction = &block->instructions[instruction_index];
		uint32_t instruction_end;
		uint8_t transfer_cycles;
		uint8_t delay_slot_cycles;
		bool limit_reached = instruction_index >= max_instructions && !repeat_pending &&
			delayed_transfer_cycles == 0 && !movd_next_fetched;
		if (limit_reached)
			break;

		if (!teak_decode(core, address, instruction)) {
			core->translation_error_address = address;
			core->translation_error = TEAK_TRANSLATION_ERROR_DECODE;
			break;
		}
		if (movd_next_fetched && tcg_is_loop_control(instruction))
			break;
		bool invalid_repeat_body = core->state.lp && !tcg_active_block_repeat_instruction_valid(core, instruction);
		if (invalid_repeat_body) {
			core->translation_error_address = instruction->address;
			core->translation_error = TEAK_TRANSLATION_ERROR_BLOCK_REPEAT_BODY;
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
		bool delayed_return = delayed_transfer_cycles != 0 && tcg_is_delayed_return(&block->instructions[0]);
		bool slot_overrun = delayed_transfer_cycles != 0 && delay_slot_cycles > delayed_transfer_cycles &&
			block->instruction_count > 1;
		if (slot_overrun && !delayed_return)
			return true;
		/*
		 * The slots after retd and retid are two cycles (TeakLite 4.41, retd, retid). Hardware runs every instruction
		 * that starts in them; a two-word one starting in the last cycle takes its second word from the return
		 * address and continues one word later (dsp-lle-delay-slot-overflow).
		 */
		if (slot_overrun && instruction->words == 2) {
			block->splits_delayed_return_slot = true;
			return true;
		}
		if (delayed_transfer_cycles != 0 && delay_slot_cycles == 0) {
			core->translation_error_address = instruction->address;
			core->translation_error = TEAK_TRANSLATION_ERROR_DELAY_SLOT;
			return false;
		}
		if (delayed_transfer_cycles != 0 && delay_slot_cycles > delayed_transfer_cycles && !delayed_return) {
			core->translation_error_address = instruction->address;
			core->translation_error = TEAK_TRANSLATION_ERROR_DELAY_SLOT;
			return false;
		}

		block->instruction_count++;
		block->words += instruction->words;
		address += instruction->words;
		instruction_end = instruction->address + instruction->words - 1;

		if (delayed_transfer_cycles != 0) {
			delayed_transfer_cycles -= MIN(delay_slot_cycles, delayed_transfer_cycles);
			if (delayed_transfer_cycles == 0)
				return true;
			continue;
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
		if (tcg_is_split_block_repeat_end(core, instruction)) {
			tcg_decode_block_repeat_split(core, block, instruction);
			return true;
		}
		bool interrupt_sample_reached = false;
		if (interrupt_sample_delay != 0) {
			interrupt_sample_delay--;
			interrupt_sample_reached = interrupt_sample_delay == 0;
		}
		if (tcg_is_interrupt_status_write(instruction)) {
			interrupt_sample_delay = tcg_interrupt_status_delay(instruction);
			interrupt_sample_reached = interrupt_sample_delay == 0;
		}
		if (interrupt_sample_reached)
			return true;
		if (tcg_is_block_repeat(instruction)) {
			block->block_repeat_setup_level[instruction_index] = core->state.bcn;
			return true;
		}
		if (instruction->opcode == TEAK_OP_BREAK)
			return true;
		if (core->state.lp && instruction_end == tcg_active_block_repeat_end(&core->state)) {
			uint8_t bcn = core->state.bcn;
			bool whole_body = bcn != 0 && bcn <= TEAK_BLOCK_REPEAT_LEVELS && !movd_next_fetched &&
				block->pc == core->state.block_repeat_start[bcn - 1] && block->pc != instruction_end;
			if (whole_body)
				block->repeat_body_level = bcn;
			return true;
		}
		if (movd_next_fetched)
			return true;
		/*
		 * rJ must not point at movd + 1 (TeakLite 4.41, movd <1>); hardware has already fetched that word. A following
		 * control transfer starts a new block so that a loop through the rewritten word is retranslated.
		 */
		if (instruction->opcode == TEAK_OP_MOVD_RN_RN) {
			movd_next_fetched = true;
			continue;
		}
		if (tcg_is_loop_control(instruction))
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

static void tcg_emit_repeat_end(const teak_insn_t *instruction, TCGLabel *start, TCGLabel *exit) {
	TCGv_i32 repc = tcg_temp_new_i32();
	TCGv_i32 stop_repc = tcg_temp_new_i32();
	TCGv_i32 block_cycles = tcg_temp_new_i32();
	TCGLabel *done = gen_new_label();

	tcg_gen_ld16u_i32(repc, tcg_env, offsetof(teak_state_t, repc));
	tcg_gen_brcondi_i32(TCG_COND_EQ, repc, 0, done);
	tcg_gen_subi_i32(repc, repc, 1);
	tcg_gen_st16_i32(repc, tcg_env, offsetof(teak_state_t, repc));
	tcg_gen_ld_i32(stop_repc, tcg_env, offsetof(teak_tcg_core_t, repeat_stop_repc));
	tcg_gen_brcond_i32(TCG_COND_GEU, repc, stop_repc, start);

	tcg_gen_ld_i32(block_cycles, tcg_env, offsetof(teak_tcg_core_t, batch_block_cycles));
	tcg_gen_sub_i32(block_cycles, block_cycles, repc);
	tcg_gen_subi_i32(block_cycles, block_cycles, 1);
	tcg_gen_st_i32(block_cycles, tcg_env, offsetof(teak_tcg_core_t, batch_block_cycles));
	tcg_gen_st_i32(tcg_constant_i32(instruction->address), tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_br(exit);
	gen_set_label(done);
	tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, repeat_active));
}

static TCGv_i32 tcg_emit_delayed_transfer_target(const teak_insn_t *instruction) {
	TCGv_i32 target;

	if (tcg_is_delayed_return(instruction))
		return tcg_emit_pop_pc(0, true);

	switch (instruction->opcode) {
		case TEAK_OP_MOV_IMM_REGISTER:
			return tcg_constant_i32(instruction->expansion);
		case TEAK_OP_MOV_REGISTER_REGISTER:
			return tcg_emit_mov_register_read(instruction, instruction->register_code);
		case TEAK_OP_MOV_MIXP_REGISTER:
			target = tcg_temp_new_i32();
			gen_helper_teak_tcg_special_register_read(target, tcg_env, tcg_constant_i32(TEAK_SPECIAL_MIXP));
			tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
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
		case TEAK_OP_MOV_STACK_REGISTER:
			return tcg_emit_stack_top_read();
		default:
			g_assert_not_reached();
	}
}

static void tcg_emit_branch_if_interrupt_deliverable(TCGLabel *label) {
	TCGv_i32 pending = tcg_temp_new_i32();
	TCGv_i32 interrupt_lines = tcg_temp_new_i32();
	TCGv_i32 ie = tcg_temp_new_i32();
	TCGv_i32 interrupt_mask = tcg_temp_new_i32();
	TCGLabel *undeliverable = gen_new_label();

	tcg_gen_ld_i32(pending, tcg_env, offsetof(teak_state_t, pending_interrupts));
	tcg_gen_ld_i32(interrupt_lines, tcg_env, offsetof(teak_state_t, interrupt_lines));
	tcg_gen_or_i32(pending, pending, interrupt_lines);
	tcg_gen_brcondi_i32(TCG_COND_EQ, pending, 0, undeliverable);
	tcg_gen_brcondi_i32(TCG_COND_TSTNE, pending, BIT(TEAK_INTERRUPT_NMI), label);
	tcg_gen_ld8u_i32(ie, tcg_env, offsetof(teak_state_t, ie));
	tcg_gen_brcondi_i32(TCG_COND_EQ, ie, 0, undeliverable);
	tcg_gen_ld8u_i32(interrupt_mask, tcg_env, offsetof(teak_state_t, interrupt_mask));
	tcg_gen_and_i32(pending, pending, interrupt_mask);
	tcg_gen_brcondi_i32(TCG_COND_TSTNE, pending, BIT(TEAK_INTERRUPT_NMI) - 1, label);
	gen_set_label(undeliverable);
}

static void tcg_emit_batch_requests(TCGLabel *exit) {
	TCGv_i32 exit_request = tcg_temp_new_i32();
	TCGv_i32 interrupt_request = tcg_temp_new_i32();

	tcg_gen_ld_i32(exit_request, tcg_env, offsetof(teak_state_t, exit_request));
	tcg_gen_brcondi_i32(TCG_COND_NE, exit_request, 0, exit);
	tcg_gen_ld_i32(interrupt_request, tcg_env, offsetof(teak_state_t, interrupt_request));
	tcg_gen_brcondi_i32(TCG_COND_NE, interrupt_request, 0, exit);
	tcg_emit_branch_if_interrupt_deliverable(exit);
}

/* Charges the next iteration to the budget, or exits when it is spent. */
static void tcg_emit_batch_next(const teak_tcg_block_t *block, TCGLabel *exit) {
	TCGv_i32 cycles_remaining = tcg_temp_new_i32();
	TCGv_i32 iterations = tcg_temp_new_i32();

	tcg_gen_ld_i32(cycles_remaining, tcg_env, offsetof(teak_tcg_core_t, batch_cycles_remaining));
	tcg_gen_brcondi_i32(TCG_COND_LTU, cycles_remaining, 2 * block->instruction_count, exit);
	tcg_gen_subi_i32(cycles_remaining, cycles_remaining, block->instruction_count);
	tcg_gen_st_i32(cycles_remaining, tcg_env, offsetof(teak_tcg_core_t, batch_cycles_remaining));

	tcg_gen_ld_i32(iterations, tcg_env, offsetof(teak_tcg_core_t, batch_iterations));
	tcg_gen_addi_i32(iterations, iterations, 1);
	tcg_gen_st_i32(iterations, tcg_env, offsetof(teak_tcg_core_t, batch_iterations));
}

static void tcg_emit_block_batch(const teak_tcg_block_t *block, TCGLabel *loop, TCGLabel *exit) {
	TCGv_i32 block_repeat_active = tcg_temp_new_i32();
	TCGv_i32 exit_reason = tcg_temp_new_i32();
	TCGv_i32 pc = tcg_temp_new_i32();

	tcg_emit_batch_requests(exit);
	tcg_gen_ld_i32(pc, tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_brcondi_i32(TCG_COND_NE, pc, block->pc, exit);
	tcg_gen_ld8u_i32(block_repeat_active, tcg_env, offsetof(teak_state_t, lp));
	tcg_gen_brcondi_i32(TCG_COND_NE, block_repeat_active, 0, exit);
	tcg_gen_ld_i32(exit_reason, tcg_env, offsetof(teak_state_t, exit_reason));
	tcg_gen_brcondi_i32(TCG_COND_NE, exit_reason, TEAK_EXIT_BRANCH, exit);
	tcg_emit_batch_next(block, exit);
	tcg_gen_st_i32(tcg_constant_i32(TEAK_EXIT_NONE), tcg_env, offsetof(teak_state_t, exit_reason));
	tcg_gen_br(loop);
}

static bool tcg_block_can_batch_repeat_body(const teak_tcg_block_t *block) {
	if (block->repeat_body_level == 0 || tcg_block_has_dynamic_cycles(block))
		return false;
	if (!block->ends_block_repeat || block->breaks_at_block_repeat_end || block->fetch_split ||
		block->splits_block_repeat_end || block->splits_delayed_return_slot)
		return false;
	for (size_t i = 0; i < block->instruction_count; i++)
		if (tcg_is_loop_control(&block->instructions[i]) || block->instructions[i].opcode == TEAK_OP_MOVD_RN_RN)
			return false;
	return true;
}

/* The block is a whole block-repeat body: run the next pass in place, as tcg_complete_block_repeat would. */
static void tcg_emit_block_repeat_batch(const teak_tcg_block_t *block, TCGLabel *loop, TCGLabel *exit) {
	const teak_insn_t *last = &block->instructions[block->instruction_count - 1];
	uint8_t level = block->repeat_body_level - 1;
	TCGv_i32 value = tcg_temp_new_i32();

	tcg_emit_batch_requests(exit);
	tcg_gen_ld_i32(value, tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_brcondi_i32(TCG_COND_NE, value, tcg_instruction_end(last), exit);
	tcg_gen_ld8u_i32(value, tcg_env, offsetof(teak_state_t, bcn));
	tcg_gen_brcondi_i32(TCG_COND_NE, value, level + 1, exit);
	tcg_gen_ld8u_i32(value, tcg_env, offsetof(teak_state_t, lp));
	tcg_gen_brcondi_i32(TCG_COND_EQ, value, 0, exit);
	tcg_gen_ld_i32(value, tcg_env, offsetof(teak_state_t, block_repeat_start) + level * sizeof(uint32_t));
	tcg_gen_brcondi_i32(TCG_COND_NE, value, block->pc, exit);
	tcg_gen_ld_i32(value, tcg_env, offsetof(teak_state_t, block_repeat_end) + level * sizeof(uint32_t));
	tcg_gen_brcondi_i32(TCG_COND_NE, value, last->address + last->words - 1, exit);
	tcg_gen_ld16u_i32(value, tcg_env, offsetof(teak_state_t, block_repeat_lc) + level * sizeof(uint16_t));
	tcg_gen_brcondi_i32(TCG_COND_EQ, value, 0, exit);
	tcg_emit_batch_next(block, exit);
	tcg_gen_ld16u_i32(value, tcg_env, offsetof(teak_state_t, block_repeat_lc) + level * sizeof(uint16_t));
	tcg_gen_subi_i32(value, value, 1);
	tcg_gen_st16_i32(value, tcg_env, offsetof(teak_state_t, block_repeat_lc) + level * sizeof(uint16_t));
	tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, block_repeat_end_reached));
	tcg_gen_st_i32(tcg_constant_i32(block->pc), tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_br(loop);
}

static teak_tcg_chain_exit_t *tcg_chain_exit_new(void) {
	if (tcg_chain_exits == NULL)
		tcg_chain_exits = g_ptr_array_new_with_free_func(g_free);

	teak_tcg_chain_exit_t *chain_exit = g_new0(teak_tcg_chain_exit_t, 1);
	g_ptr_array_add(tcg_chain_exits, chain_exit);
	return chain_exit;
}

static void tcg_emit_branch_if_nonzero_u32(size_t offset, TCGLabel *label) {
	TCGv_i32 value = tcg_temp_new_i32();

	tcg_gen_ld_i32(value, tcg_env, offset);
	tcg_gen_brcondi_i32(TCG_COND_NE, value, 0, label);
}

/* Branches when any of the byte flags at @offsets is set; they share one 64-bit load. */
static void tcg_emit_branch_if_any_flag(const size_t *offsets, size_t count, TCGLabel *label) {
	TCGv_i64 flags = tcg_temp_new_i64();
	uint8_t bytes[sizeof(uint64_t)] = {};
	size_t base = offsets[0];
	uint64_t mask;

	for (size_t i = 1; i < count; i++)
		base = MIN(base, offsets[i]);
	g_assert(base + sizeof(bytes) <= sizeof(teak_state_t));
	for (size_t i = 0; i < count; i++) {
		g_assert(offsets[i] - base < sizeof(bytes));
		bytes[offsets[i] - base] = 0xFF;
	}
	memcpy(&mask, bytes, sizeof(mask));
	tcg_gen_ld_i64(flags, tcg_env, base);
	tcg_gen_brcondi_i64(TCG_COND_TSTNE, flags, mask, label);
}

static void tcg_emit_branch_if_link_field_differs_u16(
	TCGv_ptr link,
	size_t link_offset,
	size_t state_offset,
	TCGLabel *label
) {
	TCGv_i32 expected = tcg_temp_new_i32();
	TCGv_i32 value = tcg_temp_new_i32();

	tcg_gen_ld16u_i32(expected, link, link_offset);
	tcg_gen_ld16u_i32(value, tcg_env, state_offset);
	tcg_gen_brcond_i32(TCG_COND_NE, value, expected, label);
}

static void tcg_emit_branch_if_link_field_differs_u8(
	TCGv_ptr link,
	size_t link_offset,
	size_t state_offset,
	TCGLabel *label
) {
	TCGv_i32 expected = tcg_temp_new_i32();
	TCGv_i32 value = tcg_temp_new_i32();

	tcg_gen_ld8u_i32(expected, link, link_offset);
	tcg_gen_ld8u_i32(value, tcg_env, state_offset);
	tcg_gen_brcond_i32(TCG_COND_NE, value, expected, label);
}

static void tcg_emit_branch_if_link_field_differs_u32(
	TCGv_ptr link,
	size_t link_offset,
	size_t state_offset,
	TCGLabel *label
) {
	TCGv_i32 expected = tcg_temp_new_i32();
	TCGv_i32 value = tcg_temp_new_i32();

	tcg_gen_ld_i32(expected, link, link_offset);
	tcg_gen_ld_i32(value, tcg_env, state_offset);
	tcg_gen_brcond_i32(TCG_COND_NE, value, expected, label);
}

static void tcg_emit_branch_if_link_mismatch(TCGv_ptr link, bool check_mode, TCGLabel *label) {
	TCGv_i64 cache_generation = tcg_temp_new_i64();
	TCGv_i64 link_generation = tcg_temp_new_i64();
	TCGv_i32 block_repeat_active = tcg_temp_new_i32();
	TCGLabel *block_repeat_checked = gen_new_label();

	tcg_gen_ld_i64(cache_generation, tcg_constant_ptr(&tcg_block_cache_generation), 0);
	tcg_gen_ld_i64(link_generation, link, offsetof(teak_tcg_chain_link_t, cache_generation));
	tcg_gen_brcond_i64(TCG_COND_NE, cache_generation, link_generation, label);
#ifdef CONFIG_TCG_WASM64
	TCGv_i32 evict_epoch = tcg_temp_new_i32();
	TCGv_i32 link_evict_epoch = tcg_temp_new_i32();

	tcg_gen_ld_i32(evict_epoch, tcg_constant_ptr(&w64_evict_epoch), 0);
	tcg_gen_ld_i32(link_evict_epoch, link, offsetof(teak_tcg_chain_link_t, evict_epoch));
	tcg_gen_brcond_i32(TCG_COND_NE, evict_epoch, link_evict_epoch, label);
#endif
	tcg_emit_branch_if_link_field_differs_u32(link, offsetof(teak_tcg_chain_link_t, pc),
		offsetof(teak_state_t, pc), label);
	tcg_emit_branch_if_link_field_differs_u32(link, offsetof(teak_tcg_chain_link_t, program_context),
		offsetof(teak_tcg_core_t, program_context), label);
	/* lp and bcn, laid out alike on both sides */
	QEMU_BUILD_BUG_ON(offsetof(teak_state_t, bcn) != offsetof(teak_state_t, lp) + 1);
	QEMU_BUILD_BUG_ON(offsetof(teak_tcg_chain_link_t, block_repeat_level) !=
		offsetof(teak_tcg_chain_link_t, block_repeat_active) + 1);
	tcg_emit_branch_if_link_field_differs_u16(link, offsetof(teak_tcg_chain_link_t, block_repeat_active),
		offsetof(teak_state_t, lp), label);
	if (check_mode) {
		tcg_emit_branch_if_link_field_differs_u8(link, offsetof(teak_tcg_chain_link_t, mode.product_shift),
			offsetof(teak_state_t, product_shift), label);
		tcg_emit_branch_if_link_field_differs_u8(link, offsetof(teak_tcg_chain_link_t, mode.modulo_enable),
			offsetof(teak_state_t, modulo_enable), label);
		tcg_emit_branch_if_link_field_differs_u8(link, offsetof(teak_tcg_chain_link_t, mode.sat),
			offsetof(teak_state_t, sat), label);
		tcg_emit_branch_if_link_field_differs_u8(link, offsetof(teak_tcg_chain_link_t, mode.sata),
			offsetof(teak_state_t, sata), label);
	}

	tcg_gen_ld8u_i32(block_repeat_active, tcg_env, offsetof(teak_state_t, lp));
	tcg_gen_brcondi_i32(TCG_COND_EQ, block_repeat_active, 0, block_repeat_checked);
	tcg_emit_branch_if_link_field_differs_u32(link, offsetof(teak_tcg_chain_link_t, block_repeat_end),
		offsetof(teak_state_t, block_repeat_end[0]), label);
	gen_set_label(block_repeat_checked);
}

static void tcg_emit_outermost_block_repeat_completion(TCGLabel *slow) {
	TCGv_i32 end_reached = tcg_temp_new_i32();
	TCGv_i32 block_repeat_active = tcg_temp_new_i32();
	TCGv_i32 level = tcg_temp_new_i32();
	TCGv_i32 end = tcg_temp_new_i32();
	TCGv_i32 next = tcg_temp_new_i32();
	TCGv_i32 pc = tcg_temp_new_i32();
	TCGv_i32 start = tcg_temp_new_i32();
	TCGv_i32 lc = tcg_temp_new_i32();
	TCGLabel *loop_back = gen_new_label();
	TCGLabel *pop = gen_new_label();
	TCGLabel *completed = gen_new_label();

	tcg_gen_ld8u_i32(end_reached, tcg_env, offsetof(teak_state_t, block_repeat_end_reached));
	tcg_gen_brcondi_i32(TCG_COND_EQ, end_reached, 0, completed);
	tcg_gen_ld8u_i32(block_repeat_active, tcg_env, offsetof(teak_state_t, lp));
	tcg_gen_ld8u_i32(level, tcg_env, offsetof(teak_state_t, bcn));
	tcg_gen_brcondi_i32(TCG_COND_NE, level, 1, slow);
	tcg_gen_brcondi_i32(TCG_COND_EQ, block_repeat_active, 0, slow);
	tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, block_repeat_end_reached));

	tcg_gen_ld_i32(end, tcg_env, offsetof(teak_state_t, block_repeat_end[0]));
	tcg_gen_addi_i32(next, end, 1);
	tcg_gen_andi_i32(next, next, 0xFFFFU);
	tcg_gen_ld_i32(pc, tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_brcond_i32(TCG_COND_NE, pc, next, completed);

	tcg_gen_ld_i32(start, tcg_env, offsetof(teak_state_t, block_repeat_start[0]));
	tcg_gen_ld16u_i32(lc, tcg_env, offsetof(teak_state_t, block_repeat_lc[0]));
	tcg_gen_brcondi_i32(TCG_COND_EQ, lc, 0, pop);
	tcg_gen_subi_i32(lc, lc, 1);
	tcg_gen_st16_i32(lc, tcg_env, offsetof(teak_state_t, block_repeat_lc[0]));
	tcg_gen_brcond_i32(TCG_COND_NE, start, end, loop_back);
	tcg_gen_brcondi_i32(TCG_COND_EQ, lc, 0, pop);

	gen_set_label(loop_back);
	tcg_gen_st_i32(start, tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_br(completed);

	gen_set_label(pop);
	tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, bcn));
	tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, lp));
	gen_set_label(completed);
}

static void tcg_emit_linked_chain(
	const teak_tcg_block_t *block,
	teak_tcg_chain_exit_t *chain_exit,
	TCGv_i32 block_cycles,
	TCGLabel *slow
) {
	TCGv_i32 cycles = tcg_temp_new_i32();
	TCGv_i32 cycle_limit = tcg_temp_new_i32();
	TCGv_i32 remaining_cycles = tcg_temp_new_i32();
	TCGv_i32 target_instructions = tcg_temp_new_i32();
	TCGv_ptr link = tcg_temp_new_ptr();
	TCGv_ptr target = tcg_temp_new_ptr();
	TCGLabel *linked = gen_new_label();

	/* The completion below touches none of these flags, so they are tested together and first. */
	const size_t flags[] = {
		offsetof(teak_state_t, block_repeat_break_pending),
		offsetof(teak_state_t, repeat_active),
		offsetof(teak_state_t, fetch_split_pending),
		offsetof(teak_state_t, block_repeat_end_reached),
	};

	tcg_emit_branch_if_nonzero_u32(offsetof(teak_tcg_core_t, wait_cycles), slow);
	tcg_emit_branch_if_any_flag(flags, ARRAY_SIZE(flags) - (block->ends_block_repeat ? 1 : 0), slow);
	if (block->ends_block_repeat)
		tcg_emit_outermost_block_repeat_completion(slow);
	tcg_emit_branch_if_nonzero_u32(offsetof(teak_state_t, exit_request), slow);
	tcg_emit_branch_if_interrupt_deliverable(slow);

	for (size_t i = 0; i < ARRAY_SIZE(chain_exit->links); i++) {
		TCGLabel *next_slot = gen_new_label();

		tcg_gen_movi_ptr(link, (uintptr_t) &chain_exit->links[i]);
		tcg_emit_branch_if_link_mismatch(link, !chain_exit->source_mode.specialized, next_slot);
		tcg_gen_br(linked);
		gen_set_label(next_slot);
	}
	tcg_gen_br(slow);
	gen_set_label(linked);

	tcg_gen_ld_i32(cycles, tcg_env, offsetof(teak_tcg_core_t, last_block_cycles));
	tcg_gen_add_i32(cycles, cycles, block_cycles);
	tcg_gen_ld_i32(cycle_limit, tcg_env, offsetof(teak_tcg_core_t, chain_cycle_limit));
	tcg_gen_brcond_i32(TCG_COND_LEU, cycle_limit, cycles, slow);
	tcg_gen_sub_i32(remaining_cycles, cycle_limit, cycles);
	tcg_gen_ld_i32(target_instructions, link, offsetof(teak_tcg_chain_link_t, instruction_count));
	tcg_gen_brcond_i32(TCG_COND_LTU, remaining_cycles, target_instructions, slow);

	tcg_gen_st_i32(cycles, tcg_env, offsetof(teak_tcg_core_t, last_block_cycles));
	tcg_gen_st_i32(remaining_cycles, tcg_env, offsetof(teak_tcg_core_t, batch_cycles_remaining));
	tcg_gen_st_i32(tcg_constant_i32(TEAK_EXIT_NONE), tcg_env, offsetof(teak_state_t, exit_reason));

	tcg_gen_ld_ptr(target, link, offsetof(teak_tcg_chain_link_t, code));
	tcg_gen_goto_ptr(target);
}

static void tcg_emit_block_chain(const teak_tcg_block_t *block, bool batched_loop) {
	TCGv_i32 block_cycles = tcg_temp_new_i32();
	TCGv_ptr next = tcg_temp_new_ptr();
	teak_tcg_chain_exit_t *chain_exit = tcg_chain_exit_new();
	TCGLabel *slow = gen_new_label();
	TCGLabel *done = gen_new_label();

	chain_exit->source_mode = tcg_block_mode;
	tcg_gen_ld_i32(block_cycles, tcg_env, offsetof(teak_tcg_core_t, batch_block_cycles));
	if (batched_loop) {
		/* Whole iterations, plus the current one, which an early exit may have cut short. */
		TCGv_i32 iterations = tcg_temp_new_i32();

		tcg_gen_ld_i32(iterations, tcg_env, offsetof(teak_tcg_core_t, batch_iterations));
		tcg_gen_muli_i32(iterations, iterations, block->instruction_count);
		tcg_gen_add_i32(block_cycles, block_cycles, iterations);
	}
	tcg_emit_linked_chain(block, chain_exit, block_cycles, slow);

	gen_set_label(slow);
	gen_helper_teak_tcg_chain(next, tcg_env, block_cycles, tcg_constant_i32(block->pc), tcg_constant_ptr(chain_exit));
	tcg_mark_call(TEAK_TCG_CALL_NO_MODE_WRITES);
	tcg_gen_brcondi_ptr(TCG_COND_EQ, next, 0, done);
	tcg_gen_goto_ptr(next);
	gen_set_label(done);
}

static void tcg_emit_early_exit(TCGLabel *early_exit, uint32_t skipped_instructions, TCGLabel *exit) {
	TCGv_i32 block_cycles = tcg_temp_new_i32();

	gen_set_label(early_exit);
	tcg_gen_ld_i32(block_cycles, tcg_env, offsetof(teak_tcg_core_t, batch_block_cycles));
	tcg_gen_subi_i32(block_cycles, block_cycles, skipped_instructions);
	tcg_gen_st_i32(block_cycles, tcg_env, offsetof(teak_tcg_core_t, batch_block_cycles));
	tcg_gen_br(exit);
}

static void tcg_emit_delayed_transfer_end(
	const teak_tcg_block_t *block,
	const teak_insn_t *last,
	TCGv_i32 target,
	bool interrupt_return
) {
	if (block->splits_delayed_return_slot) {
		tcg_gen_st_i32(tcg_constant_i32(tcg_instruction_end(last)), tcg_env, offsetof(teak_state_t, pc));
		tcg_gen_st8_i32(tcg_constant_i32(1), tcg_env, offsetof(teak_state_t, fetch_split_pending));
		tcg_gen_st_i32(target, tcg_env, offsetof(teak_state_t, fetch_split_target));
	} else {
		tcg_gen_st_i32(target, tcg_env, offsetof(teak_state_t, pc));
	}
	if (interrupt_return)
		tcg_emit_interrupt_return_state(target);
	tcg_gen_st_i32(tcg_constant_i32(TEAK_EXIT_BRANCH), tcg_env, offsetof(teak_state_t, exit_reason));
}

static void tcg_emit_fetch_split(const teak_tcg_block_t *block, teak_insn_t *instruction, uint8_t level) {
	TCGv_i32 pc = tcg_temp_new_i32();
	TCGLabel *done = gen_new_label();

	tcg_gen_st8_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_state_t, fetch_split_pending));
	tcg_emit_instruction(instruction, level);
	tcg_gen_ld_i32(pc, tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_brcondi_i32(TCG_COND_NE, pc, tcg_instruction_end(instruction), done);
	tcg_gen_st_i32(tcg_constant_i32((uint16_t) (block->fetch_split_target + 1)), tcg_env, offsetof(teak_state_t, pc));
	gen_set_label(done);
}

static void tcg_emit_block_repeat_split_end(teak_tcg_block_t *block, teak_insn_t *instruction, uint8_t level) {
	size_t lc_offset = offsetof(teak_state_t, block_repeat_lc) + block->block_repeat_slot * sizeof(uint16_t);
	uint16_t loop_resume_pc = block->block_repeat_start + 1;
	TCGv_i32 lc = tcg_temp_new_i32();
	TCGv_i32 pc = tcg_temp_new_i32();
	TCGLabel *last_pass = gen_new_label();
	TCGLabel *done = gen_new_label();

	tcg_gen_ld16u_i32(lc, tcg_env, lc_offset);
	tcg_gen_brcondi_i32(TCG_COND_EQ, lc, 0, last_pass);
	tcg_gen_subi_i32(lc, lc, 1);
	tcg_gen_st16_i32(lc, tcg_env, lc_offset);
	tcg_emit_instruction(&block->block_repeat_loop_instruction, level);
	tcg_gen_ld_i32(pc, tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_brcondi_i32(TCG_COND_NE, pc, tcg_instruction_end(instruction), done);
	tcg_gen_st_i32(tcg_constant_i32(loop_resume_pc), tcg_env, offsetof(teak_state_t, pc));
	tcg_gen_br(done);

	gen_set_label(last_pass);
	tcg_emit_instruction(instruction, level);
	tcg_emit_break();
	gen_set_label(done);
}

static void tcg_emit_block(void *opaque) {
	teak_tcg_block_t *block = opaque;
	TCGv_i32 delayed_transfer_target = NULL;
	TCGLabel *early_exits[TEAK_TCG_MAX_BLOCK_INSTRUCTIONS] = {};
	TCGLabel *exit = gen_new_label();
	TCGLabel *repeat = block->repeat_resume ? gen_new_label() : NULL;
	TCGLabel *loop = NULL;
	bool delayed_transfer_interrupt = false;
	bool repeat_pending = block->repeat_resume;
	bool can_batch = tcg_block_can_batch(block);
	bool can_batch_repeat_body = !can_batch && tcg_block_can_batch_repeat_body(block);
	bool has_early_exit = false;

	tcg_temp_pool_reset();
	tcg_gen_st_i32(tcg_constant_i32(0), tcg_env, offsetof(teak_tcg_core_t, batch_iterations));
	if (!tcg_block_has_dynamic_cycles(block))
		tcg_gen_st_i32(tcg_constant_i32(block->instruction_count), tcg_env,
			offsetof(teak_tcg_core_t, batch_block_cycles));
	if (can_batch || can_batch_repeat_body) {
		loop = gen_new_label();
		gen_set_label(loop);
	}

	for (size_t i = 0; i < block->instruction_count; i++) {
		teak_insn_t *instruction = &block->instructions[i];
		uint8_t setup_level = block->block_repeat_setup_level[i];

		tcg_temp_pool_release();
		tcg_memory_pc = tcg_constant_i32(instruction->address);
		tcg_memory_cycle = tcg_constant_i32(i);
		tcg_memory_access = 0;
		if (repeat_pending)
			gen_set_label(repeat);
		tcg_gen_insn_start(instruction->address, 0, 0);
		if (tcg_delayed_transfer_cycles(instruction) != 0) {
			delayed_transfer_target = tcg_emit_delayed_transfer_target(instruction);
			tcg_temp_pool_pin(tcgv_i32_temp(delayed_transfer_target));
			delayed_transfer_interrupt = instruction->opcode == TEAK_OP_DELAYED_RETURN_INTERRUPT;
		}
		bool last = i + 1 == block->instruction_count;
		if (block->fetch_split) {
			tcg_emit_fetch_split(block, instruction, setup_level);
		} else if (block->breaks_at_block_repeat_end && last) {
			tcg_emit_break_at_block_repeat_end(instruction);
		} else if (block->splits_block_repeat_end && last) {
			tcg_emit_block_repeat_split_end(block, instruction, setup_level);
		} else {
			tcg_emit_instruction(instruction, setup_level);
		}
		if (block->ends_block_repeat && last)
			tcg_gen_st8_i32(tcg_constant_i32(1), tcg_env, offsetof(teak_state_t, block_repeat_end_reached));
		if (tcg_is_repeat(instruction)) {
			repeat = gen_new_label();
			repeat_pending = true;
			continue;
		}
		if (repeat_pending) {
			tcg_emit_repeat_end(instruction, repeat, exit);
			repeat_pending = false;
		}
		if (delayed_transfer_target != NULL && last)
			tcg_emit_delayed_transfer_end(block, instruction, delayed_transfer_target, delayed_transfer_interrupt);
		bool inside_delay_slots = delayed_transfer_target != NULL && !last;
		if (tcg_may_write_data(instruction) && !inside_delay_slots) {
			TCGv_i32 exit_request = tcg_temp_new_i32();
			TCGv_i32 interrupt_request = tcg_temp_new_i32();

			early_exits[i] = gen_new_label();
			has_early_exit = true;
			tcg_gen_ld_i32(exit_request, tcg_env, offsetof(teak_state_t, exit_request));
			tcg_gen_brcondi_i32(TCG_COND_NE, exit_request, 0, early_exits[i]);
			tcg_gen_ld_i32(interrupt_request, tcg_env, offsetof(teak_state_t, interrupt_request));
			tcg_gen_brcondi_i32(TCG_COND_NE, interrupt_request, 0, early_exits[i]);
		}
	}
	tcg_temp_pool_release();
	if (can_batch)
		tcg_emit_block_batch(block, loop, exit);
	if (can_batch_repeat_body)
		tcg_emit_block_repeat_batch(block, loop, exit);
	if (has_early_exit) {
		tcg_gen_br(exit);
		for (size_t i = 0; i < block->instruction_count; i++) {
			if (early_exits[i] != NULL)
				tcg_emit_early_exit(early_exits[i], block->instruction_count - 1 - i, exit);
		}
	}
	gen_set_label(exit);
	tcg_emit_block_chain(block, can_batch || can_batch_repeat_body);
}

static bool tcg_block_has_block_repeat_end(const teak_tcg_core_t *core, const teak_tcg_block_t *block) {
	const teak_insn_t *last = &block->instructions[block->instruction_count - 1];

	if (!core->state.lp)
		return false;
	return last->address + last->words - 1 == tcg_active_block_repeat_end(&core->state);
}

static void tcg_pop_block_repeat(teak_state_t *state) {
	if (!state->lp)
		return;
	state->bcn = (state->bcn - 1) & TEAK_BLOCK_REPEAT_COUNTER_MASK;
	state->lp = state->bcn != 0;
}

static void tcg_complete_block_repeat(teak_state_t *state) {
	if (!state->lp)
		return;

	uint8_t slot = teak_block_repeat_slot(state->bcn);
	uint16_t next = (uint16_t) (state->block_repeat_end[slot] + 1);
	if (state->pc != next)
		return;

	uint16_t *lc = &state->block_repeat_lc[slot];
	/* The minimum block length is 2 words (TeakLite 4.41, bkrep <6>); hardware runs a one-word block lc times. */
	bool one_word_body = state->block_repeat_start[slot] == state->block_repeat_end[slot];
	if (*lc != 0) {
		(*lc)--;
		if (!one_word_body || *lc != 0) {
			state->pc = state->block_repeat_start[slot];
			return;
		}
	}
	tcg_pop_block_repeat(state);
}

static uint32_t tcg_block_cycles(teak_tcg_core_t *core, const teak_tcg_block_t *block) {
	const teak_insn_t *instruction = &block->instructions[0];
	uint32_t cycles = block->instruction_count;

	if (instruction->opcode == TEAK_OP_REPEAT_IMMEDIATE)
		cycles += instruction->immediate;
	if (instruction->opcode == TEAK_OP_REPEAT_REGISTER) {
		bool reads_pc = instruction->register_code == 12;
		cycles += reads_pc ? tcg_instruction_end(instruction) :
			tcg_read_register(&core->state, instruction->register_code);
	}
	if (block->repeat_resume)
		cycles += core->state.repc;
	return cycles;
}

static TranslationBlock *tcg_prepare_block(teak_tcg_core_t *core, teak_tcg_block_t *block) {
	TranslationBlock *tb;
	size_t max_instructions = TEAK_TCG_MAX_BLOCK_INSTRUCTIONS;
	int compile_error;

	tcg_check_block_cache();
	tb = tcg_find_cached_block_fast(core, block);
	if (tb != NULL)
		return tb;

	tcg_block_mode = tcg_mode_from_state(&core->state);
	while (tb == NULL) {
		if (!tcg_decode_block(core, block, max_instructions))
			return NULL;
		block->ends_block_repeat = tcg_block_has_block_repeat_end(core, block);
		block->breaks_at_block_repeat_end = block->ends_block_repeat &&
			block->instructions[block->instruction_count - 1].opcode == TEAK_OP_BREAK;

		const teak_tcg_block_cache_entry_t *cached = tcg_find_cached_block(core, block);
		if (cached != NULL) {
			tb = cached->tb;
			tcg_cache_block(core, block, tb, cached->mode);
			core->cache_decoded_hits++;
			break;
		}

#ifdef CONFIG_TCG_WASM64
		/* cache entries are per core (cache_id), so the env address is a constant */
		w64_tb_env = (uintptr_t) &core->state;
#endif
		tb = tcg_compile_block(block->pc, block->words, block->instruction_count, tcg_emit_block,
			block, &compile_error);
#ifdef CONFIG_TCG_WASM64
		w64_tb_env = 0;
#endif
		if (tb != NULL) {
			tcg_cache_block(core, block, tb, tcg_block_mode);
			tcg_fast_discover(core, block->pc);
			core->cache_compiles++;
			break;
		}

		if (compile_error == TEAK_TCG_COMPILE_MODE_WRITE) {
			tcg_block_mode.specialized = false;
			continue;
		}
		if (compile_error == TEAK_TCG_COMPILE_RETRY) {
			core->translation_error = TEAK_TRANSLATION_ERROR_RETRY;
			return NULL;
		}

		if (compile_error != -2 || block->instruction_count == 1) {
			core->translation_error = TEAK_TRANSLATION_ERROR_COMPILE;
			return NULL;
		}
		max_instructions = MAX((size_t) 1, block->instruction_count / 2);
	}
	return tb;
}

static void tcg_precompile_value_set(teak_tcg_precompile_value_t *value, uint16_t item) {
	value->kind = TEAK_TCG_PRECOMPILE_SET;
	value->values[0] = item;
	value->count = 1;
}

static bool tcg_precompile_value_merge(teak_tcg_precompile_value_t *destination,
	const teak_tcg_precompile_value_t *source
) {
	if (destination->kind == TEAK_TCG_PRECOMPILE_UNKNOWN)
		return false;
	if (source->kind == TEAK_TCG_PRECOMPILE_UNKNOWN) {
		destination->kind = TEAK_TCG_PRECOMPILE_UNKNOWN;
		return true;
	}
	if (destination->kind == TEAK_TCG_PRECOMPILE_RANGE || source->kind == TEAK_TCG_PRECOMPILE_RANGE) {
		int32_t minimum = destination->minimum;
		int32_t maximum = destination->maximum;

		if (destination->kind == TEAK_TCG_PRECOMPILE_SET) {
			minimum = (int16_t) destination->values[0];
			maximum = minimum;
			for (size_t i = 1; i < destination->count; i++) {
				minimum = MIN(minimum, (int16_t) destination->values[i]);
				maximum = MAX(maximum, (int16_t) destination->values[i]);
			}
		}
		if (source->kind == TEAK_TCG_PRECOMPILE_RANGE) {
			minimum = MIN(minimum, source->minimum);
			maximum = MAX(maximum, source->maximum);
		} else {
			for (size_t i = 0; i < source->count; i++) {
				minimum = MIN(minimum, (int16_t) source->values[i]);
				maximum = MAX(maximum, (int16_t) source->values[i]);
			}
		}
		bool changed = destination->kind != TEAK_TCG_PRECOMPILE_RANGE ||
			destination->minimum != minimum || destination->maximum != maximum;
		destination->kind = TEAK_TCG_PRECOMPILE_RANGE;
		destination->minimum = minimum;
		destination->maximum = maximum;
		return changed;
	}

	bool changed = false;
	for (size_t i = 0; i < source->count; i++) {
		bool found = false;

		for (size_t j = 0; j < destination->count; j++)
			found |= destination->values[j] == source->values[i];
		if (found)
			continue;
		if (destination->count == TEAK_TCG_PRECOMPILE_VALUES) {
			destination->kind = TEAK_TCG_PRECOMPILE_UNKNOWN;
			return true;
		}
		destination->values[destination->count++] = source->values[i];
		changed = true;
	}
	return changed;
}

static bool tcg_precompile_state_merge(teak_tcg_precompile_state_t *destination,
	const teak_tcg_precompile_state_t *source
) {
	bool changed = false;

	for (size_t i = 0; i < ARRAY_SIZE(destination->a); i++)
		changed |= tcg_precompile_value_merge(&destination->a[i], &source->a[i]);
	for (size_t i = 0; i < ARRAY_SIZE(destination->r); i++)
		changed |= tcg_precompile_value_merge(&destination->r[i], &source->r[i]);
	if (destination->flags_source != source->flags_source || destination->comparison != source->comparison) {
		if (destination->flags_source != TEAK_TCG_PRECOMPILE_FLAGS_UNKNOWN) {
			destination->flags_source = TEAK_TCG_PRECOMPILE_FLAGS_UNKNOWN;
			changed = true;
		}
	}
	return changed;
}

static teak_tcg_precompile_value_t *tcg_precompile_register(teak_tcg_precompile_state_t *state,
	uint8_t register_code
) {
	if (register_code < 6)
		return &state->r[register_code];
	if (register_code == 6)
		return &state->r[7];
	if (register_code >= 24 && register_code <= 27)
		return &state->a[register_code & 1U];
	return NULL;
}

static void tcg_precompile_value_add(teak_tcg_precompile_value_t *value, int32_t operand) {
	if (value->kind == TEAK_TCG_PRECOMPILE_SET) {
		for (size_t i = 0; i < value->count; i++)
			value->values[i] = (uint16_t) ((int16_t) value->values[i] + operand);
		return;
	}
	if (value->kind != TEAK_TCG_PRECOMPILE_RANGE)
		return;

	int32_t minimum = value->minimum + operand;
	int32_t maximum = value->maximum + operand;
	if (minimum < INT16_MIN || maximum > INT16_MAX) {
		value->kind = TEAK_TCG_PRECOMPILE_UNKNOWN;
		return;
	}
	value->minimum = minimum;
	value->maximum = maximum;
}

static void tcg_precompile_data_read(teak_tcg_core_t *core, const teak_tcg_precompile_value_t *address,
	teak_tcg_precompile_value_t *result
) {
	uint16_t addresses[TEAK_TCG_PRECOMPILE_VALUES];
	size_t count;

	if (address->kind == TEAK_TCG_PRECOMPILE_SET) {
		count = address->count;
		memcpy(addresses, address->values, count * sizeof(addresses[0]));
	} else if (address->kind == TEAK_TCG_PRECOMPILE_RANGE &&
		address->maximum - address->minimum < TEAK_TCG_PRECOMPILE_VALUES) {
		count = (size_t) (address->maximum - address->minimum + 1);
		for (size_t i = 0; i < count; i++)
			addresses[i] = (uint16_t) (address->minimum + (int32_t) i);
	} else {
		result->kind = TEAK_TCG_PRECOMPILE_UNKNOWN;
		return;
	}

	result->kind = TEAK_TCG_PRECOMPILE_SET;
	result->count = 0;
	for (size_t i = 0; i < count; i++) {
		uint16_t item;

		if (!tcg_mapped_data_read(core, addresses[i], &item)) {
			result->kind = TEAK_TCG_PRECOMPILE_UNKNOWN;
			return;
		}
		bool found = false;
		for (size_t j = 0; j < result->count; j++)
			found |= result->values[j] == item;
		if (!found)
			result->values[result->count++] = item;
	}
}

static void tcg_precompile_set_flags(teak_tcg_precompile_state_t *state, uint8_t accumulator_index,
	int32_t comparison
) {
	state->flags_source = accumulator_index == 0 ? TEAK_TCG_PRECOMPILE_FLAGS_A0 :
		TEAK_TCG_PRECOMPILE_FLAGS_A1;
	state->comparison = comparison;
}

static void tcg_precompile_apply_instruction(teak_tcg_core_t *core, teak_tcg_precompile_state_t *state,
	const teak_insn_t *instruction
) {
	teak_tcg_precompile_value_t *destination;
	teak_tcg_precompile_value_t *source;

	switch (instruction->opcode) {
		case TEAK_OP_MOV_IMM_REGISTER:
			destination = tcg_precompile_register(state, instruction->register_code);
			if (destination != NULL)
				tcg_precompile_value_set(destination, instruction->expansion);
			state->flags_source = TEAK_TCG_PRECOMPILE_FLAGS_UNKNOWN;
			break;

		case TEAK_OP_MOV_IMM8_ACCUMULATOR_LOW:
			tcg_precompile_value_set(&state->a[instruction->accumulator_index], instruction->immediate);
			tcg_precompile_set_flags(state, instruction->accumulator_index, 0);
			break;

		case TEAK_OP_MOV_REGISTER_REGISTER:
			destination = tcg_precompile_register(state, instruction->destination_register_code);
			source = tcg_precompile_register(state, instruction->register_code);
			if (destination != NULL)
				*destination = source != NULL ? *source : (teak_tcg_precompile_value_t) {};
			state->flags_source = TEAK_TCG_PRECOMPILE_FLAGS_UNKNOWN;
			break;

		case TEAK_OP_MOV_ACCUMULATOR_ACCUMULATOR:
			if (instruction->destination_accumulator >= 2) {
				uint8_t destination_index = instruction->destination_accumulator & 1U;

				if (instruction->source_accumulator >= 2) {
					state->a[destination_index] = state->a[instruction->source_accumulator & 1U];
				} else {
					state->a[destination_index].kind = TEAK_TCG_PRECOMPILE_UNKNOWN;
				}
				tcg_precompile_set_flags(state, destination_index, 0);
			} else {
				state->flags_source = TEAK_TCG_PRECOMPILE_FLAGS_UNKNOWN;
			}
			break;

		case TEAK_OP_MOV_DATA_IMM16_ACCUMULATOR: {
			teak_tcg_precompile_value_t address = {};

			tcg_precompile_value_set(&address, instruction->expansion);
			tcg_precompile_data_read(core, &address, &state->a[instruction->accumulator_index]);
			tcg_precompile_set_flags(state, instruction->accumulator_index, 0);
			break;
		}

		case TEAK_OP_MOV_DATA_RN_STEP_REGISTER:
			destination = tcg_precompile_register(state, instruction->register_code);
			if (destination != NULL)
				tcg_precompile_data_read(core, &state->r[instruction->address_register], destination);
			if (instruction->step != TEAK_STEP_ZERO)
				state->r[instruction->address_register].kind = TEAK_TCG_PRECOMPILE_UNKNOWN;
			if (instruction->register_code >= 24 && instruction->register_code <= 27) {
				tcg_precompile_set_flags(state, instruction->register_code & 1U, 0);
			} else {
				state->flags_source = TEAK_TCG_PRECOMPILE_FLAGS_UNKNOWN;
			}
			break;

		case TEAK_OP_ALU_IMMEDIATE_ACCUMULATOR:
			if (instruction->alu_operation == TEAK_ALU_ADD) {
				tcg_precompile_value_add(&state->a[instruction->accumulator_index],
					(int16_t) instruction->immediate);
				tcg_precompile_set_flags(state, instruction->accumulator_index, 0);
			} else if (instruction->alu_operation == TEAK_ALU_CMP) {
				tcg_precompile_set_flags(state, instruction->accumulator_index,
					(int16_t) instruction->immediate);
			} else {
				state->a[instruction->accumulator_index].kind = TEAK_TCG_PRECOMPILE_UNKNOWN;
				state->flags_source = TEAK_TCG_PRECOMPILE_FLAGS_UNKNOWN;
			}
			break;

		case TEAK_OP_MODA4_ACCUMULATOR:
			state->a[instruction->accumulator_index].kind = TEAK_TCG_PRECOMPILE_UNKNOWN;
			state->flags_source = TEAK_TCG_PRECOMPILE_FLAGS_UNKNOWN;
			break;

		case TEAK_OP_NOP:
		case TEAK_OP_EINT:
		case TEAK_OP_DINT:
		case TEAK_OP_LOAD_PAGE:
		case TEAK_OP_LOAD_STEPI:
		case TEAK_OP_LOAD_STEPJ:
		case TEAK_OP_LOAD_MODI:
		case TEAK_OP_LOAD_MODJ:
		case TEAK_OP_LOAD_PRODUCT_SHIFT:
		case TEAK_OP_PUSH_IMMEDIATE:
		case TEAK_OP_PUSH_REGISTER:
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
			break;

		case TEAK_OP_MOV_IMM_B_ACCUMULATOR:
		case TEAK_OP_MOV_REGISTER_B_ACCUMULATOR:
		case TEAK_OP_MOVR_B_ACCUMULATOR:
			state->flags_source = TEAK_TCG_PRECOMPILE_FLAGS_UNKNOWN;
			break;

		default:
			for (size_t i = 0; i < ARRAY_SIZE(state->a); i++)
				state->a[i].kind = TEAK_TCG_PRECOMPILE_UNKNOWN;
			for (size_t i = 0; i < ARRAY_SIZE(state->r); i++)
				state->r[i].kind = TEAK_TCG_PRECOMPILE_UNKNOWN;
			state->flags_source = TEAK_TCG_PRECOMPILE_FLAGS_UNKNOWN;
			break;
	}
}

static bool tcg_precompile_value_refine(teak_tcg_precompile_value_t *value, teak_condition_t condition,
	int32_t comparison, bool taken
) {
	int32_t minimum = INT16_MIN;
	int32_t maximum = INT16_MAX;
	bool exclude_equal = false;

	switch (condition) {
		case TEAK_COND_EQ:
			minimum = comparison;
			maximum = comparison;
			exclude_equal = !taken;
			break;
		case TEAK_COND_NEQ:
			minimum = comparison;
			maximum = comparison;
			exclude_equal = taken;
			break;
		case TEAK_COND_GT:
			minimum = comparison + 1;
			if (!taken) {
				minimum = INT16_MIN;
				maximum = comparison;
			}
			break;
		case TEAK_COND_GE:
			minimum = comparison;
			if (!taken) {
				minimum = INT16_MIN;
				maximum = comparison - 1;
			}
			break;
		case TEAK_COND_LT:
			maximum = comparison - 1;
			if (!taken) {
				minimum = comparison;
				maximum = INT16_MAX;
			}
			break;
		case TEAK_COND_LE:
			maximum = comparison;
			if (!taken) {
				minimum = comparison + 1;
				maximum = INT16_MAX;
			}
			break;
		default:
			return true;
	}

	if (value->kind == TEAK_TCG_PRECOMPILE_UNKNOWN) {
		if (exclude_equal)
			return true;
		value->kind = TEAK_TCG_PRECOMPILE_RANGE;
		value->minimum = minimum;
		value->maximum = maximum;
		return minimum <= maximum;
	}
	if (value->kind == TEAK_TCG_PRECOMPILE_RANGE) {
		if (exclude_equal && comparison > value->minimum && comparison < value->maximum)
			return true;
		if (exclude_equal) {
			if (value->minimum == comparison)
				value->minimum++;
			if (value->maximum == comparison)
				value->maximum--;
		} else {
			value->minimum = MAX(value->minimum, minimum);
			value->maximum = MIN(value->maximum, maximum);
		}
		return value->minimum <= value->maximum;
	}

	size_t output = 0;
	for (size_t i = 0; i < value->count; i++) {
		int32_t item = (int16_t) value->values[i];
		bool keep = exclude_equal ? item != comparison : item >= minimum && item <= maximum;

		if (keep)
			value->values[output++] = value->values[i];
	}
	value->count = output;
	return output != 0;
}

static bool tcg_precompile_refine(teak_tcg_precompile_state_t *state, teak_condition_t condition, bool taken) {
	teak_tcg_precompile_value_t *value;

	switch (state->flags_source) {
		case TEAK_TCG_PRECOMPILE_FLAGS_A0:
			value = &state->a[0];
			break;
		case TEAK_TCG_PRECOMPILE_FLAGS_A1:
			value = &state->a[1];
			break;
		default:
			return true;
	}
	return tcg_precompile_value_refine(value, condition, state->comparison, taken);
}

static void tcg_precompile_enqueue(teak_tcg_precompile_node_t **nodes, GQueue *queue, uint32_t address,
	const teak_tcg_precompile_state_t *state
) {
	uint16_t pc = (uint16_t) address;
	teak_tcg_precompile_node_t *node = nodes[pc];
	bool changed;

	if (node == NULL) {
		node = g_new0(teak_tcg_precompile_node_t, 1);
		node->pc = pc;
		nodes[pc] = node;
	}
	if (!node->reached) {
		node->state = *state;
		node->reached = true;
		changed = true;
	} else {
		changed = tcg_precompile_state_merge(&node->state, state);
	}
	if (changed && !node->queued) {
		node->queued = true;
		g_queue_push_tail(queue, node);
	}
}

static void tcg_precompile_enqueue_value(teak_tcg_precompile_node_t **nodes, GQueue *queue,
	const teak_tcg_precompile_value_t *value, const teak_tcg_precompile_state_t *state
) {
	if (value->kind == TEAK_TCG_PRECOMPILE_SET) {
		for (size_t i = 0; i < value->count; i++)
			tcg_precompile_enqueue(nodes, queue, value->values[i], state);
	} else if (value->kind == TEAK_TCG_PRECOMPILE_RANGE &&
		value->maximum - value->minimum < TEAK_TCG_PRECOMPILE_VALUES) {
		for (int32_t item = value->minimum; item <= value->maximum; item++)
			tcg_precompile_enqueue(nodes, queue, (uint16_t) item, state);
	}
}

size_t teak_tcg_precompile_entry(teak_tcg_core_t *core, uint32_t entry) {
	teak_tcg_precompile_node_t **nodes = g_new0(teak_tcg_precompile_node_t *,
		(size_t) TEAK_PROGRAM_ADDRESS_MASK + 1);
	bool *compiled = g_new0(bool, (size_t) TEAK_PROGRAM_ADDRESS_MASK + 1);
	GQueue *queue = g_queue_new();
	teak_tcg_precompile_state_t initial_state = {};
	size_t blocks = 0;

	uint32_t saved_pc = core->state.pc;
	uint32_t saved_error_address = core->translation_error_address;
	teak_translation_error_t saved_error = core->translation_error;
	uint8_t saved_bcn = core->state.bcn;
	uint8_t saved_lp = core->state.lp;
	uint8_t saved_repeat_active = core->state.repeat_active;
	uint8_t saved_fetch_split_pending = core->state.fetch_split_pending;

	tcg_precompile_enqueue(nodes, queue, entry, &initial_state);
	for (size_t i = 0; i < ARRAY_SIZE(teak_interrupt_vectors); i++)
		tcg_precompile_enqueue(nodes, queue, teak_interrupt_vectors[i], &initial_state);

	while (!g_queue_is_empty(queue)) {
		teak_tcg_precompile_node_t *node = g_queue_pop_head(queue);
		teak_tcg_block_t block;
		teak_tcg_precompile_state_t state = node->state;

		node->queued = false;
		core->state.pc = node->pc;
		core->state.bcn = 0;
		core->state.lp = 0;
		core->state.repeat_active = 0;
		core->state.fetch_split_pending = 0;
		TranslationBlock *tb = tcg_prepare_block(core, &block);
		if (tb == NULL)
			continue;

		if (!compiled[node->pc]) {
			compiled[node->pc] = true;
			blocks++;
		}
		for (size_t i = 0; i < block.instruction_count; i++)
			tcg_precompile_apply_instruction(core, &state, &block.instructions[i]);
		const teak_insn_t *last = &block.instructions[block.instruction_count - 1];
		uint16_t fallthrough = (uint16_t) (last->address + last->words);

		switch (last->opcode) {
			case TEAK_OP_BRANCH_ABSOLUTE:
			case TEAK_OP_BRANCH_RELATIVE: {
				teak_tcg_precompile_state_t taken = state;
				teak_tcg_precompile_state_t not_taken = state;

				if (last->condition == TEAK_COND_TRUE) {
					tcg_precompile_enqueue(nodes, queue, last->branch_target, &state);
				} else {
					if (tcg_precompile_refine(&taken, last->condition, true))
						tcg_precompile_enqueue(nodes, queue, last->branch_target, &taken);
					if (tcg_precompile_refine(&not_taken, last->condition, false))
						tcg_precompile_enqueue(nodes, queue, fallthrough, &not_taken);
				}
				break;
			}

			case TEAK_OP_CALL_ABSOLUTE:
			case TEAK_OP_CALL_RELATIVE:
				tcg_precompile_enqueue(nodes, queue, last->branch_target, &state);
				tcg_precompile_enqueue(nodes, queue, fallthrough, &state);
				break;

			case TEAK_OP_CALL_ACCUMULATOR:
				tcg_precompile_enqueue_value(nodes, queue, &state.a[last->accumulator_index], &state);
				tcg_precompile_enqueue(nodes, queue, fallthrough, &state);
				break;

			case TEAK_OP_RETURN:
			case TEAK_OP_RETURN_INTERRUPT:
				if (last->condition != TEAK_COND_TRUE)
					tcg_precompile_enqueue(nodes, queue, fallthrough, &state);
				break;

			case TEAK_OP_RETURN_STACK:
			case TEAK_OP_DELAYED_RETURN:
			case TEAK_OP_DELAYED_RETURN_INTERRUPT:
				break;

			case TEAK_OP_TRAP:
				tcg_precompile_enqueue(nodes, queue, 2, &state);
				tcg_precompile_enqueue(nodes, queue, fallthrough, &state);
				break;

			default:
				tcg_precompile_enqueue(nodes, queue, fallthrough, &state);
				break;
		}
	}

	core->state.pc = saved_pc;
	core->state.bcn = saved_bcn;
	core->state.lp = saved_lp;
	core->state.repeat_active = saved_repeat_active;
	core->state.fetch_split_pending = saved_fetch_split_pending;
	core->translation_error_address = saved_error_address;
	core->translation_error = saved_error;

	for (size_t i = 0; i <= TEAK_PROGRAM_ADDRESS_MASK; i++)
		g_free(nodes[i]);
	g_queue_free(queue);
	g_free(compiled);
	g_free(nodes);
	return blocks;
}

static uint32_t tcg_repeat_stop_repc(
	const teak_tcg_core_t *core,
	const teak_tcg_block_t *block,
	uint32_t block_cycles,
	size_t max_cycles
) {
	if (!tcg_block_has_dynamic_cycles(block) || core->state.repeat_suspended)
		return 0;

	uint32_t setup_cycles = block->repeat_resume ? 0 : 1;
	uint32_t iterations = block_cycles - setup_cycles;
	/* A new repeat accepts interrupts only after its second iteration (rep, remark <1>). */
	size_t first_stop_iterations = block->repeat_resume ? 1 : 2;
	size_t stop_iterations = MAX(max_cycles - setup_cycles, first_stop_iterations);

	if (stop_iterations >= iterations)
		return 0;
	return iterations - stop_iterations;
}

static void tcg_flush_cycles(teak_tcg_core_t *core) {
	assert(core->advanced_cycles <= core->last_block_cycles);
	uint32_t cycles = core->last_block_cycles - core->advanced_cycles;

	if (core->memory.advance_cycles != NULL && cycles != 0)
		core->memory.advance_cycles(core->memory.cycle_opaque, cycles);
	core->advanced_cycles = core->last_block_cycles;
}

static teak_tcg_chain_link_t *tcg_chain_exit_select_link(teak_tcg_chain_exit_t *chain_exit, uint32_t pc) {
	for (size_t i = 0; i < ARRAY_SIZE(chain_exit->links); i++) {
		teak_tcg_chain_link_t *link = &chain_exit->links[i];
		if (link->cache_generation == tcg_block_cache_generation && link->pc == pc)
			return link;
	}
	for (size_t i = 0; i < ARRAY_SIZE(chain_exit->links); i++) {
		teak_tcg_chain_link_t *link = &chain_exit->links[i];
		if (link->cache_generation != tcg_block_cache_generation)
			return link;
	}

	teak_tcg_chain_link_t *link = &chain_exit->links[chain_exit->next_link];
	chain_exit->next_link = (chain_exit->next_link + 1) % ARRAY_SIZE(chain_exit->links);
	return link;
}

static void tcg_chain_exit_link(
	teak_tcg_chain_exit_t *chain_exit,
	const teak_state_t *state,
	const teak_tcg_block_cache_entry_t *entry,
	const void *code
) {
	if (state->repeat_active || state->fetch_split_pending)
		return;
	if (entry->block.fetch_split || entry->block.splits_block_repeat_end)
		return;
	/* a linked jump would skip the fast-loop lookup, and the helper's preparation of a repeat */
	if (entry->fast || entry->dynamic_cycles)
		return;
	if (tcg_active_block_repeat_slots(state) > 1)
		return;
	if (chain_exit->source_mode.specialized)
		g_assert(tcg_mode_is_compatible(entry->mode, chain_exit->source_mode));

	teak_tcg_chain_link_t *link = tcg_chain_exit_select_link(chain_exit, state->pc);
	*link = (teak_tcg_chain_link_t) {
		.cache_generation = tcg_block_cache_generation,
		.code = code,
#ifdef CONFIG_TCG_WASM64
		.evict_epoch = w64_evict_epoch,
#endif
		.pc = state->pc,
		.program_context = entry->program_context,
		.block_repeat_end = state->lp ? state->block_repeat_end[0] : 0,
		.instruction_count = entry->block.instruction_count,
		.block_repeat_active = state->lp,
		.block_repeat_level = state->bcn,
		.mode = tcg_mode_from_state(state),
	};
}

void *HELPER(teak_tcg_chain)(
	void *opaque,
	uint32_t block_cycles,
	uint32_t block_pc,
	void *chain_exit
) {
	teak_state_t *state = opaque;
	teak_tcg_core_t *core = container_of(state, teak_tcg_core_t, state);
	teak_tcg_block_cache_entry_t *entry;

	g_assert(block_cycles != 0);

	block_cycles += core->wait_cycles;
	core->wait_cycles = 0;
	core->last_block_cycles += block_cycles;
	if (state->block_repeat_end_reached) {
		state->block_repeat_end_reached = 0;
		tcg_complete_block_repeat(state);
	}
	if (state->block_repeat_break_pending) {
		state->block_repeat_break_pending = 0;
		tcg_pop_block_repeat(state);
	}

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

	entry = tcg_find_chained_entry(core);
	if (entry == NULL) {
		core->chain_cache_stops++;
		return NULL;
	}
#ifdef CONFIG_TCG_WASM64
	if (entry->evict_epoch != w64_evict_epoch && w64_tc_evicted(entry->code)) {
		/* the slice loop drops it (tcg_drop_evicted_blocks) */
		core->chain_cache_stops++;
		return NULL;
	}
#endif
	if (entry->fast_epoch != tcg_fast_map_epoch) {
		entry->fast = tcg_fast_map[state->pc & TEAK_PROGRAM_ADDRESS_MASK] != 0;
		entry->fast_epoch = tcg_fast_map_epoch;
	}
	if (entry->fast && tcg_fast_lookup(core) != NULL) {
		core->chain_cache_stops++;
		return NULL;
	}
	if (entry->dynamic_cycles || core->chain_cycle_limit - core->last_block_cycles < entry->block.instruction_count) {
		/*
		 * tcg_execute_cached_slice() would come straight back with this block while the slice has cycles left,
		 * whatever the block takes: prepare it the same way.
		 */
		uint32_t entry_cycles = tcg_block_cycles(core, &entry->block);
		uint32_t remaining;

		if (core->last_block_cycles >= core->chain_slice_limit) {
			core->chain_budget_stops++;
			return NULL;
		}
		remaining = core->chain_slice_limit - core->last_block_cycles;
		if (entry->dynamic_cycles)
			core->batch_block_cycles = entry_cycles;
		core->batch_cycles_remaining = MAX(remaining, entry_cycles);
		core->chain_cycle_limit = core->last_block_cycles + core->batch_cycles_remaining;
		core->repeat_stop_repc = tcg_repeat_stop_repc(core, &entry->block, entry_cycles, remaining);
	} else {
		core->batch_cycles_remaining = core->chain_cycle_limit - core->last_block_cycles;
	}
	state->exit_reason = TEAK_EXIT_NONE;
	core->chain_links++;
	core->chain_prev = entry;
#ifdef CONFIG_TCG_WASM64
	if (entry->evict_epoch != w64_evict_epoch) {
		/* goto_ptr tail-calls a tagged table index (exec/translation-block.h) */
		const uint32_t *desc = entry->code;

		if (desc[W64_TCP_FIDX / 4] == 0)
			return (void *) entry->code;
		/* landed: stays so until a batch eviction */
		entry->target = (uintptr_t) (W64_TIDX_TAG | desc[W64_TCP_TIDX / 4]);
		entry->evict_epoch = w64_evict_epoch;
	}
	tcg_chain_exit_link(chain_exit, state, entry, (const void *) entry->target);
	return (void *) entry->target;
#else
	tcg_chain_exit_link(chain_exit, state, entry, entry->code);
	return (void *) entry->code;
#endif
}

static bool tcg_execute_prepared_block(
	teak_tcg_core_t *core,
	const teak_tcg_block_t *block,
	TranslationBlock *tb,
	size_t max_cycles
) {
	uint32_t block_cycles = tcg_block_cycles(core, block);
	uintptr_t exit;

	core->batch_iterations = 0;
	core->batch_block_cycles = block_cycles;
	core->chain_prev = NULL;
	core->batch_cycles_remaining = MAX(max_cycles, (size_t) block_cycles);
	core->chain_cycle_limit = core->last_block_cycles + core->batch_cycles_remaining;
	core->repeat_stop_repc = tcg_repeat_stop_repc(core, block, block_cycles, max_cycles);

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
	bool comparison = operation == TEAK_ALU_CMP || operation == TEAK_ALU_CMPU;
	bool saturating = false;

	switch (operation) {
		case TEAK_ALU_OR:
			result = value | normalized_operand;
			break;
		case TEAK_ALU_AND:
			result = value & normalized_operand;
			break;
		case TEAK_ALU_XOR:
			result = value ^ normalized_operand;
			break;
		case TEAK_ALU_ADD:
		case TEAK_ALU_ADDH:
		case TEAK_ALU_ADDL:
			result = value + normalized_operand;
			tcg_fast_add_sub_flags(state, value, normalized_operand, result, false);
			saturating = true;
			break;
		case TEAK_ALU_CMP:
		case TEAK_ALU_CMPU:
		case TEAK_ALU_SUB:
		case TEAK_ALU_SUBH:
		case TEAK_ALU_SUBL:
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

TEAK_FAST_INLINE uint64_t tcg_fast_aligned_product(const teak_state_t *state) {
	uint64_t product = tcg_fast_shifted_product(state) >> 16;
	return (uint64_t) ((int64_t) (product << 40) >> 40);
}

TEAK_FAST_INLINE void tcg_fast_multiply(teak_state_t *state, uint16_t x, bool unsigned_x, bool unsigned_y) {
	uint16_t y = state->y[0];
	uint32_t x_factor = unsigned_x ? x : (uint32_t) (int32_t) (int16_t) x;
	uint32_t y_factor = unsigned_y ? y : (uint32_t) (int32_t) (int16_t) y;
	uint32_t product = x_factor * y_factor;

	state->x[0] = x;
	state->p[0] = product;
	state->product_extension[0] = unsigned_x && unsigned_y ? 0 : product >> 31;
}

TEAK_FAST_INLINE bool tcg_fast_multiply_supported(teak_multiply_operation_t operation) {
	return operation <= TEAK_MULTIPLY_MSU;
}

TEAK_FAST_INLINE void tcg_fast_multiply_operation(teak_state_t *state, const teak_insn_t *instruction, uint16_t x) {
	uint8_t index = instruction->accumulator_index;
	bool unsigned_x = false;
	bool unsigned_y = false;

	switch (instruction->multiply_operation) {
		case TEAK_MULTIPLY_MPY:
			break;
		case TEAK_MULTIPLY_MPYSU:
			unsigned_x = true;
			break;
		case TEAK_MULTIPLY_MAC:
			tcg_fast_alu_accumulator(state, TEAK_ALU_ADD, index, tcg_fast_shifted_product(state));
			break;
		case TEAK_MULTIPLY_MACUS:
			tcg_fast_alu_accumulator(state, TEAK_ALU_ADD, index, tcg_fast_shifted_product(state));
			unsigned_y = true;
			break;
		case TEAK_MULTIPLY_MACSU:
			tcg_fast_alu_accumulator(state, TEAK_ALU_ADD, index, tcg_fast_shifted_product(state));
			unsigned_x = true;
			break;
		case TEAK_MULTIPLY_MAA:
			tcg_fast_alu_accumulator(state, TEAK_ALU_ADD, index, tcg_fast_aligned_product(state));
			break;
		case TEAK_MULTIPLY_MACUU:
			tcg_fast_alu_accumulator(state, TEAK_ALU_ADD, index, tcg_fast_shifted_product(state));
			unsigned_x = true;
			unsigned_y = true;
			break;
		case TEAK_MULTIPLY_MAASU:
			tcg_fast_alu_accumulator(state, TEAK_ALU_ADD, index, tcg_fast_aligned_product(state));
			unsigned_x = true;
			break;
		case TEAK_MULTIPLY_MSU:
			tcg_fast_alu_accumulator(state, TEAK_ALU_SUB, index, tcg_fast_shifted_product(state));
			break;
	}
	tcg_fast_multiply(state, x, unsigned_x, unsigned_y);
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
		case TEAK_COND_EQ:
			predicate = state->fz;
			break;
		case TEAK_COND_NEQ:
			predicate = state->fz ^ 1U;
			break;
		case TEAK_COND_GT:
			predicate = (state->fz | state->fm) ^ 1U;
			break;
		case TEAK_COND_GE:
			predicate = state->fm ^ 1U;
			break;
		case TEAK_COND_LT:
			predicate = state->fm;
			break;
		case TEAK_COND_LE:
			predicate = state->fm | state->fz;
			break;
		case TEAK_COND_NN:
			predicate = state->fn ^ 1U;
			break;
		case TEAK_COND_C:
			predicate = state->fc0;
			break;
		case TEAK_COND_V:
			predicate = state->fv;
			break;
		case TEAK_COND_E:
			predicate = state->fe;
			break;
		case TEAK_COND_L:
			predicate = state->flm | state->fvl;
			break;
		case TEAK_COND_NR:
			predicate = state->fr ^ 1U;
			break;
		case TEAK_COND_NIU0:
			predicate = state->iu[0] ^ 1U;
			break;
		case TEAK_COND_IU0:
			predicate = state->iu[0];
			break;
		case TEAK_COND_IU1:
			predicate = state->iu[1];
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
	} else if (register_code >= 26 && register_code <= 29) {
		accumulator = tcg_fast_sign_extend(state->a[register_code & 1U]);
		high = register_code >= 28;
	} else if (register_code <= 7) {
		return *tcg_fast_rn_old(state, register_code);
	} else if (register_code == 24 || register_code == 25) {
		return state->a[register_code & 1U];
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

static bool tcg_fast_readable_register(uint8_t register_code) {
	return register_code <= 7 || (register_code >= 16 && register_code <= 19) ||
		(register_code >= 24 && register_code <= 29) || register_code == 31;
}

static bool tcg_fast_supported(const teak_insn_t *instruction) {
	switch (instruction->opcode) {
		case TEAK_OP_NOP:
			return true;
		case TEAK_OP_MOV_DATA_RN_STEP_REGISTER:
			return instruction->register_code == 12 || tcg_fast_readable_register(instruction->register_code);
		case TEAK_OP_MOV_REGISTER_DATA_RN_STEP:
			return tcg_fast_readable_register(instruction->register_code);
		case TEAK_OP_MODA4_ACCUMULATOR:
		case TEAK_OP_MODB3_ACCUMULATOR:
			return instruction->condition < TEAK_COND_COUNT && (instruction->moda_operation == TEAK_MODA_CLR ||
				instruction->moda_operation == TEAK_MODA_CLRR || instruction->moda_operation == TEAK_MODA_SHR ||
				instruction->moda_operation == TEAK_MODA_SHR4);
		case TEAK_OP_MULTIPLY_DUAL_RN:
		case TEAK_OP_MULTIPLY_RN_STEP:
			return tcg_fast_multiply_supported(instruction->multiply_operation);
		case TEAK_OP_ALU_RN_STEP_ACCUMULATOR:
		case TEAK_OP_ALU_REGISTER_ACCUMULATOR:
			if (tcg_alu_uses_multiply(instruction->alu_operation) || instruction->alu_operation == TEAK_ALU_TST0 ||
				instruction->alu_operation == TEAK_ALU_TST1)
				return false;
			return instruction->opcode == TEAK_OP_ALU_RN_STEP_ACCUMULATOR || instruction->register_code == 11 ||
				tcg_fast_readable_register(instruction->register_code);
		case TEAK_OP_MOV_REGISTER_B_ACCUMULATOR:
			return instruction->register_code == 12 || tcg_fast_readable_register(instruction->register_code);
		case TEAK_OP_MOV_REGISTER_REGISTER:
			if (instruction->destination_register_code == 12)
				return true;
			if (instruction->register_code == 11)
				return instruction->destination_register_code == 24 || instruction->destination_register_code == 25;
			return (instruction->register_code == 12 || tcg_fast_readable_register(instruction->register_code)) &&
				tcg_fast_readable_register(instruction->destination_register_code);
		case TEAK_OP_MODIFY_RN:
			return true;
		case TEAK_OP_BRANCH_ABSOLUTE:
		case TEAK_OP_BRANCH_RELATIVE:
			return instruction->condition < TEAK_COND_COUNT;
		case TEAK_OP_MOV_IMM_REGISTER:
			return instruction->register_code <= 7;
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
		case TEAK_OP_NOP:
			break;

		case TEAK_OP_MOV_DATA_RN_STEP_REGISTER: {
			uint8_t code = instruction->register_code;
			uint32_t address = state->r[address_register];
			uint16_t value;

			if (code == 12)
				break;
			if (!tcg_fast_read(map, TEAK_DATA_SPACE_ALL, address, &value))
				return false;
			state->r[address_register] = tcg_fast_rn_next(state, address_register, instruction->step,
				instruction->disable_modulo, address);
			if (code <= 7) {
				*tcg_fast_rn_old(state, code) = value;
			} else if (code == 31) {
				state->shift_value = value;
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

			if (code <= 7) {
				value = *tcg_fast_rn_old(state, code);
			} else if ((code >= 16 && code <= 19) || (code >= 26 && code <= 29)) {
				uint64_t accumulator = tcg_fast_saturate(*tcg_fast_accumulator_register(state, code), state->sat, &flm);

				if (code <= 17 || code >= 28)
					accumulator >>= 16;
				value = accumulator;
			} else {
				value = tcg_fast_mov_read_register(state, code);
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
				case TEAK_MODA_CLR:
					result = 0;
					break;
				case TEAK_MODA_CLRR:
					result = 0x8000;
					break;
				case TEAK_MODA_SHR:
				case TEAK_MODA_SHR4: {
					uint32_t amount = instruction->moda_operation == TEAK_MODA_SHR4 ? 4 : 1;

					result = state->s == 0 ? (uint64_t) (tcg_fast_sign_extend(value) >> amount) : value >> amount;
					if (state->s == 0)
						state->fv = 0;
					state->fc0 = (uint32_t) (value >> (amount - 1)) & 1U;
					break;
				}
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
			} else if (code == 24 || code == 25) {
				tcg_fast_alu_accumulator(state, operation, instruction->accumulator_index,
					state->a[code - 24] & TEAK_ACCUMULATOR_MASK);
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
			uint8_t code = instruction->register_code;
			uint64_t accumulator;

			if (code == 24 || code == 25) {
				accumulator = state->a[code - 24];
			} else {
				uint16_t value = code == 12 ? next_pc : tcg_fast_mov_read_register(state, code);
				accumulator = (uint64_t) (int64_t) (int16_t) value;
			}
			tcg_fast_value_flags(state, accumulator);
			state->b[instruction->accumulator_index] = accumulator;
			break;
		}

		case TEAK_OP_MOV_REGISTER_REGISTER: {
			uint8_t code = instruction->register_code;
			uint8_t destination = instruction->destination_register_code;

			if (destination == 12)
				break;
			if (code == 11) {
				uint64_t product = tcg_fast_shifted_product(state);

				tcg_fast_value_flags(state, product);
				state->a[destination - 24] = product;
				break;
			}
			if ((code == 24 || code == 25) &&
				((destination >= 16 && destination <= 19) || (destination >= 24 && destination <= 29))) {
				uint64_t accumulator = state->a[code - 24];

				tcg_fast_value_flags(state, accumulator);
				*tcg_fast_accumulator_register(state, destination) = accumulator;
				break;
			}
			tcg_mov_write_register(state, destination, code == 12 ? next_pc : tcg_fast_mov_read_register(state, code));
			break;
		}

		case TEAK_OP_MODIFY_RN: {
			uint16_t updated = tcg_fast_rn_next(state, address_register, instruction->step, instruction->disable_modulo,
				state->r[address_register]);

			state->r[address_register] = updated;
			state->fr = updated == 0;
			break;
		}

		case TEAK_OP_BRANCH_ABSOLUTE:
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

/* The addresses @count post-increments of one address register visit, as at most three linear runs. */
typedef struct teak_fast_walk_t {
	uint32_t start[3];
	uint32_t length[3];
	uint32_t runs;
	uint32_t last;
	uint32_t next;
} teak_fast_walk_t;

TEAK_FAST_INLINE bool tcg_fast_walk(const teak_state_t *state, uint8_t address_register, uint32_t address,
	uint32_t count, teak_fast_walk_t *walk
) {
	walk->runs = 0;
	for (;;) {
		uint32_t linear;

		if (address_register < TEAK_MODULO_REGISTERS) {
			const teak_modulo_step_t *modulo = &state->modulo_steps[address_register][TEAK_STEP_INCREASE - 1];
			uint32_t low = address & modulo->mask;

			linear = modulo->mask - low;
			if (modulo->boundary >= low && modulo->boundary - low < linear)
				linear = modulo->boundary - low;
		} else {
			linear = UINT16_MAX - address;
		}
		if (walk->runs == ARRAY_SIZE(walk->start))
			return false;
		walk->start[walk->runs] = address;
		if (count <= linear) {
			walk->length[walk->runs++] = count;
			walk->last = address + count - 1;
			walk->next = address + count;
			return true;
		}
		walk->length[walk->runs++] = linear + 1;
		count -= linear + 1;
		walk->last = address + linear;
		walk->next = tcg_fast_rn_next(state, address_register, TEAK_STEP_INCREASE, false, walk->last);
		if (count == 0)
			return true;
		address = walk->next;
	}
}

/* Where tcg_fast_read() finds each of [@start, @start + @length), or NULL unless that is one flat array. */
static const uint16_t *tcg_fast_read_span(const teak_data_map_t *map, uint32_t start, uint32_t length) {
	const uint16_t *const *pages = map->read_pages[TEAK_DATA_SPACE_ALL];
	uint32_t first = start >> TEAK_DATA_PAGE_SHIFT;
	uint32_t last = (start + length - 1) >> TEAK_DATA_PAGE_SHIFT;

	if (pages[first] == NULL)
		return NULL;
	for (uint32_t page = first + 1; page <= last; page++) {
		if ((uintptr_t) pages[page] !=
			(uintptr_t) pages[first] + (page - first) * TEAK_DATA_PAGE_WORDS * sizeof(uint16_t))
			return NULL;
	}
	return pages[first] + (start & (TEAK_DATA_PAGE_WORDS - 1));
}

/* One read pointer per walked address, in visiting order. */
static bool tcg_fast_walk_reads(const teak_data_map_t *map, const teak_fast_walk_t *walk,
	const uint16_t **pointers
) {
	for (uint32_t run = 0; run < walk->runs; run++) {
		const uint16_t *span = tcg_fast_read_span(map, walk->start[run], walk->length[run]);

		if (span == NULL)
			return false;
		for (uint32_t i = 0; i < walk->length[run]; i++)
			*pointers++ = span + i;
	}
	return true;
}

/* Where tcg_fast_read() finds each run of @walk, or false unless each is one flat array. */
static bool tcg_fast_walk_spans(const teak_data_map_t *map, const teak_fast_walk_t *walk, const uint16_t **spans) {
	for (uint32_t run = 0; run < walk->runs; run++) {
		spans[run] = tcg_fast_read_span(map, walk->start[run], walk->length[run]);
		if (spans[run] == NULL)
			return false;
	}
	return true;
}

/* The words @spans hold for the walked addresses, in visiting order. */
TEAK_FAST_INLINE void tcg_fast_walk_values(const teak_fast_walk_t *walk, const uint16_t *const *spans,
	uint16_t *values
) {
	for (uint32_t run = 0; run < walk->runs; run++)
		for (uint32_t i = 0; i < walk->length[run]; i++)
			*values++ = spans[run][i];
}

/* Whether the walked addresses @first..@last - 1 are all at or above @base. */
TEAK_FAST_INLINE bool tcg_fast_walk_above(const teak_fast_walk_t *walk, uint32_t first, uint32_t last,
	uint32_t base
) {
	uint32_t offset = 0;
	bool above = true;

	for (uint32_t run = 0; run < walk->runs; run++) {
		uint32_t low = MAX(offset, first);
		uint32_t high = MIN(offset + walk->length[run], last);

		/* addresses rise within a run */
		if (low < high)
			above &= walk->start[run] + (low - offset) >= base;
		offset += walk->length[run];
	}
	return above;
}

/* The address of the @index-th walked address. */
TEAK_FAST_INLINE uint32_t tcg_fast_walk_address(const teak_fast_walk_t *walk, uint32_t index) {
	if (index < walk->length[0])
		return walk->start[0] + index;
	index -= walk->length[0];
	return index < walk->length[1] ? walk->start[1] + index : walk->start[2] + index - walk->length[1];
}

/* tcg_fast_alu_accumulator(ADD) without the flags every later instruction of the iteration overwrites. */
TEAK_FAST_INLINE uint64_t tcg_fast_mac_lazy(uint64_t accumulator, uint64_t operand, uint8_t sata, uint8_t *flm,
	uint8_t *fvl
) {
	uint64_t value = accumulator & TEAK_ACCUMULATOR_MASK;
	uint64_t normalized_operand = operand & TEAK_ACCUMULATOR_MASK;
	uint64_t result = value + normalized_operand;

	*fvl |= (uint32_t) (((value ^ result) & ~(value ^ normalized_operand)) >> TEAK_ACCUMULATOR_SIGN_BIT) & 1U;
	return tcg_fast_saturate(tcg_fast_sign_extend(result & TEAK_ACCUMULATOR_MASK), sata, flm);
}

/* How many post-increments of @address_register stay linear from @address, as tcg_fast_walk() counts them. */
TEAK_FAST_INLINE uint32_t tcg_fast_linear(const teak_state_t *state, uint8_t address_register, uint32_t address) {
	const teak_modulo_step_t *modulo = &state->modulo_steps[address_register][TEAK_STEP_INCREASE - 1];
	uint32_t low = address & modulo->mask;
	uint32_t linear = modulo->mask - low;

	if (modulo->boundary >= low && modulo->boundary - low < linear)
		linear = modulo->boundary - low;
	return linear;
}

/*
 * One 35BE iteration on guest memory itself, in the order of the iteration
 * below, for the correlator's usual shape: r0, r1 and r2 linear, the r4
 * accesses in one flat ring (or linear), saturation off.  No walks and no
 * operand copies; false leaves the state alone for the general iteration.
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
 * Whole iterations of the 35BE correlator: the address walks, the budget and
 * the exit flags are checked once at the loop head, and only the flags that
 * survive to the branch are computed.  Anything the checks reject is left to
 * the per-instruction runner.
 */
static uint32_t tcg_fast_iterate_35be(teak_tcg_core_t *core, uint32_t budget) {
	enum { ITERATION_CYCLES = ARRAY_SIZE(tcg_fast_loop_35be), MULTIPLIES = 26, X_READS = 13, R4_ACCESSES = 30 };
	static const uint8_t r4_store_index[4] = { 0, 1, 28, 29 };
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
		teak_fast_walk_t r0_walk, r1_walk, r2_walk, r4_walk;
		const uint16_t *source[4];
		const uint16_t *x_spans[ARRAY_SIZE(r2_walk.start)];
		const uint16_t *r4_spans[ARRAY_SIZE(r4_walk.start)];
		uint16_t x_values[X_READS];
		uint16_t r4_values[R4_ACCESSES];
		uint16_t *r4_store[4];
		uint16_t *r1_store[2];
		uint64_t a0;
		uint64_t a1;
		uint64_t stored;
		uint16_t value;
		bool ok = true;

		if (tcg_fast_iterate_35be_flat(&state, map, y_space_base))
			goto iterated;
		if (!tcg_fast_walk(&state, 0, state.r[0], 4, &r0_walk) || !tcg_fast_walk(&state, 1, state.r[1], 2, &r1_walk) ||
			!tcg_fast_walk(&state, 2, state.r[2], X_READS, &r2_walk) ||
			!tcg_fast_walk(&state, 4, state.r[4], R4_ACCESSES, &r4_walk))
			break;
		/* X reads below Y space, Y reads (r4 accesses 2..27) in it. */
		for (uint32_t run = 0; run < r2_walk.runs; run++)
			ok &= r2_walk.start[run] + r2_walk.length[run] <= y_space_base;
		ok &= tcg_fast_walk_above(&r4_walk, 2, 2 + MULTIPLIES, y_space_base);
		for (uint32_t i = 0; i < 4; i++) {
			r4_store[i] = tcg_fast_write_pointer(map, tcg_fast_walk_address(&r4_walk, r4_store_index[i]));
			ok &= r4_store[i] != NULL;
		}
		for (uint32_t i = 0; i < 2; i++) {
			r1_store[i] = tcg_fast_write_pointer(map, tcg_fast_walk_address(&r1_walk, i));
			ok &= r1_store[i] != NULL;
		}
		if (!ok || !tcg_fast_walk_reads(map, &r0_walk, source) ||
			!tcg_fast_walk_spans(map, &r2_walk, x_spans) || !tcg_fast_walk_spans(map, &r4_walk, r4_spans))
			break;

		value = *source[0];
		*r4_store[0] = value;
		value = *source[1];
		*r4_store[1] = value;
		/* after the stores: in a short ring the last reads are their addresses */
		tcg_fast_walk_values(&r2_walk, x_spans, x_values);
		tcg_fast_walk_values(&r4_walk, r4_spans, r4_values);
		a0 = 0x8000;
		a1 = 0x8000;

		if (state.sata && state.product_shift <= 2) {
			/*
			 * From 0x8000, 13 products of magnitude at most 2^31 (shifted left
			 * by at most 1) cannot overflow 36 bits: without saturation the
			 * chain is two plain sums.
			 */
			uint32_t shift = state.product_shift;
			uint32_t right = shift == 1;
			int64_t sum[2] = { 0x8000, 0x8000 };
			uint16_t x = 0, y = 0;
			uint32_t product = 0;

			shift -= shift != 0;
			TEAK_FAST_UNROLL
			for (uint32_t i = 0; i < MULTIPLIES; i++) {
				x = x_values[i / 2];
				y = r4_values[2 + i];
				product = (uint32_t) ((int32_t) (int16_t) x * (int32_t) (int16_t) y);
				if (i < MULTIPLIES - 1)
					sum[i & 1] += (int64_t) ((uint64_t) (int64_t) (int32_t) product << (shift & 63U)) >> right;
			}
			a0 = sum[0];
			a1 = sum[1];
			state.x[0] = x;
			state.y[0] = y;
			state.p[0] = product;
			state.product_extension[0] = product >> 31;
		} else {
			/* the chain on locals: state is in memory, its fields would round-trip */
			uint32_t shift = state.product_shift;
			uint32_t right = shift == 1;
			uint8_t sata = state.sata, flm = state.flm, fvl = state.fvl;
			uint16_t x = x_values[0], y = r4_values[2];
			uint32_t product = (uint32_t) ((int32_t) (int16_t) x * (int32_t) (int16_t) y);

			shift -= shift != 0;
			TEAK_FAST_UNROLL
			for (uint32_t i = 1; i < MULTIPLIES; i++) {
				uint64_t shifted = (uint64_t) product | (uint64_t) (product >> 31) * 0xF00000000ULL;

				shifted = (uint64_t) ((int64_t) ((uint64_t) tcg_fast_sign_extend(shifted) << (shift & 63U)) >> right);
				if (i & 1)
					a0 = tcg_fast_mac_lazy(a0, shifted, sata, &flm, &fvl);
				else
					a1 = tcg_fast_mac_lazy(a1, shifted, sata, &flm, &fvl);
				y = r4_values[2 + i];
				x = x_values[i / 2];
				product = (uint32_t) ((int32_t) (int16_t) x * (int32_t) (int16_t) y);
			}
			state.x[0] = x;
			state.y[0] = y;
			state.p[0] = product;
			state.product_extension[0] = product >> 31;
			state.flm = flm;
			state.fvl = fvl;
		}

		state.a[0] = a0;
		state.a[1] = a1;
		tcg_fast_alu_accumulator(&state, TEAK_ALU_ADD, 1, tcg_fast_shifted_product(&state));
		stored = tcg_fast_saturate(state.a[0], state.sat, &state.flm);
		*r1_store[0] = stored >> 16;
		stored = tcg_fast_saturate(state.a[1], state.sat, &state.flm);
		*r1_store[1] = stored >> 16;

		value = *source[2];
		*r4_store[2] = value;
		value = *source[3];
		state.a[0] = (uint64_t) (int64_t) (int16_t) value;
		tcg_fast_value_flags(&state, state.a[0]);
		*r4_store[3] = value;

		state.r[0] = r0_walk.next;
		state.r[1] = r1_walk.next;
		state.r[2] = tcg_fast_rn_next(&state, 2, TEAK_STEP_PLUS_STEP, false, r2_walk.last);
		state.r[4] = r4_walk.next;
iterated:
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

/* Bumped whenever tcg_fast_map changes; never 0. */
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
		if (!tcg_fast_supported(&loop->instructions[i]))
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
		for (size_t j = 0; j < tcg_fast_loops[i].count; j++)
			tcg_fast_map[tcg_fast_loops[i].instructions[j].address] = i + 1;
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
		tcg_fast_map_epoch++;
		tcg_block_cache_generation++;
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

static void tcg_fast_account(teak_tcg_core_t *core, uint32_t cycles) {
	core->last_block_cycles += cycles;
}

static bool tcg_execute_fast(teak_tcg_core_t *core, const teak_tcg_fast_site_t *site, size_t max_cycles) {
	uint32_t cycles = tcg_fast_site_run(core, site, max_cycles - core->last_block_cycles);

	if (cycles == 0)
		return false;
	tcg_fast_account(core, cycles);
	core->fast_runs++;
	return true;
}

bool teak_tcg_execute_block(teak_tcg_core_t *core) {
	teak_tcg_block_t block;
	TranslationBlock *tb;
	uint32_t block_cycles;

	core->translation_error_address = core->state.pc;
	core->translation_error = TEAK_TRANSLATION_ERROR_NONE;
	core->last_block_cycles = 0;
	core->advanced_cycles = 0;
	core->synchronization_valid = false;
	core->wait_cycles = 0;
	core->chain_slice_limit = 0;

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
	bool stable_block = !core->state.lp;

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
		if (core->state.lp)
			continue_block = false;
		if (core->state.exit_reason != TEAK_EXIT_BRANCH)
			continue_block = false;

		if (!continue_block)
			break;

		core->state.exit_reason = TEAK_EXIT_NONE;
	} while (core->last_block_cycles < max_cycles);
	return true;
}

static bool tcg_execute_cached_slice(teak_tcg_core_t *core, teak_tcg_block_cache_entry_t *entry, size_t max_cycles,
	bool *fast_next
) {
	core->chain_slice_limit = max_cycles;
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
		if (tcg_fast_map[core->state.pc & TEAK_PROGRAM_ADDRESS_MASK] != 0 && tcg_fast_lookup(core) != NULL) {
			core->state.exit_reason = TEAK_EXIT_NONE;
			*fast_next = true;
			break;
		}

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
	core->advanced_cycles = 0;
	core->synchronization_valid = false;
	core->wait_cycles = 0;
	core->chain_slice_limit = 0;

	if (qatomic_xchg(&core->state.exit_request, 0) != 0)
		return true;

	tcg_check_block_cache();
	tcg_fast_init();
	success = true;
	while (true) {
		const teak_tcg_fast_site_t *site = NULL;
		bool fast_next = false;

		if (tcg_fast_map[core->state.pc & TEAK_PROGRAM_ADDRESS_MASK] != 0)
			site = tcg_fast_lookup(core);
		if (site != NULL && tcg_execute_fast(core, site, max_cycles)) {
			if (qatomic_xchg(&core->state.exit_request, 0) != 0)
				break;

			bool interrupt_requested = qatomic_xchg(&core->state.interrupt_request, 0) != 0;
			bool interrupt_pending = interrupt_requested || tcg_pending_interrupts(&core->state) != 0;
			if (interrupt_pending && teak_tcg_service_interrupt(core))
				core->chain_interrupts++;
			if (core->last_block_cycles >= max_cycles)
				break;
			core->state.exit_reason = TEAK_EXIT_NONE;
			continue;
		}

		entry = tcg_find_cached_entry_fast(core);
		if (entry == NULL) {
			if (core->last_block_cycles == 0)
				success = tcg_execute_slice_slow(core, max_cycles);
			break;
		}
		success = tcg_execute_cached_slice(core, entry, max_cycles, &fast_next);
		if (!success || !fast_next)
			break;
	}

	tcg_flush_cycles(core);
	return success;
}
