/* SPDX-License-Identifier: MIT */
/*
 * Define wasm64 target-specific operand constraints.
 */

/*
 * Define constraint letters for register sets:
 * REGS(letter, register_mask)
 */
REGS('r', MAKE_64BIT_MASK(0, TCG_TARGET_NB_REGS))

/* Every constant is materializable (i32/i64.const take full range). */
CONST('C', TCG_CT_CONST)
