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

/* Number of abstract registers (= wasm locals).  Every register costs two
 * declared locals in every TB function (an i32 and an i64 one), and a
 * declared local is not free: the baseline tier zeroes all of them at
 * entry.  W64_LOCALPAD prices one at 8.09 us/Mi, so the 32-register file
 * was ~5.6 % of wall for registers TCG never allocated — tcgSpill reads 0
 * with 13 allocatable and 29 over 64 631 TBs with 10.  env and sp are
 * function parameters, so only the 13 allocatable registers get locals. */
#define TCG_TARGET_NB_REGS 15

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
} TCGReg;

#define TCG_AREG0          TCG_REG_R13   /* env — wasm param $env */
#define TCG_REG_CALL_STACK TCG_REG_R14   /* frame — wasm param $sp */

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

#endif /* TCG_TARGET_H */
