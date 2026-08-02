#ifndef HW_ARM_PMB887X_DSP_CORE_H
#define HW_ARM_PMB887X_DSP_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TEAK_BLOCK_REPEAT_LEVELS	4
#define TEAK_PROGRAM_ADDRESS_MASK	UINT16_MAX

typedef struct teak_tcg_core_t teak_tcg_core_t;
typedef struct teak_insn_t teak_insn_t;
typedef struct teak_memory_t teak_memory_t;
typedef struct teak_memory_space_t teak_memory_space_t;
typedef struct teak_state_t teak_state_t;

typedef enum teak_accumulator_test_t teak_accumulator_test_t;
typedef enum teak_alb_operation_t teak_alb_operation_t;
typedef enum teak_alu_operation_t teak_alu_operation_t;
typedef enum teak_condition_t teak_condition_t;
typedef enum teak_exit_t teak_exit_t;
typedef enum teak_exponent_source_t teak_exponent_source_t;
typedef enum teak_minmax_operation_t teak_minmax_operation_t;
typedef enum teak_moda_operation_t teak_moda_operation_t;
typedef enum teak_multiply_operation_t teak_multiply_operation_t;
typedef enum teak_opcode_t teak_opcode_t;
typedef enum teak_special_register_t teak_special_register_t;
typedef enum teak_step_t teak_step_t;
typedef enum teak_swap_operation_t teak_swap_operation_t;
typedef enum teak_translation_error_t teak_translation_error_t;

typedef uint16_t teak_read_fn(void *opaque, uint32_t address);
typedef void teak_write_fn(void *opaque, uint32_t address, uint16_t value);
typedef bool teak_should_invalidate_fn(void *opaque, uint32_t address);
typedef void teak_advance_cycles_fn(void *opaque, size_t cycles);

enum teak_exit_t {
	TEAK_EXIT_NONE,
	TEAK_EXIT_BRANCH,
	TEAK_EXIT_INTERRUPT,
	TEAK_EXIT_HALT,
};

enum teak_translation_error_t {
	TEAK_TRANSLATION_ERROR_NONE,
	TEAK_TRANSLATION_ERROR_DECODE,
	TEAK_TRANSLATION_ERROR_UNSUPPORTED,
	TEAK_TRANSLATION_ERROR_BLOCK_REPEAT_BODY,
	TEAK_TRANSLATION_ERROR_BREAK,
	TEAK_TRANSLATION_ERROR_REPEAT,
	TEAK_TRANSLATION_ERROR_BLOCK_REPEAT_SETUP,
	TEAK_TRANSLATION_ERROR_DELAY_SLOT,
	TEAK_TRANSLATION_ERROR_COMPILE,
	TEAK_TRANSLATION_ERROR_EXECUTION,
};

