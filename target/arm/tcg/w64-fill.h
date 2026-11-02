/*
 * WASM PORT FAST PATH (removable): Thumb fill loops, see w64-fill.c.
 *
 * HELPER(w64_fill) descriptor: Rv | Rp << 4 | Rd << 8 | Rc << 12 | Rn << 16
 * | log2(store width) << 20, plus which budgets the back-edge charges.
 * The charge argument is, per pass, insns | taken << 8 | cycles << 16:
 * the taken blt's own cycles, added before the back-edge's budget check,
 * and the cycles the back-edge prepays after it.
 */
#ifndef TARGET_ARM_W64_FILL_H
#define TARGET_ARM_W64_FILL_H

#define W64_FILL_ICOUNT (1u << 24)
#define W64_FILL_CYCLES (1u << 25)

#endif
