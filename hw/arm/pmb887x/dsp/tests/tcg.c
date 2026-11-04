#include "qemu/osdep.h"
#include "qemu-main.h"
#include "qemu/bitops.h"
#include "qemu/units.h"
#include "tcg/startup.h"

#include "hw/arm/pmb887x/dsp/core.h"
#include "hw/arm/pmb887x/dsp/tcg.h"

#define TEST_MEMORY_WORDS	0x200
#define TEST_Y_SPACE_BASE	0x100
#define TEST_CYCLE_SENSITIVE_ADDRESS	0x80
#define TEST_CYCLE_LOG_STORES	16
#define TEST_TCG_CODE_SIZE	(256 * MiB)
#define TEST_ACCUMULATOR_BITS	36
#define TEST_BRANCH_WORDS	2
#define TEST_PRODUCT_SHIFTS	4
#define TEST_MODE_KEYED_CHAIN_BLOCKS	6
#define TEST_BLOCK_EQUIVALENCE_INSTRUCTIONS	16
#define TEST_BLOCK_EQUIVALENCE_ITERATIONS	1000
#define TEST_BLOCK_EQUIVALENCE_ADDRESS_REGISTERS	6
#define TEST_BLOCK_EQUIVALENCE_ADDRESS_SPREAD	12
#define TEST_BLOCK_EQUIVALENCE_MODULO_LIMIT	0x20
#define TEST_BLOCK_EQUIVALENCE_MAX_BLOCKS	(2 * TEST_BLOCK_EQUIVALENCE_INSTRUCTIONS)

typedef struct test_memory_t test_memory_t;

struct test_memory_t {
	uint16_t words[TEST_MEMORY_WORDS];
	teak_tcg_core_t *exit_core_on_write;
};

typedef struct test_cycle_log_t test_cycle_log_t;

struct test_cycle_log_t {
	test_memory_t data;
	size_t advanced_cycles;
	size_t store_cycles[TEST_CYCLE_LOG_STORES];
	size_t stores;
};

typedef struct test_block_equivalence_trace_t test_block_equivalence_trace_t;

struct test_block_equivalence_trace_t {
	teak_state_t states[TEST_BLOCK_EQUIVALENCE_INSTRUCTIONS + 1];
	bool recorded[TEST_BLOCK_EQUIVALENCE_INSTRUCTIONS + 1];
};

int (*qemu_main)(void);

static const uint16_t test_block_equivalence_pool[] = {
	0xD022, /* mpy [r4++] [r2] a0 */
	0xD22A, /* mac [r4++] [r2++] a0 */
	0xDA22, /* mac [r4++] [r2] a1 */
	0xDA2A, /* mac [r4++] [r2++] a1 */
	0x92A0, /* addh r0 a0 */
	0x82A1, /* and r1 a0 */
	0x80A4, /* or r4 a0 */
	0x8CA5, /* cmp r5 a0 */
	0x8DA0, /* cmp r0 a1 */
	0x8FB8, /* sub a0 a1 */
	0x6780, /* not a0 always */
	0x6760, /* clr a0 always */
	0x67E0, /* dec a0 always */
	0x6700, /* shr a0 always */
	0x67D0, /* inc a0 always */
	0x6761, /* clr a0 eq */
	0x7765, /* clr a1 lt */
	0x67F1, /* copy a0 eq */
	0x77F8, /* copy a1 c */
	0x67E4, /* dec a0 ge */
	0x676A, /* clr a0 e */
	0x1F08, /* mov [r0++] a0 */
	0x1F28, /* mov [r0++] a1 */
	0x1F00, /* mov [r0] a0 */
	0x1B48, /* mov a0l [r0++] */
	0x1B60, /* mov a1l [r0] */
	0x1B4A, /* mov a0l [r2++] */
	0x0088, /* modr [r0++] */
	0x0090, /* modr [r0--] */
	0x008A, /* modr [r2++] */
	0x911A, /* tstb a0l 0x0001 */
	0x9020, /* tstb [r0] 0x0000 */
	0x5B48, /* mov st0 a0l */
	0x5900, /* mov r0 st0 */
	0x4D81, /* load 0x0001 ps */
	0x4D83, /* load 0x0003 ps */
	0x5940, /* mov r0 st2 */
	0x593A, /* mov a0l st1 */
};

static uint16_t test_memory_read(void *opaque, uint32_t address) {
	test_memory_t *memory = opaque;

	g_assert_cmpuint(address, <, ARRAY_SIZE(memory->words));
	return memory->words[address];
}

static void test_memory_write(void *opaque, uint32_t address, uint16_t value) {
	test_memory_t *memory = opaque;

	g_assert_cmpuint(address, <, ARRAY_SIZE(memory->words));
	memory->words[address] = value;
	if (memory->exit_core_on_write != NULL)
		memory->exit_core_on_write->state.exit_request = 1;
}

static teak_memory_space_t test_memory_space(test_memory_t *memory) {
	teak_memory_space_t space = {
		.opaque = memory,
		.read = test_memory_read,
		.write = test_memory_write,
	};

	return space;
}

static teak_tcg_core_t test_core_create(test_memory_t *program, test_memory_t *data) {
	teak_memory_t memory = {
		.program = test_memory_space(program),
		.data = test_memory_space(data),
		.y_space_base = TEST_Y_SPACE_BASE,
	};
	teak_tcg_core_t core;

	teak_tcg_init(&core, &memory);
	return core;
}

static void test_core_map_data(teak_tcg_core_t *core, test_memory_t *data) {
	teak_data_map_read(core, 0, ARRAY_SIZE(data->words), data->words);
	teak_data_map_write(core, 0, ARRAY_SIZE(data->words), data->words);
}

static void test_execute_until(teak_tcg_core_t *core, uint16_t pc, size_t max_blocks) {
	for (size_t i = 0; i < max_blocks && core->state.pc != pc; i++)
		g_assert_true(teak_tcg_execute_block(core));
	g_assert_cmphex(core->state.pc, ==, pc);
}

static void test_execute_slices_until(teak_tcg_core_t *core, uint16_t pc, size_t max_slices) {
	for (size_t i = 0; i < max_slices && core->state.pc != pc; i++)
		g_assert_true(teak_tcg_execute_slice(core, 1000));
	g_assert_cmphex(core->state.pc, ==, pc);
}

static void test_mov_a0_b0(uint16_t opcode) {
	test_memory_t program = {
		.words = { opcode, 0x4180, 0x0003 },
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.a[0] = 0x110101011ULL;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.b[0], ==, 0x110101011ULL);
	g_assert_cmphex(core.state.fz, ==, 0);
	g_assert_cmphex(core.state.fm, ==, 0);
	g_assert_cmphex(core.state.fn, ==, 0);
	g_assert_cmphex(core.state.fe, ==, 1);
	g_assert_cmphex(core.state.pc, ==, 3);
}

static void test_mov_full_accumulator(void) {
	/* 0xDA90: mov a0,b0; dsp-lle-instructions hardware case. */
	test_mov_a0_b0(0xDA90);
}

static void test_mov_full_accumulator_alias(void) {
	/* 0x5ED8: mov a0,b0 through the REG-to-bX encoding (TeakLite 4.41, mov REG,bX). */
	test_mov_a0_b0(0x5ED8);
}

static void test_mov_immediate_product_high(void) {
	/* 0x5E0B 0x0090: mov 0x0090,p (TeakLite 4.41, mov ##long immediate,REG). */
	test_memory_t program = {
		.words = { 0x5E0B, 0x0090, 0x4180, 0x0004 },
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.p[0] = 0x12345678;
	core.state.product_extension[0] = 1;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.p[0], ==, 0x00905678);
	g_assert_cmphex(core.state.product_extension[0], ==, 0);
	g_assert_cmphex(core.state.pc, ==, 4);
}

static void test_pop_full_accumulator(void) {
	test_memory_t program = {
		.words = { 0x5E78, 0x4180, 0x0003 },
	};
	test_memory_t data = {
		.words = { [10] = 0x8001 },
	};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.sp = 10;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 0xFFFFFFFFFFFF8001ULL);
	g_assert_cmphex(core.state.sp, ==, 11);
	g_assert_cmphex(core.state.fz, ==, 0);
	g_assert_cmphex(core.state.fm, ==, 1);
	g_assert_cmphex(core.state.fn, ==, 0);
	g_assert_cmphex(core.state.fe, ==, 0);
	g_assert_cmphex(core.state.pc, ==, 3);
}

