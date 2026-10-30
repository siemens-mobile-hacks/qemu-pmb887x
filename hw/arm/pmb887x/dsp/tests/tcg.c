#include "qemu/osdep.h"
#include "qemu-main.h"
#include "qemu/units.h"
#include "tcg/startup.h"

#include "hw/arm/pmb887x/dsp/core.h"
#include "hw/arm/pmb887x/dsp/tcg.h"

#define TEST_MEMORY_WORDS	64
#define TEST_TCG_CODE_SIZE	(16 * MiB)

typedef struct test_memory_t test_memory_t;

struct test_memory_t {
	uint16_t words[TEST_MEMORY_WORDS];
	pmb887x_dsp_tcg_core_t *exit_core_on_write;
	/* exit on this write only (1-based), or on every write when 0 */
	unsigned exit_write;
	unsigned writes;
};

int (*qemu_main)(void);

static uint16_t test_memory_read(void *opaque, uint32_t address) {
	test_memory_t *memory = opaque;

	g_assert_cmpuint(address, <, ARRAY_SIZE(memory->words));
	return memory->words[address];
}

static void test_memory_write(void *opaque, uint32_t address, uint16_t value) {
	test_memory_t *memory = opaque;

	g_assert_cmpuint(address, <, ARRAY_SIZE(memory->words));
	memory->words[address] = value;
	memory->writes++;
	if (memory->exit_core_on_write != NULL &&
		(memory->exit_write == 0 || memory->writes == memory->exit_write))
		memory->exit_core_on_write->state.exit_request = 1;
}

static pmb887x_dsp_tcg_memory_space_t test_memory_space(test_memory_t *memory) {
	pmb887x_dsp_tcg_memory_space_t space = {
		.opaque = memory,
		.read = test_memory_read,
		.write = test_memory_write,
	};

	return space;
}

static pmb887x_dsp_tcg_core_t test_core_create(test_memory_t *program, test_memory_t *data) {
	pmb887x_dsp_tcg_memory_t memory = {
		.program = test_memory_space(program),
		.data = test_memory_space(data),
		.y_space_base = 0x20,
	};
	pmb887x_dsp_tcg_core_t core;

	pmb887x_dsp_tcg_core_init(&core, &memory);
	return core;
}

static void test_core_set_direct_data(pmb887x_dsp_tcg_core_t *core, test_memory_t *data) {
	core->memory.direct_data = data->words;
	core->memory.direct_data_read_size = ARRAY_SIZE(data->words);
	core->memory.direct_data_write_size = ARRAY_SIZE(data->words);
}

static void test_execute_until(pmb887x_dsp_tcg_core_t *core, uint16_t pc, size_t max_blocks) {
	for (size_t i = 0; i < max_blocks && core->state.pc != pc; i++)
		g_assert_true(pmb887x_dsp_tcg_execute_block(core));
	g_assert_cmphex(core->state.pc, ==, pc);
}

static void test_execute_slices_until(pmb887x_dsp_tcg_core_t *core, uint16_t pc, size_t max_slices) {
	for (size_t i = 0; i < max_slices && core->state.pc != pc; i++)
		g_assert_true(pmb887x_dsp_tcg_execute_slice(core, 1000));
	g_assert_cmphex(core->state.pc, ==, pc);
}

/* Runs slices until the data write that requests an exit; returns the cycles they took. */
static size_t test_execute_slices_until_exit(pmb887x_dsp_tcg_core_t *core, test_memory_t *data, size_t max_slices) {
	size_t cycles = 0;

	for (size_t i = 0; i < max_slices && data->writes < data->exit_write; i++) {
		g_assert_true(pmb887x_dsp_tcg_execute_slice(core, 1000));
		cycles += core->last_block_cycles;
	}
	g_assert_cmpuint(data->writes, ==, data->exit_write);
	return cycles;
}

static void test_mov_a0_b0(uint16_t opcode) {
	test_memory_t program = {
		.words = { opcode, 0x4180, 0x0003 },
	};
	test_memory_t data = {};
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.a[0] = 0x110101011ULL;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.b[0], ==, 0x110101011ULL);
	g_assert_cmphex(core.state.fz, ==, 0);
	g_assert_cmphex(core.state.fm, ==, 0);
	g_assert_cmphex(core.state.fn, ==, 0);
	g_assert_cmphex(core.state.fe, ==, 1);
	g_assert_cmphex(core.state.pc, ==, 3);
}

