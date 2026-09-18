/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Definition of TranslationBlock.
 *  Copyright (c) 2003 Fabrice Bellard
 */

#ifndef EXEC_TRANSLATION_BLOCK_H
#define EXEC_TRANSLATION_BLOCK_H

#include "qemu/atomic.h"
#include "qemu/thread.h"
#include "exec/cpu-common.h"
#include "exec/vaddr.h"
#ifdef CONFIG_USER_ONLY
#include "qemu/interval-tree.h"
#include "exec/target_page.h"
#else
#include "system/ram_addr.h"
#endif

/*
 * Page tracking code uses ram addresses in system mode, and virtual
 * addresses in userspace mode.  Define tb_page_addr_t to be an
 * appropriate type.
 */
#if defined(CONFIG_USER_ONLY)
typedef vaddr tb_page_addr_t;
#define TB_PAGE_ADDR_FMT "%" VADDR_PRIx
#else
typedef ram_addr_t tb_page_addr_t;
#define TB_PAGE_ADDR_FMT RAM_ADDR_FMT
#endif

/*
 * Translation Cache-related fields of a TB.
 * This struct exists just for convenience; we keep track of TB's in a binary
 * search tree, and the only fields needed to compare TB's in the tree are
 * @ptr and @size.
 * Note: the address of search data can be obtained by adding @size to @ptr.
 */
struct tb_tc {
    const void *ptr;    /* pointer to the translated code */
    size_t size;
};

struct TranslationBlock {
    /*
     * Guest PC corresponding to this block.  This must be the true
     * virtual address.  Therefore e.g. x86 stores EIP + CS_BASE, and
     * targets like Arm, MIPS, HP-PA, which reuse low bits for ISA or
     * privilege, must store those bits elsewhere.
     *
     * If CF_PCREL, the opcodes for the TranslationBlock are written
     * such that the TB is associated only with the physical page and
     * may be run in any virtual address context.  In this case, PC
     * must always be taken from ENV in a target-specific manner.
     * Unwind information is taken as offsets from the page, to be
     * deposited into the "current" PC.
     */
    vaddr pc;

    /*
     * Target-specific data associated with the TranslationBlock, e.g.:
     * x86: the original user, the Code Segment virtual base,
     * arm: an extension of tb->flags,
     * s390x: instruction data for EXECUTE,
     * sparc: the next pc of the instruction queue (for delay slots).
     * riscv: an extension of tb->flags,
     */
    uint64_t cs_base;

    uint32_t flags; /* flags defining in which context the code was generated */
    uint32_t cflags;    /* compile flags */

/* Note that TCG_MAX_INSNS is 512; we validate this match elsewhere. */
#define CF_COUNT_MASK    0x000001ff
#define CF_NO_GOTO_TB    0x00000200 /* Do not chain with goto_tb */
#define CF_NO_GOTO_PTR   0x00000400 /* Do not chain with goto_ptr */
#define CF_SINGLE_STEP   0x00000800 /* gdbstub single-step in effect */
#define CF_MEMI_ONLY     0x00001000 /* Only instrument memory ops */
#define CF_USE_ICOUNT    0x00002000
#define CF_INVALID       0x00004000 /* TB is stale. Set with @jmp_lock held */
#define CF_PARALLEL      0x00008000 /* Generate code for a parallel context */
#define CF_NOIRQ         0x00010000 /* Generate an uninterruptible TB */
#define CF_PCREL         0x00020000 /* Opcodes in TB are PC-relative */
#define CF_BP_PAGE       0x00040000 /* Breakpoint present in code page */
#define CF_CLUSTER_MASK  0xff000000 /* Top 8 bits are cluster ID */
#define CF_CLUSTER_SHIFT 24

    /*
     * Above fields used for comparing
     */

    /* size of target code for this block (1 <= size <= TARGET_PAGE_SIZE) */
    uint16_t size;
    uint16_t icount;

    struct tb_tc tc;

    /*
     * Track tb_page_addr_t intervals that intersect this TB.
     * For user-only, the virtual addresses are always contiguous,
     * and we use a unified interval tree.  For system, we use a
     * linked list headed in each PageDesc.  Within the list, the lsb
     * of the previous pointer tells the index of page_next[], and the
     * list is protected by the PageDesc lock(s).
     */
#ifdef CONFIG_USER_ONLY
    IntervalTreeNode itree;
#else
    uintptr_t page_next[2];
    tb_page_addr_t page_addr[2];
#endif

    /* jmp_lock placed here to fill a 4-byte hole. Its documentation is below */
    QemuSpin jmp_lock;