enum teak_opcode_t {
	TEAK_OP_UNDEFINED,
	TEAK_OP_NOP,
	TEAK_OP_EINT,
	TEAK_OP_DINT,
	TEAK_OP_TRAP,
	TEAK_OP_LOAD_PAGE,
	TEAK_OP_LOAD_STEPI,
	TEAK_OP_LOAD_STEPJ,
	TEAK_OP_LOAD_MODI,
	TEAK_OP_LOAD_MODJ,
	TEAK_OP_LOAD_PRODUCT_SHIFT,
	TEAK_OP_SHIFT_CONDITIONAL,
	TEAK_OP_SHIFT_IMMEDIATE,
	TEAK_OP_MULTIPLY_IMMEDIATE,
	TEAK_OP_MULTIPLY_DATA_IMM8,
	TEAK_OP_MULTIPLY_DUAL_RN,
	TEAK_OP_MULTIPLY_RN_IMMEDIATE,
	TEAK_OP_MULTIPLY_R6,
	TEAK_OP_MULTIPLY_REGISTER,
	TEAK_OP_MULTIPLY_RN_STEP,
	TEAK_OP_MODIFY_RN,
	TEAK_OP_MOV_IMM_REGISTER,
	TEAK_OP_MOV_IMM_B_ACCUMULATOR,
	TEAK_OP_MOV_SHORT_REGISTER,
	TEAK_OP_MOV_REGISTER_REGISTER,
	TEAK_OP_MOV_REGISTER_B_ACCUMULATOR,
	TEAK_OP_MOV_ACCUMULATOR_ACCUMULATOR,
	TEAK_OP_MOV_ACCUMULATOR_LOW_SPECIAL,
	TEAK_OP_MOV_SPECIAL_ACCUMULATOR,
	TEAK_OP_MOV_MIXP_REGISTER,
	TEAK_OP_MOV_REGISTER_MIXP,
	TEAK_OP_MOV_REGISTER_ICR,
	TEAK_OP_MOV_DATA_IMM8_REGISTER,
	TEAK_OP_MOV_DATA_IMM8_ACCUMULATOR,
	TEAK_OP_MOV_DATA_IMM8_ACCUMULATOR_HIGH_EU,
	TEAK_OP_MOV_REGISTER_DATA_IMM8,
	TEAK_OP_MOV_DATA_R7_OFFSET7_ACCUMULATOR,
	TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET7,
	TEAK_OP_MOV_DATA_R7_OFFSET16_ACCUMULATOR,
	TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_R7_OFFSET16,
	TEAK_OP_MOV_DATA_RN_STEP_REGISTER,
	TEAK_OP_MOV_REGISTER_DATA_RN_STEP,
	TEAK_OP_MOV_DATA_RN_STEP_B_ACCUMULATOR,
	TEAK_OP_MOV_STACK_REGISTER,
	TEAK_OP_MOV_IMM_ICR,
	TEAK_OP_MOV_IMM8_ACCUMULATOR_LOW,
	TEAK_OP_MOV_DATA_IMM16_ACCUMULATOR,
	TEAK_OP_MOV_ACCUMULATOR_LOW_DATA_IMM16,
	TEAK_OP_MOVP_ACCUMULATOR_LOW_REGISTER,
	TEAK_OP_MOVP_RN_RN,
	TEAK_OP_MOVD_RN_RN,
	TEAK_OP_MOVS_REGISTER,
	TEAK_OP_MOVS_RN_STEP,
	TEAK_OP_MOVS_DATA_IMM8,
	TEAK_OP_MOVS_R6,
	TEAK_OP_MOVSI_REGISTER,
	TEAK_OP_MOVR_REGISTER,
	TEAK_OP_MOVR_RN_STEP,
	TEAK_OP_MOVR_RN_HIGH,
	TEAK_OP_MOVR_B_ACCUMULATOR,
	TEAK_OP_MOVR_R6,
	TEAK_OP_ALB_DATA_IMM8,
	TEAK_OP_ALB_RN_STEP,
	TEAK_OP_ALB_REGISTER,
	TEAK_OP_ALU_IMMEDIATE_ACCUMULATOR,
	TEAK_OP_ALU_DATA_IMM8_ACCUMULATOR,
	TEAK_OP_ALU_DATA_IMM16_ACCUMULATOR,
	TEAK_OP_ALU_R7_OFFSET7_ACCUMULATOR,
	TEAK_OP_ALU_R7_OFFSET16_ACCUMULATOR,
	TEAK_OP_ALU_RN_STEP_ACCUMULATOR,
	TEAK_OP_ALU_REGISTER_ACCUMULATOR,
	TEAK_OP_TEST_ACCUMULATOR_DATA_IMM8,
	TEAK_OP_MODA4_ACCUMULATOR,
	TEAK_OP_MODB3_ACCUMULATOR,
	TEAK_OP_LIMIT_ACCUMULATOR,
	TEAK_OP_EXPONENT,
	TEAK_OP_NORMALIZE,
	TEAK_OP_SWAP_ACCUMULATORS,
	TEAK_OP_BANK_EXCHANGE,
	TEAK_OP_DIVISION_STEP,
	TEAK_OP_MINIMUM_MAXIMUM,
	TEAK_OP_BLOCK_REPEAT_IMMEDIATE,
	TEAK_OP_BLOCK_REPEAT_REGISTER,
	TEAK_OP_BREAK,
	TEAK_OP_BRANCH_ABSOLUTE,
	TEAK_OP_BRANCH_RELATIVE,
	TEAK_OP_CALL_ABSOLUTE,
	TEAK_OP_CALL_ACCUMULATOR,
	TEAK_OP_CALL_RELATIVE,
	TEAK_OP_PUSH_IMMEDIATE,
	TEAK_OP_PUSH_REGISTER,
	TEAK_OP_POP_REGISTER,
	TEAK_OP_REPEAT_IMMEDIATE,
	TEAK_OP_REPEAT_REGISTER,
	TEAK_OP_RETURN,
	TEAK_OP_RETURN_INTERRUPT,
	TEAK_OP_RETURN_STACK,
	TEAK_OP_DELAYED_RETURN,
	TEAK_OP_DELAYED_RETURN_INTERRUPT,
	TEAK_OP_CONTEXT_STORE,
	TEAK_OP_CONTEXT_RESTORE,
	TEAK_OP_TSTB_IMM8,
	TEAK_OP_TSTB_RN_STEP,
	TEAK_OP_TSTB_REGISTER,
};

