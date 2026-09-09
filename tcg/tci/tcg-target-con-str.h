/* SPDX-License-Identifier: MIT */
/*
 * Define TCI target-specific operand constraints.
 * Copyright (c) 2021 Linaro
 */

/*
 * Define constraint letters for register sets:
 * REGS(letter, register_mask)
 */
REGS('r', MAKE_64BIT_MASK(0, TCG_TARGET_NB_REGS))

/* Constraint letters for constants (bits defined in tcg-target.c.inc):
 * 'S' fits a signed 16-bit immediate (ALU _ri forms),
 * 'B' fits a signed 12-bit immediate (tci_setcond32_ri).
 */
CONST('S', TCG_CT_CONST_S16)
CONST('B', TCG_CT_CONST_S12)