    /* The following data are used to directly call another TB from
     * the code of this one. This can be done either by emitting direct or
     * indirect native jump instructions. These jumps are reset so that the TB
     * just continues its execution. The TB can be linked to another one by
     * setting one of the jump targets (or patching the jump instruction). Only
     * two of such jumps are supported.
     */
#define TB_JMP_OFFSET_INVALID 0xffff /* indicates no jump generated */
    uint16_t jmp_reset_offset[2]; /* offset of original jump target */
    uint16_t jmp_insn_offset[2];  /* offset of direct jump insn */
    uintptr_t jmp_target_addr[2]; /* target address */
#ifdef CONFIG_TCG_WASM64
    /*
     * Guest addresses of this TB's goto_tb destinations, recorded by
     * translator_use_goto_tb: the wasm64 backend translates them ahead
     * of execution so the browser compiles TBs in batches
     * (accel/tcg/cpu-exec.c w64_speculate).  A hint only.
     */
    vaddr w64_succ[3];
    uint8_t w64_nsucc;
    uint8_t w64_explored;   /* speculation: every successor already exists */
    /*
     * Inline next-TB cache for this TB's goto_ptr exit (target/arm
     * gen_goto_ptr): the emitted code compares pc, tb_key_gen and the
     * key words flagged dynamic in @dynmask against the CPU and
     * tail-calls @tc on a match instead of calling helper_lookup_tb_ptr.
     * The other key words were stamped by the translator (the exit's
     * static key) and helper_lookup_tb_ptr_lc fills the slot only when
     * they match the CPU; a bump of cpu->neg.tb_key_gen retires every
     * slot at once.  @gen 0 = empty.
     */
    struct W64LookupCache {
        uint32_t pc;
        uint32_t gen;
        uint32_t key32[3];  /* ARM: hflags.flags, thumb, condexec_bits */
        uint8_t dynmask;    /* W64_LC_DYN_* bits: key32[i] is dynamic */
        const void *tc;
    } w64_lc;
#define W64_LC_DYN_FLAGS     1
#define W64_LC_DYN_THUMB     2
#define W64_LC_DYN_CONDEXEC  4

/*
 * What a goto_ptr operand means to the wasm64 backend.
 *
 * The emitted dispatch is a return_call_indirect through the shared chain
 * table, so all it needs is the target's table index.  That index used to
 * be read out of the descriptor at tb->tc.ptr — one u32 per ~576-byte
 * module staging area, i.e. one cache line per TB in a ~20 MB region that
 * nothing else in the execution path touches.  A synthetic dispatch chain
 * (tools/dispatch-probe.mjs) prices that dependent load at +2 ns over a
 * 256-TB working set and +8..9 ns over 1024-4096 TBs, on a dispatch that
 * runs ~11 M times a second.
 *
 * So the lookup helpers hand the index over directly, tagged in the high
 * half (a wasm64 heap pointer is < 2 GB, so a real pointer never has one):
 *
 *   hi != 0   W64_TIDX_TAG | tidx — tail-call table[tidx]
 *   hi == 0   a descriptor pointer (target not compiled yet, or its batch
 *             was evicted) or NULL (lookup miss): hand off to the C
 *             dispatcher, which is what used to happen when fidx was 0.
 *
 * The offsets mirror W64_DESC_FIDX / W64_DESC_TIDX in tcg/wasm64/wasm64.h,
 * which asserts they agree.
 */
#define W64_TCP_FIDX     0
#define W64_TCP_BATCH    4
#define W64_TCP_TIDX    16
#define W64_TIDX_TAG    (1ULL << 32)
/* set in the W64_TCP_BATCH word once the TB's batch module landed */
#define W64_TCP_BATCH_TAG  0x80000000u
#endif

    /*
     * Each TB has a NULL-terminated list (jmp_list_head) of incoming jumps.
     * Each TB can have two outgoing jumps, and therefore can participate
     * in two lists. The list entries are kept in jmp_list_next[2]. The least
     * significant bit (LSB) of the pointers in these lists is used to encode
     * which of the two list entries is to be used in the pointed TB.
     *
     * List traversals are protected by jmp_lock. The destination TB of each
     * outgoing jump is kept in jmp_dest[] so that the appropriate jmp_lock
     * can be acquired from any origin TB.
     *
     * jmp_dest[] are tagged pointers as well. The LSB is set when the TB is
     * being invalidated, so that no further outgoing jumps from it can be set.
     *
     * jmp_lock also protects the CF_INVALID cflag; a jump must not be chained
     * to a destination TB that has CF_INVALID set.
     */
    uintptr_t jmp_list_head;
    uintptr_t jmp_list_next[2];
    uintptr_t jmp_dest[2];
};

