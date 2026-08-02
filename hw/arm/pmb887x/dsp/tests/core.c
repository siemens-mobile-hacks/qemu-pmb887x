#include "qemu/osdep.h"

#include "hw/arm/pmb887x/dsp/core.h"

#define TEST_MEMORY_WORDS	226

typedef struct test_memory_t test_memory_t;

struct test_memory_t {
	uint16_t words[TEST_MEMORY_WORDS];
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
}

static pmb887x_dsp_tcg_memory_space_t test_memory_space(test_memory_t *memory) {
	pmb887x_dsp_tcg_memory_space_t space = {
		.opaque = memory,
		.read = test_memory_read,
		.write = test_memory_write,
	};
	return space;
}

static void test_reset(void) {
	test_memory_t program = {};
	test_memory_t data = {};
	pmb887x_dsp_tcg_memory_t memory = {
		.program = test_memory_space(&program),
		.data = test_memory_space(&data),
		.y_space_base = 0x20,
	};
	pmb887x_dsp_tcg_core_t core;
	pmb887x_dsp_tcg_core_init(&core, &memory);

	memset(&core.state, 0xFF, sizeof(core.state));
	pmb887x_dsp_tcg_core_reset(&core, 0x52345);

	g_assert_cmphex(core.state.pc, ==, 0x2345);
	g_assert_cmphex(core.state.pending_interrupts, ==, 0);
	g_assert_cmpint(core.state.exit_reason, ==, PMB887X_DSP_TCG_EXIT_NONE);
	g_assert_cmphex(core.state.a[0], ==, 0);
	g_assert_cmphex(core.state.r[7], ==, 0);
	g_assert_cmphex(core.state.fz, ==, 0);
	g_assert_cmphex(core.state.ie, ==, 0);
	g_assert_cmphex(core.state.sat, ==, 0);
	g_assert_cmphex(core.state.sata, ==, 1);
	g_assert_cmphex(core.state.s, ==, 0);
	g_assert_cmphex(core.state.page, ==, 0);
	g_assert_cmphex(core.state.alternate_page, ==, 0);
	g_assert_cmphex(core.state.product_shift, ==, 0);
	g_assert_cmphex(core.state.product_extension[0], ==, 0);
	g_assert_cmphex(core.state.product_extension[1], ==, 0);
	g_assert_cmphex(core.state.mixp, ==, 0);
	g_assert_cmphex(core.state.dvm, ==, 0);
	g_assert_cmphex(core.state.interrupt_mask, ==, 0);
	g_assert_cmphex(core.state.shadow_st0, ==, 0);
	g_assert_cmphex(core.state.shadow_st1, ==, 0);
	g_assert_cmphex(core.state.shadow_st2, ==, 0);
	g_assert_cmphex(core.state.stepi, ==, 0);
	g_assert_cmphex(core.state.stepj, ==, 0);
	g_assert_cmphex(core.state.modi, ==, 0);
	g_assert_cmphex(core.state.modj, ==, 0);
	g_assert_cmphex(core.state.r0b, ==, 0);
	g_assert_cmphex(core.state.r1b, ==, 0);
	g_assert_cmphex(core.state.r4b, ==, 0);
	g_assert_cmphex(core.state.modib, ==, 0);
	g_assert_cmphex(core.state.stepib, ==, 0);
	g_assert_cmphex(core.state.modulo_enable, ==, 0);
	g_assert_cmphex(core.state.cpc, ==, 1);
	g_assert_cmphex(core.state.iu[0], ==, 0);
	g_assert_cmphex(core.state.iu[1], ==, 0);
	g_assert_cmphex(core.state.idle, ==, 0);
	g_assert_cmphex(core.state.halted, ==, 0);
	g_assert_cmphex(core.state.trap_active, ==, 0);
	g_assert_cmphex(core.state.nmi_active, ==, 0);
	g_assert_true(core.memory.program.opaque == &program);
	g_assert_true(core.memory.data.opaque == &data);
}

static void test_modulo_address(void) {
	pmb887x_dsp_tcg_state_t state = {
		.modi = 6,
		.modj = 6,
	};

	g_assert_cmphex(pmb887x_dsp_tcg_modulo_address(&state, 0, 0x0010, 1), ==, 0x0011);
	g_assert_cmphex(pmb887x_dsp_tcg_modulo_address(&state, 0, 0x0016, 1), ==, 0x0010);
	g_assert_cmphex(pmb887x_dsp_tcg_modulo_address(&state, 2, 0x0014, 2), ==, 0x0016);
	g_assert_cmphex(pmb887x_dsp_tcg_modulo_address(&state, 2, 0x0016, 2), ==, 0x0010);
	g_assert_cmphex(pmb887x_dsp_tcg_modulo_address(&state, 4, 0x0010, -3), ==, 0x0016);
	g_assert_cmphex(pmb887x_dsp_tcg_modulo_address(&state, 5, 0x0013, -3), ==, 0x0010);

	state.modi = 5;
	g_assert_cmphex(pmb887x_dsp_tcg_modulo_address(&state, 1, 0x0016, 3), ==, 0x0011);
	g_assert_cmphex(pmb887x_dsp_tcg_modulo_address(&state, 1, 0x0015, 3), ==, 0x0010);
	state.modi = 0;
	g_assert_cmphex(pmb887x_dsp_tcg_modulo_address(&state, 3, 0x1234, 1), ==, 0x1234);
}

static void test_memory_spaces(void) {
	test_memory_t program = {};
	test_memory_t data = {};
	pmb887x_dsp_tcg_memory_t memory = {
		.program = test_memory_space(&program),
		.data = test_memory_space(&data),
		.y_space_base = 0x20,
	};
	pmb887x_dsp_tcg_core_t core;
	pmb887x_dsp_tcg_core_init(&core, &memory);

	program.words[3] = 0x1234;
	data.words[3] = 0x5678;
	g_assert_cmphex(pmb887x_dsp_tcg_program_read(&core, 3), ==, 0x1234);
	g_assert_cmphex(pmb887x_dsp_tcg_data_read(&core, 3), ==, 0x5678);

	pmb887x_dsp_tcg_program_write(&core, 5, 0xABCD);
	pmb887x_dsp_tcg_data_write(&core, 5, 0xEF01);
	g_assert_cmphex(program.words[5], ==, 0xABCD);
	g_assert_cmphex(data.words[5], ==, 0xEF01);
}

