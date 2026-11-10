/*
 * WASM PORT FAST PATH (removable): Thumb fill loops.
 *
 * The firmware's J2ME graphics fill a rectangle row by row with
 *
 *     head: movs  Rc, #0
 *           b     test
 *     loop: ldr   Rv, [Rp]        @ the colour, re-read every pass
 *           strh  Rv, [Rd]        @ or strb / str
 *           adds  Rd, #2          @ #1 / #4, the store width
 *           adds  Rc, #1
 *     test: cmp   Rc, Rn
 *           blt   loop
 *           adds  Rd, Rpitch, Rd  @ the row tail
 *           adds  Rrow, #1
 *           ldr   Rlim, [sp, #imm]
 *           cmp   Rrow, Rlim
 *           blt   head
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
 *
 * Most rows are a few passes long, so a row costs two TB entries as well:
 * the loop's TB runs on into the tail, whose blt leaves for the head's TB
 * (from the label at head, the b absorbed), and the head's blt leaves for
 * the loop's TB again.  w64_rect_match() in translate.c records each
 * exit's refund and slot in its TB.  When a row is done and the two exits
 * chain into each other, the helper also runs the last pass, the tail, the
 * head and the next row's passes, row after row, charging both TB entries
 * as their prologues and exits would.  It does that only when none of the
 * budget checks on the way would leave, and never with icount or the
 * lockstep fold.  With four TLB probes and a register write-back per row,
 * that cost as much as the two TBs it replaced, so the loop keeps the
 * rectangle's state in locals, probes once per page and writes back once.
 *
 * It always leaves at least an already-charged pass to the generated code,
 * so the loop exit, its flags and any fault happen there as before.
 *
 * Removing this file, its meson line, the helper-defs.h entries and the
 * w64_fill, w64_rect and w64_blit hooks in translate.c / translate.h /
 * translation-block.h restores the plain loops.  The blit loop is
 * described further down.
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

/* passes per helper call, the rectangle's two per row included */
#define W64_FILL_MAX_PASSES 4096

typedef struct W64Fill {
    unsigned rv, rp, rd, rc, rn, width;
    unsigned insns, taken, cycles;
    uint32_t desc;
    int mmu_idx;
} W64Fill;

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

static bool w64_fill_overlap(const uint8_t *a, size_t na,
                             const uint8_t *b, size_t nb)
{
    return a < b + nb && b < a + na;
}

static void w64_fill_store(uint8_t *h, unsigned width, uint32_t v)
{
    if (width == 1) {
        *h = v;
    } else if (width == 2) {
        stw_le_p(h, v);
    } else {
        stl_le_p(h, v);
    }
}

/* @k stores of the colour @v from @h on */
static void w64_fill_span(uint8_t *h, size_t k, unsigned width, uint32_t v)
{
    if (width == 1) {
        memset(h, v & 0xff, k);
    } else {
        /* the colour repeats every width bytes, so 8 of them fit any
         * width-aligned offset: a quarter (or half) of the stores, and of
         * their Android bounds checks */
        uint64_t pattern = width == 2 ? (v & 0xffff) * 0x0001000100010001ULL
                                      : v * 0x0000000100000001ULL;
        size_t n = k * width, i = 0;

        for (; i + 8 <= n; i += 8) {
            stq_le_p(h + i, pattern);
        }
        for (; i < n; i += width) {
            w64_fill_store(h + i, width, v);
        }
    }
}

/* the flags of the loop's cmp Rc, Rn */
static void w64_fill_cmp(CPUARMState *env, uint32_t c, uint32_t n)
{
    uint32_t res = c - n;

    env->NF = env->ZF = res;
    env->CF = c >= n;
    env->VF = (c ^ n) & (c ^ res);
}

static void w64_fill_count(uint64_t insns)
{
    uint64_t *acct = w64_tb_acct_insns();
    int64_t *ls = w64_ls_count();

    if (acct) {
        *acct += insns;
    }
    if (ls) {
        *ls += insns;
    }
}

/*
 * Run the passes after the charged one whose blt is still taken, as many
 * as the budgets, the page and @cap allow.  True when that leaves the
 * charged pass the row's last.
 */
