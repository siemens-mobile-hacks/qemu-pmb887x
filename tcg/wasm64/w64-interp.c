/*
 * wasm64 interpreter tier: execution.
 *
 * Runs a TB from the op stream the backend recorded while emitting its
 * wasm (w64-interp.h), so that a TB can run before a module exists for
 * it.  That is the whole point: a batch closes the moment one member
 * must run, which costs ~86 us and buys a module holding only 4.85 TBs;
 * 96 % of the boot's module time is that fixed per-close cost.
 *
 * The exit protocol is the emitted code's, exactly -- the dispatcher
 * cannot tell which ran:
 *   exit_tb    return the argument
 *   goto_tb    treated as never linked, so the exit_tb the frontend
 *              emits right after it runs and the dispatcher performs
 *              the tb_add_jump, as it does on the compiled path's
 *              fall-through
 *   goto_ptr   a plain descriptor pointer is handed over through the
 *              [sp-8] slot; a tagged table index cannot be tail-called
 *              from C, so it unwinds to cpu_exec for a fresh lookup
 */
#include "qemu/osdep.h"
#include "tcg/tcg.h"
#include "tcg/helper-info.h"
#include "exec/helper-head.h.inc"
#include "tcg/tcg-ldst.h"
#include "system/cpu-timers.h"
#include "exec/translation-block.h"
#include "wasm64.h"
#include "w64-interp.h"

/* ------------------------------------------------------------------ */
/* the record store, keyed by tidx (the same index the shared funcref   */
/* table uses; recycled at tb_flush, where the store is dropped)        */
/* ------------------------------------------------------------------ */

static struct w64_irec *w64_irecs;

/*
 * A TB is interpreted for its first W64_INTERP_THRESH entries and only
 * then earns a module.  The throughput is flat from 32 to 512.
 */
#define W64_INTERP_THRESH  64

void w64_irec_put(uint32_t tidx, uint32_t *code, uint32_t n)
{
    struct w64_irec *r;

    if (tidx >= W64_TIDX_N) {
        g_free(code);
        return;
    }
    if (!w64_irecs) {
        w64_irecs = g_malloc0((size_t)W64_TIDX_N * sizeof(*w64_irecs));
    }
    r = &w64_irecs[tidx];
    g_free(r->code);
    r->code = code;
    r->n = n;
}


/* The TB has a module now, so its record is never consulted again. */
void w64_irec_drop(uint32_t tidx)
{
    struct w64_irec *r;

    if (!w64_irecs || tidx >= W64_TIDX_N) {
        return;
    }
    r = &w64_irecs[tidx];
    if (r->code) {
        g_free(r->code);
        r->code = NULL;
        r->n = 0;
    }
}

void w64_irec_flush(void)
{
    uint32_t i;

    if (!w64_irecs) {
        return;
    }
    for (i = 0; i < W64_TIDX_N; i++) {
        g_free(w64_irecs[i].code);
        w64_irecs[i].code = NULL;
        w64_irecs[i].n = 0;
    }
}

static uint32_t w64_interp_run(const struct w64_irec *r, uintptr_t env,
                               uintptr_t sp, uintptr_t tp);

/* the emitted prologue's per-TB-entry accounting, in C (wasm64.h) */
static void w64_interp_acct(uint32_t icount)
{
    uint32_t f = w64_acct_flags;

    if (f & W64_ACCT_TBSTATS) {
        wasm_guest_insns += icount;
    }
    if (f & W64_ACCT_ICOUNT2) {
        uint64_t *ticks = (uint64_t *)(uintptr_t)w64_acct_addr[W64_ACCT_TICKS];
        int64_t *deadline =
            (int64_t *)(uintptr_t)w64_acct_addr[W64_ACCT_DEADLINE];
        uint64_t t = qatomic_read(ticks) + icount;

        qatomic_set(ticks, t);
        if (qatomic_read(deadline) > 0 && (int64_t)t >= qatomic_read(deadline)) {
            w64_icount2_sync_now();
        }
    }
    if ((f & W64_ACCT_LS) && w64_ls_on) {
        w64_lockstep_account(icount);
    }
}

