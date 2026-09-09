/*
 * Tiny Code Interpreter for QEMU
 *
 * Copyright (c) 2009, 2011, 2016 Stefan Weil
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "tcg/tcg.h"
#include "tcg/helper-info.h"
#include "tcg/tcg-ldst.h"
#include "disas/dis-asm.h"
#include "tcg-has.h"
#include <ffi.h>

/*
 * TCI fast paths: both guest memory accesses and plain i32/i64 helper
 * calls normally route through libffi's ffi_call, which on wasm is a JS
 * round-trip (~1.7us per call) and on native hosts still re-marshals every
 * argument.  Probe the softmmu TLB inline before calling the load/store
 * helpers and dispatch simple-signature helpers through exactly-typed
 * function pointers instead (see tci_tlb_probe / tci_call_direct below).
 */
#include "hw/core/cpu.h"
#include "exec/cpu-common.h"
#include "exec/tlb-flags.h"
#include "exec/target_page.h"


/*
 * Enable TCI assertions only when debugging TCG (and without NDEBUG defined).
 * Without assertions, the interpreter runs much faster.
 */
#if defined(CONFIG_DEBUG_TCG)
# define tci_assert(cond) assert(cond)
#else
# define tci_assert(cond) ((void)(cond))
#endif

__thread uintptr_t tci_tb_ptr;

/*
 * Load sets of arguments all at once.  The naming convention is:
 *   tci_args_<arguments>
 * where arguments is a sequence of
 *
 *   b = immediate (bit position)
 *   c = condition (TCGCond)
 *   i = immediate (uint32_t)
 *   I = immediate (tcg_target_ulong)
 *   l = label or pointer
 *   m = immediate (MemOpIdx)
 *   n = immediate (call return length)
 *   r = register
 *   s = signed ldst offset
 */

static void tci_args_l(uint32_t insn, const void *tb_ptr, void **l0)
{
    int diff = sextract32(insn, 12, 20);
    *l0 = diff ? (void *)tb_ptr + diff : NULL;
}

static void tci_args_r(uint32_t insn, TCGReg *r0)
{
    *r0 = extract32(insn, 8, 4);
}

static void tci_args_nl(uint32_t insn, const void *tb_ptr,
                        uint8_t *n0, void **l1)
{
    *n0 = extract32(insn, 8, 4);
    *l1 = sextract32(insn, 12, 20) + (void *)tb_ptr;
}

static void tci_args_rl(uint32_t insn, const void *tb_ptr,
                        TCGReg *r0, void **l1)
{
    *r0 = extract32(insn, 8, 4);
    *l1 = sextract32(insn, 12, 20) + (void *)tb_ptr;
}

static void tci_args_rr(uint32_t insn, TCGReg *r0, TCGReg *r1)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
}

static void tci_args_ri(uint32_t insn, TCGReg *r0, tcg_target_ulong *i1)
{
    *r0 = extract32(insn, 8, 4);
    *i1 = sextract32(insn, 12, 20);
}

static void tci_args_rrm(uint32_t insn, TCGReg *r0,
                         TCGReg *r1, MemOpIdx *m2)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *m2 = extract32(insn, 16, 16);
}

static void tci_args_rrr(uint32_t insn, TCGReg *r0, TCGReg *r1, TCGReg *r2)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *r2 = extract32(insn, 16, 4);
}

static void tci_args_rrs(uint32_t insn, TCGReg *r0, TCGReg *r1, int32_t *i2)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *i2 = sextract32(insn, 16, 16);
}

static void tci_args_rrbb(uint32_t insn, TCGReg *r0, TCGReg *r1,
                          uint8_t *i2, uint8_t *i3)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *i2 = extract32(insn, 16, 6);
    *i3 = extract32(insn, 22, 6);
}

static void tci_args_rrrc(uint32_t insn,
                          TCGReg *r0, TCGReg *r1, TCGReg *r2, TCGCond *c3)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *r2 = extract32(insn, 16, 4);
    *c3 = extract32(insn, 20, 4);
}

static void tci_args_rrrbb(uint32_t insn, TCGReg *r0, TCGReg *r1,
                           TCGReg *r2, uint8_t *i3, uint8_t *i4)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *r2 = extract32(insn, 16, 4);
    *i3 = extract32(insn, 20, 6);
    *i4 = extract32(insn, 26, 6);
}

static void tci_args_rrrr(uint32_t insn,
                          TCGReg *r0, TCGReg *r1, TCGReg *r2, TCGReg *r3)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *r2 = extract32(insn, 16, 4);
    *r3 = extract32(insn, 20, 4);
}

static void tci_args_rrrrrc(uint32_t insn, TCGReg *r0, TCGReg *r1,
                            TCGReg *r2, TCGReg *r3, TCGReg *r4, TCGCond *c5)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *r2 = extract32(insn, 16, 4);
    *r3 = extract32(insn, 20, 4);
    *r4 = extract32(insn, 24, 4);
    *c5 = extract32(insn, 28, 4);
}

static bool tci_compare32(uint32_t u0, uint32_t u1, TCGCond condition)
{
    bool result = false;
    int32_t i0 = u0;
    int32_t i1 = u1;
    switch (condition) {
    case TCG_COND_EQ:
        result = (u0 == u1);
        break;
    case TCG_COND_NE:
        result = (u0 != u1);
        break;
    case TCG_COND_LT:
        result = (i0 < i1);
        break;
    case TCG_COND_GE:
        result = (i0 >= i1);
        break;
    case TCG_COND_LE:
        result = (i0 <= i1);
        break;
    case TCG_COND_GT:
        result = (i0 > i1);
        break;
    case TCG_COND_LTU:
        result = (u0 < u1);
        break;
    case TCG_COND_GEU:
        result = (u0 >= u1);
        break;
    case TCG_COND_LEU:
        result = (u0 <= u1);
        break;
    case TCG_COND_GTU:
        result = (u0 > u1);
        break;
    case TCG_COND_TSTEQ:
        result = (u0 & u1) == 0;
        break;
    case TCG_COND_TSTNE:
        result = (u0 & u1) != 0;
        break;
    default:
        g_assert_not_reached();
    }
    return result;
}

static bool tci_compare64(uint64_t u0, uint64_t u1, TCGCond condition)
{
    bool result = false;
    int64_t i0 = u0;
    int64_t i1 = u1;
    switch (condition) {
    case TCG_COND_EQ:
        result = (u0 == u1);
        break;
    case TCG_COND_NE:
        result = (u0 != u1);
        break;
    case TCG_COND_LT:
        result = (i0 < i1);
        break;
    case TCG_COND_GE:
        result = (i0 >= i1);
        break;
    case TCG_COND_LE:
        result = (i0 <= i1);
        break;
    case TCG_COND_GT:
        result = (i0 > i1);
        break;
    case TCG_COND_LTU:
        result = (u0 < u1);
        break;
    case TCG_COND_GEU:
        result = (u0 >= u1);
        break;
    case TCG_COND_LEU:
        result = (u0 <= u1);
        break;
    case TCG_COND_GTU:
        result = (u0 > u1);
        break;
    case TCG_COND_TSTEQ:
        result = (u0 & u1) == 0;
        break;
    case TCG_COND_TSTNE:
        result = (u0 & u1) != 0;
        break;
    default:
        g_assert_not_reached();
    }
    return result;
}

/*
 * Inline TLB probe, mirroring the compare that native TCG backends emit
 * (prepare_host_addr() in the native tcg-target.c.inc backends and tlb_set_compare() in
 * accel/tcg/cputlb.c).  When (addr[+s-a adjust] & (page_mask | a_mask))
 * equals tlb_addr, the entry is valid, RAM-backed, writable (clean pages
 * get TLB_NOTDIRTY and watchpointed entries TLB_FORCE_SLOW, both of which
 * live in tlb_addr's flag bits and make the compare fail) and the access
 * cannot straddle a page: addr + addend is then the host address.
 * Returns NULL when the helper slow path must be taken instead.
 */
