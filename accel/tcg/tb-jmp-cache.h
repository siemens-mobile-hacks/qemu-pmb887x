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

#ifdef CONFIG_TCG_WASM64
/*
 * The wasm64 backend reaches this cache on every inline next-TB cache
 * miss, and the pc cache its emitted code probes first (cpu-exec.c
 * w64_pcc) has the same size and hash.  At 12 bits most qht lookups were
 * conflict misses; 14 was the peak measured on a boot, and the J2ME
 * working set in play still sends two thirds of the miss path's qht
 * traffic through conflicts at 14 (round fifty-three), hence 16.
 */
#define TB_JMP_CACHE_BITS 16
#else
#define TB_JMP_CACHE_BITS 12
#endif
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
#ifdef CONFIG_TCG_WASM64
    /*
     * A full flush moves this generation instead of storing NULL into
     * 65536 entries: an entry is valid only while the generation stamped
     * into the unused upper half of its pc (the guest pc is 32-bit)
     * matches.  The per-page clear and the per-TB invalidation still
     * store NULL, so a TLBIMVA does not empty the whole cache.
     */
    uint32_t gen;
#endif
    struct {
        TranslationBlock *tb;
        vaddr pc;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;

/* the pc as stored in an entry and compared against it */
static inline vaddr tb_jmp_cache_key(const CPUJumpCache *jc, vaddr pc)
{
#ifdef CONFIG_TCG_WASM64
    return pc | ((vaddr)jc->gen << 32);
#else
    return pc;
#endif
}

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