enum teak_accumulator_test_t {
	TEAK_ACCUMULATOR_TST0,
	TEAK_ACCUMULATOR_TST1,
};

enum teak_alu_operation_t {
	TEAK_ALU_OR = 0x0,
	TEAK_ALU_AND = 0x1,
	TEAK_ALU_XOR = 0x2,
	TEAK_ALU_ADD = 0x3,
	TEAK_ALU_TST0 = 0x4,
	TEAK_ALU_TST1 = 0x5,
	TEAK_ALU_CMP = 0x6,
	TEAK_ALU_SUB = 0x7,
	TEAK_ALU_MSU = 0x8,
	TEAK_ALU_ADDH = 0x9,
	TEAK_ALU_ADDL = 0xA,
	TEAK_ALU_SUBH = 0xB,
	TEAK_ALU_SUBL = 0xC,
	TEAK_ALU_SQR = 0xD,
	TEAK_ALU_SQRA = 0xE,
	TEAK_ALU_CMPU = 0xF,
};

enum teak_condition_t {
	TEAK_COND_TRUE = 0x0,
	TEAK_COND_EQ = 0x1,
	TEAK_COND_NEQ = 0x2,
	TEAK_COND_GT = 0x3,
	TEAK_COND_GE = 0x4,
	TEAK_COND_LT = 0x5,
	TEAK_COND_LE = 0x6,
	TEAK_COND_NN = 0x7,
	TEAK_COND_C = 0x8,
	TEAK_COND_V = 0x9,
	TEAK_COND_E = 0xA,
	TEAK_COND_L = 0xB,
	TEAK_COND_NR = 0xC,
	TEAK_COND_NIU0 = 0xD,
	TEAK_COND_IU0 = 0xE,
	TEAK_COND_IU1 = 0xF,
	TEAK_COND_COUNT = 0x10,
};

enum teak_moda_operation_t {
	TEAK_MODA_SHR = 0x0,
	TEAK_MODA_SHR4 = 0x1,
	TEAK_MODA_SHL = 0x2,
	TEAK_MODA_SHL4 = 0x3,
	TEAK_MODA_ROR = 0x4,
	TEAK_MODA_ROL = 0x5,
	TEAK_MODA_CLR = 0x6,
	TEAK_MODA_NOT = 0x8,
	TEAK_MODA_NEG = 0x9,
	TEAK_MODA_RND = 0xA,
	TEAK_MODA_PACR = 0xB,
	TEAK_MODA_CLRR = 0xC,
	TEAK_MODA_INC = 0xD,
	TEAK_MODA_DEC = 0xE,
	TEAK_MODA_COPY = 0xF,
};

