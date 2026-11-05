/*
 * internal execution defines for qemu
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef EXEC_TB_HASH_H
#define EXEC_TB_HASH_H

#include "exec/vaddr.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "qemu/xxhash.h"
#include "tb-jmp-cache.h"

#if defined(CONFIG_SOFTMMU) && defined(CONFIG_TCG_WASM64)

/*
 * The upstream hash below keeps 8 bits of the offset in a page, so the
 * 2048 Thumb PCs of a 4 KiB page share 256 entries, and three quarters
 * of the misses of the 64K-entry cache were TBs of one page evicting
 * each other.  Here every halfword of a page has its own entry, and a
 * multiplicative hash of the page number above 4 KiB picks one of
 * 32 blocks.  A page's entries stay one block for
 * tb_jmp_cache_clear_page (a superset for 1 KiB pages).
 */
#define TB_JMP_PAGE_BITS 11
#define TB_JMP_PAGE_SIZE (1 << TB_JMP_PAGE_BITS)
#define TB_JMP_ADDR_MASK (TB_JMP_PAGE_SIZE - 1)
#define TB_JMP_HASH_PAGE_SHIFT (TB_JMP_PAGE_BITS + 1)
#define TB_JMP_HASH_MUL 0x9e3779b1u
#define TB_JMP_HASH_MUL_SHIFT (32 - (TB_JMP_CACHE_BITS - TB_JMP_PAGE_BITS))

static inline unsigned int tb_jmp_cache_hash_page(vaddr pc)
{
    uint32_t page = (uint32_t)(pc >> TB_JMP_HASH_PAGE_SHIFT);

    return ((page * TB_JMP_HASH_MUL) >> TB_JMP_HASH_MUL_SHIFT)
           << TB_JMP_PAGE_BITS;
}

/*
 * Firmware routines run both from user-mode tasks and from privileged
 * modes, and same-PC key mismatches became most of the remaining misses:
 * two bits of the TB flags (ARM's low MMU index bits, which the
 * translator asserts) move an entry within its page's block.
 */
#define TB_JMP_HASH_FLAGS_SHIFT 4
#define TB_JMP_HASH_FLAGS(flags) \
    ((((flags) >> TB_JMP_HASH_FLAGS_SHIFT) & 3) << (TB_JMP_PAGE_BITS - 2))

static inline unsigned int tb_jmp_cache_hash_func(vaddr pc, uint32_t flags)
{
    return tb_jmp_cache_hash_page(pc) |
           (((pc >> 1) ^ TB_JMP_HASH_FLAGS(flags)) & TB_JMP_ADDR_MASK);
}

#elif defined(CONFIG_SOFTMMU)

/* Only the bottom TB_JMP_PAGE_BITS of the jump cache hash bits vary for
   addresses on the same page.  The top bits are the same.  This allows
   TLB invalidation to quickly clear a subset of the hash table.  */
#define TB_JMP_PAGE_BITS (TB_JMP_CACHE_BITS / 2)
#define TB_JMP_PAGE_SIZE (1 << TB_JMP_PAGE_BITS)
#define TB_JMP_ADDR_MASK (TB_JMP_PAGE_SIZE - 1)
#define TB_JMP_PAGE_MASK (TB_JMP_CACHE_SIZE - TB_JMP_PAGE_SIZE)

static inline unsigned int tb_jmp_cache_hash_page(vaddr pc)
{
    vaddr tmp;
    tmp = pc ^ (pc >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS));
    return (tmp >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS)) & TB_JMP_PAGE_MASK;
}

static inline unsigned int tb_jmp_cache_hash_func(vaddr pc, uint32_t flags)
{
    vaddr tmp;
    tmp = pc ^ (pc >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS));
    return (((tmp >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS)) & TB_JMP_PAGE_MASK)
           | (tmp & TB_JMP_ADDR_MASK));
}

#else

/* In user-mode we can get better hashing because we do not have a TLB */
static inline unsigned int tb_jmp_cache_hash_func(vaddr pc, uint32_t flags)
{
    return (pc ^ (pc >> TB_JMP_CACHE_BITS)) & (TB_JMP_CACHE_SIZE - 1);
}

#endif /* CONFIG_SOFTMMU */

static inline
uint32_t tb_hash_func(tb_page_addr_t phys_pc, vaddr pc,
                      uint32_t flags, uint64_t flags2, uint32_t cf_mask)
{
    return qemu_xxhash8(phys_pc, pc, flags2, flags, cf_mask);
}

#endif