bool w64_interp_try(uint32_t tidx, uint32_t icount, uintptr_t env,
                    uintptr_t sp, uintptr_t tp, uint32_t *res)
{
    struct w64_irec *r;

    if (!w64_irecs || tidx >= W64_TIDX_N) {
        return false;
    }
    r = &w64_irecs[tidx];
    if (!r->code || r->hits >= W64_INTERP_THRESH) {
        return false;
    }
    r->hits++;
    w64_interp_acct(icount);
    *res = w64_interp_run(r, env, sp, tp);
    return true;
}

/* ------------------------------------------------------------------ */
/* helper calls                                                        */
/* ------------------------------------------------------------------ */

/*
 * Signature tag for w64_call_direct():
 *   bits 0-2  argument count
 *   bits 3-4  return class (0 void, 1 u32, 2 u64)
 *   bits 5+   two bits per argument
 * Computed from TCGHelperInfo's typemask -- the same decoding tcg_out_call
 * does to pick the wasm type of every argument.
 */
#define W64_CLS_U32 1
#define W64_CLS_U64 2
#define W64_TAG_UNCLASSIFIED UINT32_MAX

#include "w64-call-direct.c.inc"

uint32_t w64_call_tag(uint32_t typemask, unsigned nargs)
{
    uint32_t tag = nargs;
    unsigned i;

    if (nargs > 5) {
        return W64_TAG_UNCLASSIFIED;
    }
    switch (typemask & 7) {
    case dh_typecode_void:
        break;
    case dh_typecode_i32:
    case dh_typecode_s32:
        tag |= W64_CLS_U32 << 3;
        break;
    case dh_typecode_i64:
    case dh_typecode_s64:
    case dh_typecode_ptr:
        tag |= W64_CLS_U64 << 3;
        break;
    default:
        return W64_TAG_UNCLASSIFIED;
    }
    for (i = 0; i < nargs; i++) {
        switch ((typemask >> ((i + 1) * 3)) & 7) {
        case dh_typecode_i32:
        case dh_typecode_s32:
            tag |= W64_CLS_U32 << (5 + 2 * i);
            break;
        case dh_typecode_i64:
        case dh_typecode_s64:
        case dh_typecode_ptr:
            tag |= W64_CLS_U64 << (5 + 2 * i);
            break;
        default:
            return W64_TAG_UNCLASSIFIED;
        }
    }
    return tag;
}

/* ------------------------------------------------------------------ */
/* guest memory                                                        */
/* ------------------------------------------------------------------ */

static uint64_t w64_iqemu_ld(CPUArchState *env, uint64_t taddr,
                             MemOpIdx oi, uintptr_t ra)
{
    MemOp mop = get_memop(oi);

    switch (mop & MO_SSIZE) {
    case MO_UB:
        return helper_ldub_mmu(env, taddr, oi, ra);
    case MO_SB:
        return (int8_t)helper_ldub_mmu(env, taddr, oi, ra);
    case MO_UW:
        return helper_lduw_mmu(env, taddr, oi, ra);
    case MO_SW:
        return (int16_t)helper_lduw_mmu(env, taddr, oi, ra);
    case MO_UL:
        return helper_ldul_mmu(env, taddr, oi, ra);
    case MO_SL:
        return (int32_t)helper_ldul_mmu(env, taddr, oi, ra);
    case MO_UQ:
        return helper_ldq_mmu(env, taddr, oi, ra);
    default:
        g_assert_not_reached();
    }
}

