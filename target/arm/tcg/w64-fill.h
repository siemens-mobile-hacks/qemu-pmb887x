/*
 * WASM PORT FAST PATH (removable): Thumb fill loops, see w64-fill.c.
 *
 * HELPER(w64_fill) descriptor: Rv | Rp << 4 | Rd << 8 | Rc << 12 | Rn << 16
 * | log2(store width) << 20, plus which budgets the back-edge charges.
 * The charge argument is, per pass, insns | taken << 8 | cycles << 16:
 * the taken blt's own cycles, added before the back-edge's budget check,
 * and the cycles the back-edge prepays after it.
 *
 * HELPER(w64_blit) descriptor: the movs immediate | the step's sp offset
 * in words << 8, plus the same budget bits; the charge argument is the
 * same.  Its refund argument is the cycles the three joins hand back: the
 * #imm path's | the literal path's << 8 | the transparent path's << 16.
 */
#ifndef TARGET_ARM_W64_FILL_H
#define TARGET_ARM_W64_FILL_H

#define W64_FILL_ICOUNT (1u << 24)
#define W64_FILL_CYCLES (1u << 25)

/*
 * TranslationBlock w64_rect kinds.  ROW is the fill loop's TB, whose exit
 * is the row tail's blt to the head; its regs are Rpitch | Rrow << 4 |
 * Rlim << 8 | the tail's sp offset in words << 16.  HEAD is the head's TB,
 * whose exit is the test's blt into the loop; its regs are Rc | Rn << 4.
 */
#define W64_RECT_ROW  1
#define W64_RECT_HEAD 2

#endif