static void test_long_block_repeat(void) {
	test_memory_t program = {
		.words = { 0x5C02, 0x0014 },
	};
	test_memory_t data = {};
	teak_tcg_core_t core;

	for (size_t i = 2; i <= 20; i++)
		program.words[i] = 0x67D0;
	program.words[21] = 0x4180;
	program.words[22] = 0x0017;
	core = test_core_create(&program, &data);

	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.pc, ==, 2);
	g_assert_cmphex(core.state.block_repeat_start[0], ==, 2);
	g_assert_cmphex(core.state.block_repeat_end[0], ==, 20);
	g_assert_cmphex(core.state.block_repeat_lc[0], ==, 2);
	g_assert_cmphex(core.state.lp, ==, 1);
	g_assert_cmphex(core.state.bcn, ==, 1);

	test_execute_until(&core, 23, 16);
	g_assert_cmphex(core.state.a[0], ==, 57);
	g_assert_cmphex(core.state.block_repeat_lc[0], ==, 0);
	g_assert_cmphex(core.state.lp, ==, 0);
	g_assert_cmphex(core.state.bcn, ==, 0);
}

static void test_interrupt_block_repeat(void) {
	test_memory_t program = {
		.words = {
			[20] = 0x5D06, [21] = 0x0019, /* bkrep r7, 0x0019. */
			[22] = 0x67D0, /* inc a0. */
			[23] = 0x67D0, /* inc a0. */
			[24] = 0x67D0, /* inc a0. */
			[25] = 0x67D0, /* inc a0. */
			[26] = 0x45C0, /* reti true. */
		},
	};
	test_memory_t data = {
		.words = { [10] = 5 },
	};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.pc = 20;
	core.state.sp = 10;
	core.state.r[7] = 1;
	core.state.ie = 0;
	core.state.maskable_interrupt_active = 1;
	core.state.lp = 1;
	core.state.bcn = 1;
	core.state.block_repeat_start[0] = 2;
	core.state.block_repeat_end[0] = 10;
	core.state.block_repeat_lc[0] = 7;

	test_execute_until(&core, 5, 16);
	g_assert_cmphex(core.state.a[0], ==, 8);
	g_assert_cmphex(core.state.block_repeat_start[0], ==, 2);
	g_assert_cmphex(core.state.block_repeat_end[0], ==, 10);
	g_assert_cmphex(core.state.block_repeat_lc[0], ==, 7);
	g_assert_cmphex(core.state.bcn, ==, 1);
	g_assert_cmphex(core.state.lp, ==, 1);
	g_assert_cmphex(core.state.sp, ==, 11);
	g_assert_cmphex(core.state.ie, ==, 1);
	g_assert_cmphex(core.state.maskable_interrupt_active, ==, 0);
}