enum teak_alb_operation_t {
	TEAK_ALB_SET,
	TEAK_ALB_RST,
	TEAK_ALB_CHNG,
	TEAK_ALB_ADDV,
	TEAK_ALB_TST0,
	TEAK_ALB_TST1,
	TEAK_ALB_CMPV,
	TEAK_ALB_SUBV,
};

enum teak_exponent_source_t {
	TEAK_EXPONENT_REGISTER,
	TEAK_EXPONENT_B_ACCUMULATOR,
	TEAK_EXPONENT_RN_STEP,
	TEAK_EXPONENT_R6,
};

enum teak_minmax_operation_t {
	TEAK_MINMAX_MAX_GE,
	TEAK_MINMAX_MAX_GT,
	TEAK_MINMAX_MIN_LE,
	TEAK_MINMAX_MIN_LT,
};

enum teak_multiply_operation_t {
	TEAK_MULTIPLY_MPY,
	TEAK_MULTIPLY_MPYSU,
	TEAK_MULTIPLY_MAC,
	TEAK_MULTIPLY_MACUS,
	TEAK_MULTIPLY_MAA,
	TEAK_MULTIPLY_MACUU,
	TEAK_MULTIPLY_MACSU,
	TEAK_MULTIPLY_MAASU,
	TEAK_MULTIPLY_MSU,
};

enum teak_step_t {
	TEAK_STEP_ZERO,
	TEAK_STEP_INCREASE,
	TEAK_STEP_DECREASE,
	TEAK_STEP_PLUS_STEP,
};

enum teak_special_register_t {
	TEAK_SPECIAL_REPC,
	TEAK_SPECIAL_DVM,
	TEAK_SPECIAL_ICR,
	TEAK_SPECIAL_X0,
	TEAK_SPECIAL_X1,
	TEAK_SPECIAL_Y1,
	TEAK_SPECIAL_MIXP,
};

enum teak_swap_operation_t {
	TEAK_SWAP_A0_B0,
	TEAK_SWAP_A0_B1,
	TEAK_SWAP_A1_B0,
	TEAK_SWAP_A1_B1,
	TEAK_SWAP_A0_B0_A1_B1,
	TEAK_SWAP_A0_B1_A1_B0,
	TEAK_SWAP_A0_TO_B0_TO_A1,
	TEAK_SWAP_A0_TO_B1_TO_A1,
	TEAK_SWAP_A1_TO_B0_TO_A0,
	TEAK_SWAP_A1_TO_B1_TO_A0,
	TEAK_SWAP_B0_TO_A0_TO_B1,
	TEAK_SWAP_B0_TO_A1_TO_B1,
	TEAK_SWAP_B1_TO_A0_TO_B0,
	TEAK_SWAP_B1_TO_A1_TO_B0,
	TEAK_SWAP_OPERATION_COUNT,
};

struct teak_memory_space_t {
	void *opaque;
	teak_read_fn *read;
	teak_write_fn *write;
	teak_should_invalidate_fn *should_invalidate;
};

struct teak_memory_t {
	teak_memory_space_t program;
	teak_memory_space_t data;
	teak_memory_space_t external;
	void *cycle_opaque;
	teak_advance_cycles_fn *advance_cycles;
	uint32_t cycle_sensitive_base;
	uint32_t cycle_sensitive_size;
	uint16_t y_space_base;
};