static void test_mov_full_accumulator(void) {
	/* EL71 Mask ROM 0801 dsp-instructions hardware case 968. */
	test_mov_a0_b0(0xDA90);
}

static void test_mov_full_accumulator_alias(void) {
	/* PMB8875 MASK ROM 0602 uses this REG-to-b0 encoding at P:5E49. */
	test_mov_a0_b0(0x5ED8);
}

static void test_mov_immediate_product_high(void) {
	/* PMB8876 Mask ROM 0801 CCH receiver uses this instruction at P:A056. */
	test_memory_t program = {
		.words = { 0x5E0B, 0x0090, 0x4180, 0x0004 },
	};
	test_memory_t data = {};
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.p[0] = 0x12345678;
	core.state.product_extension[0] = 1;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.sp = 10;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
	pmb887x_dsp_tcg_core_t core;

	for (size_t i = 2; i <= 20; i++)
		program.words[i] = 0x67D0;
	program.words[21] = 0x4180;
	program.words[22] = 0x0017;
	core = test_core_create(&program, &data);

	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	g_assert_true(pmb887x_dsp_tcg_execute_slice(&core, 10));
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.memory.wait_state_base = 0x10;
	core.memory.wait_state_size = 1;
	core.memory.wait_state_cycles = 2;
	g_assert_true(pmb887x_dsp_tcg_execute_slice(&core, 40));
	g_assert_cmpuint(core.last_block_cycles, ==, 40);
}

static void test_batched_loop_exit_after_write(void) {
	test_memory_t program = {
		.words = {
			0x2010, /* mov r0, [0x10]. */
			0x67D0, /* inc a0. */
			0x4180, 0x0000, /* br 0x0000. */
		},
	};
	test_memory_t data = { .exit_write = 3 };
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	data.exit_core_on_write = &core;
	g_assert_cmpuint(test_execute_slices_until_exit(&core, &data, 4), ==, 2 * 3 + 1);
	g_assert_cmphex(core.state.pc, ==, 1);
	g_assert_cmphex(core.state.a[0], ==, 2);
}

static void test_block_repeat_body_exit_after_write(void) {
	test_memory_t program = {
		.words = {
			0x5C04, 0x0004, /* bkrep 0x04, 0x0004. */
			0x2010, /* mov r0, [0x10]. */
			0x67D0, /* inc a0. */
			0x67D0, /* inc a0. */
			0x4180, 0x0005, /* br 0x0005. */
		},
	};
	test_memory_t data = { .exit_write = 3 };
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	data.exit_core_on_write = &core;
	g_assert_cmpuint(test_execute_slices_until_exit(&core, &data, 4), ==, 1 + 2 * 3 + 1);
	g_assert_cmphex(core.state.pc, ==, 3);
	g_assert_cmphex(core.state.a[0], ==, 4);
	g_assert_cmphex(core.state.block_repeat_lc[0], ==, 2);
	g_assert_cmphex(core.state.bcn, ==, 1);

	data.exit_core_on_write = NULL;
	test_execute_slices_until(&core, 5, 4);
	g_assert_cmphex(core.state.a[0], ==, 10);
	g_assert_cmpuint(data.writes, ==, 5);
	g_assert_cmphex(core.state.block_repeat_lc[0], ==, 0);
	g_assert_cmphex(core.state.lp, ==, 0);
	g_assert_cmphex(core.state.bcn, ==, 0);
}

static void test_relocated_fast_loop_init(test_memory_t *program, test_memory_t *data,
	pmb887x_dsp_tcg_core_t *core
) {
	static const uint16_t kernel[] = {
		/* The EL71 kernel at P:643F, here at P:0002, as the SL65 has it at P:566B. */
		0x802A, 0x1F89, 0x1FA9, 0x9688, 0x9788, 0x822A, 0x87AB, 0x1B8A, 0x1BB2,
	};

	*program = (test_memory_t) { .words = { 0x5C03, 0x000A } }; /* bkrep 0x03, 0x000a. */
	memcpy(&program->words[2], kernel, sizeof(kernel));
	program->words[11] = 0x4180; /* br 0x000d. */
	program->words[12] = 0x000D;
	*data = (test_memory_t) {};
	for (size_t i = 0; i < ARRAY_SIZE(data->words); i++)
		data->words[i] = 0x1234 + i * 0x0F31;
	*core = test_core_create(program, data);
	test_core_set_direct_data(core, data);
	core->state.r[0] = 0x04;
	core->state.r[1] = 0x10;
	core->state.r[2] = 0x24;
	core->state.y[0] = 0x0321;
}