static void test_repeat_cycle_budget(void) {
	test_memory_t program = {
		.words = {
			0x0CFF, /* rep 0x00ffu8. */
			0x67D0, /* inc a0. */
			0x4180, 0x0002, /* br 0x0002. */
		},
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	g_assert_true(teak_tcg_execute_slice(&core, 10));
	g_assert_cmpuint(core.last_block_cycles, ==, 10);
	g_assert_cmphex(core.state.a[0], ==, 9);
	g_assert_cmphex(core.state.pc, ==, 1);
	g_assert_cmphex(core.state.repeat_active, ==, 1);
	g_assert_cmphex(core.state.repc, ==, 246);

	test_execute_slices_until(&core, 2, 4);
	g_assert_cmphex(core.state.a[0], ==, 256);
	g_assert_cmphex(core.state.repeat_active, ==, 0);
}

static void test_batched_loop_wait_state_budget(void) {
	test_memory_t program = {
		.words = {
			0xD4B8, 0x0010, /* mov [0x0010] a0. */
			0x4180, 0x0000, /* br 0x0000. */
		},
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.memory.wait_state_base = 0x10;
	core.memory.wait_state_size = 1;
	core.memory.wait_state_cycles = 2;
	g_assert_true(teak_tcg_execute_slice(&core, 40));
	g_assert_cmpuint(core.last_block_cycles, ==, 40);
}

static void test_repeated_instruction_without_repeat(void) {
	test_memory_t program = {
		.words = {
			0x0CFF, /* rep 0x00ffu8. */
			0x67D0, /* inc a0. */
			0x4180, 0x0002, /* br 0x0002. */
		},
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	g_assert_true(teak_tcg_execute_slice(&core, 10));
	g_assert_cmphex(core.state.repeat_active, ==, 1);

	core.state.pc = 1;
	core.state.repeat_active = 0;
	core.state.a[0] = 0;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 1);
	g_assert_cmphex(core.state.pc, ==, 2);
}

static void test_interrupt_repeat_nested_repeat(void) {
	test_memory_t program = {
		.words = {
			0x0C1F, /* rep 0x001fu8. */
			0x67D0, /* inc a0. */
			0x4180, 0x0002, /* br 0x0002. */
			[6] = 0x0C03, /* rep 0x0003u8. */
			[7] = 0x77D0, /* inc a1. */
			[8] = 0x45C0, /* reti true. */
		},
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.sp = 10;
	g_assert_true(teak_tcg_execute_slice(&core, 5));
	g_assert_cmphex(core.state.a[0], ==, 4);
	g_assert_cmphex(core.state.pc, ==, 1);

	core.state.ie = 1;
	core.state.interrupt_mask = BIT(0);
	core.state.pending_interrupts = BIT(0);
	g_assert_true(teak_tcg_service_interrupt(&core));
	g_assert_cmphex(core.state.pc, ==, 6);
	g_assert_cmphex(data.words[9], ==, 1);

	test_execute_slices_until(&core, 2, 4);
	g_assert_cmphex(core.state.a[1], ==, 4);
	g_assert_cmphex(core.state.a[0], ==, 32);
	g_assert_cmphex(core.state.sp, ==, 10);
	g_assert_cmphex(core.state.repeat_active, ==, 0);
	g_assert_cmphex(core.state.repeat_suspended, ==, 0);
}

static void test_interrupt_repeat_resumes_on_return(void) {
	test_memory_t program = {
		.words = {
			0x0C1F, /* rep 0x001fu8. */
			0x67D0, /* inc a0. */
			0x4180, 0x0002, /* br 0x0002. */
			[6] = 0x45C0, /* reti true. */
		},
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.sp = 10;
	g_assert_true(teak_tcg_execute_slice(&core, 5));
	g_assert_cmphex(core.state.a[0], ==, 4);
	core.state.ie = 1;
	core.state.interrupt_mask = BIT(0);
	core.state.pending_interrupts = BIT(0);
	g_assert_true(teak_tcg_service_interrupt(&core));

	core.state.pc = 1;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 5);
	g_assert_cmphex(core.state.pc, ==, 2);
	g_assert_cmphex(core.state.repeat_suspended, ==, 1);

	core.state.pc = 6;
	test_execute_slices_until(&core, 2, 4);
	g_assert_cmphex(core.state.a[0], ==, 33);
	g_assert_cmphex(core.state.repeat_suspended, ==, 0);
}

static void test_subroutine_block_repeat(void) {
	test_memory_t program = {
		.words = {
			[0] = 0x5E05, [1] = 0x0003, /* mov 0x0003, r5. */
			[2] = 0x5D05, [3] = 0x000C, /* bkrep r5, 0x000c. */
			[4] = 0x41C0, [5] = 0x0014, /* call 0x0014, true. */
			[6] = 0x67D0, /* inc a0, true. */
			[7] = 0x0000, /* nop. */
			[8] = 0x0000, /* nop. */
			[9] = 0x0000, /* nop. */
			[10] = 0x0000, /* nop. */
			[11] = 0x0000, /* nop. */
			[12] = 0x0000, /* nop. */
			[13] = 0x4180, [14] = 0x000F, /* br 0x000f, true. */
			[15] = 0x4180, [16] = 0x000F, /* br 0x000f, true. */
			[20] = 0x5E06, [21] = 0x0002, /* mov 0x0002, r7. */
			[22] = 0x5D06, [23] = 0x001C, /* bkrep r7, 0x001c. */
			[24] = 0x77D0, /* inc a1, true. */
			[25] = 0x0000, /* nop. */
			[26] = 0x0000, /* nop. */
			[27] = 0x0000, /* nop. */
			[28] = 0x0000, /* nop. */
			[29] = 0x4580, /* ret true. */
		},
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.sp = 10;
	test_execute_until(&core, 15, 128);
	g_assert_cmphex(core.state.a[0], ==, 4);
	g_assert_cmphex(core.state.a[1], ==, 12);
	g_assert_cmphex(core.state.sp, ==, 10);
	g_assert_cmphex(core.state.lp, ==, 0);
	g_assert_cmphex(core.state.bcn, ==, 0);
}

static void test_status_reserved_read_bits(void) {
	test_memory_t program = {
		.words = { 0x5809, 0x582A, 0x4180, 0x0004 },
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.r[0], ==, 0x0300);
	g_assert_cmphex(core.state.r[1], ==, 0x1000);
}

static void test_multiply_status_register(void) {
	test_memory_t program = {
		.words = { 0x8048, 0x4180, 0x0003 },
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.y[0] = 2;
	core.state.sat = 1;
	core.state.ie = 1;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.x[0], ==, 3);
	g_assert_cmphex(core.state.p[0], ==, 6);
}

static void test_product_bit_tests(void) {
	/* TeakLite 4.41, 4.2.2 e and f: p is read as the shifted p sign-extended to 36 bits. */
	static const struct {
		uint16_t opcode;
		uint32_t product;
		uint8_t product_extension;
		uint8_t product_shift;
		uint16_t a0;
		bool expected_fz;
	} cases[] = {
		{ 0x88AB, 0x00010000, 0, 0, 0x8000, false }, /* tst0 a0l,p. */
		{ 0x88AB, 0x00000003, 0, 0, 0x8000, true }, /* tst0 a0l,p. */
		{ 0x88AB, 0x06260060, 0, 1, 0x0040, true }, /* tst0 a0l,p. */
		{ 0x8AAB, 0xFFFFFFFF, 1, 0, 0xFFFF, true }, /* tst1 a0l,p. */
		{ 0x8AAB, 0x00010000, 0, 0, 0xFFFF, false }, /* tst1 a0l,p. */
		{ 0x9F0B, 0xFFFFFFFF, 1, 0, 0x0000, true }, /* tstb p,#15. */
		{ 0x9F0B, 0x00010000, 0, 0, 0x0000, false }, /* tstb p,#15. */
		{ 0x960B, 0x06260060, 0, 0, 0x0000, true }, /* tstb p,#6. */
		{ 0x960B, 0x06260060, 0, 1, 0x0000, false }, /* tstb p,#6. */
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		test_memory_t program = {
			.words = { cases[i].opcode, 0x4180, 0x0003 }, /* opcode; br 0x0003. */
		};
		test_memory_t data = {};
		teak_tcg_core_t core = test_core_create(&program, &data);

		core.state.p[0] = cases[i].product;
		core.state.product_extension[0] = cases[i].product_extension;
		core.state.product_shift = cases[i].product_shift;
		core.state.a[0] = cases[i].a0;
		core.state.fz = !cases[i].expected_fz;
		g_assert_true(teak_tcg_execute_block(&core));
		g_assert_cmphex(core.state.fz, ==, cases[i].expected_fz);
	}
}

static void test_alu_status_register(void) {
	test_memory_t program = {
		.words = { 0x80A8, 0x4180, 0x0003 },
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.sat = 1;
	core.state.ie = 1;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 3);
}

static uint16_t test_zero_data_read(void *opaque, uint32_t address) {
	return 0;
}

static void test_modulo_address(void) {
	static const struct {
		uint8_t address_register;
		uint16_t modulo;
		uint16_t address;
		int8_t step;
		uint16_t expected;
	} vectors[] = {
		{ 0, 6, 0x0010, 1, 0x0011 },
		{ 0, 6, 0x0016, 1, 0x0010 },
		{ 2, 6, 0x0014, 2, 0x0016 },
		{ 2, 6, 0x0016, 2, 0x0010 },
		{ 4, 6, 0x0010, -3, 0x0016 },
		{ 5, 6, 0x0013, -3, 0x0010 },
		{ 1, 5, 0x0016, 3, 0x0011 },
		{ 1, 5, 0x0015, 3, 0x0010 },
		{ 3, 0, 0x1234, 1, 0x1234 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(vectors); i++) {
		uint8_t address_register = vectors[i].address_register;
		int8_t step = vectors[i].step;
		teak_step_t step_mode = step == 1 ? TEAK_STEP_INCREASE :
			step == -1 ? TEAK_STEP_DECREASE : TEAK_STEP_PLUS_STEP;
		bool is_i_register = address_register < 4;
		uint16_t load_modulo = (is_i_register ? 0x0200U : 0x0A00U) | vectors[i].modulo;
		uint16_t load_step = (is_i_register ? 0xDB80U : 0xDF80U) | (step & 0x7F);
		uint16_t mov_data = 0x98C0U | step_mode << 3 | address_register;
		test_memory_t program = {};
		size_t words = 0;

		program.words[words++] = load_modulo;
		if (step_mode == TEAK_STEP_PLUS_STEP)
			program.words[words++] = load_step;
		program.words[words] = mov_data;
		test_memory_t data = {};
		teak_tcg_core_t core = test_core_create(&program, &data);

		core.memory.data.read = test_zero_data_read;
		core.state.r[address_register] = vectors[i].address;
		core.state.modulo_enable = BIT(address_register);
		g_assert_true(teak_tcg_execute_block(&core));
		g_assert_cmphex(core.state.r[address_register], ==, vectors[i].expected);
	}
}

static void test_dual_memory_spaces(void) {
	test_memory_t program = {
		.words = { 0xD000, 0x4180, 0x0003 },
	};
	test_memory_t data = {
		.words = { [0x10] = 0xFFFF, [TEST_Y_SPACE_BASE - 8] = 0x8001, [TEST_Y_SPACE_BASE] = 0x8001 },
	};
	teak_tcg_core_t core = test_core_create(&program, &data);

	test_core_map_data(&core, &data);
	core.state.r[0] = 0x10;
	core.state.r[4] = TEST_Y_SPACE_BASE - 8;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.x[0], ==, 0xFFFF);
	g_assert_cmphex(core.state.y[0], ==, 0);
	g_assert_cmphex(core.state.p[0], ==, 0);

	core = test_core_create(&program, &data);
	test_core_map_data(&core, &data);
	core.state.r[0] = 0x10;
	core.state.r[4] = TEST_Y_SPACE_BASE;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.x[0], ==, 0xFFFF);
	g_assert_cmphex(core.state.y[0], ==, 0x8001);
	g_assert_cmphex(core.state.p[0], ==, 0x7FFF);
}

static void test_direct_data_read_write(void) {
	test_memory_t program = {
		.words = { 0x1B40, 0x1F41, 0x4180, 0x0004 },
	};
	test_memory_t data = {
		.words = { [0x10] = 0x8001 },
	};
	teak_tcg_core_t core = test_core_create(&program, &data);

	test_core_map_data(&core, &data);
	core.state.a[0] = 0xA55A;
	core.state.r[0] = 0x11;
	core.state.r[1] = 0x10;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(data.words[0x11], ==, 0xA55A);
	g_assert_cmphex(core.state.a[0], ==, 0x8001);
}

static void test_program_context_cache(void) {
	test_memory_t program = {
		/* inc a0; br 0x0003 always. */
		.words = { 0x67D0, 0x4180, 0x0003 },
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 1);
	g_assert_cmpuint(core.cache_compiles, ==, 1);

	/* dec a0; br 0x0003 always. */
	program.words[0] = 0x67E0;
	core.state.pc = 0;
	teak_tcg_set_program_context(&core, 1);
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 0);
	g_assert_cmpuint(core.cache_compiles, ==, 2);

	program.words[0] = 0x67D0;
	core.state.pc = 0;
	teak_tcg_set_program_context(&core, 0);
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 1);
	g_assert_cmpuint(core.cache_compiles, ==, 2);
}

static void test_program_invalidation(void) {
	test_memory_t program = {
		.words = {
			0x67D0, /* inc a0. */
			0x67D0, /* inc a0. */
			0x4180, 0x0004, /* br 0x0004, true. */
		},
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 2);
	g_assert_cmpuint(core.cache_compiles, ==, 1);

	program.words[1] = 0x67E0; /* dec a0. */
	teak_tcg_invalidate_program(&core, 1);
	core.state.pc = 0;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 2);
	g_assert_cmpuint(core.cache_compiles, ==, 2);
}