static bool w64_fill_row(CPUARMState *env, const W64Fill *f, int64_t *cap)
{
    CPUState *cs = env_cpu(env);
    unsigned width = f->width;
    uint32_t dst = env->regs[f->rd], src = env->regs[f->rp];
    int32_t budget = (int32_t)qatomic_read(&cs->neg.icount_decr.u32);
    uint8_t *hd;
    const uint8_t *hs;
    uint32_t v;
    int64_t rest, k;

    /* passes whose blt is still taken: Rc + 1 .. Rn - 1 */
    rest = (int64_t)(int32_t)env->regs[f->rn] - (int32_t)env->regs[f->rc] - 1;
    if (rest <= 0) {
        return rest == 0;
    }
    if (budget < 0 || (dst & (width - 1)) || (src & 3)) {
        return false;
    }
    k = MIN(rest, *cap);
    k = MIN(k, (TARGET_PAGE_SIZE - (dst & ~TARGET_PAGE_MASK)) / width);
    if (f->desc & W64_FILL_ICOUNT) {
        k = MIN(k, budget / f->insns);
    }
    if (f->desc & W64_FILL_CYCLES) {
        /* each pass: count += taken; exit unless limit - count > 0; prepay */
        int32_t left = (int32_t)(cs->neg.cycle_limit - cs->neg.cycle_count) -
                       (int32_t)f->taken;

        if (left <= 0) {
            return false;
        }
        k = MIN(k, (left - 1) / (f->taken + f->cycles) + 1);
    }
    if (k <= 0) {
        return false;
    }

    hd = w64_fill_host(env, dst, MMU_DATA_STORE, f->mmu_idx);
    hs = w64_fill_host(env, src, MMU_DATA_LOAD, f->mmu_idx);
    if (hd == NULL || hs == NULL || w64_fill_overlap(hs, 4, hd, k * width)) {
        return false;
    }

    v = ldl_le_p(hs);
    w64_fill_span(hd, k, width, v);

    env->regs[f->rv] = v;
    env->regs[f->rd] = dst + k * width;
    env->regs[f->rc] += k;
    w64_fill_cmp(env, env->regs[f->rc], env->regs[f->rn]);

    if (f->desc & W64_FILL_ICOUNT) {
        cs->neg.icount_decr.u16.low = budget - k * f->insns;
    }
    if (f->desc & W64_FILL_CYCLES) {
        cs->neg.cycle_count += k * (f->taken + f->cycles);
    }
    w64_fill_count(k * f->insns);
    *cap -= k;
    return k == rest;
}

/* the TB that @tb's rectangle exit chains to, NULL while it is unlinked */
static const TranslationBlock *w64_rect_link(const TranslationBlock *tb)
{
    unsigned n = tb->w64_rect.slot;
    uint64_t addr = qatomic_read(&tb->jmp_target_addr[n]);
    uintptr_t dest = qatomic_read(&tb->jmp_dest[n]);
    const TranslationBlock *next = (const TranslationBlock *)dest;

    /* the goto_tb tail-calls the table index in the slot (tb_set_jmp_target) */
    if (!(addr >> 32) || dest == 0 || (dest & 1) ||
        (uint32_t)addr != ((const uint32_t *)next->tc.ptr)[W64_TCP_TIDX / 4]) {
        return NULL;
    }
    return next;
}

/*
 * The head's TB, when the loop's TB @row is a rectangle's row whose two
 * exits chain into each other as compiled code: the interpreter tier's
 * goto_tb never chains.
 */
static const TranslationBlock *w64_rect_head(const TranslationBlock *row,
                                             const W64Fill *f)
{
    const TranslationBlock *head;

    if (row->w64_rect.kind != W64_RECT_ROW || (f->desc & W64_FILL_ICOUNT) ||
        w64_ls_count() || w64_interp_running()) {
        return NULL;
    }
    head = w64_rect_link(row);
    if (head == NULL || head->w64_rect.kind != W64_RECT_HEAD ||
        head->pc != row->w64_rect.dest || head->w64_rect.dest != row->pc ||
        head->w64_rect.regs != (f->rc | f->rn << 4) ||
        ((tb_cflags(head) ^ tb_cflags(row)) &
         (CF_COUNT_CYCLES | CF_USE_ICOUNT)) ||
        w64_rect_link(head) != row) {
        return NULL;
    }
    return head;
}

/* the host address of a store to @addr, through a one-page cache */
static uint8_t *w64_fill_page(CPUARMState *env, const W64Fill *f,
                              uint32_t addr, uint32_t *page, uintptr_t *base)
{
    uint8_t *h;

    if ((addr & TARGET_PAGE_MASK) == *page) {
        return (uint8_t *)(*base + (addr & ~TARGET_PAGE_MASK));
    }
    h = w64_fill_host(env, addr, MMU_DATA_STORE, f->mmu_idx);
    if (h) {
        *page = addr & TARGET_PAGE_MASK;
        *base = (uintptr_t)h - (addr & ~TARGET_PAGE_MASK);
    }
    return h;
}

