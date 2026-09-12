/*
 * Tiny Code Generator for QEMU — wasm64 target.
 *
 * The "machine code" is WebAssembly: each TB becomes one standalone
 * module with a single exported function
 *
 *   (func $tb (param $env i64) (param $sp i64) (param $tp i64)
 *             (result i32) ...)
 *
 * compiled through the browser WebAssembly API at first execution (see
 * tcg/wasm64/wasm64.c).  TCG registers are wasm locals: every TCG reg
 * has both an i32-typed and an i64-typed local; which one holds the
 * value is tracked per-reg at translation time ("rep").  Values in the
 * i32 local are the plain 32-bit pattern; converting to i64 is always
 * zero-extension, converting back is a wrap — the same invariants TCI
 * maintains in its 64-bit register file.
 *
 * $env is the CPUArchState pointer (TCG_AREG0), $sp the per-vCPU call
 * frame (helpers receive their arguments through it, TCI-style — the
 * backend declares no argument registers), $tp the address of the
 * thread-local w64_tb_ptr, which generated code updates before every
 * helper call so that GETPC()-using helpers unwind correctly.
 *
 * Intra-TB branches use a label-region scheme compatible with wasm's
 * structured control flow: the whole body is wrapped in one `loop`; a
 * branch to TCG label L writes L's region index into the $bp local and
 * breaks to the loop head, where a chain of `if (bp <= k)` guards —
 * one per label, opened at set_label time — admits control flow at the
 * right region.  Fall-through between regions is free, not-taken
 * conditional branches cost nothing, and taken branches walk the
 * region chain (a phase-2 br_table will remove even that).
 *
 * Phase 1 scope (doc/wasm-tcg-backend-plan.md §5): single-TB modules,
 * no chaining — every goto_tb exits to the C dispatcher, goto_ptr
 * hands the next TB back to the dispatcher through a frame slot.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TCG_TARGET_H
#define TCG_TARGET_H

#define TCG_TARGET_INSN_UNIT_SIZE 1
#define MAX_CODE_GEN_BUFFER_SIZE  ((size_t)-1)
#define MIN_TLB_MASK_TABLE_OFS    INT_MIN

/* Number of abstract registers (= wasm locals). */
#define TCG_TARGET_NB_REGS 32

typedef enum {
    TCG_REG_R0 = 0,
    TCG_REG_R1,
    TCG_REG_R2,
    TCG_REG_R3,
    TCG_REG_R4,
    TCG_REG_R5,
    TCG_REG_R6,
    TCG_REG_R7,
    TCG_REG_R8,
    TCG_REG_R9,
    TCG_REG_R10,
    TCG_REG_R11,
    TCG_REG_R12,
    TCG_REG_R13,
    TCG_REG_R14,
    TCG_REG_R15,
    TCG_REG_R16,
    TCG_REG_R17,
    TCG_REG_R18,
    TCG_REG_R19,
    TCG_REG_R20,
    TCG_REG_R21,
    TCG_REG_R22,
    TCG_REG_R23,
    TCG_REG_R24,
    TCG_REG_R25,
    TCG_REG_R26,
    TCG_REG_R27,
    TCG_REG_R28,
    TCG_REG_R29,
    TCG_REG_R30,
    TCG_REG_R31,
} TCGReg;

#define TCG_REG_TMP        TCG_REG_R28   /* generic scratch (reserved) */
#define TCG_AREG0          TCG_REG_R29   /* env — wasm local $env       */
#define TCG_REG_CALL_STACK TCG_REG_R31   /* frame — wasm local $sp      */

/* Function call generation: no argument registers; all arguments are
 * stored into the call frame at $sp (TCG_STATIC_CALL_ARGS_SIZE layout),
 * exactly like TCI.  The call emission reloads them as wasm parameters
 * of the imported helper. */
#define TCG_TARGET_CALL_STACK_OFFSET    0
#define TCG_TARGET_STACK_ALIGN          8
#define TCG_TARGET_CALL_ARG_I32         TCG_CALL_ARG_NORMAL
#define TCG_TARGET_CALL_ARG_I64         TCG_CALL_ARG_NORMAL
#define TCG_TARGET_CALL_ARG_I128        TCG_CALL_ARG_NORMAL
#define TCG_TARGET_CALL_RET_I128        TCG_CALL_RET_NORMAL

#define HAVE_TCG_QEMU_TB_EXEC

/* Optional instructions.  Conservative phase-1 set: ops without a
 * direct wasm lowering are left to the middle-end expansions. */

