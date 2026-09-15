/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"

/*
 * 14, not upstream's 12.  The wasm64 backend reaches this cache through
 * helper_lookup_tb_ptr_lc on every inline-cache miss - ~15k per Mi - and
 * at 4096 entries 11.5 % of those fell through to the qht, 95 % of them
 * as conflict misses (WASM_DIAG_LOOKUP_CONFL, not cold pcs).  16384
 * entries cut qht lookups per Mi by 62 % (el71 1718 -> 649) and 55 %
 * (cx70 1814 -> 816).  16 bits is worse than 14 on both boards, so this
 * is the peak, not a floor: locality turns against the bigger table.
 *
 * The cost this trades against is the flush, and 0083 is what made the
 * trade affordable - with the EBU's readonly flips no longer forcing a
 * full topology commit, tcg_flush_jmp_cache runs ~0/s instead of 1135/s,
 * so walking 4x the entries costs nothing.  256 KB for the one vCPU.
 */
#define TB_JMP_CACHE_BITS 14
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
typedef struct CPUJumpCache {
    struct rcu_head rcu;
    struct {
        TranslationBlock *tb;
        vaddr pc;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