static uint64_t test_random_accumulator(void) {
	uint64_t value = ((uint64_t) g_test_rand_int() << 32) | (uint32_t) g_test_rand_int();
	return (uint64_t) sextract64(value, 0, TEST_ACCUMULATOR_BITS);
}

static void test_block_equivalence_randomize(teak_state_t *state) {
	state->a[0] = test_random_accumulator();
	state->a[1] = test_random_accumulator();
	state->b[0] = test_random_accumulator();
	state->b[1] = test_random_accumulator();
	state->p[0] = g_test_rand_int();
	state->x[0] = g_test_rand_int();
	state->y[0] = g_test_rand_int();
	for (size_t i = 0; i < TEST_BLOCK_EQUIVALENCE_ADDRESS_REGISTERS; i++) {
		state->r[i] = g_test_rand_int_range(TEST_Y_SPACE_BASE - TEST_BLOCK_EQUIVALENCE_ADDRESS_SPREAD,
			TEST_Y_SPACE_BASE + TEST_BLOCK_EQUIVALENCE_ADDRESS_SPREAD);
	}
	state->fz = g_test_rand_bit();
	state->fm = g_test_rand_bit();
	state->fn = g_test_rand_bit();
	state->fv = g_test_rand_bit();
	state->fe = g_test_rand_bit();
	state->fc0 = g_test_rand_bit();
	state->fc1 = g_test_rand_bit();
	state->flm = g_test_rand_bit();
	state->fvl = g_test_rand_bit();
	state->fr = g_test_rand_bit();
	state->product_shift = g_test_rand_int_range(0, TEST_PRODUCT_SHIFTS);
	state->modulo_enable = g_test_rand_int_range(0, BIT(TEAK_MODULO_REGISTERS));
	state->modi = g_test_rand_int_range(0, TEST_BLOCK_EQUIVALENCE_MODULO_LIMIT);
	state->modj = g_test_rand_int_range(0, TEST_BLOCK_EQUIVALENCE_MODULO_LIMIT);
	state->sat = g_test_rand_bit();
	state->sata = g_test_rand_bit();
	teak_update_modulo_steps(state);
}

static void test_block_equivalence_assert_same(const teak_state_t *expected, const teak_state_t *actual) {
	g_assert_cmphex(expected->a[0], ==, actual->a[0]);
	g_assert_cmphex(expected->a[1], ==, actual->a[1]);
	g_assert_cmphex(expected->b[0], ==, actual->b[0]);
	g_assert_cmphex(expected->b[1], ==, actual->b[1]);
	g_assert_cmphex(expected->p[0], ==, actual->p[0]);
	g_assert_cmphex(expected->product_extension[0], ==, actual->product_extension[0]);
	g_assert_cmphex(expected->x[0], ==, actual->x[0]);
	g_assert_cmphex(expected->y[0], ==, actual->y[0]);
	g_assert_cmpmem(expected->r, sizeof(expected->r), actual->r, sizeof(actual->r));
	g_assert_cmpuint(expected->fz, ==, actual->fz);
	g_assert_cmpuint(expected->fm, ==, actual->fm);
	g_assert_cmpuint(expected->fn, ==, actual->fn);
	g_assert_cmpuint(expected->fv, ==, actual->fv);
	g_assert_cmpuint(expected->fe, ==, actual->fe);
	g_assert_cmpuint(expected->fc0, ==, actual->fc0);
	g_assert_cmpuint(expected->fc1, ==, actual->fc1);
	g_assert_cmpuint(expected->flm, ==, actual->flm);
	g_assert_cmpuint(expected->fvl, ==, actual->fvl);
	g_assert_cmpuint(expected->fr, ==, actual->fr);
	g_assert_cmpuint(expected->sat, ==, actual->sat);
	g_assert_cmpuint(expected->sata, ==, actual->sata);
	g_assert_cmpuint(expected->product_shift, ==, actual->product_shift);
	g_assert_cmpuint(expected->modulo_enable, ==, actual->modulo_enable);
	g_assert_cmpuint(expected->page, ==, actual->page);
	g_assert_cmpuint(expected->s, ==, actual->s);
	g_assert_cmpuint(expected->ie, ==, actual->ie);
	g_assert_cmpuint(expected->interrupt_mask, ==, actual->interrupt_mask);
}

static uint16_t test_block_equivalence_program(const uint16_t *instructions, size_t stride, test_memory_t *program) {
	uint16_t end = TEST_BLOCK_EQUIVALENCE_INSTRUCTIONS * stride;

	*program = (test_memory_t) {};
	for (size_t i = 0; i < TEST_BLOCK_EQUIVALENCE_INSTRUCTIONS; i++) {
		uint16_t address = i * stride;

		program->words[address] = instructions[i];
		if (stride != 1) {
			program->words[address + 1] = 0x4180; /* br address + stride */
			program->words[address + TEST_BRANCH_WORDS] = address + stride;
		}
	}
	program->words[end] = 0x4180; /* br end */
	program->words[end + 1] = end;
	return end;
}

static void test_block_equivalence_run(
	const uint16_t *instructions,
	size_t stride,
	const teak_state_t *initial,
	test_memory_t *data,
	bool mapped,
	test_block_equivalence_trace_t *trace
) {
	test_memory_t program;
	uint16_t end = test_block_equivalence_program(instructions, stride, &program);
	teak_tcg_core_t core = test_core_create(&program, data);

	if (mapped) {
		test_core_map_data(&core, data);
	} else {
		data->exit_core_on_write = &core;
	}
	core.state = *initial;
	*trace = (test_block_equivalence_trace_t) {};
	for (size_t block = 0; block < TEST_BLOCK_EQUIVALENCE_MAX_BLOCKS && core.state.pc != end; block++) {
		g_assert_true(teak_tcg_execute_block(&core));
		g_assert_cmpuint(core.state.pc % stride, <=, 1);

		size_t executed = DIV_ROUND_UP(core.state.pc, stride);
		trace->states[executed] = core.state;
		trace->recorded[executed] = true;
	}
	g_assert_cmphex(core.state.pc, ==, end);
	data->exit_core_on_write = NULL;
}

static void test_block_equivalence(void) {
	for (size_t iteration = 0; iteration < TEST_BLOCK_EQUIVALENCE_ITERATIONS; iteration++) {
		uint16_t instructions[TEST_BLOCK_EQUIVALENCE_INSTRUCTIONS];
		test_memory_t empty_program = {};
		test_memory_t data = {};
		bool mapped = iteration % 2 == 0;

		for (size_t i = 0; i < ARRAY_SIZE(instructions); i++) {
			size_t pool_index = g_test_rand_int_range(0, ARRAY_SIZE(test_block_equivalence_pool));

			instructions[i] = test_block_equivalence_pool[pool_index];
		}
		for (size_t i = 0; i < ARRAY_SIZE(data.words); i++)
			data.words[i] = g_test_rand_int();
		teak_state_t initial = test_core_create(&empty_program, &data).state;
		test_block_equivalence_randomize(&initial);

		test_memory_t reference_data = data;
		test_memory_t optimized_data = data;
		test_block_equivalence_trace_t reference;
		test_block_equivalence_trace_t optimized;
		test_block_equivalence_run(instructions, 1 + TEST_BRANCH_WORDS, &initial, &reference_data, mapped, &reference);
		test_block_equivalence_run(instructions, 1, &initial, &optimized_data, mapped, &optimized);

		for (size_t executed = 0; executed <= TEST_BLOCK_EQUIVALENCE_INSTRUCTIONS; executed++) {
			if (!optimized.recorded[executed])
				continue;
			g_assert_true(reference.recorded[executed]);
			test_block_equivalence_assert_same(&reference.states[executed], &optimized.states[executed]);
		}
		g_assert_true(optimized.recorded[TEST_BLOCK_EQUIVALENCE_INSTRUCTIONS]);
		g_assert_cmpmem(reference_data.words, sizeof(reference_data.words),
			optimized_data.words, sizeof(optimized_data.words));
	}
}