static void test_relocated_fast_loop(void) {
	test_memory_t program, data, reference_program, reference_data;
	pmb887x_dsp_tcg_core_t core, reference;

	/* The first pass compiles the body, which maps the loop for the second. */
	test_relocated_fast_loop_init(&program, &data, &core);
	test_execute_slices_until(&core, 13, 8);
	core.state.pc = 0;
	test_execute_slices_until(&core, 13, 8);
	g_assert_cmpuint(core.fast_runs, !=, 0);

	/* Single blocks never enter a fast loop. */
	test_relocated_fast_loop_init(&reference_program, &reference_data, &reference);
	test_execute_until(&reference, 13, 64);
	reference.state.pc = 0;
	test_execute_until(&reference, 13, 64);
	g_assert_cmpuint(reference.fast_runs, ==, 0);

	core.state.exit_reason = reference.state.exit_reason;
	g_assert_cmpmem(&core.state, sizeof(core.state), &reference.state, sizeof(reference.state));
	g_assert_cmpmem(data.words, sizeof(data.words), reference_data.words, sizeof(reference_data.words));
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	g_assert_true(pmb887x_dsp_tcg_execute_slice(&core, 10));
	g_assert_cmphex(core.state.repeat_active, ==, 1);

	core.state.pc = 1;
	core.state.repeat_active = 0;
	core.state.a[0] = 0;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.sp = 10;
	g_assert_true(pmb887x_dsp_tcg_execute_slice(&core, 5));
	g_assert_cmphex(core.state.a[0], ==, 4);
	g_assert_cmphex(core.state.pc, ==, 1);

	core.state.ie = 1;
	core.state.interrupt_mask = BIT(0);
	core.state.pending_interrupts = BIT(0);
	g_assert_true(pmb887x_dsp_tcg_service_interrupt(&core));
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.sp = 10;
	g_assert_true(pmb887x_dsp_tcg_execute_slice(&core, 5));
	g_assert_cmphex(core.state.a[0], ==, 4);
	core.state.ie = 1;
	core.state.interrupt_mask = BIT(0);
	core.state.pending_interrupts = BIT(0);
	g_assert_true(pmb887x_dsp_tcg_service_interrupt(&core));

	core.state.pc = 1;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.r[0], ==, 0x0300);
	g_assert_cmphex(core.state.r[1], ==, 0x1000);
}

static void test_multiply_status_register(void) {
	test_memory_t program = {
		.words = { 0x8048, 0x4180, 0x0003 },
	};
	test_memory_t data = {};
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.y[0] = 2;
	core.state.sat = 1;
	core.state.ie = 1;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.x[0], ==, 3);
	g_assert_cmphex(core.state.p[0], ==, 6);
}

static void test_alu_status_register(void) {
	test_memory_t program = {
		.words = { 0x80A8, 0x4180, 0x0003 },
	};
	test_memory_t data = {};
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.sat = 1;
	core.state.ie = 1;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
		pmb887x_dsp_tcg_step_t step_mode = step == 1 ? PMB887X_DSP_TCG_STEP_INCREASE :
			step == -1 ? PMB887X_DSP_TCG_STEP_DECREASE : PMB887X_DSP_TCG_STEP_PLUS_STEP;
		bool is_i_register = address_register < 4;
		uint16_t load_modulo = (is_i_register ? 0x0200U : 0x0A00U) | vectors[i].modulo;
		uint16_t load_step = (is_i_register ? 0xDB80U : 0xDF80U) | (step & 0x7F);
		uint16_t mov_data = 0x98C0U | step_mode << 3 | address_register;
		test_memory_t program = {};
		size_t words = 0;

		program.words[words++] = load_modulo;
		if (step_mode == PMB887X_DSP_TCG_STEP_PLUS_STEP)
			program.words[words++] = load_step;
		program.words[words] = mov_data;
		test_memory_t data = {};
		pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

		core.memory.data.read = test_zero_data_read;
		core.state.r[address_register] = vectors[i].address;
		core.state.modulo_enable = BIT(address_register);
		g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
		g_assert_cmphex(core.state.r[address_register], ==, vectors[i].expected);
	}
}

