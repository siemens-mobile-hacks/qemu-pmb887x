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

	g_test_add_func("/pmb887x/dsp/tcg/mov-full-accumulator", test_mov_full_accumulator);
	g_test_add_func("/pmb887x/dsp/tcg/mov-full-accumulator-alias", test_mov_full_accumulator_alias);
	g_test_add_func("/pmb887x/dsp/tcg/pop-full-accumulator", test_pop_full_accumulator);
	g_test_add_func("/pmb887x/dsp/tcg/long-block-repeat", test_long_block_repeat);
	g_test_add_func("/pmb887x/dsp/tcg/status-reserved-read-bits", test_status_reserved_read_bits);
	g_test_add_func("/pmb887x/dsp/tcg/multiply-status-register", test_multiply_status_register);
	g_test_add_func("/pmb887x/dsp/tcg/alu-status-register", test_alu_status_register);
	g_test_add_func("/pmb887x/dsp/tcg/dual-memory-spaces", test_dual_memory_spaces);
	g_test_add_func("/pmb887x/dsp/tcg/direct-data-read-write", test_direct_data_read_write);
	g_test_add_func("/pmb887x/dsp/tcg/mov-accumulator-high-extension-unaffected", test_mov_accumulator_high_extension_unaffected);
	g_test_add_func("/pmb887x/dsp/tcg/alu-accumulator-masks", test_alu_accumulator_masks);
	g_test_add_func("/pmb887x/dsp/tcg/multiply-subtract-status-register", test_multiply_subtract_status_register);
	g_test_add_func("/pmb887x/dsp/tcg/mov-stack-accumulator-low", test_mov_stack_accumulator_low);
	g_test_add_func("/pmb887x/dsp/tcg/icr-reserved-read-bits", test_icr_reserved_read_bits);
	g_test_add_func("/pmb887x/dsp/tcg/external-registers-disconnected", test_external_registers_disconnected);
	g_test_add_func("/pmb887x/dsp/tcg/movr-b-destination", test_movr_b_destination);
	g_test_add_func("/pmb887x/dsp/tcg/delayed-interrupt-return", test_delayed_interrupt_return);
	g_test_add_func("/pmb887x/dsp/tcg/nested-nmi-interrupt-return", test_nested_nmi_interrupt_return);
	return g_test_run();
}
