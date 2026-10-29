/*
 * ARM9E-S instruction timing
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TARGET_ARM_TCG_ARM9E_CYCLES_H
#define TARGET_ARM_TCG_ARM9E_CYCLES_H

/* Cycles an instruction takes when it executes; a skipped one takes one. */
uint32_t arm9e_a32_cycles(uint32_t insn);
uint32_t arm9e_thumb_cycles(uint32_t insn);

/*
 * Interlock stall cycles before an instruction; @late holds the registers
 * earlier instructions still write: bits 0-15 for the next instruction,
 * bits 16-31 for the next two.
 */
uint32_t arm9e_a32_stall(uint32_t *late, uint32_t insn);
uint32_t arm9e_thumb_stall(uint32_t *late, uint32_t insn);

#endif