#ifdef CONFIG_TCG_WASM64
/*
 * The global pc-keyed next-TB cache (accel/tcg/cpu-exec.c) as the
 * generated code sees it.  A goto_ptr whose per-TB slot misses used to
 * call the helper for this table's six-word compare; the translator emits
 * that compare instead, so the entry layout and the hash's own constants
 * have to leave cpu-exec.c.
 */
struct W64PccEnt {
    uint32_t pc;
    uint32_t gen;
    uint32_t key32[3];
    uint32_t cpu_index;
    const void *tc;
};

struct W64PccShape {
    struct W64PccEnt *tab;
    unsigned shift;         /* TARGET_PAGE_BITS - TB_JMP_PAGE_BITS */
    uint32_t page_mask;     /* TB_JMP_PAGE_MASK */
    uint32_t addr_mask;     /* TB_JMP_ADDR_MASK */
};

const struct W64PccShape *w64_pcc_shape(void);
bool w64_pcc_inline(void);

/*
 * Interpreter-tier gate (tcg/wasm64/w64-interp.c): 0 off, 1 interpret a
 * TB until it earns a module, 2 interpret always.  accel/tcg reads it to
 * decide whether speculative successor translation is worth anything --
 * see w64_speculate().
 */
extern uint32_t w64_interp_gate;
#endif

/* The alignment given to TranslationBlock during allocation. */
#define CODE_GEN_ALIGN  16

#ifdef CONFIG_TCG_WASM64
/* accel/tcg/tb-maint.c, for tcg/wasm64/wasm64.c's batch eviction. */
void tb_w64_unlink_incoming(TranslationBlock *dest);

/*
 * True while something charges tb->icount once per TB entry from the
 * emitted prologue (tcg/wasm64/tcg-target.c.inc).  A TB that can leave
 * before its last instruction over-charges those, so a frontend must
 * not merge a conditional branch's fall-through while any is armed.
 */
bool w64_tb_icount_exact(void);

/*
 * The per-TB-entry guest-instruction counter, when one is charged from
 * the prologue, else NULL.  A frontend that exits a TB early subtracts
 * what it skipped, which is what keeps that counter out of
 * w64_tb_icount_exact.
 */
uint64_t *w64_tb_acct_insns(void);
#endif

/* Hide the qatomic_read to make code a little easier on the eyes */
static inline uint32_t tb_cflags(const TranslationBlock *tb)
{
    return qatomic_read(&tb->cflags);
}

bool tcg_cflags_has(CPUState *cpu, uint32_t flags);
void tcg_cflags_set(CPUState *cpu, uint32_t flags);

static inline tb_page_addr_t tb_page_addr0(const TranslationBlock *tb)
{
#ifdef CONFIG_USER_ONLY
    return tb->itree.start;
#else
    return tb->page_addr[0];
#endif
}

static inline tb_page_addr_t tb_page_addr1(const TranslationBlock *tb)
{
#ifdef CONFIG_USER_ONLY
    tb_page_addr_t next = tb->itree.last & TARGET_PAGE_MASK;
    return next == (tb->itree.start & TARGET_PAGE_MASK) ? -1 : next;
#else
    return tb->page_addr[1];
#endif
}

static inline void tb_set_page_addr0(TranslationBlock *tb,
                                     tb_page_addr_t addr)
{
#ifdef CONFIG_USER_ONLY
    tb->itree.start = addr;
    /*
     * To begin, we record an interval of one byte.  When the translation
     * loop encounters a second page, the interval will be extended to
     * include the first byte of the second page, which is sufficient to
     * allow tb_page_addr1() above to work properly.  The final corrected
     * interval will be set by tb_page_add() from tb->size before the
     * node is added to the interval tree.
     */
    tb->itree.last = addr;
#else
    tb->page_addr[0] = addr;
#endif
}

static inline void tb_set_page_addr1(TranslationBlock *tb,
                                     tb_page_addr_t addr)
{
#ifdef CONFIG_USER_ONLY
    /* Extend the interval to the first byte of the second page.  See above. */
    tb->itree.last = addr;
#else
    tb->page_addr[1] = addr;
#endif
}

/* TranslationBlock invalidate API */
void tb_invalidate_phys_range(CPUState *cpu, tb_page_addr_t start,
                              tb_page_addr_t last);

#endif /* EXEC_TRANSLATION_BLOCK_H */