static void test_mode_keyed_cache(void) {
	static const uint64_t accumulated_products[TEST_PRODUCT_SHIFTS] = { 0x100, 0x80, 0x200, 0x400 };
	test_memory_t program = {
		.words = {
			0xD22A, /* mac [r4++] [r2++] a0 */
			0x4180, 0x0003, /* br 0x0003 */
		},
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	for (uint8_t product_shift = 0; product_shift < ARRAY_SIZE(accumulated_products); product_shift++) {
		core.state.pc = 0;
		core.state.a[0] = 0;
		core.state.p[0] = 0x100;
		core.state.product_extension[0] = 0;
		core.state.product_shift = product_shift;
		g_assert_true(teak_tcg_execute_block(&core));
		g_assert_cmphex(core.state.a[0], ==, accumulated_products[product_shift]);
	}
	g_assert_cmpuint(core.cache_compiles, ==, TEST_PRODUCT_SHIFTS);

	core.state.pc = 0;
	core.state.product_shift = 0;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmpuint(core.cache_compiles, ==, TEST_PRODUCT_SHIFTS);
}

static void test_mode_keyed_chain(void) {
	static const uint64_t accumulated_products[TEST_PRODUCT_SHIFTS] = { 20, 10, 40, 80 };
	test_memory_t program = {};
	test_memory_t data = {};
	uint16_t end = TEST_MODE_KEYED_CHAIN_BLOCKS * (1 + TEST_BRANCH_WORDS);

	for (uint16_t block = 0; block < TEST_MODE_KEYED_CHAIN_BLOCKS; block++) {
		uint16_t address = block * (1 + TEST_BRANCH_WORDS);

		program.words[address] = 0xD22A; /* mac [r4++] [r2++] a0 */
		program.words[address + 1] = 0x4180; /* br address + 3 */
		program.words[address + 2] = address + 1 + TEST_BRANCH_WORDS;
	}
	for (size_t i = 0; i < ARRAY_SIZE(data.words); i++)
		data.words[i] = 2;
	teak_tcg_core_t core = test_core_create(&program, &data);

	test_core_map_data(&core, &data);
	for (uint8_t product_shift = 0; product_shift < TEST_PRODUCT_SHIFTS; product_shift++) {
		core.state.pc = 0;
		core.state.a[0] = 0;
		core.state.p[0] = 0;
		core.state.r[2] = 0x10;
		core.state.r[4] = TEST_Y_SPACE_BASE;
		core.state.product_shift = product_shift;
		test_execute_slices_until(&core, end, TEST_MODE_KEYED_CHAIN_BLOCKS);
		g_assert_cmphex(core.state.a[0], ==, accumulated_products[product_shift]);
	}
}

static void test_mode_writing_chain(void) {
	static const uint16_t product_shift_loads[] = {
		0x4D80, /* load 0x0000 ps */
		0x4D82, /* load 0x0002 ps */
	};
	test_memory_t program = {};
	test_memory_t data = {};
	size_t block_words = 2 + TEST_BRANCH_WORDS;
	uint16_t end = TEST_MODE_KEYED_CHAIN_BLOCKS * block_words;

	for (uint16_t block = 0; block < TEST_MODE_KEYED_CHAIN_BLOCKS; block++) {
		uint16_t address = block * block_words;

		program.words[address] = product_shift_loads[block % ARRAY_SIZE(product_shift_loads)];
		program.words[address + 1] = 0xD22A; /* mac [r4++] [r2++] a0 */
		program.words[address + 2] = 0x4180; /* br address + 4 */
		program.words[address + 3] = address + block_words;
	}
	for (size_t i = 0; i < ARRAY_SIZE(data.words); i++)
		data.words[i] = 2;
	teak_tcg_core_t core = test_core_create(&program, &data);

	test_core_map_data(&core, &data);
	for (size_t run = 0; run < 2; run++) {
		core.state.pc = 0;
		core.state.a[0] = 0;
		core.state.p[0] = 0;
		core.state.r[2] = 0x10;
		core.state.r[4] = TEST_Y_SPACE_BASE;
		test_execute_slices_until(&core, end, TEST_MODE_KEYED_CHAIN_BLOCKS);
		g_assert_cmphex(core.state.a[0], ==, 32);
	}
}

static void test_mode_writing_chain_to_specialized(void) {
	static const uint64_t shifted_products[TEST_PRODUCT_SHIFTS] = { 4, 2, 8, 16 };
	test_memory_t program = {
		.words = {
			0x5920, /* mov r0 st1 */
			0x4180, 0x0003, /* br 0x0003 */
			0xD22A, /* mac [r4++] [r2++] a0 */
			0xD22A, /* mac [r4++] [r2++] a0 */
			0x4180, 0x0007, /* br 0x0007 */
		},
	};
	test_memory_t data = {};
	uint16_t end = 7;

	for (size_t i = 0; i < ARRAY_SIZE(data.words); i++)
		data.words[i] = 2;
	teak_tcg_core_t core = test_core_create(&program, &data);

	test_core_map_data(&core, &data);
	for (size_t run = 0; run < 2; run++) {
		for (uint8_t product_shift = 0; product_shift < TEST_PRODUCT_SHIFTS; product_shift++) {
			core.state.pc = 0;
			core.state.a[0] = 0;
			core.state.p[0] = 0;
			core.state.r[0] = product_shift << 10;
			core.state.r[2] = 0x10;
			core.state.r[4] = TEST_Y_SPACE_BASE;
			test_execute_slices_until(&core, end, 4);
			g_assert_cmphex(core.state.a[0], ==, shifted_products[product_shift]);
		}
	}
}

static void test_execute_cycles(teak_tcg_core_t *core, uint32_t cycles) {
	uint32_t executed = 0;

	while (executed < cycles) {
		g_assert_true(teak_tcg_execute_slice(core, cycles - executed));
		executed += core->last_block_cycles;
	}
	g_assert_cmpuint(executed, ==, cycles);
}

static teak_tcg_core_t test_two_block_loop_create(test_memory_t *program, test_memory_t *data) {
	*program = (test_memory_t) {
		.words = {
			0x67D0, /* inc a0. */
			0x4180, 0x0003, /* br 0x0003. */
			0x67D0, /* inc a0. */
			0x4180, 0x0000, /* br 0x0000. */
		},
	};
	teak_tcg_core_t core = test_core_create(program, data);

	test_execute_cycles(&core, 40);
	core.state.pc = 0;
	core.state.a[0] = 0;
	return core;
}

static void test_linked_chain_invalidation(void) {
	test_memory_t program;
	test_memory_t data = {};
	teak_tcg_core_t core = test_two_block_loop_create(&program, &data);

	program.words[3] = 0x67E0; /* dec a0. */
	teak_tcg_invalidate_program(&core, 3);
	test_execute_cycles(&core, 40);
	g_assert_cmphex(core.state.a[0], ==, 0);
}

static void test_linked_chain_cycle_budget(void) {
	test_memory_t program;
	test_memory_t data = {};
	teak_tcg_core_t core = test_two_block_loop_create(&program, &data);

	g_assert_true(teak_tcg_execute_slice(&core, 9));
	g_assert_cmpuint(core.last_block_cycles, ==, 10);
	g_assert_cmphex(core.state.a[0], ==, 5);
	g_assert_cmphex(core.state.pc, ==, 3);
}

static void test_linked_block_repeat(void) {
	test_memory_t program = {
		.words = {
			0x5C14, 0x0003, /* bkrep 0x14, 0x0003. */
			0x67D0, /* inc a0. */
			0x67D0, /* inc a0. */
			0x4180, 0x0004, /* br 0x0004. */
		},
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	test_execute_slices_until(&core, 4, 16);
	g_assert_cmphex(core.state.a[0], ==, 42);
	g_assert_cmphex(core.state.lp, ==, 0);
	g_assert_cmphex(core.state.bcn, ==, 0);
}

static void test_linked_one_word_block_repeat(void) {
	test_memory_t program = {
		.words = {
			0x5C14, 0x0002, /* bkrep 0x14, 0x0002. */
			0x67D0, /* inc a0. */
			0x4180, 0x0003, /* br 0x0003. */
		},
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	test_execute_slices_until(&core, 3, 16);
	g_assert_cmphex(core.state.a[0], ==, 20);
	g_assert_cmphex(core.state.lp, ==, 0);
	g_assert_cmphex(core.state.bcn, ==, 0);
}

static void test_linked_interrupt_enable(void) {
	static const struct {
		uint16_t words[6];
		uint16_t return_address;
	} loops[] = {
		{
			.words = {
				0x67D0, /* inc a0. */
				0x4380, /* eint. */
				0x4180, 0x0020, /* br 0x0020. */
			},
			.return_address = 0x0022,
		},
		{
			.words = {
				0x67D0, /* inc a0. */
				0x4180, 0x0023, /* br 0x0023. */
				0x4380, /* eint. */
				0x4180, 0x0020, /* br 0x0020. */
			},
			.return_address = 0x0024,
		},
	};

	for (size_t i = 0; i < ARRAY_SIZE(loops); i++) {
		test_memory_t program = {
			.words = { [6] = 0x4180, [7] = 0x0006 }, /* br 0x0006. */
		};
		test_memory_t data = {};

		memcpy(&program.words[0x20], loops[i].words, sizeof(loops[i].words));
		teak_tcg_core_t core = test_core_create(&program, &data);

		core.state.pc = 0x20;
		core.state.sp = 0x10;
		test_execute_cycles(&core, 48);

		core.state.pc = 0x20;
		core.state.a[0] = 0;
		core.state.ie = 0;
		core.state.interrupt_mask = BIT(0);
		core.state.interrupt_lines = BIT(0);
		g_assert_true(teak_tcg_execute_slice(&core, 40));
		g_assert_cmphex(core.state.pc, ==, 0x0006);
		g_assert_cmphex(core.state.a[0], ==, 1);
		g_assert_cmphex(core.state.sp, ==, 0x0F);
		g_assert_cmphex(data.words[0x0F], ==, loops[i].return_address);
	}
}

static void test_cycle_log_advance(void *opaque, size_t cycles) {
	test_cycle_log_t *log = opaque;

	log->advanced_cycles += cycles;
}

static void test_cycle_log_write(void *opaque, uint32_t address, uint16_t value) {
	test_cycle_log_t *log = opaque;

	g_assert_cmpuint(address, <, ARRAY_SIZE(log->data.words));
	g_assert_cmpuint(log->stores, <, ARRAY_SIZE(log->store_cycles));
	log->store_cycles[log->stores++] = log->advanced_cycles;
	log->data.words[address] = value;
}

static uint16_t test_cycle_log_read(void *opaque, uint32_t address) {
	test_cycle_log_t *log = opaque;

	g_assert_cmpuint(address, <, ARRAY_SIZE(log->data.words));
	return log->data.words[address];
}

static void test_linked_cycle_synchronization(void) {
	static const size_t expected_store_cycles[] = { 1, 3, 5, 7, 9, 11, 13, 15, 17, 19 };
	test_memory_t program = {
		.words = {
			0x67D0, /* inc a0. */
			0x1B40, /* mov a0l [r0]. */
			0x4180, 0x0004, /* br 0x0004. */
			0x1B40, /* mov a0l [r0]. */
			0x67D0, /* inc a0. */
			0x1B40, /* mov a0l [r0]. */
			0x4180, 0x0009, /* br 0x0009. */
			0x1B40, /* mov a0l [r0]. */
			0x4180, 0x0009, /* br 0x0009. */
		},
	};
	test_cycle_log_t log = {};
	teak_memory_t memory = {
		.program = test_memory_space(&program),
		.data = {
			.opaque = &log,
			.read = test_cycle_log_read,
			.write = test_cycle_log_write,
		},
		.y_space_base = TEST_Y_SPACE_BASE,
		.advance_cycles = test_cycle_log_advance,
		.cycle_opaque = &log,
		.cycle_sensitive_base = TEST_CYCLE_SENSITIVE_ADDRESS,
		.cycle_sensitive_size = 1,
	};
	uint32_t slice_cycles = 21;
	teak_tcg_core_t core;

	teak_tcg_init(&core, &memory);
	core.state.r[0] = TEST_CYCLE_SENSITIVE_ADDRESS;
	for (size_t warmup_run = 0; warmup_run < 2; warmup_run++) {
		log.advanced_cycles = 0;
		log.stores = 0;
		core.state.pc = 0;
		while (log.stores < ARRAY_SIZE(expected_store_cycles))
			g_assert_true(teak_tcg_execute_slice(&core, slice_cycles));
	}

	log.advanced_cycles = 0;
	log.stores = 0;
	core.state.pc = 0;
	g_assert_true(teak_tcg_execute_slice(&core, slice_cycles));
	g_assert_cmpuint(core.last_block_cycles, ==, slice_cycles);
	g_assert_cmpuint(log.advanced_cycles, ==, slice_cycles);
	g_assert_cmpuint(log.stores, ==, ARRAY_SIZE(expected_store_cycles));
	g_assert_cmpmem(log.store_cycles, sizeof(expected_store_cycles),
		expected_store_cycles, sizeof(expected_store_cycles));
}

static void test_precompile_bounded_indirect_dispatch(void) {
	test_memory_t program = {
		.words = {
			/* Isolate the mandatory interrupt-vector precompile roots. */
			[0x04] = 0x4180, 0x0004, /* br 0x0004, true. */
			[0x06] = 0x4180, 0x0006, /* br 0x0006, true. */
			[0x0E] = 0x4180, 0x000E, /* br 0x000e, true. */
			[0x10] = 0x4180, 0x0010, /* br 0x0010, true. */
			[0x14] = 0x4180, 0x0014, /* br 0x0014, true. */
			[0x16] = 0x4180, 0x0016, /* br 0x0016, true. */
			[0x18] = 0x4180, 0x0018, /* br 0x0018, true. */
			[0x20] = 0x1F40, /* mov [r0], a0l. */
			0x50A6, /* brr 0x002c, le. */
			0xCC03, /* cmp 0x0003, a0. */
			0x5083, /* brr 0x002c, gt. */
			0x86C0, 0x002F, /* add 0x002f, a0. */
			0x583A, /* mov a0l, r1. */
			0x1F41, /* mov [r1], a0l. */
			0xD480, /* calla a0l. */
			0x4180, 0x0029, /* br 0x0029, true. */
			[0x2C] = 0x4180, 0x002C, /* br 0x002c, true. */
		},
	};
	test_memory_t data = {
		.words = { [0x30] = 0x10, [0x31] = 0x14, [0x32] = 0x18 },
	};
	teak_tcg_core_t core = test_core_create(&program, &data);

	test_core_map_data(&core, &data);
	teak_tcg_precompile_entry(&core, 0x20);
	uint64_t compiles = core.cache_compiles;

	for (uint16_t pc = 0x10; pc <= 0x18; pc += 4) {
		core.state.pc = pc;
		g_assert_true(teak_tcg_execute_block(&core));
		g_assert_cmphex(core.state.pc, ==, pc);
	}
	g_assert_cmpuint(core.cache_compiles, ==, compiles);
}

static void test_direct_data_bank(void) {
	test_memory_t program = {
		/* mov [r1], a0; br 0x0003 always. */
		.words = { 0x1F41, 0x4180, 0x0003 },
	};
	test_memory_t data = {
		.words = { [0x10] = 0x1234 },
	};
	uint16_t first_bank[TEAK_DATA_PAGE_WORDS] = { [0x10] = 0xA55A };
	uint16_t second_bank[TEAK_DATA_PAGE_WORDS] = { [0x10] = 0xBEEF };
	teak_tcg_core_t core = test_core_create(&program, &data);

	test_core_map_data(&core, &data);
	teak_data_map_read(&core, 0, ARRAY_SIZE(first_bank), first_bank);
	core.state.r[1] = 0x10;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 0xA55A);

	teak_data_map_read(&core, 0, ARRAY_SIZE(second_bank), second_bank);
	core.state.pc = 0;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 0xBEEF);
}

static void test_mov_data_shift_value(void) {
	test_memory_t program = {
		/* mov [r4++], sv */
		.words = { 0x1FEC, 0x4180, 0x0003 },
	};
	test_memory_t data = {
		.words = { [0x10] = 0xA55A },
	};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.r[4] = 0x10;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.shift_value, ==, 0xA55A);
	g_assert_cmphex(core.state.r[4], ==, 0x11);
}