static void w64_iqemu_st(CPUArchState *env, uint64_t taddr, uint64_t val,
                         MemOpIdx oi, uintptr_t ra)
{
    MemOp mop = get_memop(oi);

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

/* ------------------------------------------------------------------ */
/* the loop                                                            */
/* ------------------------------------------------------------------ */

#define W64_I64(c, i)  ((uint64_t)(c)[i] | ((uint64_t)(c)[(i) + 1] << 32))

static bool w64_icond(TCGCond c, uint64_t a, uint64_t b)
{
    switch (c) {
    case TCG_COND_NEVER:  return false;
    case TCG_COND_ALWAYS: return true;
    case TCG_COND_EQ:     return a == b;
    case TCG_COND_NE:     return a != b;
    case TCG_COND_LT:     return (int64_t)a <  (int64_t)b;
    case TCG_COND_GE:     return (int64_t)a >= (int64_t)b;
    case TCG_COND_LE:     return (int64_t)a <= (int64_t)b;
    case TCG_COND_GT:     return (int64_t)a >  (int64_t)b;
    case TCG_COND_LTU:    return a <  b;
    case TCG_COND_GEU:    return a >= b;
    case TCG_COND_LEU:    return a <= b;
    case TCG_COND_GTU:    return a >  b;
    case TCG_COND_TSTEQ:  return (a & b) == 0;
    case TCG_COND_TSTNE:  return (a & b) != 0;
    default:              g_assert_not_reached();
    }
}

/* 32-bit operands are held zero-extended, so a signed compare has to
 * narrow first: (int64_t)(uint32_t)-1 is not negative. */
static bool w64_icond32(TCGCond c, uint32_t a, uint32_t b)
{
    switch (c) {
    case TCG_COND_LT:     return (int32_t)a <  (int32_t)b;
    case TCG_COND_GE:     return (int32_t)a >= (int32_t)b;
    case TCG_COND_LE:     return (int32_t)a <= (int32_t)b;
    case TCG_COND_GT:     return (int32_t)a >  (int32_t)b;
    default:              return w64_icond(c, a, b);
    }
}

static uint32_t w64_ialu32(unsigned op, uint32_t a, uint32_t b)
{
    switch (op) {
    case WI_ALU_ADD:  return a + b;
    case WI_ALU_SUB:  return a - b;
    case WI_ALU_MUL:  return a * b;
    case WI_ALU_AND:  return a & b;
    case WI_ALU_OR:   return a | b;
    case WI_ALU_XOR:  return a ^ b;
    case WI_ALU_ANDC: return a & ~b;
    case WI_ALU_ORC:  return a | ~b;
    case WI_ALU_EQV:  return ~(a ^ b);
    case WI_ALU_NAND: return ~(a & b);
    case WI_ALU_NOR:  return ~(a | b);
    case WI_ALU_SHL:  return a << (b & 31);
    case WI_ALU_SHR:  return a >> (b & 31);
    case WI_ALU_SAR:  return (int32_t)a >> (b & 31);
    case WI_ALU_ROTL: return rol32(a, b & 31);
    case WI_ALU_ROTR: return ror32(a, b & 31);
    case WI_ALU_DIVS: return (int32_t)a / (int32_t)b;
    case WI_ALU_DIVU: return a / b;
    case WI_ALU_REMS: return (int32_t)a % (int32_t)b;
    case WI_ALU_REMU: return a % b;
    case WI_ALU_CLZ:  return a ? clz32(a) : b;
    case WI_ALU_CTZ:  return a ? ctz32(a) : b;
    case WI_ALU_MULUH: return ((uint64_t)a * b) >> 32;
    case WI_ALU_MULSH: return ((int64_t)(int32_t)a * (int32_t)b) >> 32;
    default:          g_assert_not_reached();
    }
}

static uint64_t w64_ialu64(unsigned op, uint64_t a, uint64_t b)
{
    switch (op) {
    case WI_ALU_ADD:  return a + b;
    case WI_ALU_SUB:  return a - b;
    case WI_ALU_MUL:  return a * b;
    case WI_ALU_AND:  return a & b;
    case WI_ALU_OR:   return a | b;
    case WI_ALU_XOR:  return a ^ b;
    case WI_ALU_ANDC: return a & ~b;
    case WI_ALU_ORC:  return a | ~b;
    case WI_ALU_EQV:  return ~(a ^ b);
    case WI_ALU_NAND: return ~(a & b);
    case WI_ALU_NOR:  return ~(a | b);
    case WI_ALU_SHL:  return a << (b & 63);
    case WI_ALU_SHR:  return a >> (b & 63);
    case WI_ALU_SAR:  return (int64_t)a >> (b & 63);
    case WI_ALU_ROTL: return rol64(a, b & 63);
    case WI_ALU_ROTR: return ror64(a, b & 63);
    case WI_ALU_DIVS: return (int64_t)a / (int64_t)b;
    case WI_ALU_DIVU: return a / b;
    case WI_ALU_REMS: return (int64_t)a % (int64_t)b;
    case WI_ALU_REMU: return a % b;
    case WI_ALU_CLZ:  return a ? clz64(a) : b;
    case WI_ALU_CTZ:  return a ? ctz64(a) : b;
    case WI_ALU_MULUH: {
        uint64_t hi;
        mulu64(&(uint64_t){0}, &hi, a, b);
        return hi;
    }
    case WI_ALU_MULSH: {
        uint64_t hi;
        muls64(&(uint64_t){0}, &hi, a, b);
        return hi;
    }
    default:          g_assert_not_reached();
    }
}

static uint64_t w64_iun(unsigned op, uint64_t a, bool is64)
{
    switch (op) {
    case WI_UN_NEG:     return is64 ? -a : (uint32_t)-(uint32_t)a;
    case WI_UN_NOT:     return is64 ? ~a : (uint32_t)~(uint32_t)a;
    case WI_UN_CTPOP:   return is64 ? ctpop64(a) : ctpop32(a);
    case WI_UN_EXT8S:   return is64 ? (uint64_t)(int8_t)a
                                    : (uint32_t)(int8_t)a;
    case WI_UN_EXT8U:   return (uint8_t)a;
    case WI_UN_EXT16S:  return is64 ? (uint64_t)(int16_t)a
                                    : (uint32_t)(int16_t)a;
    case WI_UN_EXT16U:  return (uint16_t)a;
    case WI_UN_EXT32S:  return (uint64_t)(int32_t)a;
    case WI_UN_EXT32U:  return (uint32_t)a;
    case WI_UN_EXTRL:   return (uint32_t)a;
    case WI_UN_EXTRH:   return (uint32_t)(a >> 32);
    default:            g_assert_not_reached();
    }
}

static uint64_t w64_ibswap(unsigned size, uint64_t a, unsigned flags,
                           bool is64)
{
    uint64_t v;

    switch (size) {
    case 16:
        v = bswap16((uint16_t)a);
        if (flags & TCG_BSWAP_OS) {
            v = is64 ? (uint64_t)(int16_t)v : (uint32_t)(int16_t)v;
        } else if (!(flags & TCG_BSWAP_OZ)) {
            v = (a & ~(uint64_t)0xffff) | v;
        }
        return is64 ? v : (uint32_t)v;
    case 32:
        v = bswap32((uint32_t)a);
        if (flags & TCG_BSWAP_OS) {
            v = (uint64_t)(int32_t)v;
        }
        return is64 ? v : (uint32_t)v;
    default:
        return bswap64(a);
    }
}

static uint32_t w64_interp_run(const struct w64_irec *r, uintptr_t env,
                               uintptr_t sp, uintptr_t tp)
{
    uint64_t regs[TCG_TARGET_NB_REGS];
    const uint32_t *c = r->code;
    uint32_t pc = 0;

    memset(regs, 0, sizeof(regs));
    regs[TCG_AREG0] = env;
    regs[TCG_REG_CALL_STACK] = sp;

    for (;;) {
        uint32_t w = c[pc++];
        unsigned op = w & 0xff;
        unsigned a0 = (w >> 8) & 0xff;
        unsigned a1 = (w >> 16) & 0xff;
        unsigned a2 = (w >> 24) & 0xff;

        switch (op) {
        case WI_ALU32_RRR ... WI_ALU32_RRR + WI_ALU_N - 1:
            regs[a0] = w64_ialu32(op - WI_ALU32_RRR,
                                  (uint32_t)regs[a1], (uint32_t)regs[a2]);
            break;
        case WI_ALU64_RRR ... WI_ALU64_RRR + WI_ALU_N - 1:
            regs[a0] = w64_ialu64(op - WI_ALU64_RRR, regs[a1], regs[a2]);
            break;
        case WI_ALU32_RRI ... WI_ALU32_RRI + WI_ALU_N - 1:
            regs[a0] = w64_ialu32(op - WI_ALU32_RRI,
                                  (uint32_t)regs[a1], c[pc]);
            pc += 2;
            break;
        case WI_ALU64_RRI ... WI_ALU64_RRI + WI_ALU_N - 1:
            regs[a0] = w64_ialu64(op - WI_ALU64_RRI, regs[a1], W64_I64(c, pc));
            pc += 2;
            break;
        case WI_UN32 ... WI_UN32 + WI_UN_N - 1:
            regs[a0] = w64_iun(op - WI_UN32, regs[a1], false);
            break;
        case WI_UN64 ... WI_UN64 + WI_UN_N - 1:
            regs[a0] = w64_iun(op - WI_UN64, regs[a1], true);
            break;

        case WI_MOV32:
            regs[a0] = (uint32_t)regs[a1];
            break;
        case WI_MOV64:
            regs[a0] = regs[a1];
            break;
        case WI_MOVI32:
            regs[a0] = c[pc];
            pc += 1;
            break;
        case WI_MOVI64:
            regs[a0] = W64_I64(c, pc);
            pc += 2;
            break;
        case WI_SUBIR32:
            regs[a0] = (uint32_t)(W64_I64(c, pc) - (uint32_t)regs[a2]);
            pc += 2;
            break;
        case WI_SUBIR64:
            regs[a0] = W64_I64(c, pc) - regs[a2];
            pc += 2;
            break;

        case WI_LD8U ... WI_LD64: {
            void *p = (void *)(uintptr_t)(regs[a1] + (int32_t)c[pc]);
            pc += 1;
            switch (op) {
            case WI_LD8U:    regs[a0] = *(uint8_t *)p; break;
            case WI_LD8S32:  regs[a0] = (uint32_t)(int8_t)*(uint8_t *)p; break;
            case WI_LD8S64:  regs[a0] = (uint64_t)(int8_t)*(uint8_t *)p; break;
            case WI_LD16U:   regs[a0] = lduw_he_p(p); break;
            case WI_LD16S32: regs[a0] = (uint32_t)(int16_t)lduw_he_p(p); break;
            case WI_LD16S64: regs[a0] = (uint64_t)(int16_t)lduw_he_p(p); break;
            case WI_LD32U:   regs[a0] = (uint32_t)ldl_he_p(p); break;
            case WI_LD32S64: regs[a0] = (uint64_t)(int32_t)ldl_he_p(p); break;
            case WI_LD32:    regs[a0] = (uint32_t)ldl_he_p(p); break;
            default:         regs[a0] = ldq_he_p(p); break;
            }
            break;
        }
        case WI_ST8 ... WI_ST64: {
            void *p = (void *)(uintptr_t)(regs[a1] + (int32_t)c[pc]);
            pc += 1;
            switch (op) {
            case WI_ST8:  *(uint8_t *)p = (uint8_t)regs[a0]; break;
            case WI_ST16: stw_he_p(p, (uint16_t)regs[a0]); break;
            case WI_ST32: stl_he_p(p, (uint32_t)regs[a0]); break;
            default:      stq_he_p(p, regs[a0]); break;
            }
            break;
        }
        case WI_STI32:
        case WI_STI64: {
            void *p = (void *)(uintptr_t)(regs[a1] + (int32_t)c[pc]);
            uint64_t v = W64_I64(c, pc + 1);
            pc += 3;
            if (op == WI_STI32) {
                stl_he_p(p, (uint32_t)v);
            } else {
                stq_he_p(p, v);
            }
            break;
        }

        case WI_BSWAP16:
            regs[a0] = w64_ibswap(16, regs[a1], c[pc], a2);
            pc += 1;
            break;
        case WI_BSWAP32:
            regs[a0] = w64_ibswap(32, regs[a1], c[pc], a2);
            pc += 1;
            break;
        case WI_BSWAP64:
            regs[a0] = bswap64(regs[a1]);
            break;

        case WI_SETCOND32:
            regs[a0] = w64_icond32(c[pc], regs[a1], regs[a2]);
            pc += 1;
            break;
        case WI_SETCOND64:
            regs[a0] = w64_icond(c[pc], regs[a1], regs[a2]);
            pc += 1;
            break;
        case WI_SETCONDI32:
            regs[a0] = w64_icond32(c[pc], regs[a1], W64_I64(c, pc + 1));
            pc += 3;
            break;
        case WI_SETCONDI64:
            regs[a0] = w64_icond(c[pc], regs[a1], W64_I64(c, pc + 1));
            pc += 3;
            break;

        case WI_MOVCOND32:
        case WI_MOVCOND64: {
            bool is64 = op == WI_MOVCOND64;
            TCGCond cond = c[pc];
            uint32_t f = c[pc + 1];
            uint32_t q = pc + 2;
            uint64_t c2, vt, vf;

            if (f & 1) {
                c2 = W64_I64(c, q);
                q += 2;
            } else {
                c2 = regs[a2];
            }
            if (f & 2) {
                vt = W64_I64(c, q);
                q += 2;
            } else {
                vt = regs[(f >> 8) & 0xff];
            }
            if (f & 4) {
                vf = W64_I64(c, q);
                q += 2;
            } else {
                vf = regs[(f >> 16) & 0xff];
            }
            pc = q;
            if (is64) {
                regs[a0] = w64_icond(cond, regs[a1], c2) ? vt : vf;
            } else {
                regs[a0] = (uint32_t)(w64_icond32(cond, regs[a1], c2)
                                      ? vt : vf);
            }
            break;
        }

        case WI_BRCOND32:
            if (w64_icond32(c[pc], regs[a1], regs[a2])) {
                pc = c[pc + 1];
            } else {
                pc += 2;
            }
            break;
        case WI_BRCOND64:
            if (w64_icond(c[pc], regs[a1], regs[a2])) {
                pc = c[pc + 1];
            } else {
                pc += 2;
            }
            break;
        case WI_BRCONDI32:
            if (w64_icond32(c[pc], regs[a1], W64_I64(c, pc + 1))) {
                pc = c[pc + 3];
            } else {
                pc += 4;
            }
            break;
        case WI_BRCONDI64:
            if (w64_icond(c[pc], regs[a1], W64_I64(c, pc + 1))) {
                pc = c[pc + 3];
            } else {
                pc += 4;
            }
            break;
        case WI_BR:
            pc = c[pc];
            break;

        case WI_DEPOSIT32:
        case WI_DEPOSIT64: {
            unsigned ofs = c[pc] & 0xff, len = (c[pc] >> 8) & 0xff;
            pc += 1;
            if (op == WI_DEPOSIT32) {
                regs[a0] = deposit32((uint32_t)regs[a1], ofs, len,
                                     (uint32_t)regs[a2]);
            } else {
                regs[a0] = deposit64(regs[a1], ofs, len, regs[a2]);
            }
            break;
        }
        case WI_EXTRACT32 ... WI_SEXTRACT64: {
            unsigned ofs = c[pc] & 0xff, len = (c[pc] >> 8) & 0xff;
            pc += 1;
            switch (op) {
            case WI_EXTRACT32:
                regs[a0] = extract32((uint32_t)regs[a1], ofs, len);
                break;
            case WI_EXTRACT64:
                regs[a0] = extract64(regs[a1], ofs, len);
                break;
            case WI_SEXTRACT32:
                regs[a0] = (uint32_t)sextract32((uint32_t)regs[a1], ofs, len);
                break;
            default:
                regs[a0] = sextract64(regs[a1], ofs, len);
                break;
            }
            break;
        }

        case WI_MULU2_32: {
            uint64_t v = (uint64_t)(uint32_t)regs[a2] * (uint32_t)regs[c[pc]];
            regs[a0] = (uint32_t)v;
            regs[a1] = (uint32_t)(v >> 32);
            pc += 1;
            break;
        }
        case WI_MULS2_32: {
            int64_t v = (int64_t)(int32_t)regs[a2] * (int32_t)regs[c[pc]];
            regs[a0] = (uint32_t)v;
            regs[a1] = (uint32_t)((uint64_t)v >> 32);
            pc += 1;
            break;
        }
        case WI_MULU2_64:
            mulu64(&regs[a0], &regs[a1], regs[a2], regs[c[pc]]);
            pc += 1;
            break;
        case WI_MULS2_64:
            muls64(&regs[a0], &regs[a1], regs[a2], regs[c[pc]]);
            pc += 1;
            break;

        case WI_QEMU_LD32:
        case WI_QEMU_LD64: {
            MemOpIdx oi = c[pc];
            uintptr_t ra = (uintptr_t)W64_I64(c, pc + 1);
            uint64_t v;
            pc += 3;
            *(uintptr_t *)tp = ra;
            v = w64_iqemu_ld((CPUArchState *)env, regs[a1], oi, ra);
            regs[a0] = op == WI_QEMU_LD32 ? (uint32_t)v : v;
            break;
        }
        case WI_QEMU_ST32:
        case WI_QEMU_ST64: {
            MemOpIdx oi = c[pc];
            uintptr_t ra = (uintptr_t)W64_I64(c, pc + 1);
            pc += 3;
            *(uintptr_t *)tp = ra;
            w64_iqemu_st((CPUArchState *)env, regs[a1],
                         op == WI_QEMU_ST32 ? (uint32_t)regs[a0] : regs[a0],
                         oi, ra);
            break;
        }

        case WI_CALL: {
            uint32_t tag = c[pc];
            void *fn = (void *)(uintptr_t)W64_I64(c, pc + 1);
            uintptr_t ra = (uintptr_t)W64_I64(c, pc + 3);
            uint64_t *stack = (uint64_t *)sp;
            pc += 5;
            *(uintptr_t *)tp = ra;
            if (!w64_call_direct(fn, tag, stack)) {
                g_assert_not_reached();
            }
            /* the result lands in stack[0]; the emitted code leaves it
             * in R0 and nothing else reads the slot afterwards */
            if (((tag >> 3) & 3) == W64_CLS_U32) {
                regs[TCG_REG_R0] = w64_slot_u32(stack, 0);
            } else if (((tag >> 3) & 3) == W64_CLS_U64) {
                regs[TCG_REG_R0] = stack[0];
            }
            break;
        }

        case WI_GOTO_TB:
            /* never linked: fall through to the frontend's exit_tb */
            break;
        case WI_EXIT_TB:
            return c[pc];
        case WI_GOTO_PTR: {
            uint64_t v = regs[a0];
            /* a tagged table index cannot be tail-called from C */
            *(uintptr_t *)(sp - 8) = (v >> 32) ? 0 : (uintptr_t)v;
            return W64_EXIT_GOTOPTR;
        }
        case WI_MB:
            smp_mb();
            break;

        case WI_OP_END:
        default:
            g_assert_not_reached();
        }
    }
}
