/*
 * WASM PORT FAST PATH (removable): Thumb fill loops.
 *
 * The firmware's J2ME graphics fill a rectangle row by row with
 *
 *     loop: ldr   Rv, [Rp]        @ the colour, re-read every pass
 *           strh  Rv, [Rd]        @ or strb / str
 *           adds  Rd, #2          @ #1 / #4, the store width
 *           adds  Rc, #1
 *           cmp   Rc, Rn
 *           blt   loop
 *
 * (Siemens S75 and EL71 share the code; it is the hottest guest loop while
 * a game redraws, 5 % of the vCPU in Tower Bloxx).  The translator turns
 * the loop into a superblock that branches back to its own start
 * (w64_back_edge), but every pass still pays two TLB probes, the register
 * and flag syncs at the back-edge and the budget checks, each memory access
 * bounds-checked on Android.
 *
 * w64_fill_match() in translate.c recognises the loop when it is a whole
 * TB-start superblock and calls HELPER(w64_fill) at the back-edge, after
 * the generated code has charged the next pass.  The helper runs further
 * passes in C when they are plain RAM stores on one page and change nothing
 * but the destination, charging every pass exactly as the back-edge would
 * (icount, the cycle budget, the guest-instruction and lockstep counters).
 * It always leaves at least the already-charged pass to the generated code,
 * so the loop exit, its flags and any fault happen there as before.
 *
 * Removing this file, its meson line, the helper-defs.h entry and the
 * w64_fill hooks in translate.c / translate.h restores the plain loop.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "accel/tcg/probe.h"
#include "internals.h"
#include "helper.h"
#include "w64-fill.h"

#ifdef CONFIG_TCG_WASM64

#define W64_FILL_MAX_PASSES 4096

/*
 * The generated code's TLB hit test (an entry without flags for the page),
 * before the general probe that the miss and the flagged cases take.
 */
static void *w64_fill_host(CPUARMState *env, uint32_t addr,
                           MMUAccessType type, int mmu_idx)
{
    CPUTLBDescFast *fast = cpu_tlb_fast(env_cpu(env), mmu_idx);
    uintptr_t index = (addr >> TARGET_PAGE_BITS) &
                      (fast->mask >> CPU_TLB_ENTRY_BITS);
    CPUTLBEntry *entry = &fast->table[index];
    uintptr_t cmp = type == MMU_DATA_STORE ? entry->addr_write
                                           : entry->addr_read;

    if (cmp == (addr & TARGET_PAGE_MASK)) {
        return (void *)((uintptr_t)addr + entry->addend);
    }
    return tlb_vaddr_to_host(env, addr, type, mmu_idx);
}

void HELPER(w64_fill)(CPUARMState *env, uint32_t desc, uint32_t charge)
{
    CPUState *cs = env_cpu(env);
    unsigned rv = desc & 7, rp = (desc >> 4) & 7, rd = (desc >> 8) & 7;
    unsigned rc = (desc >> 12) & 7, rn = (desc >> 16) & 7;
    unsigned width = 1u << ((desc >> 20) & 3);
    unsigned insns = charge & 0xff, taken = (charge >> 8) & 0xff;
    unsigned cycles = charge >> 16;
    uint32_t dst = env->regs[rd], src = env->regs[rp];
    int32_t budget = (int32_t)qatomic_read(&cs->neg.icount_decr.u32);
    int mmu_idx = arm_env_mmu_index(env);
    uint8_t *hd;
    const uint8_t *hs;
    uint64_t *acct;
    int64_t *ls;
    uint32_t v, c, res;
    int64_t k;

    /* passes whose blt is still taken: Rc + 1 .. Rn - 1 */
    k = (int64_t)(int32_t)env->regs[rn] - (int32_t)env->regs[rc] - 1;
    if (k <= 0 || budget < 0 || (dst & (width - 1)) || (src & 3) ||
        arm_cpu_data_is_big_endian(env)) {
        return;
    }
    k = MIN(k, W64_FILL_MAX_PASSES);
    k = MIN(k, (TARGET_PAGE_SIZE - (dst & ~TARGET_PAGE_MASK)) / width);
    if (desc & W64_FILL_ICOUNT) {
        k = MIN(k, budget / insns);
    }
    if (desc & W64_FILL_CYCLES) {
        /* each pass: count += taken; exit unless limit - count > 0; prepay */
        int32_t left = (int32_t)(cs->neg.cycle_limit - cs->neg.cycle_count) -
                       (int32_t)taken;

        if (left <= 0) {
            return;
        }
        k = MIN(k, (left - 1) / (taken + cycles) + 1);
    }
    if (k <= 0) {
        return;
    }

    hd = w64_fill_host(env, dst, MMU_DATA_STORE, mmu_idx);
    hs = w64_fill_host(env, src, MMU_DATA_LOAD, mmu_idx);
    if (hd == NULL || hs == NULL ||
        (hs + 4 > hd && hs < hd + k * width)) {
        return;
    }

    v = ldl_le_p(hs);
    switch (width) {
    case 1:
        memset(hd, v & 0xff, k);
        break;
    case 2:
        for (int64_t i = 0; i < k; i++) {
            stw_le_p(hd + 2 * i, v);
        }
        break;
    default:
        for (int64_t i = 0; i < k; i++) {
            stl_le_p(hd + 4 * i, v);
        }
        break;
    }

    env->regs[rv] = v;
    env->regs[rd] = dst + k * width;
    c = env->regs[rc] + k;
    env->regs[rc] = c;
    /* the last pass's cmp Rc, Rn */
    res = c - env->regs[rn];
    env->NF = env->ZF = res;
    env->CF = c >= env->regs[rn];
    env->VF = (c ^ env->regs[rn]) & (c ^ res);

    if (desc & W64_FILL_ICOUNT) {
        cs->neg.icount_decr.u16.low = budget - k * insns;
    }
    if (desc & W64_FILL_CYCLES) {
        cs->neg.cycle_count += k * (taken + cycles);
    }
    acct = w64_tb_acct_insns();
    if (acct) {
        *acct += k * insns;
    }
    ls = w64_ls_count();
    if (ls) {
        *ls += k * insns;
    }
}

#endif