static void test_mov_accumulator_high_extension_unaffected(void) {
	test_memory_t program = {
		.words = { 0x6500, 0x4180, 0x0003 },
	};
	test_memory_t data = {
		.words = { 0x8000 },
	};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.a[0] = 0xFFFFFFFF00000000ULL;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 0xFFFFFFFF80000000ULL);
	g_assert_cmphex(core.state.fm, ==, 1);
	g_assert_cmphex(core.state.fn, ==, 1);
	g_assert_cmphex(core.state.fe, ==, 0);
}

static void test_alu_accumulator_masks(void) {
	test_memory_t tst0_program = {
		.words = { 0x88A0, 0x4180, 0x0003 },
	};
	test_memory_t tst1_program = {
		.words = { 0x8BA1, 0x4180, 0x0003 },
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&tst0_program, &data);

	core.state.a[0] = 0x00F0;
	core.state.r[0] = 0x0F00;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 0x00F0);
	g_assert_cmphex(core.state.fz, ==, 1);

	core = test_core_create(&tst1_program, &data);
	core.state.a[1] = 0x00F0;
	core.state.r[1] = 0xF0F0;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[1], ==, 0x00F0);
	g_assert_cmphex(core.state.fz, ==, 1);
}

static void test_multiply_subtract_status_register(void) {
	test_memory_t program = {
		.words = { 0x90A8, 0x4180, 0x0003 },
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.a[0] = 100;
	core.state.p[0] = 10;
	core.state.y[0] = 2;
	core.state.sat = 1;
	core.state.ie = 1;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 90);
	g_assert_cmphex(core.state.x[0], ==, 3);
	g_assert_cmphex(core.state.p[0], ==, 6);
}