static void *tci_tlb_probe(CPUArchState *env, uint64_t addr,
                           MemOpIdx oi, bool is_load)
{
    MemOp opc = get_memop(oi);
    unsigned s_mask = (1u << (opc & MO_SIZE)) - 1;
    unsigned a_mask = (1u << memop_alignment_bits(opc)) - 1;
    unsigned atom = opc & (7u << MO_ATOM_SHIFT);
    uint64_t page_mask = (uint64_t)(int64_t)TARGET_PAGE_MASK;
    CPUTLBDescFast *fast = cpu_tlb_fast(env_cpu(env), get_mmuidx(oi));
    CPUTLBEntry *entry;
    uint64_t tlb_addr, cmp;

    /*
     * The interpreter emits single plain wasm loads/stores; only aligned
     * ones may stand in for accesses with atomicity requirements.
     */
    if (atom != MO_ATOM_NONE && atom != MO_ATOM_IFALIGN &&
        (addr & s_mask) != 0) {
        return NULL;
    }

    entry = &fast->table[(addr >> TARGET_PAGE_BITS) &
                         (fast->mask >> CPU_TLB_ENTRY_BITS)];
    tlb_addr = is_load ? entry->addr_read : entry->addr_write;
    cmp = a_mask >= s_mask ? addr : addr + (s_mask - a_mask);

    if (likely((cmp & (page_mask | a_mask)) == tlb_addr)) {
        return (void *)(uintptr_t)(addr + entry->addend);
    }
    return NULL;
}

static uint64_t tci_qemu_ld(CPUArchState *env, uint64_t taddr,
                            MemOpIdx oi, const void *tb_ptr)
{
    MemOp mop = get_memop(oi);
    uintptr_t ra = (uintptr_t)tb_ptr;

    {
        void *haddr = tci_tlb_probe(env, taddr, oi, true);
        if (likely(haddr != NULL)) {
            uint64_t v;
            switch (mop & MO_SSIZE) {
            case MO_UB:
                return *(uint8_t *)haddr;
            case MO_SB:
                return (int8_t)*(uint8_t *)haddr;
            case MO_UW:
                v = *(uint16_t *)haddr;
                return (mop & MO_BSWAP) ? bswap16(v) : v;
            case MO_SW:
                v = *(uint16_t *)haddr;
                v = (mop & MO_BSWAP) ? bswap16(v) : v;
                return (int16_t)v;
            case MO_UL:
                v = *(uint32_t *)haddr;
                return (mop & MO_BSWAP) ? bswap32(v) : v;
            case MO_SL:
                v = *(uint32_t *)haddr;
                v = (mop & MO_BSWAP) ? bswap32(v) : v;
                return (int32_t)v;
            case MO_UQ:
                v = *(uint64_t *)haddr;
                return (mop & MO_BSWAP) ? bswap64(v) : v;
            default:
                g_assert_not_reached();
            }
        }
    }

    switch (mop & MO_SSIZE) {
    case MO_UB:
        return helper_ldub_mmu(env, taddr, oi, ra);
    case MO_SB:
        return helper_ldsb_mmu(env, taddr, oi, ra);
    case MO_UW:
        return helper_lduw_mmu(env, taddr, oi, ra);
    case MO_SW:
        return helper_ldsw_mmu(env, taddr, oi, ra);
    case MO_UL:
        return helper_ldul_mmu(env, taddr, oi, ra);
    case MO_SL:
        return helper_ldsl_mmu(env, taddr, oi, ra);
    case MO_UQ:
        return helper_ldq_mmu(env, taddr, oi, ra);
    default:
        g_assert_not_reached();
    }
}

static void tci_qemu_st(CPUArchState *env, uint64_t taddr, uint64_t val,
                        MemOpIdx oi, const void *tb_ptr)
{
    MemOp mop = get_memop(oi);
    uintptr_t ra = (uintptr_t)tb_ptr;

    {
        void *haddr = tci_tlb_probe(env, taddr, oi, false);
        if (likely(haddr != NULL)) {
            switch (mop & MO_SIZE) {
            case MO_UB:
                *(uint8_t *)haddr = (uint8_t)val;
                return;
            case MO_UW:
                *(uint16_t *)haddr =
                    (mop & MO_BSWAP) ? bswap16(val) : (uint16_t)val;
                return;
            case MO_UL:
                *(uint32_t *)haddr =
                    (mop & MO_BSWAP) ? bswap32(val) : (uint32_t)val;
                return;
            case MO_UQ:
                *(uint64_t *)haddr =
                    (mop & MO_BSWAP) ? bswap64(val) : val;
                return;
            default:
                g_assert_not_reached();
            }
        }
    }

    switch (mop & MO_SIZE) {
    case MO_UB:
        helper_stb_mmu(env, taddr, val, oi, ra);
        break;
    case MO_UW:
        helper_stw_mmu(env, taddr, val, oi, ra);
        break;
    case MO_UL:
        helper_stl_mmu(env, taddr, val, oi, ra);
        break;
    case MO_UQ:
        helper_stq_mmu(env, taddr, val, oi, ra);
        break;
    default:
        g_assert_not_reached();
    }
}

/*
 * Direct C dispatch for helper calls whose libffi signature uses only
 * i32/i64 words and at most 5 arguments.  On wasm, ffi_call() marshals
 * every argument through JS (ffi_call_js, ~1.7us); calling the helper
 * through a correctly typed function pointer is a native wasm
 * call_indirect instead.  On native hosts ffi_call() still re-marshals
 * every argument, so the same dispatch is a win there too.  Argument
 * words have already been stored into stack[0..n-1] by the preceding
 * TCI store ops (one 8-byte slot each for both i32 and i64 arguments);
 * the result is written back to stack[0] for the caller's
 * return-length switch.  Anything else returns false and the caller
 * falls back to libffi.
 *
 * The signature tag is encoded as
 *   bits 0-2  number of arguments
 *   bits 3-4  return class (0 = void, 1 = u32, 2 = u64)
 *   bits 5+   2 bits per argument (1 = u32, 2 = u64)
 * TCI_TAG_UNCLASSIFIED marks signatures that must use libffi (more
 * than 5 arguments, floating point/struct words); it is distinct from
 * the valid tag 0 of a (void) -> void helper.
 */
#define TCI_CLS_U32 1
#define TCI_CLS_U64 2
#define TCI_TAG_UNCLASSIFIED UINT32_MAX

static uint32_t tci_call_tag(const ffi_cif *cif)
{
    uint32_t tag = cif->nargs;
    unsigned i;

    if (cif->nargs > 5) {
        return TCI_TAG_UNCLASSIFIED;
    }
    switch (cif->rtype->type) {
    case FFI_TYPE_VOID:
        break;
    case FFI_TYPE_INT:
    case FFI_TYPE_UINT8:
    case FFI_TYPE_SINT8:
    case FFI_TYPE_UINT16:
    case FFI_TYPE_SINT16:
    case FFI_TYPE_UINT32:
    case FFI_TYPE_SINT32:
        tag |= TCI_CLS_U32 << 3;
        break;
    case FFI_TYPE_UINT64:
    case FFI_TYPE_SINT64:
#if UINTPTR_MAX == UINT64_MAX
    case FFI_TYPE_POINTER:
#endif
        tag |= TCI_CLS_U64 << 3;
        break;
#if UINTPTR_MAX < UINT64_MAX
    case FFI_TYPE_POINTER:
        tag |= TCI_CLS_U32 << 3;
        break;
#endif
    default:
        return TCI_TAG_UNCLASSIFIED;
    }

    for (i = 0; i < cif->nargs; i++) {
        switch (cif->arg_types[i]->type) {
        case FFI_TYPE_INT:
        case FFI_TYPE_UINT8:
        case FFI_TYPE_SINT8:
        case FFI_TYPE_UINT16:
        case FFI_TYPE_SINT16:
        case FFI_TYPE_UINT32:
        case FFI_TYPE_SINT32:
            tag |= TCI_CLS_U32 << (5 + 2 * i);
            break;
        case FFI_TYPE_UINT64:
        case FFI_TYPE_SINT64:
#if UINTPTR_MAX == UINT64_MAX
        case FFI_TYPE_POINTER:
#endif
            tag |= TCI_CLS_U64 << (5 + 2 * i);
            break;
#if UINTPTR_MAX < UINT64_MAX
        case FFI_TYPE_POINTER:
            tag |= TCI_CLS_U32 << (5 + 2 * i);
            break;
#endif
        default:
            return TCI_TAG_UNCLASSIFIED;
        }
    }
    return tag;
}