/*
 * With a row's last pass charged: run it, the tail, the head and the next
 * row's first pass, up to the back-edge that charges the second, then the
 * passes w64_fill_row() would -- row after row, while none of the checks
 * on the way would leave and nothing faults.  Nothing the rows change is
 * read back but the limit, so the state stays in locals and is written
 * back once.
 */
static void w64_fill_rect(CPUARMState *env, const W64Fill *f,
                          const TranslationBlock *row,
                          const TranslationBlock *head, int64_t cap)
{
    CPUState *cs = env_cpu(env);
    const struct W64FillRect *rr = &row->w64_rect, *rh = &head->w64_rect;
    unsigned rrow = (rr->regs >> 4) & 7, rlim = (rr->regs >> 8) & 7;
    unsigned width = f->width, lg = ctz32(width);
    bool cyc = f->desc & W64_FILL_CYCLES;
    uint32_t pitch = env->regs[rr->regs & 7], n = env->regs[f->rn];
    uint32_t src = env->regs[f->rp];
    uint32_t slot = env->regs[13] + (rr->regs >> 16) * 4;
    uint32_t d = env->regs[f->rd], nrow = env->regs[rrow];
    uint32_t c = cs->neg.cycle_count, l = cs->neg.cycle_limit;
    /* what the exits, the prologues and the first pass's blt add */
    uint32_t t1 = rr->taken - rr->refund_cycles;
    uint32_t t2 = rh->cycles + rh->taken - rh->refund_cycles;
    uint32_t t3 = rr->cycles + f->taken;
    uint32_t pass = f->taken + f->cycles;
    uint64_t tins = (uint64_t)head->icount - rh->refund_insns +
                    row->icount - rr->refund_insns + f->insns;
    uint64_t ins = 0;
    uint32_t rest = n - 2, page = 1, lim = 0, v, k = 0, rows = 0;
    uintptr_t base = 0;
    const uint8_t *hs, *hl;
    uint64_t *acct;

    /* the next row's start is width-aligned when the pitch is */
    if ((int32_t)n < 2 || ((d | pitch) & (width - 1)) || ((src | slot) & 3)) {
        return;
    }
    hs = w64_fill_host(env, src, MMU_DATA_LOAD, f->mmu_idx);
    hl = w64_fill_host(env, slot, MMU_DATA_LOAD, f->mmu_idx);
    if (hs == NULL || hl == NULL) {
        return;
    }
    /* no store below touches the colour, so it holds */
    v = ldl_le_p(hs);

    while (cap >= 2 &&
           (int32_t)qatomic_read(&cs->neg.icount_decr.u32) >= 0) {
        uint32_t next = d + width + pitch, nl, c3 = 0, kk;
        uint8_t *hd, *hn;

        if (cyc) {
            /* the head's prologue, the loop's, then the back-edge */
            if ((int32_t)(l - (c + t1)) <= 0 ||
                (int32_t)(l - (c + t1 + t2)) <= 0 ||
                (int32_t)(l - (c + t1 + t2 + t3)) <= 0) {
                break;
            }
            c3 = c + t1 + t2 + t3 + f->cycles;
        }
        hd = w64_fill_page(env, f, d, &page, &base);
        if (hd == NULL || w64_fill_overlap(hd, width, hs, 4) ||
            w64_fill_overlap(hd, width, hl, 4)) {
            break;
        }
        nl = ldl_le_p(hl);
        if ((int32_t)(nrow + 1) >= (int32_t)nl) {
            break;
        }
        hn = w64_fill_page(env, f, next, &page, &base);
        if (hn == NULL || w64_fill_overlap(hn, width, hs, 4)) {
            break;
        }
        w64_fill_store(hd, width, v);
        w64_fill_store(hn, width, v);
        d = next + width;
        nrow++;
        lim = nl;
        c = c3;
        ins += tins;
        cap -= 2;
        rows++;
        k = 0;

        /* the passes w64_fill_row() would run from that back-edge */
        kk = MIN(rest, cap);
        kk = MIN(kk, (TARGET_PAGE_SIZE - (d & ~TARGET_PAGE_MASK)) >> lg);
        if (cyc && kk) {
            int32_t left = (int32_t)(l - c) - (int32_t)f->taken;

            if (left <= 0) {
                kk = 0;
            } else if ((uint64_t)(kk - 1) * pass > (uint32_t)(left - 1)) {
                kk = MIN(kk, (left - 1) / pass + 1);
            }
        }
        if (kk) {
            uint8_t *hr = w64_fill_page(env, f, d, &page, &base);

            if (hr == NULL || w64_fill_overlap(hs, 4, hr, kk * width)) {
                kk = 0;
            } else {
                w64_fill_span(hr, kk, width, v);
            }
        }
        d += kk * width;
        c += kk * pass;
        ins += (uint64_t)kk * f->insns;
        cap -= kk;
        k = kk;
        if (kk != rest) {
            break;
        }
    }
    if (rows == 0) {
        return;
    }

    /* Rlim may be the loop's Rv or Rc, which are written after it */
    env->regs[rrow] = nrow;
    env->regs[rlim] = lim;
    env->regs[f->rv] = v;
    env->regs[f->rd] = d;
    env->regs[f->rc] = 1 + k;
    w64_fill_cmp(env, 1 + k, n);
    if (cyc) {
        cs->neg.cycle_count = c;
    }
    acct = w64_tb_acct_insns();
    if (acct) {
        *acct += ins;
    }
}