static void test_dual_memory_spaces(void) {
	test_memory_t program = {
		.words = { 0xD000, 0x4180, 0x0003 },
	};
	test_memory_t data = {
		.words = { [0x10] = 0xFFFF, [0x18] = 0x8001, [0x20] = 0x8001 },
	};
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	test_core_set_direct_data(&core, &data);
	core.state.r[0] = 0x10;
	core.state.r[4] = 0x18;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.x[0], ==, 0xFFFF);
	g_assert_cmphex(core.state.y[0], ==, 0);
	g_assert_cmphex(core.state.p[0], ==, 0);

	core = test_core_create(&program, &data);
	test_core_set_direct_data(&core, &data);
	core.state.r[0] = 0x10;
	core.state.r[4] = 0x20;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	test_core_set_direct_data(&core, &data);
	core.state.a[0] = 0xA55A;
	core.state.r[0] = 0x11;
	core.state.r[1] = 0x10;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(data.words[0x11], ==, 0xA55A);
	g_assert_cmphex(core.state.a[0], ==, 0x8001);
}

static void test_program_context_cache(void) {
	test_memory_t program = {
		/* inc a0; br 0x0003 always. */
		.words = { 0x67D0, 0x4180, 0x0003 },
	};
	test_memory_t data = {};
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 1);
	g_assert_cmpuint(core.cache_compiles, ==, 1);

	/* dec a0; br 0x0003 always. */
	program.words[0] = 0x67E0;
	core.state.pc = 0;
	pmb887x_dsp_tcg_set_program_context(&core, 1);
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 0);
	g_assert_cmpuint(core.cache_compiles, ==, 2);

	program.words[0] = 0x67D0;
	core.state.pc = 0;
	pmb887x_dsp_tcg_set_program_context(&core, 0);
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 2);
	g_assert_cmpuint(core.cache_compiles, ==, 1);

	program.words[1] = 0x67E0; /* dec a0. */
	pmb887x_dsp_tcg_invalidate_program(&core, 1);
	core.state.pc = 0;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 2);
	g_assert_cmpuint(core.cache_compiles, ==, 2);
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	test_core_set_direct_data(&core, &data);
	teak_tcg_precompile_entry(&core, 0x20);
	uint64_t compiles = core.cache_compiles;

	for (uint16_t pc = 0x10; pc <= 0x18; pc += 4) {
		core.state.pc = pc;
		g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
	uint16_t first_bank[] = { 0xA55A };
	uint16_t second_bank[] = { 0xBEEF };
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	test_core_set_direct_data(&core, &data);
	core.memory.direct_data_bank = first_bank;
	core.memory.direct_data_bank_base = 0x10;
	core.memory.direct_data_bank_size = ARRAY_SIZE(first_bank);
	core.state.r[1] = 0x10;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 0xA55A);

	core.memory.direct_data_bank = second_bank;
	core.state.pc = 0;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.r[4] = 0x10;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.shift_value, ==, 0xA55A);
	g_assert_cmphex(core.state.r[4], ==, 0x11);
}

static void test_mov_data_control_register(void) {
	test_memory_t program = {
		/* mov [r2++], cfgj; mov [r3], st1 */
		.words = { 0x1DEA, 0x1D23, 0x4180, 0x0004 },
	};
	test_memory_t data = {
		.words = { [0x10] = 0x0183, [0x18] = 0x0005 },
	};
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.r[2] = 0x10;
	core.state.r[3] = 0x18;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.stepj, ==, 0x03);
	g_assert_cmphex(core.state.modj, ==, 0x03);
	g_assert_cmphex(core.state.r[2], ==, 0x11);
	g_assert_cmphex(core.state.page, ==, 0x05);
}

static void test_mov_control_register_data(void) {
	test_memory_t program = {
		/* mov cfgi, [r1]; mov a0, [r0] */
		.words = { 0x19C1, 0x1B00, 0x4180, 0x0004 },
	};
	test_memory_t data = {};
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.stepi = 0x05;
	core.state.modi = 0x02;
	core.state.r[0] = 0x10;
	core.state.r[1] = 0x11;
	core.state.a[0] = 0x0000000123456789ULL;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(data.words[0x11], ==, 0x0105);
	g_assert_cmphex(data.words[0x10], ==, 0x6789);
}