static bool tci_call_direct(void *func, uint32_t tag, uint64_t *stack)
{
    switch (tag) {
    case 0x0000: /* (void) -> void */
        ((void (*)(void))func)();
        return true;
    case 0x0008: /* (void) -> u32 */
        stack[0] = ((uint32_t (*)(void))func)();
        return true;
    case 0x0010: /* (void) -> u64 */
        stack[0] = ((uint64_t (*)(void))func)();
        return true;
    case 0x0021: /* (i32) -> void */
        ((void (*)(uint32_t))func)((uint32_t)stack[0]);
        return true;
    case 0x0041: /* (i64) -> void */
        ((void (*)(uint64_t))func)(stack[0]);
        return true;
    case 0x0029: /* (i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t))func)((uint32_t)stack[0]);
        return true;
    case 0x0049: /* (i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t))func)(stack[0]);
        return true;
    case 0x0031: /* (i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t))func)((uint32_t)stack[0]);
        return true;
    case 0x0051: /* (i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t))func)(stack[0]);
        return true;
    case 0x00a2: /* (i32,i32) -> void */
        ((void (*)(uint32_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1]);
        return true;
    case 0x00c2: /* (i64,i32) -> void */
        ((void (*)(uint64_t, uint32_t))func)(stack[0], (uint32_t)stack[1]);
        return true;
    case 0x0122: /* (i32,i64) -> void */
        ((void (*)(uint32_t, uint64_t))func)((uint32_t)stack[0], stack[1]);
        return true;
    case 0x0142: /* (i64,i64) -> void */
        ((void (*)(uint64_t, uint64_t))func)(stack[0], stack[1]);
        return true;
    case 0x00aa: /* (i32,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1]);
        return true;
    case 0x00ca: /* (i64,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint32_t))func)(stack[0], (uint32_t)stack[1]);
        return true;
    case 0x012a: /* (i32,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint64_t))func)((uint32_t)stack[0], stack[1]);
        return true;
    case 0x014a: /* (i64,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint64_t))func)(stack[0], stack[1]);
        return true;
    case 0x00b2: /* (i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1]);
        return true;
    case 0x00d2: /* (i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t))func)(stack[0], (uint32_t)stack[1]);
        return true;
    case 0x0132: /* (i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t))func)((uint32_t)stack[0], stack[1]);
        return true;
    case 0x0152: /* (i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t))func)(stack[0], stack[1]);
        return true;
    case 0x02a3: /* (i32,i32,i32) -> void */
        ((void (*)(uint32_t, uint32_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2]);
        return true;
    case 0x02c3: /* (i64,i32,i32) -> void */
        ((void (*)(uint64_t, uint32_t, uint32_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2]);
        return true;
    case 0x0323: /* (i32,i64,i32) -> void */
        ((void (*)(uint32_t, uint64_t, uint32_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2]);
        return true;
    case 0x0343: /* (i64,i64,i32) -> void */
        ((void (*)(uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], (uint32_t)stack[2]);
        return true;
    case 0x04a3: /* (i32,i32,i64) -> void */
        ((void (*)(uint32_t, uint32_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2]);
        return true;
    case 0x04c3: /* (i64,i32,i64) -> void */
        ((void (*)(uint64_t, uint32_t, uint64_t))func)(stack[0], (uint32_t)stack[1], stack[2]);
        return true;
    case 0x0523: /* (i32,i64,i64) -> void */
        ((void (*)(uint32_t, uint64_t, uint64_t))func)((uint32_t)stack[0], stack[1], stack[2]);
        return true;
    case 0x0543: /* (i64,i64,i64) -> void */
        ((void (*)(uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2]);
        return true;
    case 0x02ab: /* (i32,i32,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint32_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2]);
        return true;
    case 0x02cb: /* (i64,i32,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint32_t, uint32_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2]);
        return true;
    case 0x032b: /* (i32,i64,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint64_t, uint32_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2]);
        return true;
    case 0x034b: /* (i64,i64,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], (uint32_t)stack[2]);
        return true;
    case 0x04ab: /* (i32,i32,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint32_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2]);
        return true;
    case 0x04cb: /* (i64,i32,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint32_t, uint64_t))func)(stack[0], (uint32_t)stack[1], stack[2]);
        return true;
    case 0x052b: /* (i32,i64,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint64_t, uint64_t))func)((uint32_t)stack[0], stack[1], stack[2]);
        return true;
    case 0x054b: /* (i64,i64,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2]);
        return true;
    case 0x02b3: /* (i32,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2]);
        return true;
    case 0x02d3: /* (i64,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint32_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2]);
        return true;
    case 0x0333: /* (i32,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint32_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2]);
        return true;
    case 0x0353: /* (i64,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], (uint32_t)stack[2]);
        return true;
    case 0x04b3: /* (i32,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2]);
        return true;
    case 0x04d3: /* (i64,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint64_t))func)(stack[0], (uint32_t)stack[1], stack[2]);
        return true;
    case 0x0533: /* (i32,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint64_t))func)((uint32_t)stack[0], stack[1], stack[2]);
        return true;
    case 0x0553: /* (i64,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2]);
        return true;
    case 0x0aa4: /* (i32,i32,i32,i32) -> void */
        ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0ac4: /* (i64,i32,i32,i32) -> void */
        ((void (*)(uint64_t, uint32_t, uint32_t, uint32_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0b24: /* (i32,i64,i32,i32) -> void */
        ((void (*)(uint32_t, uint64_t, uint32_t, uint32_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0b44: /* (i64,i64,i32,i32) -> void */
        ((void (*)(uint64_t, uint64_t, uint32_t, uint32_t))func)(stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0ca4: /* (i32,i32,i64,i32) -> void */
        ((void (*)(uint32_t, uint32_t, uint64_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0cc4: /* (i64,i32,i64,i32) -> void */
        ((void (*)(uint64_t, uint32_t, uint64_t, uint32_t))func)(stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0d24: /* (i32,i64,i64,i32) -> void */
        ((void (*)(uint32_t, uint64_t, uint64_t, uint32_t))func)((uint32_t)stack[0], stack[1], stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0d44: /* (i64,i64,i64,i32) -> void */
        ((void (*)(uint64_t, uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], stack[2], (uint32_t)stack[3]);
        return true;
    case 0x12a4: /* (i32,i32,i32,i64) -> void */
        ((void (*)(uint32_t, uint32_t, uint32_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3]);
        return true;
    case 0x12c4: /* (i64,i32,i32,i64) -> void */
        ((void (*)(uint64_t, uint32_t, uint32_t, uint64_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3]);
        return true;
    case 0x1324: /* (i32,i64,i32,i64) -> void */
        ((void (*)(uint32_t, uint64_t, uint32_t, uint64_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], stack[3]);
        return true;
    case 0x1344: /* (i64,i64,i32,i64) -> void */
        ((void (*)(uint64_t, uint64_t, uint32_t, uint64_t))func)(stack[0], stack[1], (uint32_t)stack[2], stack[3]);
        return true;
    case 0x14a4: /* (i32,i32,i64,i64) -> void */
        ((void (*)(uint32_t, uint32_t, uint64_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], stack[3]);
        return true;
    case 0x14c4: /* (i64,i32,i64,i64) -> void */
        ((void (*)(uint64_t, uint32_t, uint64_t, uint64_t))func)(stack[0], (uint32_t)stack[1], stack[2], stack[3]);
        return true;
    case 0x1524: /* (i32,i64,i64,i64) -> void */
        ((void (*)(uint32_t, uint64_t, uint64_t, uint64_t))func)((uint32_t)stack[0], stack[1], stack[2], stack[3]);
        return true;
    case 0x1544: /* (i64,i64,i64,i64) -> void */
        ((void (*)(uint64_t, uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2], stack[3]);
        return true;
    case 0x0aac: /* (i32,i32,i32,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0acc: /* (i64,i32,i32,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0b2c: /* (i32,i64,i32,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint64_t, uint32_t, uint32_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0b4c: /* (i64,i64,i32,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint64_t, uint32_t, uint32_t))func)(stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0cac: /* (i32,i32,i64,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint32_t, uint64_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0ccc: /* (i64,i32,i64,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint32_t, uint64_t, uint32_t))func)(stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0d2c: /* (i32,i64,i64,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint64_t, uint64_t, uint32_t))func)((uint32_t)stack[0], stack[1], stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0d4c: /* (i64,i64,i64,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], stack[2], (uint32_t)stack[3]);
        return true;
    case 0x12ac: /* (i32,i32,i32,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3]);
        return true;
    case 0x12cc: /* (i64,i32,i32,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint32_t, uint32_t, uint64_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3]);
        return true;
    case 0x132c: /* (i32,i64,i32,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint64_t, uint32_t, uint64_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], stack[3]);
        return true;
    case 0x134c: /* (i64,i64,i32,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint64_t, uint32_t, uint64_t))func)(stack[0], stack[1], (uint32_t)stack[2], stack[3]);
        return true;
    case 0x14ac: /* (i32,i32,i64,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint32_t, uint64_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], stack[3]);
        return true;
    case 0x14cc: /* (i64,i32,i64,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint32_t, uint64_t, uint64_t))func)(stack[0], (uint32_t)stack[1], stack[2], stack[3]);
        return true;
    case 0x152c: /* (i32,i64,i64,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint64_t, uint64_t, uint64_t))func)((uint32_t)stack[0], stack[1], stack[2], stack[3]);
        return true;
    case 0x154c: /* (i64,i64,i64,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2], stack[3]);
        return true;
    case 0x0ab4: /* (i32,i32,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0ad4: /* (i64,i32,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0b34: /* (i32,i64,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint32_t, uint32_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0b54: /* (i64,i64,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint32_t, uint32_t))func)(stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0cb4: /* (i32,i32,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint64_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0cd4: /* (i64,i32,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint64_t, uint32_t))func)(stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0d34: /* (i32,i64,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint64_t, uint32_t))func)((uint32_t)stack[0], stack[1], stack[2], (uint32_t)stack[3]);
        return true;
    case 0x0d54: /* (i64,i64,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], stack[2], (uint32_t)stack[3]);
        return true;
    case 0x12b4: /* (i32,i32,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint32_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3]);
        return true;
    case 0x12d4: /* (i64,i32,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint64_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3]);
        return true;
    case 0x1334: /* (i32,i64,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint32_t, uint64_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], stack[3]);
        return true;
    case 0x1354: /* (i64,i64,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint32_t, uint64_t))func)(stack[0], stack[1], (uint32_t)stack[2], stack[3]);
        return true;
    case 0x14b4: /* (i32,i32,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint64_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], stack[3]);
        return true;
    case 0x14d4: /* (i64,i32,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint64_t, uint64_t))func)(stack[0], (uint32_t)stack[1], stack[2], stack[3]);
        return true;
    case 0x1534: /* (i32,i64,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint64_t, uint64_t))func)((uint32_t)stack[0], stack[1], stack[2], stack[3]);
        return true;
    case 0x1554: /* (i64,i64,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2], stack[3]);
        return true;
    case 0x2aa5: /* (i32,i32,i32,i32,i32) -> void */
        ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2ac5: /* (i64,i32,i32,i32,i32) -> void */
        ((void (*)(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2b25: /* (i32,i64,i32,i32,i32) -> void */
        ((void (*)(uint32_t, uint64_t, uint32_t, uint32_t, uint32_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2b45: /* (i64,i64,i32,i32,i32) -> void */
        ((void (*)(uint64_t, uint64_t, uint32_t, uint32_t, uint32_t))func)(stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2ca5: /* (i32,i32,i64,i32,i32) -> void */
        ((void (*)(uint32_t, uint32_t, uint64_t, uint32_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2cc5: /* (i64,i32,i64,i32,i32) -> void */
        ((void (*)(uint64_t, uint32_t, uint64_t, uint32_t, uint32_t))func)(stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2d25: /* (i32,i64,i64,i32,i32) -> void */
        ((void (*)(uint32_t, uint64_t, uint64_t, uint32_t, uint32_t))func)((uint32_t)stack[0], stack[1], stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2d45: /* (i64,i64,i64,i32,i32) -> void */
        ((void (*)(uint64_t, uint64_t, uint64_t, uint32_t, uint32_t))func)(stack[0], stack[1], stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x32a5: /* (i32,i32,i32,i64,i32) -> void */
        ((void (*)(uint32_t, uint32_t, uint32_t, uint64_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x32c5: /* (i64,i32,i32,i64,i32) -> void */
        ((void (*)(uint64_t, uint32_t, uint32_t, uint64_t, uint32_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x3325: /* (i32,i64,i32,i64,i32) -> void */
        ((void (*)(uint32_t, uint64_t, uint32_t, uint64_t, uint32_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x3345: /* (i64,i64,i32,i64,i32) -> void */
        ((void (*)(uint64_t, uint64_t, uint32_t, uint64_t, uint32_t))func)(stack[0], stack[1], (uint32_t)stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x34a5: /* (i32,i32,i64,i64,i32) -> void */
        ((void (*)(uint32_t, uint32_t, uint64_t, uint64_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x34c5: /* (i64,i32,i64,i64,i32) -> void */
        ((void (*)(uint64_t, uint32_t, uint64_t, uint64_t, uint32_t))func)(stack[0], (uint32_t)stack[1], stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x3525: /* (i32,i64,i64,i64,i32) -> void */
        ((void (*)(uint32_t, uint64_t, uint64_t, uint64_t, uint32_t))func)((uint32_t)stack[0], stack[1], stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x3545: /* (i64,i64,i64,i64,i32) -> void */
        ((void (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x4aa5: /* (i32,i32,i32,i32,i64) -> void */
        ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4ac5: /* (i64,i32,i32,i32,i64) -> void */
        ((void (*)(uint64_t, uint32_t, uint32_t, uint32_t, uint64_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4b25: /* (i32,i64,i32,i32,i64) -> void */
        ((void (*)(uint32_t, uint64_t, uint32_t, uint32_t, uint64_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4b45: /* (i64,i64,i32,i32,i64) -> void */
        ((void (*)(uint64_t, uint64_t, uint32_t, uint32_t, uint64_t))func)(stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4ca5: /* (i32,i32,i64,i32,i64) -> void */
        ((void (*)(uint32_t, uint32_t, uint64_t, uint32_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4cc5: /* (i64,i32,i64,i32,i64) -> void */
        ((void (*)(uint64_t, uint32_t, uint64_t, uint32_t, uint64_t))func)(stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4d25: /* (i32,i64,i64,i32,i64) -> void */
        ((void (*)(uint32_t, uint64_t, uint64_t, uint32_t, uint64_t))func)((uint32_t)stack[0], stack[1], stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4d45: /* (i64,i64,i64,i32,i64) -> void */
        ((void (*)(uint64_t, uint64_t, uint64_t, uint32_t, uint64_t))func)(stack[0], stack[1], stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x52a5: /* (i32,i32,i32,i64,i64) -> void */
        ((void (*)(uint32_t, uint32_t, uint32_t, uint64_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3], stack[4]);
        return true;
    case 0x52c5: /* (i64,i32,i32,i64,i64) -> void */
        ((void (*)(uint64_t, uint32_t, uint32_t, uint64_t, uint64_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3], stack[4]);
        return true;
    case 0x5325: /* (i32,i64,i32,i64,i64) -> void */
        ((void (*)(uint32_t, uint64_t, uint32_t, uint64_t, uint64_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], stack[3], stack[4]);
        return true;
    case 0x5345: /* (i64,i64,i32,i64,i64) -> void */
        ((void (*)(uint64_t, uint64_t, uint32_t, uint64_t, uint64_t))func)(stack[0], stack[1], (uint32_t)stack[2], stack[3], stack[4]);
        return true;
    case 0x54a5: /* (i32,i32,i64,i64,i64) -> void */
        ((void (*)(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], stack[3], stack[4]);
        return true;
    case 0x54c5: /* (i64,i32,i64,i64,i64) -> void */
        ((void (*)(uint64_t, uint32_t, uint64_t, uint64_t, uint64_t))func)(stack[0], (uint32_t)stack[1], stack[2], stack[3], stack[4]);
        return true;
    case 0x5525: /* (i32,i64,i64,i64,i64) -> void */
        ((void (*)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t))func)((uint32_t)stack[0], stack[1], stack[2], stack[3], stack[4]);
        return true;
    case 0x5545: /* (i64,i64,i64,i64,i64) -> void */
        ((void (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2], stack[3], stack[4]);
        return true;
    case 0x2aad: /* (i32,i32,i32,i32,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2acd: /* (i64,i32,i32,i32,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2b2d: /* (i32,i64,i32,i32,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint64_t, uint32_t, uint32_t, uint32_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2b4d: /* (i64,i64,i32,i32,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint64_t, uint32_t, uint32_t, uint32_t))func)(stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2cad: /* (i32,i32,i64,i32,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint32_t, uint64_t, uint32_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2ccd: /* (i64,i32,i64,i32,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint32_t, uint64_t, uint32_t, uint32_t))func)(stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2d2d: /* (i32,i64,i64,i32,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint64_t, uint64_t, uint32_t, uint32_t))func)((uint32_t)stack[0], stack[1], stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2d4d: /* (i64,i64,i64,i32,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint64_t, uint64_t, uint32_t, uint32_t))func)(stack[0], stack[1], stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x32ad: /* (i32,i32,i32,i64,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint64_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x32cd: /* (i64,i32,i32,i64,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint32_t, uint32_t, uint64_t, uint32_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x332d: /* (i32,i64,i32,i64,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint64_t, uint32_t, uint64_t, uint32_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x334d: /* (i64,i64,i32,i64,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint64_t, uint32_t, uint64_t, uint32_t))func)(stack[0], stack[1], (uint32_t)stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x34ad: /* (i32,i32,i64,i64,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint32_t, uint64_t, uint64_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x34cd: /* (i64,i32,i64,i64,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint32_t, uint64_t, uint64_t, uint32_t))func)(stack[0], (uint32_t)stack[1], stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x352d: /* (i32,i64,i64,i64,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint64_t, uint64_t, uint64_t, uint32_t))func)((uint32_t)stack[0], stack[1], stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x354d: /* (i64,i64,i64,i64,i32) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x4aad: /* (i32,i32,i32,i32,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4acd: /* (i64,i32,i32,i32,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint32_t, uint32_t, uint32_t, uint64_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4b2d: /* (i32,i64,i32,i32,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint64_t, uint32_t, uint32_t, uint64_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4b4d: /* (i64,i64,i32,i32,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint64_t, uint32_t, uint32_t, uint64_t))func)(stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4cad: /* (i32,i32,i64,i32,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint32_t, uint64_t, uint32_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4ccd: /* (i64,i32,i64,i32,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint32_t, uint64_t, uint32_t, uint64_t))func)(stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4d2d: /* (i32,i64,i64,i32,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint64_t, uint64_t, uint32_t, uint64_t))func)((uint32_t)stack[0], stack[1], stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4d4d: /* (i64,i64,i64,i32,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint64_t, uint64_t, uint32_t, uint64_t))func)(stack[0], stack[1], stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x52ad: /* (i32,i32,i32,i64,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint64_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3], stack[4]);
        return true;
    case 0x52cd: /* (i64,i32,i32,i64,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint32_t, uint32_t, uint64_t, uint64_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3], stack[4]);
        return true;
    case 0x532d: /* (i32,i64,i32,i64,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint64_t, uint32_t, uint64_t, uint64_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], stack[3], stack[4]);
        return true;
    case 0x534d: /* (i64,i64,i32,i64,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint64_t, uint32_t, uint64_t, uint64_t))func)(stack[0], stack[1], (uint32_t)stack[2], stack[3], stack[4]);
        return true;
    case 0x54ad: /* (i32,i32,i64,i64,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], stack[3], stack[4]);
        return true;
    case 0x54cd: /* (i64,i32,i64,i64,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint32_t, uint64_t, uint64_t, uint64_t))func)(stack[0], (uint32_t)stack[1], stack[2], stack[3], stack[4]);
        return true;
    case 0x552d: /* (i32,i64,i64,i64,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t))func)((uint32_t)stack[0], stack[1], stack[2], stack[3], stack[4]);
        return true;
    case 0x554d: /* (i64,i64,i64,i64,i64) -> u32 */
        stack[0] = ((uint32_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2], stack[3], stack[4]);
        return true;
    case 0x2ab5: /* (i32,i32,i32,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2ad5: /* (i64,i32,i32,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2b35: /* (i32,i64,i32,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint32_t, uint32_t, uint32_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2b55: /* (i64,i64,i32,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint32_t, uint32_t, uint32_t))func)(stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2cb5: /* (i32,i32,i64,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint64_t, uint32_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2cd5: /* (i64,i32,i64,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint64_t, uint32_t, uint32_t))func)(stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2d35: /* (i32,i64,i64,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint64_t, uint32_t, uint32_t))func)((uint32_t)stack[0], stack[1], stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x2d55: /* (i64,i64,i64,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint32_t, uint32_t))func)(stack[0], stack[1], stack[2], (uint32_t)stack[3], (uint32_t)stack[4]);
        return true;
    case 0x32b5: /* (i32,i32,i32,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint32_t, uint64_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x32d5: /* (i64,i32,i32,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint64_t, uint32_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x3335: /* (i32,i64,i32,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint32_t, uint64_t, uint32_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x3355: /* (i64,i64,i32,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint32_t, uint64_t, uint32_t))func)(stack[0], stack[1], (uint32_t)stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x34b5: /* (i32,i32,i64,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint64_t, uint64_t, uint32_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x34d5: /* (i64,i32,i64,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint64_t, uint64_t, uint32_t))func)(stack[0], (uint32_t)stack[1], stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x3535: /* (i32,i64,i64,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint64_t, uint64_t, uint32_t))func)((uint32_t)stack[0], stack[1], stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x3555: /* (i64,i64,i64,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], stack[2], stack[3], (uint32_t)stack[4]);
        return true;
    case 0x4ab5: /* (i32,i32,i32,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4ad5: /* (i64,i32,i32,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t, uint64_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4b35: /* (i32,i64,i32,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint32_t, uint32_t, uint64_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4b55: /* (i64,i64,i32,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint32_t, uint32_t, uint64_t))func)(stack[0], stack[1], (uint32_t)stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4cb5: /* (i32,i32,i64,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint64_t, uint32_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4cd5: /* (i64,i32,i64,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint64_t, uint32_t, uint64_t))func)(stack[0], (uint32_t)stack[1], stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4d35: /* (i32,i64,i64,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint64_t, uint32_t, uint64_t))func)((uint32_t)stack[0], stack[1], stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x4d55: /* (i64,i64,i64,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint32_t, uint64_t))func)(stack[0], stack[1], stack[2], (uint32_t)stack[3], stack[4]);
        return true;
    case 0x52b5: /* (i32,i32,i32,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint32_t, uint64_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3], stack[4]);
        return true;
    case 0x52d5: /* (i64,i32,i32,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint64_t, uint64_t))func)(stack[0], (uint32_t)stack[1], (uint32_t)stack[2], stack[3], stack[4]);
        return true;
    case 0x5335: /* (i32,i64,i32,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint32_t, uint64_t, uint64_t))func)((uint32_t)stack[0], stack[1], (uint32_t)stack[2], stack[3], stack[4]);
        return true;
    case 0x5355: /* (i64,i64,i32,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint32_t, uint64_t, uint64_t))func)(stack[0], stack[1], (uint32_t)stack[2], stack[3], stack[4]);
        return true;
    case 0x54b5: /* (i32,i32,i64,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t))func)((uint32_t)stack[0], (uint32_t)stack[1], stack[2], stack[3], stack[4]);
        return true;
    case 0x54d5: /* (i64,i32,i64,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint64_t, uint64_t, uint64_t))func)(stack[0], (uint32_t)stack[1], stack[2], stack[3], stack[4]);
        return true;
    case 0x5535: /* (i32,i64,i64,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t))func)((uint32_t)stack[0], stack[1], stack[2], stack[3], stack[4]);
        return true;
    case 0x5555: /* (i64,i64,i64,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2], stack[3], stack[4]);
        return true;
    default:
        return false;
    }
}

/* Interpret pseudo code in tb. */
/*
 * Disable CFI checks.
 * One possible operation in the pseudo code is a call to binary code.
 * Therefore, disable CFI checks in the interpreter function
 */
uintptr_t QEMU_DISABLE_CFI tcg_qemu_tb_exec(CPUArchState *env,
                                            const void *v_tb_ptr)
{
    const uint32_t *tb_ptr = v_tb_ptr;
    tcg_target_ulong regs[TCG_TARGET_NB_REGS];
    uint64_t stack[(TCG_STATIC_CALL_ARGS_SIZE + TCG_STATIC_FRAME_SIZE)
                   / sizeof(uint64_t)];
    bool carry = false;

    regs[TCG_AREG0] = (tcg_target_ulong)env;
    regs[TCG_REG_CALL_STACK] = (uintptr_t)stack;
    tci_assert(tb_ptr);

    for (;;) {
        uint32_t insn;
        TCGOpcode opc;
        TCGReg r0, r1, r2, r3, r4;
        tcg_target_ulong t1;
        TCGCond condition;
        uint8_t pos, len;
        uint32_t tmp32;
        uint64_t taddr;
        MemOpIdx oi;
        int32_t ofs;
        void *ptr;

        insn = *tb_ptr++;
        opc = extract32(insn, 0, 8);

        switch (opc) {
        case INDEX_op_call:
            {
                void *call_slots[MAX_CALL_IARGS];
                ffi_cif *cif;
                void *func;
                unsigned i, s, n;

                tci_args_nl(insn, tb_ptr, &len, &ptr);
                func = ((void **)ptr)[0];
                cif = ((void **)ptr)[1];

                {
                    uint32_t tag = tci_call_tag(cif);

                    if (tag != TCI_TAG_UNCLASSIFIED) {
                        /* Helpers may need the "return address" */
                        tci_tb_ptr = (uintptr_t)tb_ptr;
                        if (tci_call_direct(func, tag, stack)) {
                            goto tci_call_done;
                        }
                    }
                }

                n = cif->nargs;
                for (i = s = 0; i < n; ++i) {
                    ffi_type *t = cif->arg_types[i];
                    call_slots[i] = &stack[s];
                    s += DIV_ROUND_UP(t->size, 8);
                }

                /* Helper functions may need to access the "return address" */
                tci_tb_ptr = (uintptr_t)tb_ptr;
                ffi_call(cif, func, stack, call_slots);
            }
        tci_call_done: ;

            switch (len) {
            case 0: /* void */
                break;
            case 1: /* uint32_t */
                /*
                 * The result winds up "left-aligned" in the stack[0] slot.
                 * Note that libffi has an odd special case in that it will
                 * always widen an integral result to ffi_arg.
                 */
                if (sizeof(ffi_arg) == 8) {
                    regs[TCG_REG_R0] = (uint32_t)stack[0];
                } else {
                    regs[TCG_REG_R0] = *(uint32_t *)stack;
                }
                break;
            case 2: /* uint64_t */
                memcpy(&regs[TCG_REG_R0], stack, 8);
                break;
            case 3: /* Int128 */
                memcpy(&regs[TCG_REG_R0], stack, 16);
                break;
            default:
                g_assert_not_reached();
            }
            break;

        case INDEX_op_br:
            tci_args_l(insn, tb_ptr, &ptr);
            tb_ptr = ptr;
            continue;
        case INDEX_op_setcond:
            tci_args_rrrc(insn, &r0, &r1, &r2, &condition);
            regs[r0] = tci_compare64(regs[r1], regs[r2], condition);
            break;
        case INDEX_op_movcond:
            tci_args_rrrrrc(insn, &r0, &r1, &r2, &r3, &r4, &condition);
            tmp32 = tci_compare64(regs[r1], regs[r2], condition);
            regs[r0] = regs[tmp32 ? r3 : r4];
            break;
        case INDEX_op_mov:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = regs[r1];
            break;
        case INDEX_op_tci_movi:
            tci_args_ri(insn, &r0, &t1);
            regs[r0] = t1;
            break;
        case INDEX_op_tci_movl:
            tci_args_rl(insn, tb_ptr, &r0, &ptr);
            regs[r0] = *(tcg_target_ulong *)ptr;
            break;
        case INDEX_op_tci_setcarry:
            carry = true;
            break;

            /* Load/store operations (32 bit). */

        case INDEX_op_ld8u:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(uint8_t *)ptr;
            break;
        case INDEX_op_ld8s:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(int8_t *)ptr;
            break;
        case INDEX_op_ld16u:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(uint16_t *)ptr;
            break;
        case INDEX_op_ld16s:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(int16_t *)ptr;
            break;
        case INDEX_op_ld:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(tcg_target_ulong *)ptr;
            break;
        case INDEX_op_st8:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            *(uint8_t *)ptr = regs[r0];
            break;
        case INDEX_op_st16:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            *(uint16_t *)ptr = regs[r0];
            break;
        case INDEX_op_st:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            *(tcg_target_ulong *)ptr = regs[r0];
            break;

            /* Arithmetic operations (mixed 32/64 bit). */

        case INDEX_op_add:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] + regs[r2];
            break;
        case INDEX_op_sub:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] - regs[r2];
            break;
        case INDEX_op_mul:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] * regs[r2];
            break;
        case INDEX_op_and:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] & regs[r2];
            break;
        case INDEX_op_or:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] | regs[r2];
            break;
        case INDEX_op_xor:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] ^ regs[r2];
            break;
        case INDEX_op_andc:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] & ~regs[r2];
            break;
        case INDEX_op_orc:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] | ~regs[r2];
            break;
        case INDEX_op_eqv:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ~(regs[r1] ^ regs[r2]);
            break;
        case INDEX_op_nand:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ~(regs[r1] & regs[r2]);
            break;
        case INDEX_op_nor:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ~(regs[r1] | regs[r2]);
            break;
        case INDEX_op_neg:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = -regs[r1];
            break;
        case INDEX_op_not:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = ~regs[r1];
            break;
        case INDEX_op_ctpop:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = ctpop64(regs[r1]);
            break;
        case INDEX_op_addco:
            tci_args_rrr(insn, &r0, &r1, &r2);
            t1 = regs[r1] + regs[r2];
            carry = t1 < regs[r1];
            regs[r0] = t1;
            break;
        case INDEX_op_addci:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] + regs[r2] + carry;
            break;
        case INDEX_op_addcio:
            tci_args_rrr(insn, &r0, &r1, &r2);
            if (carry) {
                t1 = regs[r1] + regs[r2] + 1;
                carry = t1 <= regs[r1];
            } else {
                t1 = regs[r1] + regs[r2];
                carry = t1 < regs[r1];
            }
            regs[r0] = t1;
            break;
        case INDEX_op_subbo:
            tci_args_rrr(insn, &r0, &r1, &r2);
            carry = regs[r1] < regs[r2];
            regs[r0] = regs[r1] - regs[r2];
            break;
        case INDEX_op_subbi:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] - regs[r2] - carry;
            break;
        case INDEX_op_subbio:
            tci_args_rrr(insn, &r0, &r1, &r2);
            if (carry) {
                carry = regs[r1] <= regs[r2];
                regs[r0] = regs[r1] - regs[r2] - 1;
            } else {
                carry = regs[r1] < regs[r2];
                regs[r0] = regs[r1] - regs[r2];
            }
            break;
        case INDEX_op_muls2:
            tci_args_rrrr(insn, &r0, &r1, &r2, &r3);
            muls64(&regs[r0], &regs[r1], regs[r2], regs[r3]);
            break;
        case INDEX_op_mulu2:
            tci_args_rrrr(insn, &r0, &r1, &r2, &r3);
            mulu64(&regs[r0], &regs[r1], regs[r2], regs[r3]);
            break;

            /* Arithmetic operations (32 bit). */

        case INDEX_op_tci_divs32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (int32_t)regs[r1] / (int32_t)regs[r2];
            break;
        case INDEX_op_tci_divu32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (uint32_t)regs[r1] / (uint32_t)regs[r2];
            break;
        case INDEX_op_tci_rems32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (int32_t)regs[r1] % (int32_t)regs[r2];
            break;
        case INDEX_op_tci_remu32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (uint32_t)regs[r1] % (uint32_t)regs[r2];
            break;
        case INDEX_op_tci_clz32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            tmp32 = regs[r1];
            regs[r0] = tmp32 ? clz32(tmp32) : regs[r2];
            break;
        case INDEX_op_tci_ctz32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            tmp32 = regs[r1];
            regs[r0] = tmp32 ? ctz32(tmp32) : regs[r2];
            break;
        case INDEX_op_tci_setcond32:
            tci_args_rrrc(insn, &r0, &r1, &r2, &condition);
            regs[r0] = tci_compare32(regs[r1], regs[r2], condition);
            break;
        case INDEX_op_tci_movcond32:
            tci_args_rrrrrc(insn, &r0, &r1, &r2, &r3, &r4, &condition);
            tmp32 = tci_compare32(regs[r1], regs[r2], condition);
            regs[r0] = regs[tmp32 ? r3 : r4];
            break;

            /* Shift/rotate operations. */

        case INDEX_op_shl:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] << (regs[r2] % TCG_TARGET_REG_BITS);
            break;
        case INDEX_op_shr:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] >> (regs[r2] % TCG_TARGET_REG_BITS);
            break;
        case INDEX_op_sar:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ((tcg_target_long)regs[r1]
                        >> (regs[r2] % TCG_TARGET_REG_BITS));
            break;
        case INDEX_op_tci_rotl32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = rol32(regs[r1], regs[r2] & 31);
            break;
        case INDEX_op_tci_rotr32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ror32(regs[r1], regs[r2] & 31);
            break;
        case INDEX_op_deposit:
            tci_args_rrrbb(insn, &r0, &r1, &r2, &pos, &len);
            regs[r0] = deposit64(regs[r1], pos, len, regs[r2]);
            break;
        case INDEX_op_extract:
            tci_args_rrbb(insn, &r0, &r1, &pos, &len);
            regs[r0] = extract64(regs[r1], pos, len);
            break;
        case INDEX_op_sextract:
            tci_args_rrbb(insn, &r0, &r1, &pos, &len);
            regs[r0] = sextract64(regs[r1], pos, len);
            break;
        case INDEX_op_brcond:
            tci_args_rl(insn, tb_ptr, &r0, &ptr);
            if (regs[r0]) {
                tb_ptr = ptr;
            }
            break;
        case INDEX_op_bswap16:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = bswap16(regs[r1]);
            break;
        case INDEX_op_bswap32:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = bswap32(regs[r1]);
            break;

            /* Load/store operations (64 bit). */

        case INDEX_op_ld32u:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(uint32_t *)ptr;
            break;
        case INDEX_op_ld32s:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(int32_t *)ptr;
            break;
        case INDEX_op_st32:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            *(uint32_t *)ptr = regs[r0];
            break;

            /* Arithmetic operations (64 bit). */

        case INDEX_op_divs:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (int64_t)regs[r1] / (int64_t)regs[r2];
            break;
        case INDEX_op_divu:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (uint64_t)regs[r1] / (uint64_t)regs[r2];
            break;
        case INDEX_op_rems:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (int64_t)regs[r1] % (int64_t)regs[r2];
            break;
        case INDEX_op_remu:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (uint64_t)regs[r1] % (uint64_t)regs[r2];
            break;
        case INDEX_op_clz:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] ? clz64(regs[r1]) : regs[r2];
            break;
        case INDEX_op_ctz:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] ? ctz64(regs[r1]) : regs[r2];
            break;

            /* Shift/rotate operations (64 bit). */

        case INDEX_op_rotl:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = rol64(regs[r1], regs[r2] & 63);
            break;
        case INDEX_op_rotr:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ror64(regs[r1], regs[r2] & 63);
            break;
        case INDEX_op_ext_i32_i64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = (int32_t)regs[r1];
            break;
        case INDEX_op_extu_i32_i64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = (uint32_t)regs[r1];
            break;
        case INDEX_op_bswap64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = bswap64(regs[r1]);
            break;

            /* QEMU specific operations. */

        case INDEX_op_exit_tb:
            tci_args_l(insn, tb_ptr, &ptr);
            return (uintptr_t)ptr;

        case INDEX_op_goto_tb:
            tci_args_l(insn, tb_ptr, &ptr);
            tb_ptr = *(void **)ptr;
            break;

        case INDEX_op_goto_ptr:
            tci_args_r(insn, &r0);
            ptr = (void *)regs[r0];
            if (!ptr) {
                return 0;
            }
            tb_ptr = ptr;
            break;

        case INDEX_op_qemu_ld:
            tci_args_rrm(insn, &r0, &r1, &oi);
            taddr = regs[r1];
            regs[r0] = tci_qemu_ld(env, taddr, oi, tb_ptr);
            break;
        case INDEX_op_tci_qemu_ld_rrr:
            tci_args_rrr(insn, &r0, &r1, &r2);
            taddr = regs[r1];
            oi = regs[r2];
            regs[r0] = tci_qemu_ld(env, taddr, oi, tb_ptr);
            break;

        case INDEX_op_qemu_st:
            tci_args_rrm(insn, &r0, &r1, &oi);
            taddr = regs[r1];
            tci_qemu_st(env, taddr, regs[r0], oi, tb_ptr);
            break;
        case INDEX_op_tci_qemu_st_rrr:
            tci_args_rrr(insn, &r0, &r1, &r2);
            taddr = regs[r1];
            oi = regs[r2];
            tci_qemu_st(env, taddr, regs[r0], oi, tb_ptr);
            break;

        case INDEX_op_mb:
            /* Ensure ordering for all kinds */
            smp_mb();
            break;
        default:
            g_assert_not_reached();
        }
    }
}

/*
 * Disassembler that matches the interpreter
 */

static const char *str_r(TCGReg r)
{
    static const char regs[TCG_TARGET_NB_REGS][4] = {
        "r0", "r1", "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
        "r8", "r9", "r10", "r11", "r12", "r13", "env", "sp"
    };

    QEMU_BUILD_BUG_ON(TCG_AREG0 != TCG_REG_R14);
    QEMU_BUILD_BUG_ON(TCG_REG_CALL_STACK != TCG_REG_R15);

    assert((unsigned)r < TCG_TARGET_NB_REGS);
    return regs[r];
}

static const char *str_c(TCGCond c)
{
    static const char cond[16][8] = {
        [TCG_COND_NEVER] = "never",
        [TCG_COND_ALWAYS] = "always",
        [TCG_COND_EQ] = "eq",
        [TCG_COND_NE] = "ne",
        [TCG_COND_LT] = "lt",
        [TCG_COND_GE] = "ge",
        [TCG_COND_LE] = "le",
        [TCG_COND_GT] = "gt",
        [TCG_COND_LTU] = "ltu",
        [TCG_COND_GEU] = "geu",
        [TCG_COND_LEU] = "leu",
        [TCG_COND_GTU] = "gtu",
        [TCG_COND_TSTEQ] = "tsteq",
        [TCG_COND_TSTNE] = "tstne",
    };

    assert((unsigned)c < ARRAY_SIZE(cond));
    assert(cond[c][0] != 0);
    return cond[c];
}

/* Disassemble TCI bytecode. */
int print_insn_tci(bfd_vma addr, disassemble_info *info)
{
    const uint32_t *tb_ptr = (const void *)(uintptr_t)addr;
    const TCGOpDef *def;
    const char *op_name;
    uint32_t insn;
    TCGOpcode op;
    TCGReg r0, r1, r2, r3, r4;
    tcg_target_ulong i1;
    int32_t s2;
    TCGCond c;
    MemOpIdx oi;
    uint8_t pos, len;
    void *ptr;

    /* TCI is always the host, so we don't need to load indirect. */
    insn = *tb_ptr++;

    info->fprintf_func(info->stream, "%08x  ", insn);

    op = extract32(insn, 0, 8);
    def = &tcg_op_defs[op];
    op_name = def->name;

    switch (op) {
    case INDEX_op_br:
    case INDEX_op_exit_tb:
    case INDEX_op_goto_tb:
        tci_args_l(insn, tb_ptr, &ptr);
        info->fprintf_func(info->stream, "%-12s  %p", op_name, ptr);
        break;

    case INDEX_op_goto_ptr:
        tci_args_r(insn, &r0);
        info->fprintf_func(info->stream, "%-12s  %s", op_name, str_r(r0));
        break;

    case INDEX_op_call:
        tci_args_nl(insn, tb_ptr, &len, &ptr);
        info->fprintf_func(info->stream, "%-12s  %d, %p", op_name, len, ptr);
        break;

    case INDEX_op_brcond:
        tci_args_rl(insn, tb_ptr, &r0, &ptr);
        info->fprintf_func(info->stream, "%-12s  %s, 0, ne, %p",
                           op_name, str_r(r0), ptr);
        break;

    case INDEX_op_setcond:
    case INDEX_op_tci_setcond32:
        tci_args_rrrc(insn, &r0, &r1, &r2, &c);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %s",
                           op_name, str_r(r0), str_r(r1), str_r(r2), str_c(c));
        break;

    case INDEX_op_tci_movi:
        tci_args_ri(insn, &r0, &i1);
        info->fprintf_func(info->stream, "%-12s  %s, 0x%" TCG_PRIlx,
                           op_name, str_r(r0), i1);
        break;

    case INDEX_op_tci_movl:
        tci_args_rl(insn, tb_ptr, &r0, &ptr);
        info->fprintf_func(info->stream, "%-12s  %s, %p",
                           op_name, str_r(r0), ptr);
        break;

    case INDEX_op_tci_setcarry:
        info->fprintf_func(info->stream, "%-12s", op_name);
        break;

    case INDEX_op_ld8u:
    case INDEX_op_ld8s:
    case INDEX_op_ld16u:
    case INDEX_op_ld16s:
    case INDEX_op_ld32u:
    case INDEX_op_ld:
    case INDEX_op_st8:
    case INDEX_op_st16:
    case INDEX_op_st32:
    case INDEX_op_st:
        tci_args_rrs(insn, &r0, &r1, &s2);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %d",
                           op_name, str_r(r0), str_r(r1), s2);
        break;

    case INDEX_op_bswap16:
    case INDEX_op_bswap32:
    case INDEX_op_ctpop:
    case INDEX_op_mov:
    case INDEX_op_neg:
    case INDEX_op_not:
    case INDEX_op_ext_i32_i64:
    case INDEX_op_extu_i32_i64:
    case INDEX_op_bswap64:
        tci_args_rr(insn, &r0, &r1);
        info->fprintf_func(info->stream, "%-12s  %s, %s",
                           op_name, str_r(r0), str_r(r1));
        break;

    case INDEX_op_add:
    case INDEX_op_addci:
    case INDEX_op_addcio:
    case INDEX_op_addco:
    case INDEX_op_and:
    case INDEX_op_andc:
    case INDEX_op_clz:
    case INDEX_op_ctz:
    case INDEX_op_divs:
    case INDEX_op_divu:
    case INDEX_op_eqv:
    case INDEX_op_mul:
    case INDEX_op_nand:
    case INDEX_op_nor:
    case INDEX_op_or:
    case INDEX_op_orc:
    case INDEX_op_rems:
    case INDEX_op_remu:
    case INDEX_op_rotl:
    case INDEX_op_rotr:
    case INDEX_op_sar:
    case INDEX_op_shl:
    case INDEX_op_shr:
    case INDEX_op_sub:
    case INDEX_op_subbi:
    case INDEX_op_subbio:
    case INDEX_op_subbo:
    case INDEX_op_xor:
        tci_args_rrr(insn, &r0, &r1, &r2);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s",
                           op_name, str_r(r0), str_r(r1), str_r(r2));
        break;

    case INDEX_op_tci_ctz32:
    case INDEX_op_tci_clz32:
    case INDEX_op_tci_divs32:
    case INDEX_op_tci_divu32:
    case INDEX_op_tci_rems32:
    case INDEX_op_tci_remu32:
    case INDEX_op_tci_rotl32:
    case INDEX_op_tci_rotr32:
        tci_args_rrr(insn, &r0, &r1, &r2);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s",
                           op_name, str_r(r0), str_r(r1), str_r(r2));
        break;

    case INDEX_op_deposit:
        tci_args_rrrbb(insn, &r0, &r1, &r2, &pos, &len);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %d, %d",
                           op_name, str_r(r0), str_r(r1), str_r(r2), pos, len);
        break;

    case INDEX_op_extract:
    case INDEX_op_sextract:
        tci_args_rrbb(insn, &r0, &r1, &pos, &len);
        info->fprintf_func(info->stream, "%-12s  %s,%s,%d,%d",
                           op_name, str_r(r0), str_r(r1), pos, len);
        break;

    case INDEX_op_tci_movcond32:
    case INDEX_op_movcond:
        tci_args_rrrrrc(insn, &r0, &r1, &r2, &r3, &r4, &c);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %s, %s, %s",
                           op_name, str_r(r0), str_r(r1), str_r(r2),
                           str_r(r3), str_r(r4), str_c(c));
        break;

    case INDEX_op_muls2:
    case INDEX_op_mulu2:
        tci_args_rrrr(insn, &r0, &r1, &r2, &r3);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %s",
                           op_name, str_r(r0), str_r(r1),
                           str_r(r2), str_r(r3));
        break;

    case INDEX_op_qemu_ld:
    case INDEX_op_qemu_st:
        tci_args_rrm(insn, &r0, &r1, &oi);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %x",
                           op_name, str_r(r0), str_r(r1), oi);
        break;

    case INDEX_op_tci_qemu_ld_rrr:
    case INDEX_op_tci_qemu_st_rrr:
        tci_args_rrr(insn, &r0, &r1, &r2);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s",
                           op_name, str_r(r0), str_r(r1), str_r(r2));
        break;

    case INDEX_op_qemu_ld2:
    case INDEX_op_qemu_st2:
        tci_args_rrrr(insn, &r0, &r1, &r2, &r3);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %s",
                           op_name, str_r(r0), str_r(r1),
                           str_r(r2), str_r(r3));
        break;

    case 0:
        /* tcg_out_nop_fill uses zeros */
        if (insn == 0) {
            info->fprintf_func(info->stream, "align");
            break;
        }
        /* fall through */

    default:
        info->fprintf_func(info->stream, "illegal opcode %d", op);
        break;
    }

    return sizeof(insn);
}