struct teak_insn_t {
	teak_opcode_t opcode;
	uint32_t address;
	uint16_t word;
	uint16_t expansion;
	uint16_t immediate;
	int16_t memory_offset;
	uint32_t branch_target;
	uint64_t alu_operand;
	uint8_t words;
	uint8_t register_code;
	uint8_t destination_register_code;
	uint8_t address_register;
	uint8_t y_address_register;
	uint8_t accumulator_index;
	uint8_t source_accumulator;
	uint8_t destination_accumulator;
	uint8_t memory_address;
	uint8_t bit_index;
	teak_accumulator_test_t accumulator_test;
	teak_alu_operation_t alu_operation;
	teak_moda_operation_t moda_operation;
	teak_exponent_source_t exponent_source;
	teak_minmax_operation_t minmax_operation;
	bool write_accumulator;
	bool memory_source;
	bool minmax_b_accumulator;
	teak_alb_operation_t alb_operation;
	teak_multiply_operation_t multiply_operation;
	teak_condition_t condition;
	teak_step_t step;
	teak_step_t y_step;
	teak_swap_operation_t swap_operation;
	teak_special_register_t special_register;
	int8_t shift;
	bool disable_modulo;
	bool context_switch;
};

struct teak_state_t {
	uint64_t a[2];
	uint64_t b[2];
	uint32_t p[2];
	uint32_t pc;
	uint32_t trace_pc;
	uint32_t pending_interrupts;
	uint32_t interrupt_lines;
	uint32_t exit_request;
	uint32_t block_repeat_start[TEAK_BLOCK_REPEAT_LEVELS];
	uint32_t block_repeat_end[TEAK_BLOCK_REPEAT_LEVELS];
	teak_exit_t exit_reason;
	uint16_t r[8];
	uint16_t x[2];
	uint16_t y[2];
	uint16_t sp;
	uint16_t repc;
	uint16_t shift_value;
	uint16_t mixp;
	uint16_t dvm;
	uint16_t block_repeat_lc[TEAK_BLOCK_REPEAT_LEVELS];
	uint16_t modi;
	uint16_t modj;
	uint16_t r0b;
	uint16_t r1b;
	uint16_t r4b;
	uint16_t modib;
	uint16_t shadow_st0;
	uint16_t shadow_st1;
	uint16_t shadow_st2;
	uint16_t extension[4];
	uint8_t fz;
	uint8_t fm;
	uint8_t fn;
	uint8_t fv;
	uint8_t fe;
	uint8_t fc0;
	uint8_t fc1;
	uint8_t flm;
	uint8_t fvl;
	uint8_t fr;
	uint8_t ie;
	uint8_t sat;
	uint8_t sata;
	uint8_t s;
	uint8_t page;
	uint8_t alternate_page;
	uint8_t product_shift;
	uint8_t product_extension[2];
	uint8_t interrupt_mask;
	uint8_t interrupt_context;
	uint8_t nonmaskable_context;
	uint8_t stepi;
	uint8_t stepj;
	uint8_t stepib;
	uint8_t modulo_enable;
	uint8_t cpc;
	uint8_t repeat_active;
	uint8_t lp;
	uint8_t bcn;
	uint8_t iu[2];
	uint8_t ou[2];
	uint8_t idle;
	uint8_t halted;
	uint8_t trap_active;
	uint8_t nmi_active;
};

struct teak_tcg_core_t {
	teak_state_t state;
	teak_memory_t memory;
	uint64_t cache_id;
	uint32_t last_block_cycles;
	uint32_t last_block_count;
	uint32_t translation_error_address;
	teak_translation_error_t translation_error;
	uint32_t synchronized_cycles;
	uint32_t synchronization_offset;
	uint32_t synchronization_access;
	uint32_t pending_cycles;
	bool synchronization_valid;
};

void teak_tcg_init(teak_tcg_core_t *core, const teak_memory_t *memory);
void teak_tcg_reset(teak_tcg_core_t *core, uint32_t pc);
uint16_t teak_program_read(teak_tcg_core_t *core, uint32_t address);
void teak_program_write(teak_tcg_core_t *core, uint32_t address, uint16_t value);
uint16_t teak_data_read(teak_tcg_core_t *core, uint32_t address);
void teak_data_write(teak_tcg_core_t *core, uint32_t address, uint16_t value);
uint16_t teak_modulo_address(const teak_state_t *state, uint8_t register_index, uint16_t address, int16_t step);
bool teak_decode(teak_tcg_core_t *core, uint32_t address, teak_insn_t *instruction);

#endif