static void test_mov_accumulator_high_extension_unaffected(void) {
	test_memory_t program = {
		.words = { 0x6500, 0x4180, 0x0003 },
	};
	test_memory_t data = {
		.words = { 0x8000 },
	};
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.a[0] = 0xFFFFFFFF00000000ULL;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&tst0_program, &data);

	core.state.a[0] = 0x00F0;
	core.state.r[0] = 0x0F00;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, 0x00F0);
	g_assert_cmphex(core.state.fz, ==, 1);

	core = test_core_create(&tst1_program, &data);
	core.state.a[1] = 0x00F0;
	core.state.r[1] = 0xF0F0;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[1], ==, 0x00F0);
	g_assert_cmphex(core.state.fz, ==, 1);
}

static void test_multiply_subtract_status_register(void) {
	test_memory_t program = {
		.words = { 0x90A8, 0x4180, 0x0003 },
	};
	test_memory_t data = {};
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.a[0] = 100;
	core.state.p[0] = 10;
	core.state.y[0] = 2;
	core.state.sat = 1;
	core.state.ie = 1;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.a[0] = 0x123456789ULL;
	core.state.sp = 10;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.a[0], ==, (uint64_t) (int64_t) (int16_t) 0xFF0B);
	g_assert_cmphex(core.state.nonmaskable_context, ==, 1);
	g_assert_cmphex(core.state.interrupt_context, ==, 5);
}

static void test_external_registers_disconnected(void) {
	test_memory_t program = {
		.words = { 0x2901, 0x0000, 0x0000, 0x5814, 0x4180, 0x0006 },
	};
	test_memory_t data = {};
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.r[0] = 0x10;
	core.state.b[0] = 0x123456789ULL;
	core.state.b[1] = 0xFEDCBA987ULL;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.r[0], ==, 0x12);
	g_assert_cmphex(core.state.b[0], ==, 0xFEDCBA987ULL);
	g_assert_cmphex(core.state.b[1], ==, 0xFEDCBA987ULL);
	g_assert_cmphex(core.state.fm, ==, 1);
	g_assert_cmphex(core.state.pc, ==, 4);
}

static void test_delayed_interrupt_return(void) {
	/* 0xD7C0 0x2102 0xD4BC 0x0010: retid; mov 2, a0l; mov a0l, [0x0010]. */
	test_memory_t program = {
		.words = { 0xD7C0, 0x2102, 0xD4BC, 0x0010 },
	};
	test_memory_t data = {
		.words = { [5] = 10, [0x10] = 0xA55A },
	};
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.sp = 5;
	core.state.maskable_interrupt_active = 1;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.pc, ==, 11);
	g_assert_cmphex(core.state.a[0], ==, 2);
	g_assert_cmphex(core.state.sp, ==, 6);
	g_assert_cmphex(core.state.ie, ==, 1);
	g_assert_cmphex(core.state.maskable_interrupt_active, ==, 0);
	g_assert_cmphex(data.words[0x10], ==, 0xA55A);
}

static void test_delayed_interrupt_return_exit_request(void) {
	/* 0xD7C0 0x2010 0x0000: retid; mov r0, [0x10]; nop. */
	test_memory_t program = {
		.words = { 0xD7C0, 0x2010, 0x0000 },
	};
	test_memory_t data = {
		.words = { [5] = 10 },
	};
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	data.exit_core_on_write = &core;
	core.state.r[0] = 0xA55A;
	core.state.sp = 5;
	core.state.maskable_interrupt_active = 1;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.pc, ==, 11);
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	data.exit_core_on_write = &core;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.pc, ==, 1);
	g_assert_cmpuint(core.last_block_cycles, ==, 1);
}

static void test_delayed_return_break(void) {
	/* 0xD780 0xD3C0 0x0000: retd; break; nop at EL71 GSM DSP P:0x08D98. */
	test_memory_t program = {
		.words = { 0xD780, 0xD3C0, 0x0000 },
	};
	test_memory_t data = {
		.words = { [5] = 10 },
	};
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.sp = 5;
	core.state.lp = 1;
	core.state.bcn = 1;
	core.state.block_repeat_start[0] = 0;
	core.state.block_repeat_end[0] = 2;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.a[0] = 0x1234;
	core.state.sp = 5;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
	g_assert_cmphex(core.state.pc, ==, 10);
	g_assert_cmphex(core.state.sp, ==, 6);
	g_assert_cmphex(data.words[0x10], ==, 0x1234);
	g_assert_cmphex(data.words[0x11], ==, 0x1234);
}