static void test_mov_stack_accumulator_low(void) {
	test_memory_t program = {
		.words = { 0x47FA, 0x4180, 0x0003 },
	};
	test_memory_t data = {
		.words = { [10] = 0xA55A },
	};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.a[0] = 0x123456789ULL;
	core.state.sp = 10;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 0xA55A);
	g_assert_cmphex(core.state.sp, ==, 10);
	g_assert_cmphex(core.state.fz, ==, 0);
	g_assert_cmphex(core.state.fm, ==, 0);
}

static void test_icr_reserved_read_bits(void) {
	test_memory_t program = {
		.words = { 0x4F8B, 0xD4D2, 0x4180, 0x0004 },
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, (uint64_t) (int64_t) (int16_t) 0xFF0B);
	g_assert_cmphex(core.state.nonmaskable_context, ==, 1);
	g_assert_cmphex(core.state.interrupt_context, ==, 5);
}

static void test_external_registers_disconnected(void) {
	test_memory_t program = {
		.words = { 0x2901, 0x0000, 0x0000, 0x5814, 0x4180, 0x0006 },
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.r[0], ==, 0);
	g_assert_cmphex(core.state.pc, ==, 6);
}

static void test_movr_b_destination(void) {
	test_memory_t program = {
		.words = { 0x8864, 0x8964, 0x4180, 0x0004 },
	};
	test_memory_t data = {
		.words = { [0x10] = 0x8001, [0x11] = 0x8001 },
	};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.r[0] = 0x10;
	core.state.b[0] = 0x123456789ULL;
	core.state.b[1] = 0xFEDCBA987ULL;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.r[0], ==, 0x12);
	g_assert_cmphex(core.state.b[0], ==, 0xFEDCBA987ULL);
	g_assert_cmphex(core.state.b[1], ==, 0xFEDCBA987ULL);
	g_assert_cmphex(core.state.fm, ==, 1);
	g_assert_cmphex(core.state.pc, ==, 4);
}

static void test_delayed_interrupt_return(void) {
	/*
	 * 0xD7C0 0x2102 0xD4BC 0x0010: retid; mov 2, a0l; mov a0l, [0x0010]. The store starts in the last delay cycle and
	 * takes its address from P:000A, the return address (dsp-lle-delay-slot-overflow).
	 */
	test_memory_t program = {
		.words = { 0xD7C0, 0x2102, 0xD4BC, 0x0010, [10] = 0x0020 },
	};
	test_memory_t data = {
		.words = { [5] = 10, [0x10] = 0xA55A },
	};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.sp = 5;
	core.state.maskable_interrupt_active = 1;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.pc, ==, 11);
	g_assert_cmphex(core.state.a[0], ==, 2);
	g_assert_cmphex(core.state.sp, ==, 6);
	g_assert_cmphex(core.state.ie, ==, 1);
	g_assert_cmphex(core.state.maskable_interrupt_active, ==, 0);
	g_assert_cmphex(data.words[0x20], ==, 2);
	g_assert_cmphex(data.words[0x10], ==, 0xA55A);
}

static void test_delayed_return_borrowed_word_invalidation(void) {
	/* 0xD780 0x2102 0xD4BC 0x0010: retd; mov 2, a0l; mov a0l, [P:000A] (the second word comes from P:000A). */
	test_memory_t program = {
		.words = { 0xD780, 0x2102, 0xD4BC, 0x0010, [10] = 0x0020 },
	};
	test_memory_t data = {
		.words = { [5] = 10 },
	};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.sp = 5;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(data.words[0x20], ==, 2);

	program.words[10] = 0x0030;
	teak_tcg_invalidate_program(&core, 10);
	core.state.pc = 0;
	core.state.sp = 5;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.pc, ==, 11);
	g_assert_cmphex(data.words[0x30], ==, 2);
}

static void test_delayed_interrupt_return_exit_request(void) {
	/* 0xD7C0 0x2010 0x0000: retid; mov r0, [0x10]; nop. */
	test_memory_t program = {
		.words = { 0xD7C0, 0x2010, 0x0000 },
	};
	test_memory_t data = {
		.words = { [5] = 10 },
	};
	teak_tcg_core_t core = test_core_create(&program, &data);

	data.exit_core_on_write = &core;
	core.state.r[0] = 0xA55A;
	core.state.sp = 5;
	core.state.maskable_interrupt_active = 1;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.pc, ==, 10);
	g_assert_cmphex(core.state.sp, ==, 6);
	g_assert_cmphex(core.state.ie, ==, 1);
	g_assert_cmphex(core.state.maskable_interrupt_active, ==, 0);
	g_assert_cmphex(data.words[0x10], ==, 0xA55A);
}

static void test_exit_request_counts_executed_cycles(void) {
	test_memory_t program = {
		.words = {
			0x2010, /* mov r0, [0x10]. */
			0x0000, /* nop. */
			0x0000, /* nop. */
			0x0000, /* nop. */
			0x4180, 0x0006, /* br 0x0006. */
		},
	};
	test_memory_t data = {};
	teak_tcg_core_t core = test_core_create(&program, &data);

	data.exit_core_on_write = &core;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.pc, ==, 1);
	g_assert_cmpuint(core.last_block_cycles, ==, 1);
}

static void test_delayed_return_break(void) {
	/* 0xD780 0xD3C0 0x0000: retd; break; nop. */
	test_memory_t program = {
		.words = { 0xD780, 0xD3C0, 0x0000 },
	};
	test_memory_t data = {
		.words = { [5] = 10 },
	};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.sp = 5;
	core.state.lp = 1;
	core.state.bcn = 1;
	core.state.block_repeat_start[0] = 0;
	core.state.block_repeat_end[0] = 2;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.pc, ==, 10);
	g_assert_cmphex(core.state.sp, ==, 6);
	g_assert_cmphex(core.state.lp, ==, 0);
	g_assert_cmphex(core.state.bcn, ==, 0);
}

