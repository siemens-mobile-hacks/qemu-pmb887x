/*
 * WASM PORT FAST PATH (removable): state at the bottom of linear memory.
 *
 * A TB reaches the state of the CPU it was translated for at constant
 * addresses (w64_tb_env in tcg/wasm64).  V8 on arm64 builds a constant
 * address of 64 KiB or more with two instructions, movz and movk, before
 * the access that uses it: a fifth of all the instructions of a TB.  An
 * address below 16380 goes into a 4-byte load or store itself (32760 for
 * 8 bytes, 4095 for one byte), so state down here costs a TB nothing to
 * reach.  x86 folds any constant address into the access: nothing changes
 * there, and only an arm64 host can measure this (doc/performance.md,
 * "On a phone").
 *
 * The link leaves the space from 1024 up to the global base to this
 * (-sGLOBAL_BASE, scripts/build-qemu-wasm64.sh); emscripten uses nothing
 * below 1024 either.  The places are fixed, because the DSP's state has
 * to come first and the CPU object is created first.
 *
 * Remove: this file and its users (grep W64_LOW and w64_low_), the
 * link flag, and the pointer CPUTLB.d became (include/hw/core/cpu.h).
 */
#ifndef QEMU_W64_LOW_H
#define QEMU_W64_LOW_H

#define W64_LOW_INSNS   0x0100      /* wasm_guest_insns, 8 bytes */
#define W64_LOW_EPOCH   0x0108      /* w64_evict_epoch, 4 bytes */
#define W64_LOW_DSP     0x0400      /* the DSP runtime, its core's state first */
#define W64_LOW_CPU     0x2800      /* the board's CPU object */

/*
 * @size zeroed bytes at @at, one of the places above.  NULL when the place
 * is taken or the link reserved less: the caller allocates as usual then.
 */
void *w64_low_place(uintptr_t at, size_t size);
/* Give a place back; false when @p is not one, and is the caller's to free. */
bool w64_low_release(void *p);

#endif