#define TCG_TARGET_HAS_addco_i32        0
#define TCG_TARGET_HAS_addc1o_i32       0
#define TCG_TARGET_HAS_addci_i32        0
#define TCG_TARGET_HAS_addcio_i32       0
#define TCG_TARGET_HAS_subbo_i32        0
#define TCG_TARGET_HAS_subb1o_i32       0
#define TCG_TARGET_HAS_subbi_i32        0
#define TCG_TARGET_HAS_subbio_i32       0

#define TCG_TARGET_HAS_bswap16_i32      1
#define TCG_TARGET_HAS_bswap32_i32      1
#define TCG_TARGET_HAS_div_i32          1
#define TCG_TARGET_HAS_rem_i32          1
#define TCG_TARGET_HAS_ext8s_i32        1
#define TCG_TARGET_HAS_ext16s_i32       1
#define TCG_TARGET_HAS_ext8u_i32        1
#define TCG_TARGET_HAS_ext16u_i32       1
#define TCG_TARGET_HAS_andc_i32         1
#define TCG_TARGET_HAS_deposit_i32      1
#define TCG_TARGET_HAS_extract_i32      1
#define TCG_TARGET_HAS_sextract_i32     1
#define TCG_TARGET_HAS_extract2_i32     0
#define TCG_TARGET_HAS_eqv_i32          1
#define TCG_TARGET_HAS_nand_i32         1
#define TCG_TARGET_HAS_nor_i32          1
#define TCG_TARGET_HAS_clz_i32          1
#define TCG_TARGET_HAS_ctz_i32          1
#define TCG_TARGET_HAS_ctpop_i32        1
#define TCG_TARGET_HAS_neg_i32          1
#define TCG_TARGET_HAS_not_i32          1
#define TCG_TARGET_HAS_orc_i32          1
#define TCG_TARGET_HAS_rot_i32          1
#define TCG_TARGET_HAS_movcond_i32      1
#define TCG_TARGET_HAS_negsetcond_i32   0
#define TCG_TARGET_HAS_muls2_i32        1
#define TCG_TARGET_HAS_mulu2_i32        1
#define TCG_TARGET_HAS_mulsh_i32        1
#define TCG_TARGET_HAS_muluh_i32        1
#define TCG_TARGET_HAS_qemu_st8_i32     0

#define TCG_TARGET_HAS_extr_i64_i32     0
#define TCG_TARGET_HAS_extrl_i64_i32    1
#define TCG_TARGET_HAS_extrh_i64_i32    1

#define TCG_TARGET_HAS_bswap16_i64      1
#define TCG_TARGET_HAS_bswap32_i64      1
#define TCG_TARGET_HAS_bswap64_i64      1
#define TCG_TARGET_HAS_deposit_i64      1
#define TCG_TARGET_HAS_extract_i64      1
#define TCG_TARGET_HAS_sextract_i64     1
#define TCG_TARGET_HAS_extract2_i64     0
#define TCG_TARGET_HAS_div_i64          1
#define TCG_TARGET_HAS_rem_i64          1
#define TCG_TARGET_HAS_ext8s_i64        1
#define TCG_TARGET_HAS_ext16s_i64       1
#define TCG_TARGET_HAS_ext32s_i64       1
#define TCG_TARGET_HAS_ext8u_i64        1
#define TCG_TARGET_HAS_ext16u_i64       1
#define TCG_TARGET_HAS_ext32u_i64       1
#define TCG_TARGET_HAS_andc_i64         1
#define TCG_TARGET_HAS_eqv_i64          1
#define TCG_TARGET_HAS_nand_i64         1
#define TCG_TARGET_HAS_nor_i64          1
#define TCG_TARGET_HAS_clz_i64          1
#define TCG_TARGET_HAS_ctz_i64          1
#define TCG_TARGET_HAS_ctpop_i64        1
#define TCG_TARGET_HAS_neg_i64          1
#define TCG_TARGET_HAS_not_i64          1
#define TCG_TARGET_HAS_orc_i64          1
#define TCG_TARGET_HAS_rot_i64          1
#define TCG_TARGET_HAS_movcond_i64      1
#define TCG_TARGET_HAS_negsetcond_i64   0
#define TCG_TARGET_HAS_muls2_i64        0
#define TCG_TARGET_HAS_mulu2_i64        0
#define TCG_TARGET_HAS_mulsh_i64        0
#define TCG_TARGET_HAS_muluh_i64        0

#define TCG_TARGET_HAS_qemu_ldst_i128 0

#endif /* TCG_TARGET_H */