static void test_delayed_return_stores(void) {
	/* 0xD780 0x3810 0x3811: retd; mov a0l, [page:0x10]; mov a0l, [page:0x11]. */
	test_memory_t program = {
		.words = { 0xD780, 0x3810, 0x3811 },
	};
	test_memory_t data = {
		.words = { [5] = 10, [0x10] = 0xA55A, [0x11] = 0x5AA5 },
	};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.a[0] = 0x1234;
	core.state.sp = 5;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.pc, ==, 10);
	g_assert_cmphex(core.state.sp, ==, 6);
	g_assert_cmphex(data.words[0x10], ==, 0x1234);
	g_assert_cmphex(data.words[0x11], ==, 0x1234);
}

static void test_nested_nmi_interrupt_return(void) {
	/* 0xD7C0 0x2102 0xD4BC 0x0010: retid; mov 2, a0l; mov a0l, [0x0010] with the address taken from P:000A. */
	test_memory_t program = {
		.words = { 0xD7C0, 0x2102, 0xD4BC, 0x0010, [10] = 0x0020 },
	};
	test_memory_t data = {
		.words = { [5] = 10, [0x10] = 0xA55A },
	};
	teak_tcg_core_t core = test_core_create(&program, &data);

	core.state.sp = 5;
	core.state.nmi_active = 1;
	core.state.maskable_interrupt_active = 1;
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_true(teak_tcg_execute_block(&core));
	g_assert_cmphex(core.state.pc, ==, 11);
	g_assert_cmphex(core.state.sp, ==, 6);
	g_assert_cmphex(core.state.ie, ==, 1);
	g_assert_cmphex(core.state.nmi_active, ==, 0);
	g_assert_cmphex(core.state.maskable_interrupt_active, ==, 1);
}

int main(int argc, char **argv) {
	g_test_init(&argc, &argv, NULL);
	tcg_init(TEST_TCG_CODE_SIZE, 0, 1);
	tcg_prologue_init();
	tcg_register_thread();

	g_test_add_func("/pmb887x/dsp/tcg/modulo-address", test_modulo_address);
	g_test_add_func("/pmb887x/dsp/tcg/mov-full-accumulator", test_mov_full_accumulator);
	g_test_add_func("/pmb887x/dsp/tcg/mov-full-accumulator-alias", test_mov_full_accumulator_alias);
	g_test_add_func("/pmb887x/dsp/tcg/mov-immediate-product-high", test_mov_immediate_product_high);
	g_test_add_func("/pmb887x/dsp/tcg/pop-full-accumulator", test_pop_full_accumulator);
	g_test_add_func("/pmb887x/dsp/tcg/long-block-repeat", test_long_block_repeat);
	g_test_add_func("/pmb887x/dsp/tcg/interrupt-block-repeat", test_interrupt_block_repeat);
	g_test_add_func("/pmb887x/dsp/tcg/repeat-cycle-budget", test_repeat_cycle_budget);
	g_test_add_func("/pmb887x/dsp/tcg/batched-loop-wait-state-budget", test_batched_loop_wait_state_budget);
	g_test_add_func("/pmb887x/dsp/tcg/repeated-instruction-without-repeat", test_repeated_instruction_without_repeat);
	g_test_add_func("/pmb887x/dsp/tcg/interrupt-repeat-nested-repeat", test_interrupt_repeat_nested_repeat);
	g_test_add_func("/pmb887x/dsp/tcg/interrupt-repeat-resumes-on-return", test_interrupt_repeat_resumes_on_return);
	g_test_add_func("/pmb887x/dsp/tcg/subroutine-block-repeat", test_subroutine_block_repeat);
	g_test_add_func("/pmb887x/dsp/tcg/status-reserved-read-bits", test_status_reserved_read_bits);
	g_test_add_func("/pmb887x/dsp/tcg/multiply-status-register", test_multiply_status_register);
	g_test_add_func("/pmb887x/dsp/tcg/product-bit-tests", test_product_bit_tests);
	g_test_add_func("/pmb887x/dsp/tcg/alu-status-register", test_alu_status_register);
	g_test_add_func("/pmb887x/dsp/tcg/dual-memory-spaces", test_dual_memory_spaces);
	g_test_add_func("/pmb887x/dsp/tcg/direct-data-read-write", test_direct_data_read_write);
	g_test_add_func("/pmb887x/dsp/tcg/program-context-cache", test_program_context_cache);
	g_test_add_func("/pmb887x/dsp/tcg/program-invalidation", test_program_invalidation);
	g_test_add_func("/pmb887x/dsp/tcg/block-equivalence", test_block_equivalence);
	g_test_add_func("/pmb887x/dsp/tcg/mode-keyed-cache", test_mode_keyed_cache);
	g_test_add_func("/pmb887x/dsp/tcg/mode-keyed-chain", test_mode_keyed_chain);
	g_test_add_func("/pmb887x/dsp/tcg/mode-writing-chain", test_mode_writing_chain);
	g_test_add_func("/pmb887x/dsp/tcg/mode-writing-chain-to-specialized", test_mode_writing_chain_to_specialized);
	g_test_add_func("/pmb887x/dsp/tcg/linked-chain-invalidation", test_linked_chain_invalidation);
	g_test_add_func("/pmb887x/dsp/tcg/linked-chain-cycle-budget", test_linked_chain_cycle_budget);
	g_test_add_func("/pmb887x/dsp/tcg/linked-block-repeat", test_linked_block_repeat);
	g_test_add_func("/pmb887x/dsp/tcg/linked-one-word-block-repeat", test_linked_one_word_block_repeat);
	g_test_add_func("/pmb887x/dsp/tcg/linked-interrupt-enable", test_linked_interrupt_enable);
	g_test_add_func("/pmb887x/dsp/tcg/linked-cycle-synchronization", test_linked_cycle_synchronization);
	g_test_add_func("/pmb887x/dsp/tcg/precompile-bounded-indirect-dispatch", test_precompile_bounded_indirect_dispatch);
	g_test_add_func("/pmb887x/dsp/tcg/direct-data-bank", test_direct_data_bank);
	g_test_add_func("/pmb887x/dsp/tcg/mov-data-shift-value", test_mov_data_shift_value);
	g_test_add_func("/pmb887x/dsp/tcg/mov-accumulator-high-extension-unaffected", test_mov_accumulator_high_extension_unaffected);
	g_test_add_func("/pmb887x/dsp/tcg/alu-accumulator-masks", test_alu_accumulator_masks);
	g_test_add_func("/pmb887x/dsp/tcg/multiply-subtract-status-register", test_multiply_subtract_status_register);
	g_test_add_func("/pmb887x/dsp/tcg/mov-stack-accumulator-low", test_mov_stack_accumulator_low);
	g_test_add_func("/pmb887x/dsp/tcg/icr-reserved-read-bits", test_icr_reserved_read_bits);
	g_test_add_func("/pmb887x/dsp/tcg/external-registers-disconnected", test_external_registers_disconnected);
	g_test_add_func("/pmb887x/dsp/tcg/movr-b-destination", test_movr_b_destination);
	g_test_add_func("/pmb887x/dsp/tcg/delayed-interrupt-return", test_delayed_interrupt_return);
	g_test_add_func("/pmb887x/dsp/tcg/delayed-return-borrowed-word-invalidation",
		test_delayed_return_borrowed_word_invalidation);
	g_test_add_func("/pmb887x/dsp/tcg/delayed-interrupt-return-exit-request", test_delayed_interrupt_return_exit_request);
	g_test_add_func("/pmb887x/dsp/tcg/exit-request-counts-executed-cycles", test_exit_request_counts_executed_cycles);
	g_test_add_func("/pmb887x/dsp/tcg/delayed-return-break", test_delayed_return_break);
	g_test_add_func("/pmb887x/dsp/tcg/delayed-return-stores", test_delayed_return_stores);
	g_test_add_func("/pmb887x/dsp/tcg/nested-nmi-interrupt-return", test_nested_nmi_interrupt_return);
	return g_test_run();
}