void HELPER(w64_fill)(CPUARMState *env, const void *tbp, uint32_t desc,
                      uint32_t charge)
{
    const TranslationBlock *row = tbp, *head;
    W64Fill f = {
        .rv = desc & 7,
        .rp = (desc >> 4) & 7,
        .rd = (desc >> 8) & 7,
        .rc = (desc >> 12) & 7,
        .rn = (desc >> 16) & 7,
        .width = 1u << ((desc >> 20) & 3),
        .insns = charge & 0xff,
        .taken = (charge >> 8) & 0xff,
        .cycles = charge >> 16,
        .desc = desc,
        .mmu_idx = arm_env_mmu_index(env),
    };
    int64_t cap = W64_FILL_MAX_PASSES;

    if (arm_cpu_data_is_big_endian(env) || !w64_fill_row(env, &f, &cap)) {
        return;
    }
    head = w64_rect_head(row, &f);
    if (head) {
        w64_fill_rect(env, &f, row, head, cap);
    }
}

/*
 * The same firmware draws a colour-keyed 16-bit image a pixel at a time
 * (S75 at a0a05540, EL71 at a0a16890; 7 % of the stopwatch's vCPU):
 *
 *     loop:  ldrh  r1, [r3]
 *            cmp   r1, r0          @ the transparent colour
 *            beq   skip
 *            ldr   r4, =colour
 *            cmp   r1, r7          @ r7 is 0xe000
 *            beq   store
 *            movs  r4, #imm
 *            cmp   r1, #1
 *            beq   store
 *            adds  r4, r1, #0
 *     store: strh  r4, [r2]
 *     skip:  ldr   r1, [sp, #imm]  @ the destination step
 *            add   r3, ip
 *            adds  r2, r1, r2
 *            subs  r5, #1
 *            cmp   r5, #0
 *            bne   loop
 *
 * w64_blit_match() in translate.c recognises it at the start of a TB with
 * the three forward branches joined, and calls HELPER(w64_blit) at the
 * back-edge like w64_fill.  A pass's charge depends on its pixel: a taken
 * branch costs its cycles and its join hands back the instructions it
 * skipped.  The helper only runs passes that no budget check could stop
 * whatever their pixels are, a page of the source and of the destination
 * at a time, in guest order, so the two may overlap.
 */

#define W64_BLIT_SKIP_IMM     1
#define W64_BLIT_SKIP_LITERAL 4
#define W64_BLIT_SKIP_KEY     8

/* how many accesses from @addr on, @step apart, stay on its page */
static uint32_t w64_blit_room(uint32_t addr, int32_t step)
{
    uint32_t off = addr & ~TARGET_PAGE_MASK;

    if (step > 0) {
        return (TARGET_PAGE_SIZE - 2 - off) / step + 1;
    } else if (step < 0) {
        return off / -(int64_t)step + 1;
    }
    return UINT32_MAX;
}