static void test_decode(void) {
	static const uint8_t accumulator_half_registers[] = { 18, 16, 19, 17, 26, 28, 27, 29 };
	test_memory_t program = {
		.words = {
			0x5E08, 0x0001, 0x4380, 0x4180, 0x2010, 0xFFFF, 0x0000, 0x67D0,
			0x77D0, 0x67E0, 0x6760, 0x6780, 0x6740, 0x6750, 0x6790, 0x6700,
			0x6710, 0x6720, 0x6730, 0x67DF, 0x7FFF,
			0x6808, 0x2809, 0x042A, 0x43C0, 0x4191, 0x3456, 0x5032,
			0x41E4, 0xABCD, 0x17E5, 0x4581, 0x45C1, 0x0910,
			0xC67F, 0xCD80, 0xCE55, 0xC012, 0xC334, 0xC556, 0xA812, 0xAB34,
			0x86C0, 0xFFFF, 0x8DC0, 0x1234, 0x80C0, 0xABCD,
			0xA012, 0xA334, 0xA456, 0xA678, 0xAC9A, 0xAFBC,
			0x4012, 0x437F, 0x4456, 0x4778, 0x4C01, 0x4F40,
			0xD4F8, 0x0108, 0xD5F9, 0x0123, 0xD4FA, 0x0234,
			0xD5DB, 0x0008, 0xD4DE, 0x0010, 0xD5DF, 0x0020,
			0x80A0, 0x83A5, 0x84A6, 0x87A7, 0x8CA3, 0x8FA4,
			0x81B8, 0x82B9, 0x85B8, 0x86B9, 0x8DB8, 0x8EB9,
			0x80BA, 0x83BB, 0x84BC, 0x87BD, 0x8DBA, 0x8EBD,
			0x80AD, 0x83B0, 0x84B1, 0x87B2, 0x8CB3, 0x8FAD,
			0x8080, 0x8389, 0x8494, 0x879A, 0x8C9D, 0x8F8F,
			0xDBFE, 0xDF83,
			0x1C88, 0x1CD3, 0x1CFD, 0x1C00, 0x1841, 0x18AC, 0x18D0, 0x18FE,
			0x1F08, 0x1F28, 0x1F48, 0x1F68, 0x1F88, 0x1FA8, 0x1E08, 0x1E28,
			0x1E48, 0x1E68, 0x98C8, 0x99D9, 0x1B48, 0x1B88, 0x1A08, 0x1A48,
			0x9020, 0x9329, 0x9834, 0x9F3E,
			0x03FF, 0x0B23,
			0x0098, 0x00AD,
			0x5F40, 0xCAFE,
			0x5E46, 0x5E67,
			0x47E5,
			0x58E6,
			0x0C7F,
			0x0D06,
			0x5E07, 0xBEEF, 0x5E0D, 0x0101,
			0x5C02, 0x0123,
			0x5D46, 0x0123,
			0xD3C0,
			0xD780, 0xD7C0,
			0xD380, 0xD390,
			0x45D3,
			0x4D83,
			0x08FE,
			0x86AB,
			0x8045, 0x8947,
			0x802A, 0x893D,
			0xE855,
			0x800B, 0xFFFE, 0x891D, 0xFFFE,
			0xD000, 0xD95E,
			0x5EA0, 0x5EA3,
			0x8A45,
			0xE255,
			0x820B, 0x1234,
			0x8A25, 0xDA5E, 0x5EA5,
			0xE655,
			0x860B, 0x5678,
			0x8E25, 0xDE5E, 0x5EAD, 0x8E45,
			0xE455, 0x5EA9, 0x8F45, 0xDF5E,
			0x8B45, 0xDD5E, 0x5EAB,
			0xBA55, 0xBD55, 0x9A8D, 0x9D9D, 0x9AA7, 0x9DA5,
			0xB155, 0x918D, 0x91A5, 0x91DD, 0xBEEF, 0xD1DE,
			0xB355, 0xB555, 0xB755, 0xB955, 0xBF55,
			0x9280, 0x9489, 0x9694, 0x989A, 0x9E85,
			0x93A0, 0x95A5, 0x97A6, 0x99A7, 0x9FA3,
			0x93FC, 0x9EDF, 0xDAA3, 0xD6EB,
		},
	};
	test_memory_t data = {};
	pmb887x_dsp_tcg_memory_t memory = {
		.program = test_memory_space(&program),
		.data = test_memory_space(&data),
		.y_space_base = 0x20,
	};
	pmb887x_dsp_tcg_core_t core;
	pmb887x_dsp_tcg_core_init(&core, &memory);
	pmb887x_dsp_tcg_instruction_t instruction;
	const uint8_t moda_operations[] = { 0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
	const uint8_t modb_operations[] = { 0, 1, 2, 3, 4, 5, 6 };
	const uint16_t short_extension_bases[] = { 0x2900, 0x2D00, 0x3900, 0x3D00 };

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_IMM_REGISTER);
	g_assert_cmphex(instruction.expansion, ==, 0x0001);
	g_assert_cmpuint(instruction.words, ==, 2);
	g_assert_cmpuint(instruction.register_code, ==, 8);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 2, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_EINT);
	g_assert_cmpuint(instruction.words, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 3, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_BRANCH_ABSOLUTE);
	g_assert_cmphex(instruction.expansion, ==, 0x2010);
	g_assert_cmphex(instruction.branch_target, ==, 0x2010);
	g_assert_cmpint(instruction.condition, ==, PMB887X_DSP_TCG_CONDITION_TRUE);
	g_assert_cmpuint(instruction.words, ==, 2);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 5, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_TSTB_IMM8);
	g_assert_cmpuint(instruction.words, ==, 1);
	g_assert_cmphex(instruction.memory_address, ==, 0xFF);
	g_assert_cmpuint(instruction.bit_index, ==, 0xF);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 6, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_NOP);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 7, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MODA4_ACCUMULATOR);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);
	g_assert_cmpint(instruction.moda_operation, ==, PMB887X_DSP_TCG_MODA_INC);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 8, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MODA4_ACCUMULATOR);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_cmpint(instruction.moda_operation, ==, PMB887X_DSP_TCG_MODA_INC);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 9, &instruction));
	g_assert_cmpint(instruction.moda_operation, ==, PMB887X_DSP_TCG_MODA_DEC);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 10, &instruction));
	g_assert_cmpint(instruction.moda_operation, ==, PMB887X_DSP_TCG_MODA_CLR);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 11, &instruction));
	g_assert_cmpint(instruction.moda_operation, ==, PMB887X_DSP_TCG_MODA_NOT);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 12, &instruction));
	g_assert_cmpint(instruction.moda_operation, ==, PMB887X_DSP_TCG_MODA_ROR);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 13, &instruction));
	g_assert_cmpint(instruction.moda_operation, ==, PMB887X_DSP_TCG_MODA_ROL);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 14, &instruction));
	g_assert_cmpint(instruction.moda_operation, ==, PMB887X_DSP_TCG_MODA_NEG);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 15, &instruction));
	g_assert_cmpint(instruction.moda_operation, ==, PMB887X_DSP_TCG_MODA_SHR);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 16, &instruction));
	g_assert_cmpint(instruction.moda_operation, ==, PMB887X_DSP_TCG_MODA_SHR4);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 17, &instruction));
	g_assert_cmpint(instruction.moda_operation, ==, PMB887X_DSP_TCG_MODA_SHL);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 18, &instruction));
	g_assert_cmpint(instruction.moda_operation, ==, PMB887X_DSP_TCG_MODA_SHL4);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 19, &instruction));
	g_assert_cmpint(instruction.moda_operation, ==, PMB887X_DSP_TCG_MODA_INC);
	g_assert_cmpint(instruction.condition, ==, PMB887X_DSP_TCG_CONDITION_IU1);

	g_assert_false(pmb887x_dsp_tcg_decode(&core, 20, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_UNDEFINED);
	g_assert_cmpuint(instruction.words, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 21, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_DATA_IMM8_REGISTER);
	g_assert_cmpuint(instruction.register_code, ==, 2);
	g_assert_cmphex(instruction.memory_address, ==, 8);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 22, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_REGISTER_DATA_IMM8);
	g_assert_cmpuint(instruction.register_code, ==, 4);
	g_assert_cmphex(instruction.memory_address, ==, 9);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 23, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_LOAD_PAGE);
	g_assert_cmphex(instruction.immediate, ==, 0x2A);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 24, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_DINT);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 25, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_BRANCH_ABSOLUTE);
	g_assert_cmphex(instruction.branch_target, ==, 0x3456);
	g_assert_cmpint(instruction.condition, ==, PMB887X_DSP_TCG_CONDITION_EQ);
	g_assert_cmpuint(instruction.words, ==, 2);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 27, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_BRANCH_RELATIVE);
	g_assert_cmphex(instruction.branch_target, ==, 31);
	g_assert_cmpint(instruction.condition, ==, PMB887X_DSP_TCG_CONDITION_NEQ);
	g_assert_cmpuint(instruction.words, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 28, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_CALL_ABSOLUTE);
	g_assert_cmphex(instruction.branch_target, ==, 0xABCD);
	g_assert_cmpint(instruction.condition, ==, PMB887X_DSP_TCG_CONDITION_GE);
	g_assert_cmpuint(instruction.words, ==, 2);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 30, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_CALL_RELATIVE);
	g_assert_cmphex(instruction.branch_target, ==, 29);
	g_assert_cmpint(instruction.condition, ==, PMB887X_DSP_TCG_CONDITION_LT);
	g_assert_cmpuint(instruction.words, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 31, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_RETURN);
	g_assert_cmpint(instruction.condition, ==, PMB887X_DSP_TCG_CONDITION_EQ);
	g_assert_cmpuint(instruction.words, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 32, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_RETURN_INTERRUPT);
	g_assert_cmpint(instruction.condition, ==, PMB887X_DSP_TCG_CONDITION_EQ);
	g_assert_false(instruction.context_switch);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 33, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_RETURN_STACK);
	g_assert_cmphex(instruction.immediate, ==, 0x10);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 34, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_IMMEDIATE_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_ADD);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);
	g_assert_cmphex(instruction.immediate, ==, 0x7F);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 35, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_CMP);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_cmphex(instruction.immediate, ==, 0x80);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 36, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_SUB);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);
	g_assert_cmphex(instruction.immediate, ==, 0x55);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 37, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_OR);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);
	g_assert_cmphex(instruction.immediate, ==, 0x12);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 38, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_AND);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_cmphex(instruction.immediate, ==, 0x34);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 39, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_XOR);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_cmphex(instruction.immediate, ==, 0x56);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 40, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_TEST_ACCUMULATOR_DATA_IMM8);
	g_assert_cmpint(instruction.accumulator_test, ==, PMB887X_DSP_TCG_ACCUMULATOR_TST0);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);
	g_assert_cmphex(instruction.memory_address, ==, 0x12);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 41, &instruction));
	g_assert_cmpint(instruction.accumulator_test, ==, PMB887X_DSP_TCG_ACCUMULATOR_TST1);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_cmphex(instruction.memory_address, ==, 0x34);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 42, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_IMMEDIATE_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_ADD);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);
	g_assert_cmpuint(instruction.words, ==, 2);
	g_assert_cmphex(instruction.immediate, ==, 0xFFFF);
	g_assert_cmphex(instruction.alu_operand, ==, UINT64_MAX);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 44, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_CMP);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_cmphex(instruction.alu_operand, ==, 0x1234);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 46, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_OR);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);
	g_assert_cmphex(instruction.alu_operand, ==, 0xABCD);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 48, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_DATA_IMM8_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_OR);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);
	g_assert_cmphex(instruction.memory_address, ==, 0x12);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 49, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_AND);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 50, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_XOR);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 51, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_ADD);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 52, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_CMP);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 53, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_SUB);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_cmphex(instruction.memory_address, ==, 0xBC);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 54, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_R7_OFFSET7_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_OR);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);
	g_assert_cmpint(instruction.memory_offset, ==, 0x12);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 55, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_AND);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_cmpint(instruction.memory_offset, ==, -1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 56, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_XOR);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 57, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_ADD);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 58, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_CMP);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 59, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_SUB);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_cmpint(instruction.memory_offset, ==, -64);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 60, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_DATA_IMM16_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_OR);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);
	g_assert_cmphex(instruction.expansion, ==, 0x0108);
	g_assert_cmpuint(instruction.words, ==, 2);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 62, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_AND);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 64, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_XOR);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 66, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_R7_OFFSET16_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_ADD);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_cmphex(instruction.expansion, ==, 8);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 68, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_CMP);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 70, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_SUB);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 72, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_REGISTER_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_OR);
	g_assert_cmpuint(instruction.register_code, ==, 0);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 73, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_AND);
	g_assert_cmpuint(instruction.register_code, ==, 5);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 74, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_XOR);
	g_assert_cmpuint(instruction.register_code, ==, 6);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 75, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_ADD);
	g_assert_cmpuint(instruction.register_code, ==, 7);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 76, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_CMP);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 77, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_SUB);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 78, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_REGISTER_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_OR);
	g_assert_cmpuint(instruction.register_code, ==, 24);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 79, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_AND);
	g_assert_cmpuint(instruction.register_code, ==, 25);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 80, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_XOR);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 81, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_ADD);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 82, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_CMP);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 83, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_SUB);
	g_assert_cmpuint(instruction.register_code, ==, 25);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 84, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_OR);
	g_assert_cmpuint(instruction.register_code, ==, 26);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 85, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_AND);
	g_assert_cmpuint(instruction.register_code, ==, 27);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 86, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_XOR);
	g_assert_cmpuint(instruction.register_code, ==, 28);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 87, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_ADD);
	g_assert_cmpuint(instruction.register_code, ==, 29);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 88, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_CMP);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 89, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_SUB);
	g_assert_cmpuint(instruction.register_code, ==, 29);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 90, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_OR);
	g_assert_cmpuint(instruction.register_code, ==, 13);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 91, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_AND);
	g_assert_cmpuint(instruction.register_code, ==, 16);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 92, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_XOR);
	g_assert_cmpuint(instruction.register_code, ==, 17);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 93, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_ADD);
	g_assert_cmpuint(instruction.register_code, ==, 18);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 94, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_CMP);
	g_assert_cmpuint(instruction.register_code, ==, 19);
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 95, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_SUB);
	g_assert_cmpuint(instruction.register_code, ==, 13);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 96, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_RN_STEP_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_OR);
	g_assert_cmpuint(instruction.address_register, ==, 0);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_ZERO);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 97, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_AND);
	g_assert_cmpuint(instruction.address_register, ==, 1);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_INCREASE);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 98, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_XOR);
	g_assert_cmpuint(instruction.address_register, ==, 4);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_DECREASE);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 99, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_ADD);
	g_assert_cmpuint(instruction.address_register, ==, 2);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_PLUS_STEP);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 100, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_CMP);
	g_assert_cmpuint(instruction.address_register, ==, 5);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_PLUS_STEP);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 101, &instruction));
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_SUB);
	g_assert_cmpuint(instruction.address_register, ==, 7);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_INCREASE);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 102, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_LOAD_STEPI);
	g_assert_cmphex(instruction.immediate, ==, 0x7E);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 103, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_LOAD_STEPJ);
	g_assert_cmphex(instruction.immediate, ==, 3);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 104, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_DATA_RN_STEP_REGISTER);
	g_assert_cmpuint(instruction.register_code, ==, 4);
	g_assert_cmpuint(instruction.address_register, ==, 0);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_INCREASE);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 105, &instruction));
	g_assert_cmpuint(instruction.register_code, ==, 6);
	g_assert_cmpuint(instruction.address_register, ==, 3);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_DECREASE);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 106, &instruction));
	g_assert_cmpuint(instruction.register_code, ==, 7);
	g_assert_cmpuint(instruction.address_register, ==, 5);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_PLUS_STEP);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 107, &instruction));
	g_assert_cmpuint(instruction.register_code, ==, 0);
	g_assert_cmpuint(instruction.address_register, ==, 0);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_ZERO);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 108, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_REGISTER_DATA_RN_STEP);
	g_assert_cmpuint(instruction.register_code, ==, 2);
	g_assert_cmpuint(instruction.address_register, ==, 1);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_ZERO);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 109, &instruction));
	g_assert_cmpuint(instruction.register_code, ==, 5);
	g_assert_cmpuint(instruction.address_register, ==, 4);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_INCREASE);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 110, &instruction));
	g_assert_cmpuint(instruction.register_code, ==, 6);
	g_assert_cmpuint(instruction.address_register, ==, 0);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_DECREASE);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 111, &instruction));
	g_assert_cmpuint(instruction.register_code, ==, 7);
	g_assert_cmpuint(instruction.address_register, ==, 6);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_PLUS_STEP);

	for (uint32_t address = 112; address <= 121; address++) {
		g_assert_true(pmb887x_dsp_tcg_decode(&core, address, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_DATA_RN_STEP_REGISTER);
	}
	g_assert_cmpuint(instruction.register_code, ==, 19);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 122, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_DATA_RN_STEP_B_ACCUMULATOR);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);
	g_assert_cmpuint(instruction.address_register, ==, 0);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_INCREASE);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 123, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_DATA_RN_STEP_B_ACCUMULATOR);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_cmpuint(instruction.address_register, ==, 1);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_PLUS_STEP);

	for (uint32_t address = 124; address <= 127; address++) {
		g_assert_true(pmb887x_dsp_tcg_decode(&core, address, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_REGISTER_DATA_RN_STEP);
	}
	g_assert_cmpuint(instruction.register_code, ==, 18);

	for (uint32_t address = 128; address <= 131; address++) {
		g_assert_true(pmb887x_dsp_tcg_decode(&core, address, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_TSTB_RN_STEP);
	}
	g_assert_cmpuint(instruction.address_register, ==, 6);
	g_assert_cmpuint(instruction.bit_index, ==, 15);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_PLUS_STEP);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 132, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_LOAD_MODI);
	g_assert_cmphex(instruction.immediate, ==, 0x1FF);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 133, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_LOAD_MODJ);
	g_assert_cmphex(instruction.immediate, ==, 0x123);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 134, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MODIFY_RN);
	g_assert_cmpuint(instruction.address_register, ==, 0);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_PLUS_STEP);
	g_assert_false(instruction.disable_modulo);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 135, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MODIFY_RN);
	g_assert_cmpuint(instruction.address_register, ==, 5);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_INCREASE);
	g_assert_true(instruction.disable_modulo);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 136, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_PUSH_IMMEDIATE);
	g_assert_cmphex(instruction.expansion, ==, 0xCAFE);
	g_assert_cmpuint(instruction.words, ==, 2);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 138, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_PUSH_REGISTER);
	g_assert_cmpuint(instruction.register_code, ==, 6);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 139, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_POP_REGISTER);
	g_assert_cmpuint(instruction.register_code, ==, 7);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 140, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_STACK_REGISTER);
	g_assert_cmpuint(instruction.register_code, ==, 5);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 141, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_REGISTER_REGISTER);
	g_assert_cmpuint(instruction.register_code, ==, 6);
	g_assert_cmpuint(instruction.destination_register_code, ==, 7);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 142, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_REPEAT_IMMEDIATE);
	g_assert_cmphex(instruction.immediate, ==, 0x7F);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 143, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_REPEAT_REGISTER);
	g_assert_cmpuint(instruction.register_code, ==, 6);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 144, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_IMM_REGISTER);
	g_assert_cmpuint(instruction.register_code, ==, 7);
	g_assert_cmphex(instruction.expansion, ==, 0xBEEF);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 146, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_IMM_REGISTER);
	g_assert_cmpuint(instruction.register_code, ==, 13);
	g_assert_cmphex(instruction.expansion, ==, 0x0101);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 148, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_BLOCK_REPEAT_IMMEDIATE);
	g_assert_cmphex(instruction.immediate, ==, 2);
	g_assert_cmphex(instruction.branch_target, ==, 0x0123);
	g_assert_cmpuint(instruction.words, ==, 2);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 150, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_BLOCK_REPEAT_REGISTER);
	g_assert_cmpuint(instruction.register_code, ==, 6);
	g_assert_cmphex(instruction.branch_target, ==, 0x0123);
	g_assert_cmpuint(instruction.words, ==, 2);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 152, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_BREAK);
	g_assert_cmpuint(instruction.words, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 153, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_DELAYED_RETURN);
	g_assert_cmpuint(instruction.words, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 154, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_DELAYED_RETURN_INTERRUPT);
	g_assert_cmpuint(instruction.words, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 155, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_CONTEXT_STORE);
	g_assert_cmpuint(instruction.words, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 156, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_CONTEXT_RESTORE);
	g_assert_cmpuint(instruction.words, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 157, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_RETURN_INTERRUPT);
	g_assert_cmpint(instruction.condition, ==, PMB887X_DSP_TCG_CONDITION_GT);
	g_assert_true(instruction.context_switch);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 158, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_LOAD_PRODUCT_SHIFT);
	g_assert_cmphex(instruction.immediate, ==, 3);
	g_assert_cmpuint(instruction.words, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 159, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_IMMEDIATE);
	g_assert_cmphex(instruction.immediate, ==, 0xFE);
	g_assert_cmpuint(instruction.words, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 160, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_REGISTER_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_ADD);
	g_assert_cmpuint(instruction.register_code, ==, 11);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 161, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_REGISTER);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MPY);
	g_assert_cmpuint(instruction.register_code, ==, 5);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 162, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_REGISTER);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MPYSU);
	g_assert_cmpuint(instruction.register_code, ==, 7);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 163, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_RN_STEP);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MPY);
	g_assert_cmpuint(instruction.address_register, ==, 2);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_INCREASE);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 164, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_RN_STEP);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MPYSU);
	g_assert_cmpuint(instruction.address_register, ==, 5);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_PLUS_STEP);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 165, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_DATA_IMM8);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MPY);
	g_assert_cmphex(instruction.memory_address, ==, 0x55);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 166, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_RN_IMMEDIATE);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MPY);
	g_assert_cmpuint(instruction.address_register, ==, 3);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_INCREASE);
	g_assert_cmphex(instruction.expansion, ==, 0xFFFE);
	g_assert_cmpuint(instruction.words, ==, 2);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 168, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_RN_IMMEDIATE);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MPYSU);
	g_assert_cmpuint(instruction.address_register, ==, 5);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_PLUS_STEP);
	g_assert_cmphex(instruction.expansion, ==, 0xFFFE);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_cmpuint(instruction.words, ==, 2);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 170, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_DUAL_RN);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MPY);
	g_assert_cmpuint(instruction.y_address_register, ==, 4);
	g_assert_cmpint(instruction.y_step, ==, PMB887X_DSP_TCG_STEP_ZERO);
	g_assert_cmpuint(instruction.address_register, ==, 0);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_ZERO);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 171, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_DUAL_RN);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MPYSU);
	g_assert_cmpuint(instruction.y_address_register, ==, 5);
	g_assert_cmpint(instruction.y_step, ==, PMB887X_DSP_TCG_STEP_DECREASE);
	g_assert_cmpuint(instruction.address_register, ==, 2);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_PLUS_STEP);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 172, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_R6);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MPY);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 173, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_R6);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MPYSU);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 174, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_REGISTER);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MAC);
	g_assert_cmpuint(instruction.register_code, ==, 5);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 175, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_DATA_IMM8);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MAC);
	g_assert_cmphex(instruction.memory_address, ==, 0x55);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 176, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_RN_IMMEDIATE);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MAC);
	g_assert_cmpuint(instruction.address_register, ==, 3);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_INCREASE);
	g_assert_cmphex(instruction.expansion, ==, 0x1234);
	g_assert_cmpuint(instruction.words, ==, 2);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 178, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_RN_STEP);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MAC);
	g_assert_cmpuint(instruction.address_register, ==, 5);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 179, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_DUAL_RN);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MAC);
	g_assert_cmpuint(instruction.y_address_register, ==, 5);
	g_assert_cmpuint(instruction.address_register, ==, 2);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 180, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_R6);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MAC);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 181, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_DATA_IMM8);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MACSU);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 182, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_RN_IMMEDIATE);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MACSU);
	g_assert_cmphex(instruction.expansion, ==, 0x5678);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 184, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_RN_STEP);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MACSU);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 185, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_DUAL_RN);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MACSU);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 186, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_R6);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MACSU);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 187, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_REGISTER);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MACSU);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 188, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_DATA_IMM8);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MAA);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 189, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_R6);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MAA);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 190, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_REGISTER);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MAASU);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 191, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_DUAL_RN);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MAASU);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 192, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_REGISTER);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MACUS);
	g_assert_cmpuint(instruction.register_code, ==, 5);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 193, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_DUAL_RN);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MACUU);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 194, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_R6);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MACUU);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 195, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_DATA_IMM8_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_SQR);
	g_assert_cmphex(instruction.memory_address, ==, 0x55);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 196, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_DATA_IMM8_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_SQRA);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 197, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_RN_STEP_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_SQR);
	g_assert_cmpuint(instruction.address_register, ==, 5);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_INCREASE);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 198, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_RN_STEP_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_SQRA);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_PLUS_STEP);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 199, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_REGISTER_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_SQR);
	g_assert_cmpuint(instruction.register_code, ==, 7);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 200, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_REGISTER_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_SQRA);
	g_assert_cmpuint(instruction.register_code, ==, 5);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 201, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_DATA_IMM8_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_MSU);
	g_assert_cmphex(instruction.memory_address, ==, 0x55);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 202, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_RN_STEP_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_MSU);
	g_assert_cmpuint(instruction.address_register, ==, 5);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_INCREASE);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 203, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALU_REGISTER_ACCUMULATOR);
	g_assert_cmpint(instruction.alu_operation, ==, PMB887X_DSP_TCG_ALU_MSU);
	g_assert_cmpuint(instruction.register_code, ==, 5);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 204, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_RN_IMMEDIATE);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MSU);
	g_assert_cmpuint(instruction.address_register, ==, 5);
	g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_PLUS_STEP);
	g_assert_cmphex(instruction.expansion, ==, 0xBEEF);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 206, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MULTIPLY_DUAL_RN);
	g_assert_cmpint(instruction.multiply_operation, ==, PMB887X_DSP_TCG_MULTIPLY_MSU);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	const pmb887x_dsp_tcg_alu_operation_t operations[] = {
		PMB887X_DSP_TCG_ALU_ADDH,
		PMB887X_DSP_TCG_ALU_ADDL,
		PMB887X_DSP_TCG_ALU_SUBH,
		PMB887X_DSP_TCG_ALU_SUBL,
		PMB887X_DSP_TCG_ALU_CMPU,
	};
	const pmb887x_dsp_tcg_opcode_t opcodes[] = {
		PMB887X_DSP_TCG_OPCODE_ALU_DATA_IMM8_ACCUMULATOR,
		PMB887X_DSP_TCG_OPCODE_ALU_RN_STEP_ACCUMULATOR,
		PMB887X_DSP_TCG_OPCODE_ALU_REGISTER_ACCUMULATOR,
	};
	for (size_t opcode_index = 0; opcode_index < ARRAY_SIZE(opcodes); opcode_index++) {
		for (size_t operation_index = 0; operation_index < ARRAY_SIZE(operations); operation_index++) {
			uint32_t address = 207U + opcode_index * ARRAY_SIZE(operations) + operation_index;

			g_assert_true(pmb887x_dsp_tcg_decode(&core, address, &instruction));
			g_assert_cmpint(instruction.opcode, ==, opcodes[opcode_index]);
			g_assert_cmpint(instruction.alu_operation, ==, operations[operation_index]);
		}
	}
	g_assert_cmphex(instruction.register_code, ==, 3);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 222, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_SHIFT_IMMEDIATE);
	g_assert_cmpuint(instruction.source_accumulator, ==, 0);
	g_assert_cmpuint(instruction.destination_accumulator, ==, 3);
	g_assert_cmpint(instruction.shift, ==, -4);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 223, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_SHIFT_IMMEDIATE);
	g_assert_cmpuint(instruction.source_accumulator, ==, 3);
	g_assert_cmpuint(instruction.destination_accumulator, ==, 1);
	g_assert_cmpint(instruction.shift, ==, 31);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 224, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_SHIFT_CONDITIONAL);
	g_assert_cmpuint(instruction.source_accumulator, ==, 2);
	g_assert_cmpuint(instruction.destination_accumulator, ==, 1);
	g_assert_cmpint(instruction.condition, ==, PMB887X_DSP_TCG_CONDITION_GT);

	g_assert_true(pmb887x_dsp_tcg_decode(&core, 225, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_SHIFT_CONDITIONAL);
	g_assert_cmpuint(instruction.source_accumulator, ==, 1);
	g_assert_cmpuint(instruction.destination_accumulator, ==, 3);
	g_assert_cmpint(instruction.condition, ==, PMB887X_DSP_TCG_CONDITION_L);

	for (uint16_t source = 0; source < 4; source++) {
		for (uint16_t destination = 0; destination < 4; destination++) {
			for (int16_t shift = -32; shift <= 31; shift++) {
				program.words[0] = 0x9240U | source << 10 | destination << 7 | ((uint16_t) shift & 0x3FU);
				g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
				g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_SHIFT_IMMEDIATE);
				g_assert_cmpuint(instruction.source_accumulator, ==, source);
				g_assert_cmpuint(instruction.destination_accumulator, ==, destination);
				g_assert_cmpint(instruction.shift, ==, shift);
			}

			for (uint16_t condition = 0; condition < PMB887X_DSP_TCG_CONDITION_COUNT; condition++) {
				program.words[0] = 0xD280U | source << 10 | destination << 5 | condition;
				g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
				g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_SHIFT_CONDITIONAL);
				g_assert_cmpuint(instruction.source_accumulator, ==, source);
				g_assert_cmpuint(instruction.destination_accumulator, ==, destination);
				g_assert_cmpuint(instruction.condition, ==, condition);
			}
		}
	}

	program.words[0] = 0xD480;
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_CALL_ACCUMULATOR);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);
	g_assert_cmpuint(instruction.words, ==, 1);

	program.words[0] = 0xD580;
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_CALL_ACCUMULATOR);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	program.words[0] = 0x4F90;
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_IMM_ICR);
	g_assert_cmphex(instruction.immediate, ==, 0x10);

	program.words[0] = 0x3101;
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_IMM8_ACCUMULATOR_LOW);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);
	g_assert_cmphex(instruction.immediate, ==, 1);

	program.words[0] = 0xD4B8;
	program.words[1] = 0x1234;
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_DATA_IMM16_ACCUMULATOR);
	g_assert_cmpuint(instruction.accumulator_index, ==, 0);
	g_assert_cmphex(instruction.expansion, ==, 0x1234);
	g_assert_cmpuint(instruction.words, ==, 2);

	program.words[0] = 0xD5BC;
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_ACCUMULATOR_LOW_DATA_IMM16);
	g_assert_cmpuint(instruction.accumulator_index, ==, 1);

	for (size_t i = 0; i < ARRAY_SIZE(moda_operations); i++) {
		for (uint16_t accumulator = 0; accumulator < 2; accumulator++) {
			for (uint16_t condition = 0; condition < PMB887X_DSP_TCG_CONDITION_COUNT; condition++) {
				program.words[0] = 0x6700U | accumulator << 12 | moda_operations[i] << 4 | condition;
				g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
				g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MODA4_ACCUMULATOR);
				g_assert_cmpuint(instruction.accumulator_index, ==, accumulator);
				g_assert_cmpuint(instruction.moda_operation, ==, moda_operations[i]);
				g_assert_cmpuint(instruction.condition, ==, condition);
			}
		}
	}

	for (size_t i = 0; i < ARRAY_SIZE(modb_operations); i++) {
		for (uint16_t accumulator = 0; accumulator < 2; accumulator++) {
			for (uint16_t condition = 0; condition < PMB887X_DSP_TCG_CONDITION_COUNT; condition++) {
				program.words[0] = 0x6F00U | accumulator << 12 | modb_operations[i] << 4 | condition;
				g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
				g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MODB3_ACCUMULATOR);
				g_assert_cmpuint(instruction.accumulator_index, ==, accumulator);
				g_assert_cmpuint(instruction.moda_operation, ==, modb_operations[i]);
				g_assert_cmpuint(instruction.condition, ==, condition);
			}
		}
	}

	for (uint16_t source = 0; source < 2; source++) {
		for (uint16_t destination = 0; destination < 2; destination++) {
			program.words[0] = 0x49C0U | source << 5 | destination << 4;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_LIMIT_ACCUMULATOR);
			g_assert_cmpuint(instruction.source_accumulator, ==, source);
			g_assert_cmpuint(instruction.destination_accumulator, ==, destination);
		}
	}

	for (uint16_t accumulator = 0; accumulator < 2; accumulator++) {
		for (uint16_t address_register = 0; address_register < 8; address_register++) {
			for (uint16_t step = 0; step < 4; step++) {
				program.words[0] = 0x94C0U | accumulator << 8 | step << 3 | address_register;
				g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
				g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_NORMALIZE);
				g_assert_cmpuint(instruction.accumulator_index, ==, accumulator);
				g_assert_cmpuint(instruction.address_register, ==, address_register);
				g_assert_cmpuint(instruction.step, ==, step);
			}
		}
	}
	for (uint16_t operation = 0; operation < PMB887X_DSP_TCG_SWAP_OPERATION_COUNT; operation++) {
		program.words[0] = 0x4980U | operation;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_SWAP_ACCUMULATORS);
		g_assert_cmpuint(instruction.swap_operation, ==, operation);
	}
	for (uint16_t operation = PMB887X_DSP_TCG_SWAP_OPERATION_COUNT; operation < 16; operation++) {
		program.words[0] = 0x4980U | operation;
		g_assert_false(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_UNDEFINED);
	}
	for (uint16_t flags = 0; flags < 64; flags++) {
		program.words[0] = 0x4B80U | flags;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_BANK_EXCHANGE);
		g_assert_cmpuint(instruction.immediate, ==, flags & 0xFU);
	}
	program.words[0] = 0x0020;
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_TRAP);
	g_assert_cmpuint(instruction.words, ==, 1);
	for (uint16_t source = 0; source < 4; source++) {
		for (uint16_t destination = 0; destination < 4; destination++) {
			program.words[0] = 0xD290U | source << 10 | destination << 5;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_ACCUMULATOR_ACCUMULATOR);
			g_assert_cmpuint(instruction.source_accumulator, ==, source);
			g_assert_cmpuint(instruction.destination_accumulator, ==, destination);
		}

		program.words[0] = 0xD298U | source << 10;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_ACCUMULATOR_LOW_SPECIAL);
		g_assert_cmpuint(instruction.source_accumulator, ==, source);
		g_assert_cmpint(instruction.special_register, ==, PMB887X_DSP_TCG_SPECIAL_DVM);

		program.words[0] = 0xD2D8U | source << 10;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.special_register, ==, PMB887X_DSP_TCG_SPECIAL_X0);

		program.words[0] = 0xD394U | source;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.special_register, ==, PMB887X_DSP_TCG_SPECIAL_X1);

		program.words[0] = 0xD384U | source;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_CONTEXT_STORE);
	}
	for (uint16_t destination = 0; destination < 4; destination++) {
		for (uint16_t special = 0; special < 4; special++) {
			program.words[0] = 0xD490U | destination << 5 | special;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_SPECIAL_ACCUMULATOR);
			g_assert_cmpuint(instruction.destination_accumulator, ==, destination);
			g_assert_cmpint(instruction.special_register, ==, special);
		}

		program.words[0] = 0x49C1U | destination << 4;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_LIMIT_ACCUMULATOR);

		program.words[0] = 0xD299U | destination << 10;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_ACCUMULATOR_LOW_SPECIAL);
		g_assert_cmpuint(instruction.source_accumulator, ==, destination);
		g_assert_cmpint(instruction.special_register, ==, PMB887X_DSP_TCG_SPECIAL_DVM);
	}
	for (uint16_t register_code = 0; register_code < 32; register_code++) {
		program.words[0] = 0x47C0U | register_code;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_MIXP_REGISTER);
		g_assert_cmpuint(instruction.destination_register_code, ==, register_code);

		program.words[0] = 0x5E80U | register_code;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_REGISTER_MIXP);
		g_assert_cmpuint(instruction.register_code, ==, register_code);

		program.words[0] = 0x4FC0U | register_code;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_REGISTER_ICR);
		g_assert_cmpuint(instruction.register_code, ==, register_code);
	}
	for (uint16_t immediate = 0; immediate <= UINT8_MAX; immediate++) {
		for (uint16_t destination = 0; destination < 8; destination++) {
			program.words[0] = 0x2300U | destination << 10 | immediate;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_SHORT_REGISTER);
			g_assert_cmpuint(instruction.register_code, ==, destination);
			g_assert_cmphex(instruction.immediate, ==, (uint16_t) (int16_t) (int8_t) immediate);
		}
		for (uint16_t destination = 0; destination < 2; destination++) {
			program.words[0] = 0x2500U | destination << 12 | immediate;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_SHORT_REGISTER);
			g_assert_cmpuint(instruction.register_code, ==, destination + 28);
		}
		for (uint16_t destination = 0; destination < 4; destination++) {
			program.words[0] = short_extension_bases[destination] | immediate;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_SHORT_REGISTER);
			g_assert_cmpuint(instruction.register_code, ==, destination + 20);
		}
		program.words[0] = 0x0500U | immediate;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_SHORT_REGISTER);
		g_assert_cmpuint(instruction.register_code, ==, 31);
	}

	for (uint16_t accumulator = 0; accumulator < 2; accumulator++) {
		for (uint16_t address = 0; address <= UINT8_MAX; address++) {
			program.words[0] = 0x0E00U | accumulator << 8 | address;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_DIVISION_STEP);
			g_assert_cmpuint(instruction.accumulator_index, ==, accumulator);
			g_assert_cmpuint(instruction.memory_address, ==, address);
		}
	}

	for (uint16_t operation = 0; operation < 4; operation++) {
		uint16_t accumulator_base = 0x8460U + operation * 0x0200U;

		for (uint16_t accumulator = 0; accumulator < 2; accumulator++) {
			for (uint16_t step = 0; step < 4; step++) {
				program.words[0] = accumulator_base | accumulator << 8 | step << 3;
				g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
				g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MINIMUM_MAXIMUM);
				g_assert_cmpuint(instruction.minmax_operation, ==, operation);
				g_assert_cmpuint(instruction.accumulator_index, ==, accumulator);
				g_assert_cmpuint(instruction.step, ==, step);
				g_assert_false(instruction.memory_source);
			}
		}
	}

	for (uint16_t operation = 0; operation < 4; operation++) {
		for (uint16_t accumulator = 0; accumulator < 2; accumulator++) {
			for (uint16_t step = 0; step < 4; step++) {
				if (operation < 2) {
					program.words[0] = 0x8060U | operation << 9 | accumulator << 8 | step << 3;
				} else {
					program.words[0] = 0x47A0U | (operation - 2) << 2 | accumulator << 3 | step;
				}
				g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
				g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MINIMUM_MAXIMUM);
				g_assert_cmpuint(instruction.minmax_operation, ==, operation);
				g_assert_cmpuint(instruction.accumulator_index, ==, accumulator);
				g_assert_cmpuint(instruction.step, ==, step);
				g_assert_true(instruction.memory_source);
			}
		}
	}

	for (uint16_t source = 0; source < 32; source++) {
		program.words[0] = 0x9440U | source;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_EXPONENT);
		g_assert_cmpint(instruction.exponent_source, ==, PMB887X_DSP_TCG_EXPONENT_REGISTER);
		g_assert_cmpuint(instruction.register_code, ==, source);
		g_assert_false(instruction.write_accumulator);

		for (uint16_t destination = 0; destination < 2; destination++) {
			program.words[0] = 0x9040U | destination << 8 | source;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_EXPONENT);
			g_assert_cmpint(instruction.exponent_source, ==, PMB887X_DSP_TCG_EXPONENT_REGISTER);
			g_assert_cmpuint(instruction.register_code, ==, source);
			g_assert_true(instruction.write_accumulator);
			g_assert_cmpuint(instruction.destination_accumulator, ==, destination);
		}
	}

	for (uint16_t source = 0; source < 2; source++) {
		program.words[0] = 0x9460U | source;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.exponent_source, ==, PMB887X_DSP_TCG_EXPONENT_B_ACCUMULATOR);
		g_assert_cmpuint(instruction.accumulator_index, ==, source);
		g_assert_false(instruction.write_accumulator);

		for (uint16_t destination = 0; destination < 2; destination++) {
			program.words[0] = 0x9060U | destination << 8 | source;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.exponent_source, ==, PMB887X_DSP_TCG_EXPONENT_B_ACCUMULATOR);
			g_assert_cmpuint(instruction.accumulator_index, ==, source);
			g_assert_true(instruction.write_accumulator);
			g_assert_cmpuint(instruction.destination_accumulator, ==, destination);
		}
	}

	for (uint16_t source = 0; source < 8; source++) {
		for (uint16_t step = 0; step < 4; step++) {
			program.words[0] = 0x9C40U | step << 3 | source;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.exponent_source, ==, PMB887X_DSP_TCG_EXPONENT_RN_STEP);
			g_assert_cmpuint(instruction.address_register, ==, source);
			g_assert_cmpuint(instruction.step, ==, step);
			g_assert_false(instruction.write_accumulator);

			for (uint16_t destination = 0; destination < 2; destination++) {
				program.words[0] = 0x9840U | destination << 8 | step << 3 | source;
				g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
				g_assert_cmpint(instruction.exponent_source, ==, PMB887X_DSP_TCG_EXPONENT_RN_STEP);
				g_assert_cmpuint(instruction.address_register, ==, source);
				g_assert_cmpuint(instruction.step, ==, step);
				g_assert_true(instruction.write_accumulator);
				g_assert_cmpuint(instruction.destination_accumulator, ==, destination);
			}
		}
	}

	program.words[0] = 0xD7C1;
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_DELAYED_RETURN_INTERRUPT);

	program.words[0] = 0xD382U;
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
	g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_CONTEXT_STORE);

	program.words[0] = 0xD392U;
	g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
	g_assert_cmpint(instruction.exponent_source, ==, PMB887X_DSP_TCG_EXPONENT_R6);
	g_assert_true(instruction.write_accumulator);
	g_assert_cmpuint(instruction.destination_accumulator, ==, 1);

	for (uint16_t accumulator = 0; accumulator < 2; accumulator++) {
		for (uint16_t destination = 0; destination < 32; destination++) {
			program.words[0] = 0x0040U | accumulator << 5 | destination;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOVP_ACCUMULATOR_LOW_REGISTER);
			g_assert_cmpuint(instruction.accumulator_index, ==, accumulator);
			g_assert_cmpuint(instruction.destination_register_code, ==, destination);
		}
	}

	for (uint16_t source = 0; source < 8; source++) {
		for (uint16_t source_step = 0; source_step < 4; source_step++) {
			for (uint16_t destination = 0; destination < 4; destination++) {
				for (uint16_t destination_step = 0; destination_step < 4; destination_step++) {
					program.words[0] = 0x0600U | destination_step << 7 | destination << 5 |
						source_step << 3 | source;
					g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
					g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOVP_RN_RN);
					g_assert_cmpuint(instruction.address_register, ==, source);
					g_assert_cmpuint(instruction.destination_register_code, ==, destination);
					g_assert_cmpuint(instruction.step, ==, source_step);
					g_assert_cmpuint(instruction.y_step, ==, destination_step);
				}
			}
		}
	}

	for (uint16_t source = 0; source < 4; source++) {
		for (uint16_t source_step = 0; source_step < 4; source_step++) {
			for (uint16_t destination = 0; destination < 2; destination++) {
				for (uint16_t destination_step = 0; destination_step < 4; destination_step++) {
					program.words[0] = 0x5F80U | destination_step << 5 | source_step << 3 |
						destination << 2 | source;
					g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
					g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOVD_RN_RN);
					g_assert_cmpuint(instruction.address_register, ==, source);
					g_assert_cmpuint(instruction.destination_register_code, ==, destination + 4);
					g_assert_cmpuint(instruction.step, ==, source_step);
					g_assert_cmpuint(instruction.y_step, ==, destination_step);
				}
			}
		}
	}

	for (uint16_t source = 0; source < 32; source++) {
		for (uint16_t destination = 0; destination < 4; destination++) {
			program.words[0] = 0x0100U | destination << 5 | source;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOVS_REGISTER);
			g_assert_cmpuint(instruction.register_code, ==, source);
			g_assert_cmpuint(instruction.destination_accumulator, ==, destination);
		}
	}

	for (uint16_t source = 0; source < 8; source++) {
		for (uint16_t step = 0; step < 4; step++) {
			for (uint16_t destination = 0; destination < 4; destination++) {
				program.words[0] = 0x0180U | destination << 5 | step << 3 | source;
				g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
				g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOVS_RN_STEP);
				g_assert_cmpuint(instruction.address_register, ==, source);
				g_assert_cmpuint(instruction.step, ==, step);
				g_assert_cmpuint(instruction.destination_accumulator, ==, destination);
			}
		}
	}

	for (uint16_t destination = 0; destination < 4; destination++) {
		for (uint16_t address = 0; address <= UINT8_MAX; address++) {
			program.words[0] = 0x6300U | destination << 11 | address;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOVS_DATA_IMM8);
			g_assert_cmpuint(instruction.memory_address, ==, address);
			g_assert_cmpuint(instruction.destination_accumulator, ==, destination);
		}
	}

	for (uint16_t destination = 0; destination < 2; destination++) {
		program.words[0] = 0x5F42U | destination;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_PUSH_IMMEDIATE);
	}

	for (uint16_t source = 0; source < 8; source++) {
		for (uint16_t destination = 0; destination < 4; destination++) {
			for (uint16_t shift = 0; shift < 32; shift++) {
				int16_t expected_shift = shift < 16 ? shift : shift - 32;

				program.words[0] = 0x4080U | source << 9 | destination << 5 | shift;
				g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
				g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOVSI_REGISTER);
				g_assert_cmpuint(instruction.register_code, ==, source);
				g_assert_cmpuint(instruction.destination_accumulator, ==, destination);
				g_assert_cmpint(instruction.shift, ==, expected_shift);
			}
		}
	}

	for (uint16_t source = 0; source < 32; source++) {
		for (uint16_t destination = 0; destination < 2; destination++) {
			program.words[0] = 0x9CC0U | destination << 8 | source;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOVR_REGISTER);
			g_assert_cmpuint(instruction.register_code, ==, source);
			g_assert_cmpuint(instruction.destination_accumulator, ==, destination + 2);
		}
	}

	for (uint16_t source = 0; source < 8; source++) {
		for (uint16_t step = 0; step < 4; step++) {
			for (uint16_t destination = 0; destination < 2; destination++) {
				program.words[0] = 0x9CE0U | destination << 8 | step << 3 | source;
				g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
				g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOVR_RN_STEP);
				g_assert_cmpuint(instruction.address_register, ==, source);
				g_assert_cmpuint(instruction.step, ==, step);
				g_assert_cmpuint(instruction.destination_accumulator, ==, destination + 2);
			}
		}
	}

	{
		static const uint8_t expected_registers[] = { 0, 4, 2, 5 };
		static const pmb887x_dsp_tcg_step_t expected_steps[] = {
			PMB887X_DSP_TCG_STEP_INCREASE,
			PMB887X_DSP_TCG_STEP_DECREASE,
			PMB887X_DSP_TCG_STEP_ZERO,
			PMB887X_DSP_TCG_STEP_PLUS_STEP,
		};

		for (uint16_t source = 0; source < 4; source++) {
			for (uint16_t step = 0; step < 4; step++) {
				for (uint16_t destination = 0; destination < 4; destination++) {
					program.words[0] = 0x8864U | destination << 8 | source << 3 | step;
					g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
					g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOVR_RN_HIGH);
					g_assert_cmpuint(instruction.address_register, ==, expected_registers[source]);
					g_assert_cmpuint(instruction.step, ==, expected_steps[step]);
					g_assert_cmpuint(instruction.destination_accumulator, ==, destination);
				}
			}
		}
	}

	for (uint16_t source = 0; source < 2; source++) {
		for (uint16_t destination = 0; destination < 2; destination++) {
			program.words[0] = 0x5DF4U | source << 1 | destination;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOVR_B_ACCUMULATOR);
			g_assert_cmpuint(instruction.source_accumulator, ==, source);
			g_assert_cmpuint(instruction.destination_accumulator, ==, destination + 2);
		}
	}

	for (uint16_t destination = 0; destination < 2; destination++) {
		program.words[0] = 0x8961U | destination << 3;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOVR_R6);
		g_assert_cmpuint(instruction.destination_accumulator, ==, destination + 2);
	}

	for (uint16_t operation = 0; operation < 8; operation++) {
		program.words[0] = 0xE100U | operation << 9 | 0x55U;
		program.words[1] = 0xA55A;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALB_DATA_IMM8);
		g_assert_cmpint(instruction.alb_operation, ==, operation);
		g_assert_cmphex(instruction.memory_address, ==, 0x55);
		g_assert_cmphex(instruction.expansion, ==, 0xA55A);
		g_assert_cmpuint(instruction.words, ==, 2);

		program.words[0] = 0x80E0U | operation << 9 | 3U << 3 | 5U;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALB_RN_STEP);
		g_assert_cmpint(instruction.alb_operation, ==, operation);
		g_assert_cmpuint(instruction.address_register, ==, 5);
		g_assert_cmpint(instruction.step, ==, PMB887X_DSP_TCG_STEP_PLUS_STEP);

		program.words[0] = 0x81E0U | operation << 9 | 26U;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_ALB_REGISTER);
		g_assert_cmpint(instruction.alb_operation, ==, operation);
		g_assert_cmpuint(instruction.register_code, ==, 26);
	}

	for (uint16_t address = 0; address < 0x100; address++) {
		for (uint16_t register_index = 0; register_index < 8; register_index++) {
			program.words[0] = 0x6000U | register_index << 10 | address;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_DATA_IMM8_REGISTER);
			g_assert_cmpuint(instruction.register_code, ==, register_index);
			g_assert_cmphex(instruction.memory_address, ==, address);

			program.words[0] = 0x2000U | register_index << 9 | address;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_REGISTER_DATA_IMM8);
			g_assert_cmpuint(instruction.register_code, ==, register_index);
			g_assert_cmphex(instruction.memory_address, ==, address);

			program.words[0] = 0x6200U | register_index << 10 | address;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_DATA_IMM8_REGISTER);
			g_assert_cmpuint(instruction.register_code, ==, accumulator_half_registers[register_index]);
			g_assert_cmphex(instruction.memory_address, ==, address);

			program.words[0] = 0x3000U | register_index << 9 | address;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_REGISTER_DATA_IMM8);
			g_assert_cmpuint(instruction.register_code, ==, accumulator_half_registers[register_index]);
			g_assert_cmphex(instruction.memory_address, ==, address);
		}

		for (uint16_t accumulator = 0; accumulator < 4; accumulator++) {
			program.words[0] = 0x6100U | accumulator << 11 | address;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_DATA_IMM8_ACCUMULATOR);
			g_assert_cmpuint(instruction.destination_accumulator, ==, accumulator);
			g_assert_cmphex(instruction.memory_address, ==, address);
		}

		for (uint16_t accumulator = 0; accumulator < 2; accumulator++) {
			program.words[0] = 0x6500U | accumulator << 12 | address;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_DATA_IMM8_ACCUMULATOR_HIGH_EU);
			g_assert_cmpuint(instruction.accumulator_index, ==, accumulator);
			g_assert_cmphex(instruction.memory_address, ==, address);
		}

		program.words[0] = 0x6D00U | address;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_DATA_IMM8_REGISTER);
		g_assert_cmpuint(instruction.register_code, ==, 31);
		g_assert_cmphex(instruction.memory_address, ==, address);

		program.words[0] = 0x7D00U | address;
		g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
		g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_REGISTER_DATA_IMM8);
		g_assert_cmpuint(instruction.register_code, ==, 31);
		g_assert_cmphex(instruction.memory_address, ==, address);
	}

	for (int16_t offset = -64; offset < 64; offset++) {
		for (uint16_t accumulator = 0; accumulator < 2; accumulator++) {
			uint16_t encoded_offset = (uint16_t) offset & 0x7FU;

			program.words[0] = 0xD880U | accumulator << 8 | encoded_offset;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_DATA_R7_OFFSET7_ACCUMULATOR);
			g_assert_cmpuint(instruction.accumulator_index, ==, accumulator);
			g_assert_cmpint(instruction.memory_offset, ==, offset);
			g_assert_cmpuint(instruction.words, ==, 1);

			program.words[0] = 0xDC80U | accumulator << 8 | encoded_offset;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET7);
			g_assert_cmpuint(instruction.accumulator_index, ==, accumulator);
			g_assert_cmpint(instruction.memory_offset, ==, offset);
			g_assert_cmpuint(instruction.words, ==, 1);
		}
	}

	for (uint32_t offset = 0; offset <= UINT16_MAX; offset++) {
		program.words[1] = offset;
		for (uint16_t accumulator = 0; accumulator < 2; accumulator++) {
			program.words[0] = 0xD498U | accumulator << 8;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_DATA_R7_OFFSET16_ACCUMULATOR);
			g_assert_cmpuint(instruction.accumulator_index, ==, accumulator);
			g_assert_cmpint(instruction.memory_offset, ==, (int16_t) offset);
			g_assert_cmpuint(instruction.words, ==, 2);

			program.words[0] = 0xD49CU | accumulator << 8;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET16);
			g_assert_cmpuint(instruction.accumulator_index, ==, accumulator);
			g_assert_cmpint(instruction.memory_offset, ==, (int16_t) offset);
			g_assert_cmpuint(instruction.words, ==, 2);
		}
	}

	for (uint32_t immediate = 0; immediate <= UINT16_MAX; immediate++) {
		program.words[1] = immediate;
		for (uint16_t accumulator = 0; accumulator < 2; accumulator++) {
			program.words[0] = 0x5E20U | accumulator << 8;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_IMM_B_ACCUMULATOR);
			g_assert_cmpuint(instruction.accumulator_index, ==, accumulator);
			g_assert_cmphex(instruction.expansion, ==, immediate);
			g_assert_cmpuint(instruction.words, ==, 2);
		}
	}

	for (uint16_t register_code = 0; register_code < 32; register_code++) {
		if (register_code == 24 || register_code == 25)
			continue;
		for (uint16_t destination = 0; destination < 32; destination++) {
			program.words[0] = 0x5800U | destination << 5 | register_code;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_REGISTER_REGISTER);
			g_assert_cmpuint(instruction.register_code, ==, register_code);
			g_assert_cmpuint(instruction.destination_register_code, ==, destination);
			g_assert_cmpuint(instruction.words, ==, 1);
		}
	}

	for (uint16_t register_code = 0; register_code < 32; register_code++) {
		for (uint16_t accumulator = 0; accumulator < 2; accumulator++) {
			program.words[0] = 0x5EC0U | accumulator << 5 | register_code;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_MOV_REGISTER_B_ACCUMULATOR);
			g_assert_cmpuint(instruction.register_code, ==, register_code);
			g_assert_cmpuint(instruction.accumulator_index, ==, accumulator);
			g_assert_cmpuint(instruction.words, ==, 1);
		}
	}

	for (uint16_t bit = 0; bit < 16; bit++) {
		for (uint16_t register_code = 0; register_code < 32; register_code++) {
			program.words[0] = 0x9000U | bit << 8 | register_code;
			g_assert_true(pmb887x_dsp_tcg_decode(&core, 0, &instruction));
			g_assert_cmpint(instruction.opcode, ==, PMB887X_DSP_TCG_OPCODE_TSTB_REGISTER);
			g_assert_cmpuint(instruction.register_code, ==, register_code);
			g_assert_cmpuint(instruction.bit_index, ==, bit);
			g_assert_cmpuint(instruction.words, ==, 1);
		}
	}
}

int main(int argc, char **argv) {
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/pmb887x/dsp/tcg/reset", test_reset);
	g_test_add_func("/pmb887x/dsp/tcg/memory-spaces", test_memory_spaces);
	g_test_add_func("/pmb887x/dsp/tcg/modulo-address", test_modulo_address);
	g_test_add_func("/pmb887x/dsp/tcg/decode", test_decode);
	return g_test_run();
}