static void test_nested_nmi_interrupt_return(void) {
	test_memory_t program = {
		.words = { 0xD7C0, 0x2102, 0xD4BC, 0x0010 },
	};
	test_memory_t data = {
		.words = { [5] = 10, [0x10] = 0xA55A },
	};
	pmb887x_dsp_tcg_core_t core = test_core_create(&program, &data);

	core.state.sp = 5;
	core.state.nmi_active = 1;
	core.state.maskable_interrupt_active = 1;
	g_assert_true(pmb887x_dsp_tcg_execute_block(&core));
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
	g_test_add_func("/pmb887x/dsp/tcg/batched-loop-exit-after-write", test_batched_loop_exit_after_write);
	g_test_add_func("/pmb887x/dsp/tcg/block-repeat-body-exit-after-write", test_block_repeat_body_exit_after_write);
	g_test_add_func("/pmb887x/dsp/tcg/relocated-fast-loop", test_relocated_fast_loop);
	g_test_add_func("/pmb887x/dsp/tcg/repeated-instruction-without-repeat", test_repeated_instruction_without_repeat);
	g_test_add_func("/pmb887x/dsp/tcg/interrupt-repeat-nested-repeat", test_interrupt_repeat_nested_repeat);
	g_test_add_func("/pmb887x/dsp/tcg/interrupt-repeat-resumes-on-return", test_interrupt_repeat_resumes_on_return);
	g_test_add_func("/pmb887x/dsp/tcg/subroutine-block-repeat", test_subroutine_block_repeat);
	g_test_add_func("/pmb887x/dsp/tcg/status-reserved-read-bits", test_status_reserved_read_bits);
	g_test_add_func("/pmb887x/dsp/tcg/multiply-status-register", test_multiply_status_register);
	g_test_add_func("/pmb887x/dsp/tcg/alu-status-register", test_alu_status_register);
	g_test_add_func("/pmb887x/dsp/tcg/dual-memory-spaces", test_dual_memory_spaces);
	g_test_add_func("/pmb887x/dsp/tcg/direct-data-read-write", test_direct_data_read_write);
	g_test_add_func("/pmb887x/dsp/tcg/program-context-cache", test_program_context_cache);
	g_test_add_func("/pmb887x/dsp/tcg/program-invalidation", test_program_invalidation);
	g_test_add_func("/pmb887x/dsp/tcg/precompile-bounded-indirect-dispatch", test_precompile_bounded_indirect_dispatch);
	g_test_add_func("/pmb887x/dsp/tcg/direct-data-bank", test_direct_data_bank);
	g_test_add_func("/pmb887x/dsp/tcg/mov-data-shift-value", test_mov_data_shift_value);
	g_test_add_func("/pmb887x/dsp/tcg/mov-data-control-register", test_mov_data_control_register);
	g_test_add_func("/pmb887x/dsp/tcg/mov-control-register-data", test_mov_control_register_data);
	g_test_add_func("/pmb887x/dsp/tcg/mov-accumulator-high-extension-unaffected", test_mov_accumulator_high_extension_unaffected);
	g_test_add_func("/pmb887x/dsp/tcg/alu-accumulator-masks", test_alu_accumulator_masks);
	g_test_add_func("/pmb887x/dsp/tcg/multiply-subtract-status-register", test_multiply_subtract_status_register);
	g_test_add_func("/pmb887x/dsp/tcg/mov-stack-accumulator-low", test_mov_stack_accumulator_low);
	g_test_add_func("/pmb887x/dsp/tcg/icr-reserved-read-bits", test_icr_reserved_read_bits);
	g_test_add_func("/pmb887x/dsp/tcg/external-registers-disconnected", test_external_registers_disconnected);
	g_test_add_func("/pmb887x/dsp/tcg/movr-b-destination", test_movr_b_destination);
	g_test_add_func("/pmb887x/dsp/tcg/delayed-interrupt-return", test_delayed_interrupt_return);
	g_test_add_func("/pmb887x/dsp/tcg/delayed-interrupt-return-exit-request", test_delayed_interrupt_return_exit_request);
	g_test_add_func("/pmb887x/dsp/tcg/exit-request-counts-executed-cycles", test_exit_request_counts_executed_cycles);
	g_test_add_func("/pmb887x/dsp/tcg/delayed-return-break", test_delayed_return_break);
	g_test_add_func("/pmb887x/dsp/tcg/delayed-return-stores", test_delayed_return_stores);
	g_test_add_func("/pmb887x/dsp/tcg/nested-nmi-interrupt-return", test_nested_nmi_interrupt_return);
	return g_test_run();
}