void HELPER(w64_blit)(CPUARMState *env, uint32_t lit, uint32_t desc,
                      uint32_t refund, uint32_t charge)
{
    CPUState *cs = env_cpu(env);
    int mmu_idx = arm_env_mmu_index(env);
    unsigned insns = charge & 0xff, taken = (charge >> 8) & 0xff;
    unsigned cycles = charge >> 16;
    /* what a pass adds to the cycles on top of @cycles + @taken */
    int32_t cyc_imm = taken - (refund & 0xff);
    int32_t cyc_lit = taken - ((refund >> 8) & 0xff);
    int32_t cyc_key = taken - ((refund >> 16) & 0xff);
    uint32_t src = env->regs[3], dst = env->regs[2], r4 = env->regs[4];
    uint32_t key = env->regs[0], alt = env->regs[7], imm = desc & 0xff;
    uint32_t sstep = env->regs[12], dstep, colour;
    uint32_t slot = env->regs[13] + ((desc >> 8) & 0xff) * 4;
    int32_t budget = (int32_t)qatomic_read(&cs->neg.icount_decr.u32);
    /* passes whose bne is still taken */
    uint32_t k = env->regs[5] - 1, done = 0;
    uint32_t n_imm = 0, n_lit = 0, n_key = 0;
    const uint8_t *hslot, *hlit;
    uint64_t skipped;

    if (k == 0 || budget < 0 || arm_cpu_data_is_big_endian(env) ||
        ((src | dst | sstep) & 1) || (slot & 3)) {
        return;
    }
    k = MIN(k, W64_FILL_MAX_PASSES);
    if (desc & W64_FILL_ICOUNT) {
        /* w64_refund() hands nothing back once a device saw the clock */
        if (cs->neg.can_do_io) {
            return;
        }
        k = MIN(k, budget / insns);
    }
    if (desc & W64_FILL_CYCLES) {
        /* each pass: count += its branches; exit unless limit - count > 0;
         * prepay -- with the dearest pixel every time */
        int32_t most = MAX(MAX(cyc_imm, cyc_lit), MAX(cyc_key, 0));
        int32_t left = (int32_t)(cs->neg.cycle_limit - cs->neg.cycle_count) -
                       (int32_t)taken - most;

        if (left <= 0) {
            return;
        }
        k = MIN(k, (left - 1) / (cycles + taken + most) + 1);
    }
    hslot = w64_fill_host(env, slot, MMU_DATA_LOAD, mmu_idx);
    hlit = w64_fill_host(env, lit, MMU_DATA_LOAD, mmu_idx);
    if (hslot == NULL || hlit == NULL) {
        return;
    }
    dstep = ldl_le_p(hslot);
    colour = ldl_le_p(hlit);
    if (dstep & 1) {
        return;
    }

    while (k) {
        uint32_t n = MIN(k, MIN(w64_blit_room(src, sstep),
                                w64_blit_room(dst, dstep)));
        const uint8_t *hs = w64_fill_host(env, src, MMU_DATA_LOAD, mmu_idx);
        uint8_t *hd = w64_fill_host(env, dst, MMU_DATA_STORE, mmu_idx);
        const uint8_t *page;

        if (hs == NULL || hd == NULL) {
            break;
        }
        /* no store may reach the step or the literal */
        page = hd - (dst & ~TARGET_PAGE_MASK);
        if (w64_fill_overlap(page, TARGET_PAGE_SIZE, hslot, 4) ||
            w64_fill_overlap(page, TARGET_PAGE_SIZE, hlit, 4)) {
            break;
        }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t p = lduw_le_p(hs);

            if (p == key) {
                n_key++;
            } else {
                if (p == alt) {
                    r4 = colour;
                    n_lit++;
                } else if (p == 1) {
                    r4 = imm;
                    n_imm++;
                } else {
                    r4 = p;
                }
                stw_le_p(hd, r4);
            }
            hs += (int32_t)sstep;
            hd += (int32_t)dstep;
        }
        src += n * sstep;
        dst += n * dstep;
        done += n;
        k -= n;
    }
    if (done == 0) {
        return;
    }

    env->regs[1] = dstep;
    env->regs[2] = dst;
    env->regs[3] = src;
    env->regs[4] = r4;
    env->regs[5] -= done;
    w64_fill_cmp(env, env->regs[5], 0);

    skipped = (uint64_t)n_imm * W64_BLIT_SKIP_IMM +
              (uint64_t)n_lit * W64_BLIT_SKIP_LITERAL +
              (uint64_t)n_key * W64_BLIT_SKIP_KEY;
    if (desc & W64_FILL_ICOUNT) {
        cs->neg.icount_decr.u16.low = budget - done * insns + skipped;
    }
    if (desc & W64_FILL_CYCLES) {
        cs->neg.cycle_count += done * (cycles + taken) + n_imm * cyc_imm +
                               n_lit * cyc_lit + n_key * cyc_key;
    }
    w64_fill_count((uint64_t)done * insns - skipped);
}

#endif
