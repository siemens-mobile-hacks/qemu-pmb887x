/*
 * Tiny Code Generator for QEMU — wasm64 target emitters.
 *
 * Emits raw wasm bytecode (one module per TB) into the code generation
 * buffer; see tcg/wasm64/tcg-target.h for the design overview and
 * tcg/wasm64/wasm64.h for the descriptor layout.
 *
 * Phase 1 skeleton (doc/wasm-tcg-backend-plan.md §5): single-TB
 * modules, no chaining, helper calls through imported funcrefs.
 * Phase 2: goto_tb chaining through the shared funcref table + batched
 * modules.  Phase 3: qemu_ld/st emit the TLB probe + size-specialized
 * access inline (see the qemu_ld/st section below); W64_NOTLB=1 falls
 * back to the phase-1 helper-only path.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/wasm-diag.h"
#include "wasm64.h"
#include "w64-interp.h"
#include "system/cpu-timers.h"      /* icount2_enabled() */
#include "exec/icount.h"            /* icount_enabled() */

/* ------------------------------------------------------------------ */
/* per-TB assembly state                                              */
/* ------------------------------------------------------------------ */

#define W64_MAX_FIXUPS  1024
#define W64_MAX_BLK     128     /* nested mode: one block per label */
#define W64_MAX_LABELS  1024

/* w64_type / w64_import / table caps come from wasm64.h (the batch
 * union tables in wasm64.c use them too) */

struct w64_fixup {          /* forward branch to an unplaced label */
    uint32_t pos;           /* byte offset of the 5-byte padded i32.const */
    uint32_t label;
};

static __thread struct {
    uint16_t label_idx[W64_MAX_LABELS];  /* 0 = unplaced, else 1..n */
    uint16_t n_labels;
    bool overflow;          /* a fixed-size emitter table filled up: fail
                             * the translation (tcg_out_tb_finalize -> -2,
                             * tb_gen_code retries with half the insns) */
    struct w64_fixup fixup[W64_MAX_FIXUPS];
    int n_fixup;

    struct w64_type type[W64_MAX_TYPES];
    uint8_t n_types;
    struct w64_import imp[W64_MAX_IMPORTS];
    uint8_t n_imp;

    /* structured-control-flow block stack (for br depth arithmetic) */
    struct { uint8_t kind; int label; } blk[W64_MAX_BLK];
    int n_blk;
    int loop_idx;           /* blk[] index of the top-level label loop */
    /* nested mode: every TCG branch in this TB is forward (the ARM
     * frontend never emits a backward one), so the labels become nested
     * blocks opened at TB start in set order (last label outermost) and
     * a branch is a plain br / br_if — no dispatch loop, no $bp, no
     * per-label compare, no loop phis for V8's register allocator.  A
     * backward branch falls back to the loop + sibling-if scheme --
     * unless it is the one target/arm does emit, a guest loop's
     * back-edge to the TB's own first instruction.  That target is
     * always the first label placed, so nested mode keeps its blocks:
     * a wasm loop opens where label 1 sits -- past gen_tb_start's
     * icount check, which the back-edge must not re-run -- and the
     * labels after it nest inside.  Only the last label, the one the
     * check itself branches to, stays outside. */
    bool nested;
    bool selfloop;

    /* register representation: bit r set = value in the i64 local */
    uint32_t rep;
} W;

enum {
    W64_BLK_REGION,         /* if (bp <= k) label region            */
    W64_BLK_IF,             /* transient emitter if                 */
    W64_BLK_LOOP,           /* the top-level label dispatch loop    */
    W64_BLK_LABEL,          /* nested mode: block closed at its label */
};

/* ------------------------------------------------------------------ */
/* raw byte emission                                                   */
/* ------------------------------------------------------------------ */

static void w64_u8(TCGContext *s, uint8_t v)
{
    *s->code_ptr++ = v;
}

static void w64_bytes(TCGContext *s, const void *p, unsigned n)
{
    memcpy(s->code_ptr, p, n);
    s->code_ptr += n;
}

static void w64_uleb(TCGContext *s, uint64_t v)
{
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        w64_u8(s, v ? b | 0x80 : b);
    } while (v);
}

/* unsigned LEB padded to 5 bytes (patchable; value must fit u32) */
static void w64_uleb32_p5(TCGContext *s, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++) {
        w64_u8(s, (v & 0x7f) | 0x80);
        v >>= 7;
    }
    w64_u8(s, v & 0x7f);
}

static void w64_sleb32(TCGContext *s, int32_t v)
{
    bool more = true;
    while (more) {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) {
            more = false;
        } else {
            b |= 0x80;
        }
        w64_u8(s, b);
    }
}

static void w64_sleb64(TCGContext *s, int64_t v)
{
    bool more = true;
    while (more) {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) {
            more = false;
        } else {
            b |= 0x80;
        }
        w64_u8(s, b);
    }
}

/* ------------------------------------------------------------------ */
/* helper-call emission (per-TB import indices + batch fixups)        */
/* ------------------------------------------------------------------ */

/* `call <funcidx>`: the index LEB is padded to 2 bytes so the batch
 * assembler (wasm64.c) can rewrite it in place — per-TB temp-module
 * index -> batch-union index — without shifting any byte that follows.
 * w64_callidx records the site (position + union index) in CF; the
 * list is reset at every tcg_out_tb_start, so aborted overflow
 * retries cannot leak entries into the batch arena — only finalized
 * TBs contribute (w64_batch_member copies CF under the member's
 * fixed range). */
static __thread struct w64_cfix *CF;
static __thread uint32_t n_cf, cap_cf;

static void w64_callidx(TCGContext *s, uint8_t imp, uint8_t uimp)
{
    w64_u8(s, (uint8_t)((imp & 0x7f) | 0x80));
    w64_u8(s, (uint8_t)(imp >> 7));
    if (n_cf == cap_cf) {
        cap_cf = cap_cf ? cap_cf * 2 : 32;
        CF = g_realloc(CF, (size_t)cap_cf * sizeof(*CF));
    }
    CF[n_cf].pos = s->code_ptr - 2 - s->code_buf;
    CF[n_cf].uimp = uimp;
    n_cf++;
}

/* ------------------------------------------------------------------ */
/* locals and register access                                          */
/* ------------------------------------------------------------------ */

/* params: 0 = $env, 1 = $sp, 2 = $tp; then locals:
 *   3 .. 3+NB-1                 i32 reg locals (+ $bp at 3+NB)
 *   3+NB+1 .. 3+NB+NB           i64 reg locals
 *   3+2*NB+1                    i32 scratch ($scr32)
 *   3+2*NB+2..+4                three i64 scratch locals              */
#define W64_P_ENV     0
#define W64_P_SP      1
#define W64_P_TP      2
#define W64_L32(r)    (3 + (r))
#define W64_L64(r)    (3 + TCG_TARGET_NB_REGS + 1 + (r))
#define W64_L_BP      (3 + TCG_TARGET_NB_REGS)
#define W64_L_SCR32   (3 + 2 * TCG_TARGET_NB_REGS + 1)
#define W64_L_SCR0    (W64_L_SCR32 + 1)
#define W64_L_SCR1    (W64_L_SCR0 + 1)
#define W64_L_SCR2    (W64_L_SCR1 + 1)   /* ld/st addr snapshot */

/* type-matched scratch local */
static int w64_scr(TCGType t)
{
    return t == TCG_TYPE_I32 ? W64_L_SCR32 : W64_L_SCR0;
}

/* Translation-time knobs, fixed for the life of the process.
 *
 * The per-TB-entry statistics (wasm_tb_stats: TB entries and guest
 * insns, read by the page and the benchmark tooling through
 * wasm_tbs/wasm_insns) cost two read-modify-writes at every TB entry
 * (~5 M/s, ~50 of the ~560 bytes of an average TB module).  Under
 * icount the guest instruction count is already exact in the icount
 * state and wasm_insns() derives it there, so the prologue maintains
 * the counters only on non-icount boards (the LG ones) or when
 * W64_TBSTATS=1 asks for the TB-entry count too. */
static bool w64_tbstats_inline(void)
{
    static int mode = -1;
    if (mode < 0) {
        mode = !icount_enabled() || getenv("W64_TBSTATS") != NULL;
    }
    return mode != 0;
}

/* The lockstep fold is armed per process (W64_LOCKSTEP, set by the page
 * before qemu starts).  An unarmed process omits the w64_ls_on test and
 * import from every prologue and the w64_chain_stop term from every
 * goto_tb/goto_ptr; nothing else ever sets w64_chain_stop. */
/* W64_LOCALPAD=N declares N extra, never-referenced i64 locals in every
 * TB function.  Measurement knob only: wasm requires locals to be zero at
 * entry and the baseline tier has no liveness analysis, so it zeroes every
 * declared local on every call — tools/locals-probe.mjs prices the
 * backend's own ~70 at +38.9 ns on a 49.9 ns baseline call, and this
 * reads the same slope in the guest, where only some fraction of the
 * dispatches are still running baseline code. */
static unsigned w64_locals_pad(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("W64_LOCALPAD");
        n = e ? atoi(e) : 0;
    }
    return n;
}

/* W64_BYTEPAD=N adds N never-executed i64 add/store pairs (~7 bytes each)
 * to every TB body, behind a test of $scr32 which is still 0 here.  The
 * companion knob to W64_LOCALPAD: it prices *emitted bytes* against
 * module compile time (modNs), which is what decides whether shrinking
 * the inline TLB probe -- 37 % of emitted bytes -- is worth building.
 * The execution cost it adds is one load and one not-taken branch per TB
 * entry, the same at every N, so modNs is the response variable. */
static unsigned w64_byte_pad(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("W64_BYTEPAD");
        n = e ? atoi(e) : 0;
    }
    return n;
}

/* W64_CALLPAD=N adds N calls to w64_callpad_sink() per TB entry.  Its
 * companion knobs price bytes (W64_BYTEPAD) and locals (W64_LOCALPAD);
 * this one prices the wasm->wasm import call itself, which is what a
 * helper costs a TB before the helper does anything.  padSink is the
 * exact call count, so the A/B divides straight into ns per call. */
static unsigned w64_call_pad(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("W64_CALLPAD");
        n = e ? atoi(e) : 0;
    }
    return n;
}

static bool w64_ls_armed(void)
{
    static int mode = -1;
    if (mode < 0) {
        mode = getenv("W64_LOCKSTEP") != NULL;
    }
    return mode != 0;
}

static int w64_local32(TCGReg r)
{
    if (r == TCG_AREG0 || r == TCG_REG_CALL_STACK) {
        /* env/frame are only ever accessed as i64 */
        g_assert_not_reached();
    }
    return W64_L32(r);
}

static int w64_local64(TCGReg r)
{
    if (r == TCG_AREG0) {
        return W64_P_ENV;
    }
    if (r == TCG_REG_CALL_STACK) {
        return W64_P_SP;
    }
    return W64_L64(r);
}

static bool w64_rep_is64(TCGReg r)
{
    return r == TCG_AREG0 || r == TCG_REG_CALL_STACK || (W.rep >> r) & 1;
}

static void w64_local_get(TCGContext *s, int l)
{
    w64_u8(s, 0x20);
    w64_uleb(s, l);
}

static void w64_local_set(TCGContext *s, int l)
{
    w64_u8(s, 0x21);
    w64_uleb(s, l);
}

/* local.set + local.get in one op: the value stays on the value stack. */
static void w64_local_tee(TCGContext *s, int l)
{
    w64_u8(s, 0x22);
    w64_uleb(s, l);
}

/* Push TCG reg r as an i32 value (wrapping if held in the i64 local). */
static void w64_get_i32(TCGContext *s, TCGReg r)
{
    if (w64_rep_is64(r)) {
        w64_local_get(s, w64_local64(r));
        w64_u8(s, 0xa7);                    /* i32.wrap_i64 */
    } else {
        w64_local_get(s, w64_local32(r));
    }
}

/* Push TCG reg r as an i64 value (zero-extending an i32 rep). */
static void w64_get_i64(TCGContext *s, TCGReg r)
{
    if (w64_rep_is64(r)) {
        w64_local_get(s, w64_local64(r));
    } else {
        w64_local_get(s, w64_local32(r));
        w64_u8(s, 0xad);                    /* i64.extend_i32_u */
    }
}

static void w64_get(TCGContext *s, TCGReg r, TCGType t)
{
    if (t == TCG_TYPE_I32) {
        w64_get_i32(s, r);
    } else {
        w64_get_i64(s, r);
    }
}

/* Pop a value into reg r, marking its representation. */
static void w64_set_i32(TCGContext *s, TCGReg r)
{
    w64_local_set(s, w64_local32(r));
    W.rep &= ~(1u << r);
}

static void w64_set_i64(TCGContext *s, TCGReg r)
{
    w64_local_set(s, w64_local64(r));
    W.rep |= 1u << r;
}

static void w64_const_i32(TCGContext *s, int32_t v)
{
    w64_u8(s, 0x41);
    w64_sleb32(s, v);
}

static void w64_const_i64(TCGContext *s, int64_t v)
{
    w64_u8(s, 0x42);
    w64_sleb64(s, v);
}

static void w64_const(TCGContext *s, TCGType t, tcg_target_long v)
{
    if (t == TCG_TYPE_I32) {
        w64_const_i32(s, (int32_t)v);
    } else {
        w64_const_i64(s, v);
    }
}

/* ------------------------------------------------------------------ */
/* structured control flow                                             */
/* ------------------------------------------------------------------ */

static void w64_blk_push(TCGContext *s, uint8_t kind, int label)
{
    if (W.n_blk >= W64_MAX_BLK) {
        W.overflow = true;
        return;
    }
    w64_u8(s, kind == W64_BLK_LOOP ? 0x03 :
              kind == W64_BLK_LABEL ? 0x02 : 0x04);
    w64_u8(s, 0x40);                        /* void */
    W.blk[W.n_blk].kind = kind;
    W.blk[W.n_blk].label = label;
    W.n_blk++;
}

static void w64_blk_pop(TCGContext *s)
{
    tcg_debug_assert(W.n_blk > 0);
    W.n_blk--;
    w64_u8(s, 0x0b);                        /* end */
}

/* branch to the top-level label loop (from anywhere) */
static void w64_br_loop(TCGContext *s)
{
    w64_u8(s, 0x0c);                        /* br */
    w64_uleb(s, W.n_blk - 1 - W.loop_idx);
}

/* ------------------------------------------------------------------ */
/* types and imports                                                   */
/* ------------------------------------------------------------------ */

static uint8_t w64_add_type(unsigned np, const uint8_t *p, uint8_t ret)
{
    int i, j;
    for (i = 0; i < W.n_types; i++) {
        if (W.type[i].np != np || W.type[i].ret != ret) {
            continue;
        }
        for (j = 0; j < (int)np; j++) {
            if (W.type[i].p[j] != p[j]) {
                break;
            }
        }
        if (j == (int)np) {
            return i;
        }
    }
    tcg_debug_assert(W.n_types < W64_MAX_TYPES);
    if (W.n_types >= W64_MAX_TYPES) {
        W.overflow = true;
        return 0;
    }
    W.type[W.n_types].np = np;
    memcpy(W.type[W.n_types].p, p, np);
    W.type[W.n_types].ret = ret;
    return W.n_types++;
}

static uint8_t w64_import_idx(uint32_t fptr, uint8_t type)
{
    int i;
    for (i = 0; i < W.n_imp; i++) {
        if (W.imp[i].fptr == fptr && W.imp[i].type == type) {
            return i;
        }
    }
    tcg_debug_assert(W.n_imp < W64_MAX_IMPORTS);
    if (W.n_imp >= W64_MAX_IMPORTS) {
        W.overflow = true;
        return 0;
    }
    W.imp[W.n_imp].fptr = fptr;
    W.imp[W.n_imp].type = type;
    return W.n_imp++;
}

/* ------------------------------------------------------------------ */
/* module frame (emitted around the body at fixed offsets)             */
/* ------------------------------------------------------------------ */


/* number the labels in set order and decide the branch scheme */
static void w64_scan_labels(TCGContext *s)
{
    TCGOp *op;
    int prologue_target = -1;   /* label branched to before any is placed */

    W.nested = true;
    W.selfloop = false;
    QTAILQ_FOREACH(op, &s->ops, link) {
        TCGLabel *l;

        switch (op->opc) {
        case INDEX_op_set_label:
            l = arg_label(op->args[0]);
            if (l->id >= W64_MAX_LABELS || W.n_labels == 0xffff) {
                W.overflow = true;
                return;
            }
            W.label_idx[l->id] = ++W.n_labels;
            break;
        case INDEX_op_br:
            l = arg_label(op->args[0]);
            goto check;
        case INDEX_op_brcond:
            l = arg_label(op->args[3]);
        check:
            if (W.n_labels == 0) {
                if (prologue_target >= 0 && prologue_target != l->id) {
                    prologue_target = W64_MAX_LABELS;
                } else {
                    prologue_target = l->id;
                }
            }
            if (l->id < W64_MAX_LABELS && W.label_idx[l->id] != 0) {
                if (W.label_idx[l->id] == 1) {
                    W.selfloop = true;      /* back-edge to the TB's top */
                } else {
                    W.nested = false;       /* any other backward branch */
                }
            }
            break;
        default:
            break;
        }
    }
    /*
     * The guest loop starts where label 1 is placed, so everything before
     * it -- gen_tb_start's icount check -- stays outside, and the only
     * label whose block can still wrap it is the one placed last.
     */
    if (W.selfloop &&
        (W.n_labels < 2 || (prologue_target >= 0 &&
                            (prologue_target >= W64_MAX_LABELS ||
                             W.label_idx[prologue_target] != W.n_labels)))) {
        W.nested = false;
    }
    if (W.nested && W.n_labels + 9 > W64_MAX_BLK) {
        W.overflow = true;                  /* retry with fewer insns */
    }
    if (!W.nested) {
        W.selfloop = false;
        /* the loop scheme numbers labels as they are placed */
        memset(W.label_idx, 0,
               MIN((unsigned)s->nb_labels, W64_MAX_LABELS) *
               sizeof(W.label_idx[0]));
        W.n_labels = 0;
    }
}

static void w64_emit_loop_head(TCGContext *s)
{
    int k;

    if (W.nested) {
        if (W.selfloop) {
            /* only the last label wraps the prologue; the rest open
             * inside the guest loop, at label 1 (tcg_out_set_label) */
            w64_blk_push(s, W64_BLK_LABEL, W.n_labels);
            return;
        }
        for (k = W.n_labels; k >= 1; k--) {
            w64_blk_push(s, W64_BLK_LABEL, k);
        }
        return;
    }

    /* loop { if (u32.le_u (local.get $bp) (i32.const 0)) { <region 0> */
    w64_u8(s, 0x03);                        /* loop void */
    w64_u8(s, 0x40);
    tcg_debug_assert(W.n_blk == 0);
    W.blk[W.n_blk].kind = W64_BLK_LOOP;
    W.blk[W.n_blk].label = -1;
    W.n_blk++;
    W.loop_idx = 0;

    w64_local_get(s, W64_L_BP);
    w64_const_i32(s, 0);
    w64_u8(s, 0x4d);                        /* i32.le_u */
    w64_blk_push(s, W64_BLK_REGION, 0);
}

/* ------------------------------------------------------------------ */
/* interpreter-tier recorder (w64-interp.h)                            */
/*                                                                     */
/* Every emitter below also appends to a per-TB op stream, from the    */
/* same already-register-allocated operands, so the recorded program   */
/* and the emitted wasm are the same program by construction.  An      */
/* emitter that cannot be encoded calls w64_ir_bad(): that TB is then  */
/* never interpreted and takes today's path, so correctness never      */
/* depends on this being complete.                                     */
/* ------------------------------------------------------------------ */

#define W64_IREC_MAX   (1 << 14)        /* words; a TB overflowing is bad */
#define W64_IREC_LBL   512
#define W64_IREC_FIX   1024

static struct {
    uint32_t code[W64_IREC_MAX];
    uint32_t n;
    bool bad;
    bool on;
    uint32_t lbl[W64_IREC_LBL];         /* label id -> word index */
    uint32_t fix_at[W64_IREC_FIX];      /* word holding a target to patch */
    uint16_t fix_lbl[W64_IREC_FIX];
    uint32_t n_fix;
} IR;

static void w64_ir_bad(void)
{
    IR.bad = true;
}

static void w64_ir_reset(unsigned nlabels)
{
    IR.n = 0;
    IR.n_fix = 0;
    IR.bad = false;
    IR.on = w64_interp_on();
    if (IR.on) {
        /* only ids below nb_labels are ever set or resolved */
        memset(IR.lbl, 0xff,
               MIN(nlabels, W64_IREC_LBL) * sizeof(IR.lbl[0]));
    }
}

static void w64_ir_w(uint32_t w)
{
    if (IR.n >= W64_IREC_MAX) {
        IR.bad = true;
        return;
    }
    IR.code[IR.n++] = w;
}

static void w64_ir_w64(uint64_t v)
{
    w64_ir_w((uint32_t)v);
    w64_ir_w((uint32_t)(v >> 32));
}

static void w64_ir_op(unsigned op, unsigned a0, unsigned a1, unsigned a2)
{
    if (!IR.on) {
        return;
    }
    w64_ir_w(WI_W0(op, a0, a1, a2));
}

/* record the word index this label resolves to */
static void w64_ir_set_label(const TCGLabel *l)
{
    if (!IR.on) {
        return;
    }
    if (l->id >= W64_IREC_LBL) {
        IR.bad = true;
        return;
    }
    IR.lbl[l->id] = IR.n;
}

/* emit a placeholder target word and note it for w64_ir_finish */
static void w64_ir_target(const TCGLabel *l)
{
    if (!IR.on) {
        return;
    }
    if (l->id >= W64_IREC_LBL || IR.n_fix >= W64_IREC_FIX) {
        IR.bad = true;
        w64_ir_w(0);
        return;
    }
    IR.fix_at[IR.n_fix] = IR.n;
    IR.fix_lbl[IR.n_fix] = l->id;
    IR.n_fix++;
    w64_ir_w(0);
}

static void w64_ir_imm32(uint32_t v)
{
    if (IR.on) {
        w64_ir_w(v);
    }
}

static void w64_ir_imm64(uint64_t v)
{
    if (IR.on) {
        w64_ir_w64(v);
    }
}

static void w64_ir_ldst(unsigned op, TCGReg v, TCGReg b, intptr_t o)
{
    if (!IR.on) {
        return;
    }
    if (o != (intptr_t)(int32_t)o) {
        IR.bad = true;
        return;
    }
    w64_ir_w(WI_W0(op, v, b, 0));
    w64_ir_w((uint32_t)(int32_t)o);
}

static void w64_ir_sti(TCGType t, tcg_target_long v, TCGReg b, intptr_t o)
{
    if (!IR.on) {
        return;
    }
    if (o != (intptr_t)(int32_t)o) {
        IR.bad = true;
        return;
    }
    w64_ir_w(WI_W0(t == TCG_TYPE_I32 ? WI_STI32 : WI_STI64, 0, b, 0));
    w64_ir_w((uint32_t)(int32_t)o);
    w64_ir_w64((uint64_t)(int64_t)v);
}

static void w64_ir_movi(TCGType t, TCGReg r0, tcg_target_long v)
{
    if (!IR.on) {
        return;
    }
    if (t == TCG_TYPE_I32) {
        w64_ir_w(WI_W0(WI_MOVI32, r0, 0, 0));
        w64_ir_w((uint32_t)v);
    } else {
        w64_ir_w(WI_W0(WI_MOVI64, r0, 0, 0));
        w64_ir_w64((uint64_t)(int64_t)v);
    }
}

static void w64_ir_un(TCGType t, unsigned un, TCGReg r0, TCGReg r1)
{
    w64_ir_op((t == TCG_TYPE_I32 ? WI_UN32 : WI_UN64) + un, r0, r1, 0);
}

static void w64_ir_alu(TCGType t, unsigned alu, TCGReg r0, TCGReg r1,
                       TCGReg r2)
{
    w64_ir_op((t == TCG_TYPE_I32 ? WI_ALU32_RRR : WI_ALU64_RRR) + alu,
              r0, r1, r2);
}

static void w64_ir_alui(TCGType t, unsigned alu, TCGReg r0, TCGReg r1,
                        tcg_target_long i2)
{
    w64_ir_op((t == TCG_TYPE_I32 ? WI_ALU32_RRI : WI_ALU64_RRI) + alu,
              r0, r1, 0);
    w64_ir_imm64((uint64_t)(int64_t)i2);
}

static void w64_ir_subir(TCGType t, TCGReg r0, tcg_target_long i1, TCGReg r2)
{
    w64_ir_op(t == TCG_TYPE_I32 ? WI_SUBIR32 : WI_SUBIR64, r0, 0, r2);
    w64_ir_imm64((uint64_t)(int64_t)i1);
}

static void w64_ir_setcond(TCGType t, TCGCond c, TCGReg r0, TCGReg r1,
                           TCGReg r2)
{
    w64_ir_op(t == TCG_TYPE_I32 ? WI_SETCOND32 : WI_SETCOND64, r0, r1, r2);
    w64_ir_imm32(c);
}

static void w64_ir_setcondi(TCGType t, TCGCond c, TCGReg r0, TCGReg r1,
                            tcg_target_long i2)
{
    w64_ir_op(t == TCG_TYPE_I32 ? WI_SETCONDI32 : WI_SETCONDI64, r0, r1, 0);
    w64_ir_imm32(c);
    w64_ir_imm64((uint64_t)(int64_t)i2);
}

static void w64_ir_brcond(TCGType t, TCGCond c, TCGReg r1, TCGReg r2,
                          const TCGLabel *l)
{
    w64_ir_op(t == TCG_TYPE_I32 ? WI_BRCOND32 : WI_BRCOND64, 0, r1, r2);
    w64_ir_imm32(c);
    w64_ir_target(l);
}

static void w64_ir_brcondi(TCGType t, TCGCond c, TCGReg r1,
                           tcg_target_long i2, const TCGLabel *l)
{
    w64_ir_op(t == TCG_TYPE_I32 ? WI_BRCONDI32 : WI_BRCONDI64, 0, r1, 0);
    w64_ir_imm32(c);
    w64_ir_imm64((uint64_t)(int64_t)i2);
    w64_ir_target(l);
}

static void w64_ir_movcond(TCGType t, TCGCond c, TCGReg ret, TCGReg c1,
                           TCGArg c2, bool const_c2, TCGArg vt, bool const_vt,
                           TCGArg vf, bool const_vf)
{
    unsigned flags = (const_c2 ? 1 : 0) | (const_vt ? 2 : 0) |
                     (const_vf ? 4 : 0);

    w64_ir_op(t == TCG_TYPE_I32 ? WI_MOVCOND32 : WI_MOVCOND64,
              ret, c1, const_c2 ? 0 : (unsigned)c2);
    w64_ir_imm32(c);
    w64_ir_imm32(flags | ((const_vt ? 0u : (unsigned)vt) << 8) |
                 ((const_vf ? 0u : (unsigned)vf) << 16));
    if (const_c2) {
        w64_ir_imm64((uint64_t)(int64_t)c2);
    }
    if (const_vt) {
        w64_ir_imm64((uint64_t)(int64_t)vt);
    }
    if (const_vf) {
        w64_ir_imm64((uint64_t)(int64_t)vf);
    }
}

static void w64_ir_mul2(unsigned op, TCGReg a0, TCGReg a1, TCGReg a2,
                        TCGReg a3)
{
    w64_ir_op(op, a0, a1, a2);
    w64_ir_imm32(a3);
}

static void w64_ir_field(unsigned op, TCGReg a0, TCGReg a1, TCGReg a2,
                         unsigned ofs, unsigned len)
{
    w64_ir_op(op, a0, a1, a2);
    w64_ir_imm32(ofs | (len << 8));
}

static void w64_ir_memop(unsigned op, TCGReg data, TCGReg addr, MemOpIdx oi,
                         uintptr_t ra)
{
    w64_ir_op(op, data, addr, 0);
    w64_ir_imm32(oi);
    w64_ir_imm64(ra);
}

static void w64_ir_call(const void *target, const TCGHelperInfo *info,
                        uintptr_t ra)
{
    if (!IR.on) {
        return;
    }
    if (info->out_kind != TCG_CALL_RET_NORMAL) {
        IR.bad = true;
        return;
    }
    {
        uint32_t tag = w64_call_tag(info->typemask, info->nr_in);
        if (tag == UINT32_MAX) {
            w64_ir_bad();
            return;
        }
        w64_ir_w(WI_W0(WI_CALL, 0, 0, 0));
        w64_ir_w(tag);
    }
    w64_ir_w64((uint64_t)(uintptr_t)target);
    w64_ir_w64(ra);
}

static void w64_ir_finish(uint32_t tidx)
{
    uint32_t *code;
    uint32_t i;

    if (!IR.on || IR.bad || IR.n == 0) {
        return;
    }
    for (i = 0; i < IR.n_fix; i++) {
        uint32_t t = IR.lbl[IR.fix_lbl[i]];
        if (t == 0xffffffffu) {
            return;                     /* label never placed: not ours */
        }
        IR.code[IR.fix_at[i]] = t;
    }
    code = g_malloc((size_t)IR.n * sizeof(uint32_t));
    memcpy(code, IR.code, (size_t)IR.n * sizeof(uint32_t));
    w64_irec_put(tidx, code, IR.n);
}

static void tcg_out_set_label(TCGContext *s, TCGLabel *l)
{
    int i;

    w64_ir_set_label(l);

    if (W.nested) {
        uint16_t idx = l->id < W64_MAX_LABELS ? W.label_idx[l->id] : 0;

        if (W.selfloop && idx == 1) {
            /*
             * The guest loop starts here, just past the prologue; label 1
             * gets no block of its own -- a back-edge branches to the loop
             * -- and every other label nests inside it.
             */
            if (W.n_blk != 1) {
                W.overflow = true;
                return;
            }
            w64_blk_push(s, W64_BLK_LOOP, -1);
            W.loop_idx = W.n_blk - 1;
            for (i = W.n_labels - 1; i >= 2; i--) {
                w64_blk_push(s, W64_BLK_LABEL, i);
            }
            return;
        }
        if (W.selfloop && idx == W.n_labels) {
            if (W.n_blk != 2 || W.blk[1].kind != W64_BLK_LOOP) {
                W.overflow = true;
                return;
            }
            w64_blk_pop(s);                 /* end: the guest loop */
        }
        /* the innermost open block is this label's */
        if (W.n_blk == 0 || W.blk[W.n_blk - 1].kind != W64_BLK_LABEL ||
            idx == 0 || W.blk[W.n_blk - 1].label != idx) {
            W.overflow = true;
            return;
        }
        w64_blk_pop(s);
        return;
    }

    if (l->id >= W64_MAX_LABELS || W.n_labels == 0xffff) {
        W.overflow = true;
        return;
    }
    W.label_idx[l->id] = ++W.n_labels;

    /* patch forward branches to this label */
    for (i = 0; i < W.n_fixup; i++) {
        if (W.fixup[i].label == l->id) {
            tcg_insn_unit *p = s->code_buf + W.fixup[i].pos;
            uint32_t v = W.n_labels;
            int b;
            for (b = 0; b < 4; b++) {
                p[b] = (v & 0x7f) | 0x80;
                v >>= 7;
            }
            p[4] = v & 0x7f;
            W.fixup[i] = W.fixup[--W.n_fixup];
            i--;
        }
    }

    /* close region k-1, open region k: if (bp <= k) { <code> } */
    w64_blk_pop(s);
    w64_local_get(s, W64_L_BP);
    w64_const_i32(s, W.n_labels);
    w64_u8(s, 0x4d);                        /* i32.le_u */
    w64_blk_push(s, W64_BLK_REGION, W.n_labels);
}

/* ------------------------------------------------------------------ */
/* basic operations                                                    */
/* ------------------------------------------------------------------ */

static const struct {
    uint8_t i32, i64;
} w64_cmp[16] = {
    [TCG_COND_EQ]  = { 0x46, 0x51 },
    [TCG_COND_NE]  = { 0x47, 0x52 },
    [TCG_COND_LT]  = { 0x48, 0x53 },
    [TCG_COND_LTU] = { 0x49, 0x54 },
    [TCG_COND_GT]  = { 0x4a, 0x55 },
    [TCG_COND_GTU] = { 0x4b, 0x56 },
    [TCG_COND_LE]  = { 0x4c, 0x57 },
    [TCG_COND_LEU] = { 0x4d, 0x58 },
    [TCG_COND_GE]  = { 0x4e, 0x59 },
    [TCG_COND_GEU] = { 0x4f, 0x5a },
};

static void w64_cmp_op(TCGContext *s, TCGType t, TCGCond c)
{
    w64_u8(s, t == TCG_TYPE_I32 ? w64_cmp[c].i32 : w64_cmp[c].i64);
}

/* generic reg-reg-reg binary integer op */
static void w64_binop_rrr(TCGContext *s, TCGType t, uint8_t op32, uint8_t op64,
                          TCGReg r0, TCGReg r1, TCGReg r2)
{
    w64_get(s, r1, t);
    w64_get(s, r2, t);
    w64_u8(s, t == TCG_TYPE_I32 ? op32 : op64);
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, r0);
    } else {
        w64_set_i64(s, r0);
    }
}

static void w64_binop_rri(TCGContext *s, TCGType t, uint8_t op32, uint8_t op64,
                          TCGReg r0, TCGReg r1, tcg_target_long i2)
{
    w64_get(s, r1, t);
    w64_const(s, t, i2);
    w64_u8(s, t == TCG_TYPE_I32 ? op32 : op64);
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, r0);
    } else {
        w64_set_i64(s, r0);
    }
}

/* ------------------------------------------------------------------ */
/* loads/stores (TCG-internal, via the wasm linear memory)             */
/* ------------------------------------------------------------------ */

/* push the effective address of base+ofs as an i64 */
static void w64_addr(TCGContext *s, TCGReg base, intptr_t ofs)
{
    if (ofs == 0) {
        w64_get_i64(s, base);
        return;
    }
    w64_get_i64(s, base);
    w64_const_i64(s, ofs);
    w64_u8(s, 0x7c);                        /* i64.add */
}

/* memarg (align, offset) with offset folded into the memarg when it
 * fits u32; otherwise the address was pre-computed and offset is 0 */
static void w64_memarg(TCGContext *s, unsigned align, uint64_t offset)
{
    w64_uleb(s, align);
    w64_uleb(s, offset);
}

static void w64_load(TCGContext *s, uint8_t opc, TCGType t, TCGReg r0,
                     TCGReg base, intptr_t ofs, unsigned align)
{
    if (ofs >= 0 && ofs <= 0xffffffffu) {
        w64_get_i64(s, base);
        w64_u8(s, opc);
        w64_memarg(s, align, ofs);
    } else {
        w64_addr(s, base, ofs);
        w64_u8(s, opc);
        w64_memarg(s, align, 0);
    }
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, r0);
    } else {
        w64_set_i64(s, r0);
    }
}

static void w64_store(TCGContext *s, uint8_t opc, TCGType t, TCGReg val,
                      TCGReg base, intptr_t ofs, unsigned align)
{
    if (ofs >= 0 && ofs <= 0xffffffffu) {
        w64_get_i64(s, base);
        w64_get(s, val, t);
        w64_u8(s, opc);
        w64_memarg(s, align, ofs);
    } else {
        w64_addr(s, base, ofs);
        w64_get(s, val, t);
        w64_u8(s, opc);
        w64_memarg(s, align, 0);
    }
}

/* frame bounds check, mirroring TCI (defensive: offsets are small) */
static void w64_frame_check(TCGReg base, intptr_t ofs)
{
    if (base == TCG_REG_CALL_STACK) {
        tcg_debug_assert(ofs >= 0);
        tcg_debug_assert(ofs < (TCG_STATIC_CALL_ARGS_SIZE +
                                TCG_STATIC_FRAME_SIZE));
    }
}

static void tcg_out_ld(TCGContext *s, TCGType type, TCGReg val, TCGReg base,
                       intptr_t ofs)
{
    w64_ir_ldst(type == TCG_TYPE_I32 ? WI_LD32 : WI_LD64, val, base, ofs);
    w64_frame_check(base, ofs);
    switch (type) {
    case TCG_TYPE_I32:
        w64_load(s, 0x28, type, val, base, ofs, 2);
        break;
    case TCG_TYPE_I64:
        w64_load(s, 0x29, type, val, base, ofs, 3);
        break;
    default:
        g_assert_not_reached();
    }
}

static void tcg_out_st(TCGContext *s, TCGType type, TCGReg val, TCGReg base,
                       intptr_t ofs)
{
    w64_ir_ldst(type == TCG_TYPE_I32 ? WI_ST32 : WI_ST64, val, base, ofs);
    w64_frame_check(base, ofs);
    switch (type) {
    case TCG_TYPE_I32:
        w64_store(s, 0x36, type, val, base, ofs, 2);
        break;
    case TCG_TYPE_I64:
        w64_store(s, 0x37, type, val, base, ofs, 3);
        break;
    default:
        g_assert_not_reached();
    }
}

static void tcg_out_ld8u(TCGContext *s, TCGType t, TCGReg v, TCGReg b, intptr_t o)
{
    w64_ir_ldst(t == TCG_TYPE_I32 ? WI_LD8U : WI_LD8U, v, b, o);
    w64_load(s, t == TCG_TYPE_I32 ? 0x2d : 0x31, t, v, b, o, 0);
}

static void tcg_out_ld8s(TCGContext *s, TCGType t, TCGReg v, TCGReg b, intptr_t o)
{
    w64_ir_ldst(t == TCG_TYPE_I32 ? WI_LD8S32 : WI_LD8S64, v, b, o);
    w64_load(s, t == TCG_TYPE_I32 ? 0x2c : 0x30, t, v, b, o, 0);
}

static void tcg_out_ld16u(TCGContext *s, TCGType t, TCGReg v, TCGReg b, intptr_t o)
{
    w64_ir_ldst(t == TCG_TYPE_I32 ? WI_LD16U : WI_LD16U, v, b, o);
    w64_load(s, t == TCG_TYPE_I32 ? 0x2f : 0x33, t, v, b, o, 1);
}

static void tcg_out_ld16s(TCGContext *s, TCGType t, TCGReg v, TCGReg b, intptr_t o)
{
    w64_ir_ldst(t == TCG_TYPE_I32 ? WI_LD16S32 : WI_LD16S64, v, b, o);
    w64_load(s, t == TCG_TYPE_I32 ? 0x2e : 0x32, t, v, b, o, 1);
}

static void tcg_out_ld32u(TCGContext *s, TCGType t, TCGReg v, TCGReg b, intptr_t o)
{
    w64_ir_ldst(t == TCG_TYPE_I32 ? WI_LD32U : WI_LD32U, v, b, o);
    w64_load(s, t == TCG_TYPE_I32 ? 0x28 : 0x35, t, v, b, o, 2);
}

static void tcg_out_ld32s(TCGContext *s, TCGType t, TCGReg v, TCGReg b, intptr_t o)
{
    w64_ir_ldst(t == TCG_TYPE_I32 ? WI_LD32S64 : WI_LD32S64, v, b, o);
    w64_load(s, 0x34, TCG_TYPE_I64, v, b, o, 2);
}

static void tcg_out_st8(TCGContext *s, TCGType t, TCGReg v, TCGReg b, intptr_t o)
{
    w64_ir_ldst(WI_ST8, v, b, o);
    w64_store(s, t == TCG_TYPE_I32 ? 0x3a : 0x3c, t, v, b, o, 0);
}

static void tcg_out_st16(TCGContext *s, TCGType t, TCGReg v, TCGReg b, intptr_t o)
{
    w64_ir_ldst(WI_ST16, v, b, o);
    w64_store(s, t == TCG_TYPE_I32 ? 0x3b : 0x3d, t, v, b, o, 1);
}

static bool tcg_out_sti(TCGContext *s, TCGType t, TCGArg v, TCGReg b, intptr_t o)
{
    w64_ir_sti(t, v, b, o);
    uint8_t opc = t == TCG_TYPE_I32 ? 0x36 : 0x37;
    unsigned align = t == TCG_TYPE_I32 ? 2 : 3;

    w64_frame_check(b, o);
    if (o >= 0 && o <= 0xffffffffu) {
        w64_get_i64(s, b);
        w64_const(s, t, v);
        w64_u8(s, opc);
        w64_memarg(s, align, o);
    } else {
        w64_addr(s, b, o);
        w64_const(s, t, v);
        w64_u8(s, opc);
        w64_memarg(s, align, 0);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* moves and extensions                                                */
/* ------------------------------------------------------------------ */

static bool tcg_out_mov(TCGContext *s, TCGType type, TCGReg ret, TCGReg arg)
{
    w64_ir_op(type == TCG_TYPE_I32 ? WI_MOV32 : WI_MOV64, ret, arg, 0);
    tcg_debug_assert(ret != arg);
    if (type == TCG_TYPE_I32) {
        if (w64_rep_is64(arg)) {
            w64_get_i32(s, arg);
            w64_set_i32(s, ret);
        } else {
            w64_local_get(s, w64_local32(arg));
            w64_set_i32(s, ret);
        }
    } else {
        if (w64_rep_is64(arg)) {
            w64_local_get(s, w64_local64(arg));
            w64_set_i64(s, ret);
        } else {
            w64_get_i64(s, arg);
            w64_set_i64(s, ret);
        }
    }
    return true;
}

static void tcg_out_movi(TCGContext *s, TCGType type,
                         TCGReg ret, tcg_target_long arg)
{
    w64_ir_movi(type, ret, arg);
    switch (type) {
    case TCG_TYPE_I32:
        w64_const_i32(s, (int32_t)arg);
        w64_set_i32(s, ret);
        break;
    case TCG_TYPE_I64:
        w64_const_i64(s, arg);
        w64_set_i64(s, ret);
        break;
    default:
        g_assert_not_reached();
    }
}

static void tcg_out_ext8s(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1)
{
    w64_ir_un(t, WI_UN_EXT8S, r0, r1);
    if (t == TCG_TYPE_I32) {
        w64_get_i32(s, r1);
        w64_u8(s, 0xc0);                    /* i32.extend8_s */
        w64_set_i32(s, r0);
    } else {
        w64_get_i64(s, r1);
        w64_u8(s, 0xc2);                    /* i64.extend8_s */
        w64_set_i64(s, r0);
    }
}

static void tcg_out_ext16s(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1)
{
    w64_ir_un(t, WI_UN_EXT16S, r0, r1);
    if (t == TCG_TYPE_I32) {
        w64_get_i32(s, r1);
        w64_u8(s, 0xc1);                    /* i32.extend16_s */
        w64_set_i32(s, r0);
    } else {
        w64_get_i64(s, r1);
        w64_u8(s, 0xc3);                    /* i64.extend16_s */
        w64_set_i64(s, r0);
    }
}

static void tcg_out_ext8u(TCGContext *s, TCGReg r0, TCGReg r1)
{
    w64_ir_un(TCG_TYPE_I64, WI_UN_EXT8U, r0, r1);
    w64_get_i32(s, r1);
    w64_const_i32(s, 0xff);
    w64_u8(s, 0x71);                        /* i32.and */
    w64_set_i32(s, r0);
}

static void tcg_out_ext16u(TCGContext *s, TCGReg r0, TCGReg r1)
{
    w64_ir_un(TCG_TYPE_I64, WI_UN_EXT16U, r0, r1);
    w64_get_i32(s, r1);
    w64_const_i32(s, 0xffff);
    w64_u8(s, 0x71);                        /* i32.and */
    w64_set_i32(s, r0);
}

static void tcg_out_ext32s(TCGContext *s, TCGReg r0, TCGReg r1)
{
    w64_ir_un(TCG_TYPE_I64, WI_UN_EXT32S, r0, r1);
    w64_get_i64(s, r1);
    w64_u8(s, 0xc4);                        /* i64.extend32_s */
    w64_set_i64(s, r0);
}

static void tcg_out_ext32u(TCGContext *s, TCGReg r0, TCGReg r1)
{
    w64_ir_un(TCG_TYPE_I64, WI_UN_EXT32U, r0, r1);
    w64_get_i32(s, r1);
    w64_u8(s, 0xad);                        /* i64.extend_i32_u */
    w64_set_i64(s, r0);
}

static void tcg_out_exts_i32_i64(TCGContext *s, TCGReg r0, TCGReg r1)
{
    w64_ir_un(TCG_TYPE_I64, WI_UN_EXT32S, r0, r1);
    w64_get_i32(s, r1);
    w64_u8(s, 0xac);                        /* i64.extend_i32_s */
    w64_set_i64(s, r0);
}

static void tcg_out_extu_i32_i64(TCGContext *s, TCGReg r0, TCGReg r1)
{
    w64_ir_un(TCG_TYPE_I64, WI_UN_EXT32U, r0, r1);
    w64_get_i32(s, r1);
    w64_u8(s, 0xad);                        /* i64.extend_i32_u */
    w64_set_i64(s, r0);
}

static void tcg_out_extrl_i64_i32(TCGContext *s, TCGReg r0, TCGReg r1)
{
    w64_ir_un(TCG_TYPE_I64, WI_UN_EXTRL, r0, r1);
    w64_get_i32(s, r1);                     /* includes the wrap */
    w64_set_i32(s, r0);
}

static void tcg_out_extrh_i64_i32(TCGContext *s, TCGReg r0, TCGReg r1)
{
    w64_ir_un(TCG_TYPE_I64, WI_UN_EXTRH, r0, r1);
    w64_get_i64(s, r1);
    w64_const_i64(s, 32);
    w64_u8(s, 0x88);                        /* i64.shr_u */
    w64_u8(s, 0xa7);                        /* i32.wrap_i64 */
    w64_set_i32(s, r0);
}

static void tcg_out_addi_ptr(TCGContext *s, TCGReg r0, TCGReg r1,
                             tcg_target_long v)
{
    if (v == 0) {
        tcg_out_mov(s, TCG_TYPE_PTR, r0, r1);
    } else {
        w64_ir_alui(TCG_TYPE_I64, WI_ALU_ADD, r0, r1, v);
        w64_get_i64(s, r1);
        w64_const_i64(s, v);
        w64_u8(s, 0x7c);                    /* i64.add */
        w64_set_i64(s, r0);
    }
}

static bool tcg_out_xchg(TCGContext *s, TCGType t, TCGReg r1, TCGReg r2)
{
    return false;
}

/* ------------------------------------------------------------------ */
/* branches, exits                                                     */
/* ------------------------------------------------------------------ */

/* Set $bp to label's region index and break to the loop head.  The
 * compare producing the branch condition (for brcond) must already be
 * on the stack when cond is true. */
static void w64_br_to_label(TCGContext *s, TCGLabel *l, bool cond)
{
    uint16_t idx = W.label_idx[l->id];

    if (W.nested) {
        int i;

        for (i = W.n_blk - 1; i >= 0; i--) {
            if (W.blk[i].kind == W64_BLK_LABEL && W.blk[i].label == idx) {
                break;
            }
        }
        if (i < 0 && W.selfloop && idx == 1) {
            /* the back-edge: label 1 is the guest loop, not a block */
            w64_u8(s, cond ? 0x0d : 0x0c);  /* br_if / br */
            w64_uleb(s, W.n_blk - 1 - W.loop_idx);
            return;
        }
        if (idx == 0 || i < 0) {
            W.overflow = true;
            return;
        }
        w64_u8(s, cond ? 0x0d : 0x0c);      /* br_if / br */
        w64_uleb(s, W.n_blk - 1 - i);
        return;
    }

    if (cond) {
        w64_blk_push(s, W64_BLK_IF, -1);
    }
    if (idx == 0) {
        /* forward branch: patch later */
        w64_u8(s, 0x41);                    /* i32.const */
        if (W.n_fixup >= W64_MAX_FIXUPS) {
            W.overflow = true;
        } else {
            W.fixup[W.n_fixup].pos = s->code_ptr - s->code_buf;
            W.fixup[W.n_fixup].label = l->id;
            W.n_fixup++;
        }
        w64_uleb32_p5(s, 0);
    } else {
        w64_const_i32(s, idx);
    }
    w64_local_set(s, W64_L_BP);
    w64_br_loop(s);
    if (cond) {
        w64_blk_pop(s);
    }
}

static void tcg_out_br(TCGContext *s, TCGLabel *l)
{
    w64_ir_op(WI_BR, 0, 0, 0);
    w64_ir_target(l);
    w64_br_to_label(s, l, false);
}

static void tcg_out_exit_tb(TCGContext *s, uintptr_t arg)
{
    w64_ir_op(WI_EXIT_TB, 0, 0, 0);
    w64_ir_imm32((uint32_t)arg);
    w64_const_i32(s, (int32_t)arg);
    w64_u8(s, 0x0f);                        /* return */
}

static bool w64_xcount(void);
static void w64_diag_bump(TCGContext *s, int idx);

static void tcg_out_goto_tb(TCGContext *s, int which)
{
    w64_ir_op(WI_GOTO_TB, which, 0, 0);
    /* Chaining through the shared funcref table (phase 2).  The chain
     * slot is tb->jmp_target_addr[which] — qemu core links it
     * (tb_add_jump) and resets it to this TB's own reset address
     * (tb_reset_jump), exactly as with TCI's indirect jumps; we read the
     * slot at runtime.  tb_set_jmp_target stores the *table index* of the
     * target there, tagged in the high half (exec/translation-block.h),
     * and a reset address is a wasm64 heap pointer below 2 GB, so one
     * test separates the two:
     *
     *   block
     *     $scr0 = i64.load &tb->jmp_target_addr[which]
     *     br_if 0 (($scr0 >> 32) == 0 || w64_chain_stop != 0)
     *     return_call_indirect $tbsig, table 0
     *                          ($env, $sp, $tp, i32.wrap $scr0)
     *   end
     *
     * The chain then reads one word — this TB's own slot — where it used
     * to follow that word into the target's descriptor for fidx and tidx:
     * a cache line per TB, in a ~20 MB region nothing else in the
     * execution path touches.  tools/dispatch-probe.mjs prices that
     * dependent load at +2 ns over a 256-TB working set and +8..9 ns over
     * 1024-4096 of them.
     *
     * The fall-through runs the exit_tb the frontend emits right after
     * goto_tb, returning (tb|which) to cpu_exec — which then performs
     * the very tb_add_jump that links the pair.  w64_chain_stop is the
     * lockstep-budget brake (see wasm64.c): it forces the chain to
     * unwind to the dispatcher at the next goto_tb so the leg stops at
     * the exact insn budget (the term is only emitted in a process armed
     * with W64_LOCKSTEP).  A chain target has always been instantiated by
     * the time the chain can be taken — tb_add_jump is immediately
     * followed by executing the target — so its table entry is live; the
     * one thing that can unregister it afterwards is batch eviction,
     * which unlinks the incoming jumps of every member it drops
     * (tcg/wasm64/wasm64.c).  Invalidation unlinks sources via
     * tb_jmp_unlink before the TB may be freed, and the wasm load of the
     * still-live slot keeps native-TCG unlink memory-ordering
     * semantics. */
    uintptr_t slot = get_jmp_target_addr(s, which);

    set_jmp_reset_offset(s, which);

    tcg_debug_assert(W.n_blk < W64_MAX_BLK);
    w64_u8(s, 0x02);                        /* block void */
    w64_u8(s, 0x40);
    W.blk[W.n_blk].kind = W64_BLK_IF;
    W.blk[W.n_blk].label = -1;
    W.n_blk++;

    w64_const_i64(s, (int64_t)slot);
    w64_u8(s, 0x29);                        /* i64.load */
    w64_memarg(s, 3, 0);
    w64_local_tee(s, W64_L_SCR0);
    w64_const_i64(s, 32);
    w64_u8(s, 0x88);                        /* i64.shr_u */
    w64_u8(s, 0x50);                        /* i64.eqz: not linked */
    if (w64_ls_armed()) {
        w64_const_i64(s, (int64_t)(uintptr_t)&w64_chain_stop);
        w64_u8(s, 0x28);                    /* i32.load (i64 addr: memory64) */
        w64_memarg(s, 2, 0);
        w64_u8(s, 0x72);                    /* i32.or */
    }
    w64_u8(s, 0x0d);                        /* br_if */
    w64_uleb(s, 0);

    if (w64_xcount()) {
        w64_diag_bump(s, which ? WASM_DIAG_X_GOTOTB1 : WASM_DIAG_X_GOTOTB);
        w64_local_get(s, W64_L_SCR0);
        w64_u8(s, 0xa7);                    /* i32.wrap_i64 */
        w64_const_i32(s, (int32_t)ldl_p(s->code_buf + W64_DESC_TIDX));
        w64_u8(s, 0x46);                    /* i32.eq */
        w64_blk_push(s, W64_BLK_IF, -1);
        w64_diag_bump(s, WASM_DIAG_X_SELF);
        w64_blk_pop(s);
    }

    w64_local_get(s, W64_P_ENV);
    w64_local_get(s, W64_P_SP);
    w64_local_get(s, W64_P_TP);
    w64_local_get(s, W64_L_SCR0);
    w64_u8(s, 0xa7);                        /* i32.wrap_i64: the tidx */
    w64_u8(s, 0x13);                        /* return_call_indirect */
    w64_uleb(s, 0);                         /* type 0: the TB signature */
    w64_uleb(s, 0);                         /* table 0: the shared table */

    w64_blk_pop(s);                         /* end */
}

static void tcg_out_goto_ptr(TCGContext *s, TCGReg a0)
{
    w64_ir_op(WI_GOTO_PTR, a0, 0, 0);
    /* The lookup helpers hand over the target's shared-table index with
     * W64_TIDX_TAG in the high half (exec/translation-block.h), so this
     * tail-call reads nothing of the target: every bx lr / pop {pc} /
     * ldr pc used to unwind to the C dispatcher and re-enter through the
     * batch run thunk (0.5-1.7M exits/s early in the boot), then paid two
     * loads from the target's descriptor to get here.
     *
     *   block
     *     $scr0 = a0
     *     br_if 0 (($scr0 >> 32) == 0 || w64_chain_stop != 0)
     *     return_call_indirect $tbsig, table 0
     *                          ($env, $sp, $tp, i32.wrap $scr0)
     *   end
     *   ... the handoff-slot fallback below: $scr0 is then NULL (lookup
     *   miss) or a descriptor pointer whose fidx is 0 (target not
     *   compiled yet, or its batch was evicted), and the C dispatcher
     *   compiles / re-ensures it exactly as before.
     *
     * A wasm64 heap pointer is below 2 GB, so the high half separates the
     * two cases without a tag test of its own. */
    tcg_debug_assert(W.n_blk < W64_MAX_BLK);
    w64_u8(s, 0x02);                        /* block void */
    w64_u8(s, 0x40);
    W.blk[W.n_blk].kind = W64_BLK_IF;
    W.blk[W.n_blk].label = -1;
    W.n_blk++;

    w64_get_i64(s, a0);
    w64_local_tee(s, W64_L_SCR0);
    w64_const_i64(s, 32);
    w64_u8(s, 0x88);                        /* i64.shr_u */
    w64_u8(s, 0x50);                        /* i64.eqz: not a table index */
    if (w64_ls_armed()) {
        w64_const_i64(s, (int64_t)(uintptr_t)&w64_chain_stop);
        w64_u8(s, 0x28);                    /* i32.load (i64 addr) */
        w64_memarg(s, 2, 0);
        w64_u8(s, 0x72);                    /* i32.or */
    }
    w64_u8(s, 0x0d);                        /* br_if */
    w64_uleb(s, 0);

    if (w64_xcount()) {
        w64_diag_bump(s, WASM_DIAG_X_GOTOPTR);
    }

    w64_local_get(s, W64_P_ENV);
    w64_local_get(s, W64_P_SP);
    w64_local_get(s, W64_P_TP);
    w64_local_get(s, W64_L_SCR0);
    w64_u8(s, 0xa7);                        /* i32.wrap_i64: the tidx */
    w64_u8(s, 0x13);                        /* return_call_indirect */
    w64_uleb(s, 0);                         /* type 0: the TB signature */
    w64_uleb(s, 0);                         /* table 0: the shared table */

    w64_blk_pop(s);                         /* end */

    /* store the next TB's code pointer at [sp-8] and return the
     * handoff code; the C dispatcher continues the chain. */
    w64_local_get(s, W64_P_SP);
    w64_const_i64(s, -8);
    w64_u8(s, 0x7c);                        /* i64.add */
    w64_local_get(s, W64_L_SCR0);
    w64_u8(s, 0x37);                        /* i64.store */
    w64_memarg(s, 3, 0);
    w64_const_i32(s, (int32_t)W64_EXIT_GOTOPTR);
    w64_u8(s, 0x0f);                        /* return */
}

static void tcg_out_mb(TCGContext *s, unsigned a0)
{
    if (a0 != 0) {
        w64_ir_op(WI_MB, 0, 0, 0);
    }
    /*
     * The guest is single-vCPU, but the linear memory is shared with
     * other pthreads (main-loop timers, the DSP worker); wasm plain
     * loads/stores carry no cross-thread ordering, so honour the
     * barrier (atomic.fence, 0xfe 0x03 0x00).  ARMv5 firmware issues
     * essentially no barriers, so the cost is nil.
     */
    if (a0 != 0) {
        w64_u8(s, 0xfe);
        w64_u8(s, 0x03);
        w64_u8(s, 0x00);
    }
}

void tb_target_set_jmp_target(const TranslationBlock *tb, int n,
                              uintptr_t jmp_rx, uintptr_t jmp_rw)
{
    /* Nothing to patch: the chain slot is tb->jmp_target_addr[n]
     * itself (writable TB memory); tb_set_jmp_target has just updated
     * it, and the emitted goto_tb code re-reads it at runtime (TCI's
     * "always indirect" scheme, see tcg_out_goto_tb). */
}

/* ------------------------------------------------------------------ */
/* calls                                                               */
/* ------------------------------------------------------------------ */

/*
 * Retaddr marker for GETPC()-using helpers.
 *
 * cpu_unwind_data_from_tb() resolves a retaddr by walking the per-insn
 * end offsets for the first one *strictly greater* than
 * (retaddr - GETPC_ADJ): a real backend's GETPC() is a return address,
 * which always sits after the call, i.e. strictly inside the faulting
 * insn's code range.  Here the marker is emitted before the call
 * sequence, and for a guest insn whose code starts with that call (any
 * plain load/store: the address is already in a register) the raw
 * offset equals the insn's start = the previous insn's end, so the
 * unwind resolved one guest insn too early.  cpu_io_recompile() then
 * rewound to the previous insn and re-executed it — an EL71 boot
 * re-ran an `ldm r0!` twice before a buffered flash program and wrote
 * the erased pattern, aborting with ">>EXIT<< FILE: flash" (0x0506).
 *
 * Bias by GETPC_ADJ so the subtraction lands exactly on the insn start,
 * which the strict comparison attributes to this insn.  The call
 * sequence that follows is always longer than the bias, so the marker
 * still refers to this insn and never to the next one.
 */
#define W64_GETPC_ADJ 2         /* GETPC_ADJ, accel/tcg/tb-internal.h */
static inline uintptr_t w64_pc(TCGContext *s)
{
    return (uintptr_t)s->code_buf + (s->code_ptr - s->code_buf)
           + W64_GETPC_ADJ;
}

/* typemask typecode -> wasm type (0 = i32, 1 = i64); 0xff invalid */
static uint8_t w64_typecode_to_wasm(int tc)
{
    switch (tc) {
    case dh_typecode_i32:
    case dh_typecode_s32:
        return 0;
    case dh_typecode_i64:
    case dh_typecode_s64:
    case dh_typecode_ptr:
        return 1;
    default:
        return 0xff;
    }
}

static void tcg_out_call(TCGContext *s, const tcg_insn_unit *target,
                         const TCGHelperInfo *info)
{
    uint32_t mask = info->typemask;
    uint8_t p[8], ret;
    unsigned np = info->nr_in, i;
    uintptr_t ra = w64_pc(s);
    uint8_t imp, uimp;

    tcg_debug_assert(info->out_kind == TCG_CALL_RET_NORMAL);
    tcg_debug_assert(np <= ARRAY_SIZE(p));
    w64_ir_call(target, info, ra);

    /* retaddr for GETPC()-using helpers */
    w64_local_get(s, W64_P_TP);
    w64_const_i64(s, ra);
    w64_u8(s, 0x37);
    w64_memarg(s, 3, 0);

    /* reload arguments from the call frame (slot i at $sp + 8*i) */
    for (i = 0; i < np; i++) {
        int tc = (mask >> ((i + 1) * 3)) & 7;
        p[i] = w64_typecode_to_wasm(tc);
        tcg_debug_assert(p[i] != 0xff);
        w64_local_get(s, W64_P_SP);
        w64_u8(s, p[i] ? 0x29 /* i64.load */ : 0x28 /* i32.load */);
        w64_memarg(s, p[i] ? 3 : 2, 8 * i);
    }

    /* ret type: inline switch — a literal decoding, no helper (bring-up
     * paranoia after observing an -O3 inlining artifact) */
    switch (mask & 7) {
    case 0:
        ret = 0xff;                    /* void */
        break;
    case 2:
    case 3:
        ret = 0;                       /* i32 */
        break;
    case 4:
    case 5:
    case 6:
        ret = 1;                       /* i64 / ptr */
        break;
    default:
        g_assert_not_reached();
    }

    {
        uint8_t lt = w64_add_type(np, p, ret);
        uint8_t ut = w64_union_type(np, p, ret);
        imp = w64_import_idx((uint32_t)(uintptr_t)target, lt);
        uimp = w64_union_import((uint32_t)(uintptr_t)target, ut);
    }
    w64_u8(s, 0x10);                        /* call */
    w64_callidx(s, imp, uimp);

    {   /* translation-time helper-import diagnostics (W64_DEBUG=1) */
        static int env_checked, debug;
        if (!env_checked) {
            env_checked = 1;
            debug = getenv("W64_DEBUG") != NULL;
        }
        if (debug) {
            fprintf(stderr, "W64CALL %s fptr=%u np=%u ret=%u mask=%08x rtc=%u "
                    "rtcv=%u tcv0=%u tcv4=%u tcv6=%u\n",
                    info->name ? info->name : "?",
                    (unsigned)(uintptr_t)target, np, ret, mask,
                    (unsigned)(mask & 7),
                    (unsigned)w64_typecode_to_wasm(mask & 7),
                    (unsigned)w64_typecode_to_wasm(0),
                    (unsigned)w64_typecode_to_wasm(4),
                    (unsigned)w64_typecode_to_wasm(6));
        }
    }

    /* result -> oarg register (ret: 0 = i32, 1 = i64, 0xff = void) */
    if (ret == 1) {
        w64_local_set(s, W64_L64(TCG_REG_R0));
        W.rep |= 1u << TCG_REG_R0;
    } else if (ret == 0) {
        w64_local_set(s, W64_L32(TCG_REG_R0));
        W.rep &= ~(1u << TCG_REG_R0);
    }
}

/* ------------------------------------------------------------------ */
/* qemu_ld/st (phase 3: inline TLB probe + size-specialized access;   */
/* the miss arm is the phase-1 *_mmu helper path)                      */
/* ------------------------------------------------------------------ */

/* The probe mirrors tci_tlb_probe (patch 0011, lockstep-proven) with
 * the TLB layout constants from tcg.c's tlb_mask_table_ofs:
 *
 *   entry = fast->table[(addr >> TARGET_PAGE_BITS) &
 *                        (fast->mask >> CPU_TLB_ENTRY_BITS)]
 *   hit   = entry->addr_{read,write} ==
 *           ((a_mask >= s_mask ? addr : addr + (s_mask - a_mask))
 *            & (TARGET_PAGE_MASK | a_mask))
 *
 * TLB flag bits (INVALID/NOTDIRTY/FORCE_SLOW, bits 6..8) sit below the
 * page bits and above the largest alignment, so any flagged entry
 * (MMIO, clean, watchpointed, plugin) fails the compare and takes the
 * helper slow path; a clean hit gives a host pointer into linear
 * memory (addr + addend) where the size-specialized wasm access runs
 * directly.  Atomics: only MO_ATOM_NONE/IFALIGN accesses inline (same
 * policy as tci); bswap falls back to the helper.  armv5 unaligned
 * accesses (MO_UNALN, atom=IFALIGN) inline fine — a plain wasm load
 * IS the emulator's natural LE load; the adj term catches page
 * straddling (slow path splits it). */

QEMU_BUILD_BUG_ON(CPU_TLB_ENTRY_BITS != 5);            /* 64-bit host */
QEMU_BUILD_BUG_ON(sizeof(CPUTLBDescFast) != 16);
QEMU_BUILD_BUG_ON(offsetof(CPUTLBEntry, addr_read) != 0);
QEMU_BUILD_BUG_ON(offsetof(CPUTLBEntry, addr_write) != 8);
QEMU_BUILD_BUG_ON(offsetof(CPUTLBEntry, addend) != 24);

struct w64_tlb_ctx {
    int32_t  fast_ofs;    /* env-relative &cpu->neg.tlb.f[mmuidx] */
    uint64_t tlb_mask;    /* TARGET_PAGE_MASK | a_mask */
    unsigned shr;         /* TARGET_PAGE_BITS - CPU_TLB_ENTRY_BITS */
    unsigned adj;         /* s_mask - a_mask when a_mask < s_mask */
    unsigned cmp_ofs;     /* offsetof addr_read / addr_write */
};

/* W64_NOTLB=1 disables the inline probe (A/B measurement, debugging) */
static bool w64_tlb_inline(void)
{
    static int mode = -1;
    if (mode < 0) {
        mode = getenv("W64_NOTLB") == NULL;
    }
    return mode != 0;
}

/* W64_LDSTCOUNT emits counter bumps into the generated code, which is how
 * the *dynamic* cost of a memop becomes visible at all: the static
 * WASM_DIAG_LDST_GEN* counters count translations, and a memop translated
 * once can run a billion times.
 *
 *   1  miss arms only -- the bump sits next to an indirect helper call
 *      that dwarfs it, so this one is nearly free
 *   2  ...and every execution (WASM_DIAG_LDST_EXEC), which is the memops
 *      -per-guest-instruction denominator.  On the hot path by
 *      construction: expect it to cost.
 *
 * Either way the generated code changes -- a measurement knob, never an
 * A/B.  Per-Mi rates stay exact under it (icount determinism); wall-clock
 * numbers do not. */
static int w64_ldst_count(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("W64_LDSTCOUNT");
        mode = e ? atoi(e) : 0;
    }
    return mode;
}

/* ++*(uint64_t *)&wasm_diag_stat[idx], at the link-time constant address:
 * the generated modules import the main module's linear memory, so the
 * counter the tools read through _wasm_memstat is directly addressable. */
static void w64_diag_bump(TCGContext *s, int idx)
{
    int64_t a = (int64_t)(uintptr_t)&wasm_diag_stat[idx];

    w64_const_i64(s, a);
    w64_const_i64(s, a);
    w64_u8(s, 0x29);                        /* i64.load */
    w64_memarg(s, 3, 0);
    w64_const_i64(s, 1);
    w64_u8(s, 0x7c);                        /* i64.add */
    w64_u8(s, 0x37);                        /* i64.store */
    w64_memarg(s, 3, 0);
}

/* W64_TBHIST=1: ++w64_tbhist[tidx] in the prologue.  The slot address is
 * known at translation time, so this is one load and one store at a
 * constant address -- the cheapest per-TB-entry counter the backend can
 * emit, and still a measurement build like W64_LDSTCOUNT. */
static bool w64_tbhist_on(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("W64_TBHIST") != NULL;
    }
    return on;
}

static void w64_tbhist_emit(TCGContext *s, uint32_t tidx)
{
    int64_t a = (int64_t)(uintptr_t)w64_tbhist_slot(tidx);

    w64_const_i64(s, a);
    w64_const_i64(s, a);
    w64_u8(s, 0x28);                        /* i32.load */
    w64_memarg(s, 2, 0);
    w64_const_i32(s, 1);
    w64_u8(s, 0x6a);                        /* i32.add */
    w64_u8(s, 0x36);                        /* i32.store */
    w64_memarg(s, 2, 0);
}

/* W64_XCOUNT=1 counts TB exits in the generated code: which chain a TB
 * entry actually left by, and whether a goto_tb went back to its own TB.
 * Same class as W64_LDSTCOUNT -- the generated code changes, so per-Mi
 * rates stay exact and wall-clock numbers do not. */
static bool w64_xcount(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("W64_XCOUNT") != NULL;
    }
    return on;
}

/* W64_LDSTPAD=N emits N filler ALU ops on every memop, executed or not.
 *
 * This is the calibration instrument the phase timers are not: a timed
 * interval prices a phase against an empty interval and so inherits the
 * clock's floor, which is how round eighteen briefly "found" a 94 ns BQL
 * acquire.  A pad prices wasm instructions against *each other* - run the
 * same fixed guest work at N = 0, 8, 16 and the slope of wall against N is
 * ns per wasm instruction on the memop path, with the load and the store
 * that make the chain live cancelling out of the slope.
 *
 * The unit must survive the optimising tier, and the first two candidates
 * did not.  A dropped pure result folds to nothing at all; an `i64.add`
 * chain folds to x+(sum of constants); and xor-then-rotate, which looks
 * like a mixing step, folds just as completely, because xor distributes
 * over rotate - rotl(x^a, k) is rotl(x,k) ^ rotl(a,k), so N units collapse
 * to one rotate and one xor.  That one was measured before it was caught:
 * 8 units read 1.64 ns per memop, ~6x too cheap for 32 ALU ops.
 *
 * add and xor do not distribute over each other in either direction, so
 * alternating them has no algebraic collapse, and the chain ends in a
 * store the engine cannot prove dead. */
static int w64_ldst_pad(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("W64_LDSTPAD");
        n = e ? atoi(e) : 0;
    }
    return n;
}

static void w64_emit_pad(TCGContext *s, int units)
{
    int64_t a = (int64_t)(uintptr_t)&wasm_diag_stat[WASM_DIAG_PAD_SINK];
    int i;

    w64_const_i64(s, a);
    w64_const_i64(s, a);
    w64_u8(s, 0x29);                        /* i64.load */
    w64_memarg(s, 3, 0);
    for (i = 0; i < units; i++) {
        w64_const_i64(s, 0x9e3779b97f4a7c15ULL + i);
        w64_u8(s, 0x7c);                    /* i64.add */
        w64_const_i64(s, 0xc2b2ae3d27d4eb4fULL - i);
        w64_u8(s, 0x85);                    /* i64.xor */
    }
    w64_u8(s, 0x37);                        /* i64.store */
    w64_memarg(s, 3, 0);
}

/* W64_TLBDUP=N emits N extra inline TLB probes on every memop, each
 * against a different mmu index so the mask/table loads cannot be
 * common-subexpressed away, their results or'd together and folded into
 * the pad sink so nothing is dead.  The N=1 -> N=2 slope is one whole
 * probe plus one i32.or, which is what the probe costs: the standing
 * "~5 % of wall" is `W64_LDSTPAD`'s ns-per-instruction extrapolated over
 * an instruction count, and that calibration measured a *dependent ALU
 * chain*, which a probe (two loads deep, several independent operands)
 * is not.  Measurement build, like W64_LDSTCOUNT: per-Mi rates stay
 * exact under it, wall-clock numbers do not. */
static int w64_tlb_dup(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("W64_TLBDUP");
        n = e ? atoi(e) : 0;
    }
    return n;
}

static bool w64_tlb_setup(struct w64_tlb_ctx *tc, TCGContext *s,
                          MemOpIdx oi, bool is_ld)
{
    MemOp mop = get_memop(oi);
    unsigned atom = mop & MO_ATOM_MASK;
    unsigned a_mask = (1u << memop_alignment_bits(mop)) - 1;
    unsigned s_mask = (1u << (mop & MO_SIZE)) - 1;

    if (!w64_tlb_inline() || (mop & MO_BSWAP) ||
        (atom != MO_ATOM_NONE && atom != MO_ATOM_IFALIGN)) {
        return false;
    }

    tc->fast_ofs = tlb_mask_table_ofs(s, get_mmuidx(oi));
    tc->tlb_mask = (uint64_t)(int64_t)(int)TARGET_PAGE_MASK | a_mask;
    tc->shr = TARGET_PAGE_BITS - CPU_TLB_ENTRY_BITS;
    tc->adj = a_mask >= s_mask ? 0 : s_mask - a_mask;
    tc->cmp_ofs = is_ld ? offsetof(CPUTLBEntry, addr_read)
                        : offsetof(CPUTLBEntry, addr_write);
    return true;
}

/* push the address tag an entry's addr_read/addr_write must equal */
static void w64_tlb_key(TCGContext *s, int al, const struct w64_tlb_ctx *tc)
{
    w64_local_get(s, al);
    if (tc->adj) {
        w64_const_i64(s, tc->adj);
        w64_u8(s, 0x7c);                    /* i64.add */
    }
    w64_const_i64(s, (int64_t)tc->tlb_mask);
    w64_u8(s, 0x83);                        /* i64.and */
}

/* Emit the probe: $scr0 = entry, $scr1 = tlb_addr, then the hit
 * condition (i32) on the value stack for the caller's `if`.  @al is
 * the local holding the zero-extended guest address (the $scr2
 * snapshot — never a TCG register, whose representation the arms may
 * still mutate when data == addr). */
static void w64_tlb_probe(TCGContext *s, int al, const struct w64_tlb_ctx *tc)
{
    /* $scr0 = env + fast_ofs (negative; memarg offsets are unsigned) */
    w64_local_get(s, W64_P_ENV);
    w64_const_i64(s, tc->fast_ofs);
    w64_u8(s, 0x7c);                        /* i64.add */
    w64_local_tee(s, W64_L_SCR0);

    /* $scr0 = table[(addr >> page_bits) & (mask >> entry_bits)] */
    w64_u8(s, 0x29);                        /* i64.load */
    w64_memarg(s, 3, 0);                    /* mask */
    w64_local_get(s, al);
    w64_const_i64(s, tc->shr);
    w64_u8(s, 0x88);                        /* i64.shr_u */
    w64_u8(s, 0x83);                        /* i64.and */
    w64_local_get(s, W64_L_SCR0);
    w64_u8(s, 0x29);                        /* i64.load */
    w64_memarg(s, 3, 8);                    /* table */
    w64_u8(s, 0x7c);                        /* i64.add */
    w64_local_tee(s, W64_L_SCR0);

    /* hit = (tlb_addr == ((addr + adj) & tlb_mask)); tlb_addr has no
     * other reader, so it stays on the value stack */
    w64_u8(s, 0x29);                        /* i64.load */
    w64_memarg(s, 3, tc->cmp_ofs);

    w64_tlb_key(s, al, tc);
    w64_u8(s, 0x51);                        /* i64.eq */
}

/* W64_TLBCHEAP=N prices the *replacement* for the inline probe before it
 * is built, in exactly the units W64_TLBDUP prices the current one: a
 * per-memop-site one-entry cache of (page -> host addend), tagged with a
 * global generation so a TLB flush invalidates every site at once.
 *
 *   hit = load(slot) == ((addr & (PAGE_MASK|a_mask)) ^ gen)
 *   haddr = addr + load(slot + 8)
 *
 * against the current probe's two loads to reach the fast table, an index
 * computation and a third load to reach the entry.  The slot pool here is
 * 1 MB and indexed by a translation-time counter, so the duplicate pays a
 * realistically scattered access the way a real pool would and does not
 * flatter itself with one always-hot cache line.  The generation load
 * reads a real always-hot global.  Measurement only: the result is folded
 * into the pad sink and nothing acts on it. */
/* Backing store for the measurement knobs' per-site slots: 8 MB, and
 * allocated only when one of them is on, so an ordinary build reserves
 * nothing for it. */
static uint64_t *w64_fakeslot;
static unsigned w64_fakeslot_n;

static uint64_t *w64_fakeslot_next(void)
{
    if (!w64_fakeslot) {
        w64_fakeslot = g_malloc0((size_t)(1 << 20) * sizeof(uint64_t));
    }
    return &w64_fakeslot[(w64_fakeslot_n++ & 0x7ffff) * 2];
}

static int w64_tlb_cheap(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("W64_TLBCHEAP");
        n = e ? atoi(e) : 0;
    }
    return n;
}

static void w64_tlb_cheap_probe(TCGContext *s, int al,
                                const struct w64_tlb_ctx *tc)
{
    uint64_t *slot = w64_fakeslot_next();

    w64_local_get(s, al);
    w64_const_i64(s, (int64_t)tc->tlb_mask);
    w64_u8(s, 0x83);                            /* i64.and */
    w64_const_i64(s, (int64_t)w64_acct_addr[2]);
    w64_u8(s, 0x29);                            /* i64.load (generation) */
    w64_memarg(s, 3, 0);
    w64_u8(s, 0x85);                            /* i64.xor */
    w64_const_i64(s, (int64_t)(uintptr_t)slot);
    w64_u8(s, 0x29);                            /* i64.load (tag) */
    w64_memarg(s, 3, 0);
    w64_u8(s, 0x51);                            /* i64.eq */
}

/* W64_TLBHIT=1 runs the per-site cache for real -- check, then fill the
 * slot when it misses -- and counts hits and misses.  The slot holds only
 * the page key, so this is the page-stability hit rate a real design would
 * see between TLB flushes; the flush term is separately computable from
 * tlbFlush + tlbFlushRange and the number of live sites. */
static bool w64_tlb_hit(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("W64_TLBHIT") != NULL;
    }
    return on;
}

static void w64_tlb_hit_emit(TCGContext *s, int al, MemOpIdx oi, bool is_ld)
{
    struct w64_tlb_ctx tc;
    uint64_t *slot;

    if (!w64_tlb_hit() || !w64_tlb_setup(&tc, s, oi, is_ld)) {
        return;
    }
    slot = w64_fakeslot_next();

    w64_local_get(s, al);
    w64_const_i64(s, (int64_t)tc.tlb_mask);
    w64_u8(s, 0x83);                            /* i64.and */
    w64_local_tee(s, W64_L_SCR1);
    w64_const_i64(s, (int64_t)(uintptr_t)slot);
    w64_u8(s, 0x29);                            /* i64.load */
    w64_memarg(s, 3, 0);
    w64_u8(s, 0x51);                            /* i64.eq */
    w64_blk_push(s, W64_BLK_IF, -1);
    w64_diag_bump(s, WASM_DIAG_TLBC_HIT);
    w64_u8(s, 0x05);                            /* else */
    w64_diag_bump(s, WASM_DIAG_TLBC_MISS);
    w64_const_i64(s, (int64_t)(uintptr_t)slot);
    w64_local_get(s, W64_L_SCR1);
    w64_u8(s, 0x37);                            /* i64.store */
    w64_memarg(s, 3, 0);
    w64_blk_pop(s);
}

/* W64_TLBSIMD=N is the same cache with the two words read as one
 * `v128.load`, and the generation kept in a local instead of memory -- so
 * the whole check costs exactly one load, against the current probe's
 * three, and lane 1 carries the addend the current `haddr` needs a fourth
 * load for.  If the probe's cost really is its load count, this is the
 * floor of the redesign. */
static int w64_tlb_simd(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("W64_TLBSIMD");
        n = e ? atoi(e) : 0;
    }
    return n;
}

static void w64_tlb_simd_probe(TCGContext *s, int al,
                               const struct w64_tlb_ctx *tc)
{
    uint64_t *slot = w64_fakeslot_next();

    w64_local_get(s, al);
    w64_const_i64(s, (int64_t)tc->tlb_mask);
    w64_u8(s, 0x83);                            /* i64.and */
    w64_local_get(s, W64_L_SCR1);               /* stands in for $gen */
    w64_u8(s, 0x85);                            /* i64.xor */
    w64_const_i64(s, (int64_t)(uintptr_t)slot);
    w64_u8(s, 0xfd); w64_uleb(s, 0x00);         /* v128.load */
    w64_memarg(s, 4, 0);
    w64_u8(s, 0xfd); w64_uleb(s, 0x1d); w64_u8(s, 0);  /* i64x2.extract_lane 0 */
    w64_u8(s, 0x51);                            /* i64.eq */
}

/* N-1 extra probes against mmu index ^ 1 (a different mask/table pair, so
 * no load is common-subexpressed with the real probe), each result stored
 * to its own slot.  At N=1 the store is of a constant, so the N=1 -> N=2
 * wall slope is one probe and nothing else.
 *
 * The sink must be a plain store to a *per-site* address, never a
 * read-modify-write of one global: an RMW makes every memop in the program
 * wait on the previous one's store-to-load forward, which puts the
 * duplicate's whole dependency chain on a serialized critical path and
 * prices its latency instead of its cost.  Measured both ways, that
 * inflated the probe by ~2x and made a one-load check look as expensive as
 * a three-load one. */
static void w64_tlb_dup_emit(TCGContext *s, int al, MemOpIdx oi, bool is_ld)
{
    bool simd = w64_tlb_simd() > 0;
    bool cheap = !simd && w64_tlb_cheap() > 0;
    int n = simd ? w64_tlb_simd() : cheap ? w64_tlb_cheap() : w64_tlb_dup();
    struct w64_tlb_ctx tc;
    uint64_t *sink;
    int i;

    if (n < 1 || !w64_tlb_setup(&tc, s, oi, is_ld)) {
        return;
    }
    tc.fast_ofs = tlb_mask_table_ofs(s, get_mmuidx(oi) ^ 1);
    sink = w64_fakeslot_next();

    w64_const_i64(s, (int64_t)(uintptr_t)sink);
    if (n == 1) {
        w64_const_i32(s, 0);
    } else {
        for (i = 1; i < n; i++) {
            if (simd) {
                w64_tlb_simd_probe(s, al, &tc);
            } else if (cheap) {
                w64_tlb_cheap_probe(s, al, &tc);
            } else {
                w64_tlb_probe(s, al, &tc);
            }
            if (i > 1) {
                w64_u8(s, 0x72);                /* i32.or */
            }
        }
    }
    w64_u8(s, 0x36);                            /* i32.store */
    w64_memarg(s, 2, 0);
}

/* push [addr + entry->addend] (i64) for the fast access; @al as above */
static void w64_tlb_haddr(TCGContext *s, int al)
{
    w64_local_get(s, al);
    w64_local_get(s, W64_L_SCR0);
    w64_u8(s, 0x29);                        /* i64.load */
    w64_memarg(s, 3, 24);                   /* addend */
    w64_u8(s, 0x7c);                        /* i64.add */
}

/* size/sign-specialized fast access opcodes, indexed [is64][MO_SSIZE]
 * (loads) / [is64][MO_SIZE] (stores) */
static const uint8_t w64_fast_ld[2][16] = {
    [0][MO_UB] = 0x2d, [0][MO_SB] = 0x2c,   /* i32.load8_{u,s}   */
    [0][MO_UW] = 0x2f, [0][MO_SW] = 0x2e,   /* i32.load16_{u,s}  */
    [0][MO_UL] = 0x28, [0][MO_SL] = 0x28,   /* i32.load          */
    [1][MO_UB] = 0x31, [1][MO_SB] = 0x30,   /* i64.load8_{u,s}   */
    [1][MO_UW] = 0x33, [1][MO_SW] = 0x32,   /* i64.load16_{u,s}  */
    [1][MO_UL] = 0x35, [1][MO_SL] = 0x34,   /* i64.load32_{u,s}  */
    [1][MO_UQ] = 0x29, [1][MO_SQ] = 0x29,   /* i64.load          */
};
static const uint8_t w64_fast_st[2][16] = {
    [0][MO_UB] = 0x3a, [0][MO_UW] = 0x3b, [0][MO_UL] = 0x36,
    [1][MO_UQ] = 0x37,
};

/* the fast arm of a guest load/store, factored out so a second check in
 * front of the inline probe can emit it too */
static void w64_ld_fast(TCGContext *s, TCGType type, TCGReg data,
                        MemOp mop, unsigned s_bits)
{
    w64_tlb_haddr(s, W64_L_SCR2);
    w64_u8(s, w64_fast_ld[type == TCG_TYPE_I64][mop & MO_SSIZE]);
    w64_memarg(s, s_bits, 0);
    if (type == TCG_TYPE_I32) {
        w64_set_i32(s, data);
    } else {
        w64_set_i64(s, data);
    }
}

static void w64_st_fast(TCGContext *s, TCGReg data, bool q, unsigned s_bits)
{
    w64_tlb_haddr(s, W64_L_SCR2);
    if (q) {
        w64_get_i64(s, data);
    } else {
        w64_get_i32(s, data);
    }
    w64_u8(s, w64_fast_st[q][s_bits]);
    w64_memarg(s, s_bits, 0);
}

static void tgen_qemu_ld(TCGContext *s, TCGType type, TCGReg data,
                         TCGReg addr, MemOpIdx oi)
{
    MemOp mop = get_memop(oi);
    uintptr_t ra = w64_pc(s);
    uint8_t p[4] = { 1, 1, 0, 1 };
    uint8_t imp, uimp, lt, ut;
    struct w64_tlb_ctx tc;
    bool fast;
    unsigned s_bits = mop & MO_SIZE;

    tcg_debug_assert(type == TCG_TYPE_I32 || type == TCG_TYPE_I64);
    tcg_debug_assert(s_bits <= MO_64);
    if (type == TCG_TYPE_I32) {
        tcg_debug_assert(s_bits <= MO_32);
    }
    w64_ir_memop(type == TCG_TYPE_I32 ? WI_QEMU_LD32 : WI_QEMU_LD64,
                 data, addr, oi, ra);

    /* helper signature: (env*, uint64_t addr, uint32_t oi, uintptr_t ra)
     * -> tcg_target_ulong (i64) */
    lt = w64_add_type(4, p, 1);
    ut = w64_union_type(4, p, 1);
    imp = w64_import_idx(
        (uint32_t)(uintptr_t)qemu_ld_helpers[mop & MO_SSIZE], lt);
    uimp = w64_union_import(
        (uint32_t)(uintptr_t)qemu_ld_helpers[mop & MO_SSIZE], ut);

    fast = w64_tlb_setup(&tc, s, oi, true);
    wasm_diag_stat[WASM_DIAG_LDST_GEN]++;
    if (!fast) {
        wasm_diag_stat[WASM_DIAG_LDST_GEN_NOPROBE]++;
    }
    if (w64_ldst_count() >= 2) {
        w64_diag_bump(s, WASM_DIAG_LDST_EXEC);
    }
    if (w64_ldst_pad()) {
        w64_emit_pad(s, w64_ldst_pad());
    }
    if (fast) {
        /* Snapshot the zero-extended address before the arms: with
         * data == addr (legal — e.g. ldrd's 64-bit load into the
         * register that also holds its address) an arm's local.set of
         * the data register would flip the tracked representation and
         * make the arm emitted second read a stale local. */
        w64_get_i64(s, addr);
        w64_local_set(s, W64_L_SCR2);
        w64_tlb_dup_emit(s, W64_L_SCR2, oi, true);
        w64_tlb_hit_emit(s, W64_L_SCR2, oi, true);
        w64_tlb_probe(s, W64_L_SCR2, &tc);
        w64_blk_push(s, W64_BLK_IF, -1);
        w64_ld_fast(s, type, data, mop, s_bits);
        w64_u8(s, 0x05);                    /* else */
    }

    /* miss (or probe disabled): the *_mmu helper (phase-1 path) */
    if (w64_ldst_count()) {
        w64_diag_bump(s, fast ? WASM_DIAG_LDST_MISS : WASM_DIAG_LDST_NOPROBE);
    }
    w64_local_get(s, W64_P_ENV);
    if (fast) {
        w64_local_get(s, W64_L_SCR2);       /* addr snapshot */
    } else {
        w64_get_i64(s, addr);               /* 32-bit guest addr: zext */
    }
    w64_const_i32(s, oi);
    w64_const_i64(s, ra);
    w64_u8(s, 0x10);
    w64_callidx(s, imp, uimp);
    if (type == TCG_TYPE_I32) {
        w64_u8(s, 0xa7);                    /* i32.wrap_i64 */
        w64_set_i32(s, data);
    } else {
        w64_set_i64(s, data);
    }

    if (fast) {
        w64_blk_pop(s);                     /* end (if/else) */
    }
}

static const TCGOutOpQemuLdSt outop_qemu_ld = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tgen_qemu_ld,
};

static void tgen_qemu_st(TCGContext *s, TCGType type, TCGReg data,
                         TCGReg addr, MemOpIdx oi)
{
    MemOp mop = get_memop(oi);
    uintptr_t ra = w64_pc(s);
    bool q = (mop & MO_SIZE) == MO_64;
    uint8_t p[5] = { 1, 1, q, 0, 1 };
    uint8_t imp, uimp, lt, ut;
    struct w64_tlb_ctx tc;
    bool fast;
    unsigned s_bits = mop & MO_SIZE;

    /* helper signature: (env*, uint64_t addr, val, uint32_t oi,
     * uintptr_t ra) -> void; val: i32 for b/w/l, i64 for q */
    w64_ir_memop(type == TCG_TYPE_I32 ? WI_QEMU_ST32 : WI_QEMU_ST64,
                 data, addr, oi, ra);
    lt = w64_add_type(5, p, 0xff);
    ut = w64_union_type(5, p, 0xff);
    imp = w64_import_idx(
        (uint32_t)(uintptr_t)qemu_st_helpers[s_bits], lt);
    uimp = w64_union_import(
        (uint32_t)(uintptr_t)qemu_st_helpers[s_bits], ut);

    fast = w64_tlb_setup(&tc, s, oi, false);
    wasm_diag_stat[WASM_DIAG_LDST_GEN]++;
    if (!fast) {
        wasm_diag_stat[WASM_DIAG_LDST_GEN_NOPROBE]++;
    }
    if (w64_ldst_count() >= 2) {
        w64_diag_bump(s, WASM_DIAG_LDST_EXEC);
    }
    if (w64_ldst_pad()) {
        w64_emit_pad(s, w64_ldst_pad());
    }
    if (fast) {
        w64_get_i64(s, addr);               /* snapshot (see ld above) */
        w64_local_set(s, W64_L_SCR2);
        w64_tlb_dup_emit(s, W64_L_SCR2, oi, false);
        w64_tlb_hit_emit(s, W64_L_SCR2, oi, false);
        w64_tlb_probe(s, W64_L_SCR2, &tc);
        w64_blk_push(s, W64_BLK_IF, -1);
        w64_st_fast(s, data, q, s_bits);
        w64_u8(s, 0x05);                    /* else */
    }

    /* miss (or probe disabled): the *_mmu helper (phase-1 path) */
    if (w64_ldst_count()) {
        w64_diag_bump(s, fast ? WASM_DIAG_LDST_MISS : WASM_DIAG_LDST_NOPROBE);
    }
    w64_local_get(s, W64_P_ENV);
    if (fast) {
        w64_local_get(s, W64_L_SCR2);       /* addr snapshot */
    } else {
        w64_get_i64(s, addr);
    }
    if (q) {
        w64_get_i64(s, data);
    } else {
        w64_get_i32(s, data);
    }
    w64_const_i32(s, oi);
    w64_const_i64(s, ra);
    w64_u8(s, 0x10);
    w64_callidx(s, imp, uimp);

    if (fast) {
        w64_blk_pop(s);                     /* end (if/else) */
    }
}

static const TCGOutOpQemuLdSt outop_qemu_st = {
    .base.static_constraint = C_O0_I2(r, r),
    .out = tgen_qemu_st,
};

static const TCGOutOpQemuLdSt2 outop_qemu_ld2 = {
    .base.static_constraint = C_NotImplemented,
};

static const TCGOutOpQemuLdSt2 outop_qemu_st2 = {
    .base.static_constraint = C_NotImplemented,
};

/* ------------------------------------------------------------------ */
/* arithmetic / logic outops                                           */
/* ------------------------------------------------------------------ */

#define W64_BINOP(NM, UP, OP32, OP64)                                         \
static void tgen_##NM(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1, TCGReg r2) \
{                                                                          \
    w64_ir_alu(t, WI_ALU_##UP, r0, r1, r2);                                \
    w64_binop_rrr(s, t, OP32, OP64, r0, r1, r2);                           \
}                                                                          \
static void tgen_##NM##i(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1,  \
                         tcg_target_long i2)                              \
{                                                                          \
    w64_ir_alui(t, WI_ALU_##UP, r0, r1, i2);                               \
    w64_binop_rri(s, t, OP32, OP64, r0, r1, i2);                           \
}                                                                          \
static const TCGOutOpBinary outop_##NM = {                                 \
    .base.static_constraint = C_O1_I2(r, r, rC),                           \
    .out_rrr = tgen_##NM,                                                  \
    .out_rri = tgen_##NM##i,                                               \
};

W64_BINOP(add, ADD, 0x6a, 0x7c)
W64_BINOP(mul, MUL, 0x6c, 0x7e)
W64_BINOP(and, AND, 0x71, 0x83)
W64_BINOP(or, OR,  0x72, 0x84)
W64_BINOP(xor, XOR, 0x73, 0x85)
W64_BINOP(shl, SHL, 0x74, 0x86)
W64_BINOP(shr, SHR, 0x76, 0x88)
W64_BINOP(sar, SAR, 0x75, 0x87)
W64_BINOP(rotl, ROTL, 0x77, 0x89)
W64_BINOP(rotr, ROTR, 0x78, 0x8a)
W64_BINOP(divs, DIVS, 0x6d, 0x7f)
W64_BINOP(divu, DIVU, 0x6e, 0x80)
W64_BINOP(rems, REMS, 0x6f, 0x81)
W64_BINOP(remu, REMU, 0x70, 0x82)

static void tgen_sub(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1, TCGReg r2)
{
    w64_ir_alu(t, WI_ALU_SUB, r0, r1, r2);
    w64_binop_rrr(s, t, 0x6b, 0x7d, r0, r1, r2);
}
static void tgen_subi(TCGContext *s, TCGType t, TCGReg r0, tcg_target_long i1,
                      TCGReg r2)
{
    /* constant first operand */
    w64_ir_subir(t, r0, i1, r2);
    w64_const(s, t, i1);
    w64_get(s, r2, t);
    w64_u8(s, t == TCG_TYPE_I32 ? 0x6b : 0x7d);   /* sub */
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, r0);
    } else {
        w64_set_i64(s, r0);
    }
}
static const TCGOutOpSubtract outop_sub = {
    .base.static_constraint = C_O1_I2(r, rC, r),
    .out_rrr = tgen_sub,
    .out_rir = tgen_subi,
};

/* a & ~b, a | ~b, ~(a^b), ~(a&b), ~(a|b) */
static void tgen_andc(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1, TCGReg r2)
{
    w64_ir_alu(t, WI_ALU_ANDC, r0, r1, r2);
    w64_get(s, r1, t);
    w64_get(s, r2, t);
    w64_const(s, t, -1);
    w64_u8(s, t == TCG_TYPE_I32 ? 0x73 : 0x85);   /* xor */
    w64_u8(s, t == TCG_TYPE_I32 ? 0x71 : 0x83);   /* and */
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, r0);
    } else {
        w64_set_i64(s, r0);
    }
}
static void tgen_andci(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1,
                       tcg_target_long i2)
{
    w64_ir_alui(t, WI_ALU_ANDC, r0, r1, i2);
    w64_get(s, r1, t);
    w64_const(s, t, ~i2);
    w64_u8(s, t == TCG_TYPE_I32 ? 0x71 : 0x83);   /* and */
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, r0);
    } else {
        w64_set_i64(s, r0);
    }
}
static const TCGOutOpBinary outop_andc = {
    .base.static_constraint = C_O1_I2(r, r, rC),
    .out_rrr = tgen_andc,
    .out_rri = tgen_andci,
};

static void tgen_orc(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1, TCGReg r2)
{
    w64_ir_alu(t, WI_ALU_ORC, r0, r1, r2);
    w64_get(s, r1, t);
    w64_get(s, r2, t);
    w64_const(s, t, -1);
    w64_u8(s, t == TCG_TYPE_I32 ? 0x73 : 0x85);   /* xor */
    w64_u8(s, t == TCG_TYPE_I32 ? 0x72 : 0x84);   /* or */
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, r0);
    } else {
        w64_set_i64(s, r0);
    }
}
static void tgen_orci(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1,
                      tcg_target_long i2)
{
    w64_ir_alui(t, WI_ALU_ORC, r0, r1, i2);
    w64_get(s, r1, t);
    w64_const(s, t, ~i2);
    w64_u8(s, t == TCG_TYPE_I32 ? 0x72 : 0x84);   /* or */
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, r0);
    } else {
        w64_set_i64(s, r0);
    }
}
static const TCGOutOpBinary outop_orc = {
    .base.static_constraint = C_O1_I2(r, r, rC),
    .out_rrr = tgen_orc,
    .out_rri = tgen_orci,
};

#define W64_NOTOP(NM, UP, OP32, OP64, INV32, INV64)                            \
static void tgen_##NM(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1, TCGReg r2) \
{                                                                          \
    w64_ir_alu(t, WI_ALU_##UP, r0, r1, r2);                                \
    w64_get(s, r1, t);                                                     \
    w64_get(s, r2, t);                                                     \
    w64_u8(s, t == TCG_TYPE_I32 ? OP32 : OP64);                            \
    w64_const(s, t, -1);                                                   \
    w64_u8(s, t == TCG_TYPE_I32 ? INV32 : INV64);                          \
    if (t == TCG_TYPE_I32) {                                               \
        w64_set_i32(s, r0);                                                \
    } else {                                                               \
        w64_set_i64(s, r0);                                                \
    }                                                                      \
}                                                                          \
static void tgen_##NM##i(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1,  \
                         tcg_target_long i2)                              \
{                                                                          \
    w64_ir_alui(t, WI_ALU_##UP, r0, r1, i2);                               \
    w64_get(s, r1, t);                                                     \
    w64_const(s, t, i2);                                                   \
    w64_u8(s, t == TCG_TYPE_I32 ? OP32 : OP64);                            \
    w64_const(s, t, -1);                                                   \
    w64_u8(s, t == TCG_TYPE_I32 ? INV32 : INV64);                          \
    if (t == TCG_TYPE_I32) {                                               \
        w64_set_i32(s, r0);                                                \
    } else {                                                               \
        w64_set_i64(s, r0);                                                \
    }                                                                      \
}                                                                          \
static const TCGOutOpBinary outop_##NM = {                                 \
    .base.static_constraint = C_O1_I2(r, r, rC),                           \
    .out_rrr = tgen_##NM,                                                  \
    .out_rri = tgen_##NM##i,                                               \
};

W64_NOTOP(eqv, EQV,  0x73, 0x85, 0x73, 0x85)   /* ~(a^b)  */
W64_NOTOP(nand, NAND, 0x71, 0x83, 0x73, 0x85)   /* ~(a&b)  */
W64_NOTOP(nor, NOR,  0x72, 0x84, 0x73, 0x85)   /* ~(a|b)  */

static void tgen_neg(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1)
{
    w64_ir_un(t, WI_UN_NEG, r0, r1);
    w64_const(s, t, 0);
    w64_get(s, r1, t);
    w64_u8(s, t == TCG_TYPE_I32 ? 0x6b : 0x7d);   /* sub */
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, r0);
    } else {
        w64_set_i64(s, r0);
    }
}
static const TCGOutOpUnary outop_neg = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_neg,
};

static void tgen_not(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1)
{
    w64_ir_un(t, WI_UN_NOT, r0, r1);
    w64_get(s, r1, t);
    w64_const(s, t, -1);
    w64_u8(s, t == TCG_TYPE_I32 ? 0x73 : 0x85);   /* xor */
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, r0);
    } else {
        w64_set_i64(s, r0);
    }
}
static const TCGOutOpUnary outop_not = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_not,
};

static void tgen_ctpop(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1)
{
    w64_ir_un(t, WI_UN_CTPOP, r0, r1);
    w64_get(s, r1, t);
    w64_u8(s, t == TCG_TYPE_I32 ? 0x69 : 0x7b);   /* popcnt */
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, r0);
    } else {
        w64_set_i64(s, r0);
    }
}
static const TCGOutOpUnary outop_ctpop = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_ctpop,
};

/* clz/ctz: wasm returns width for zero; TCG wants arg2 in that case */
#define W64_CNTOP(NM, UP, OP32, OP64)                                          \
static void tgen_##NM(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1, TCGReg r2) \
{                                                                          \
    w64_ir_alu(t, WI_ALU_##UP, r0, r1, r2);                                \
    int scr_ = w64_scr(t);                                                \
    w64_get(s, r1, t);                                                     \
    w64_u8(s, t == TCG_TYPE_I32 ? OP32 : OP64);                            \
    w64_local_set(s, scr_);                                                \
    w64_get(s, r1, t);                                                     \
    w64_u8(s, t == TCG_TYPE_I32 ? 0x45 : 0x50);   /* eqz */                \
    w64_u8(s, 0x04);                       /* if (result t) */            \
    w64_u8(s, t == TCG_TYPE_I32 ? 0x7f : 0x7e);                            \
    w64_get(s, r2, t);                                                     \
    w64_u8(s, 0x05);                       /* else */                     \
    w64_local_get(s, scr_);                                                \
    w64_u8(s, 0x0b);                       /* end */                      \
    if (t == TCG_TYPE_I32) {                                               \
        w64_set_i32(s, r0);                                                \
    } else {                                                               \
        w64_set_i64(s, r0);                                                \
    }                                                                      \
}                                                                          \
static void tgen_##NM##i(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1,  \
                         tcg_target_long i2)                              \
{                                                                          \
    w64_ir_alui(t, WI_ALU_##UP, r0, r1, i2);                               \
    int scr_ = w64_scr(t);                                                \
    w64_get(s, r1, t);                                                     \
    w64_u8(s, t == TCG_TYPE_I32 ? OP32 : OP64);                            \
    w64_local_set(s, scr_);                                                \
    w64_get(s, r1, t);                                                     \
    w64_u8(s, t == TCG_TYPE_I32 ? 0x45 : 0x50);   /* eqz */                \
    w64_u8(s, 0x04);                       /* if (result t) */            \
    w64_u8(s, t == TCG_TYPE_I32 ? 0x7f : 0x7e);                            \
    w64_const(s, t, i2);                                                   \
    w64_u8(s, 0x05);                       /* else */                     \
    w64_local_get(s, scr_);                                                \
    w64_u8(s, 0x0b);                       /* end */                      \
    if (t == TCG_TYPE_I32) {                                               \
        w64_set_i32(s, r0);                                                \
    } else {                                                               \
        w64_set_i64(s, r0);                                                \
    }                                                                      \
}                                                                          \
static const TCGOutOpBinary outop_##NM = {                                 \
    .base.static_constraint = C_O1_I2(r, r, rC),                           \
    .out_rrr = tgen_##NM,                                                  \
    .out_rri = tgen_##NM##i,                                               \
};

W64_CNTOP(clz, CLZ, 0x67, 0x79)
W64_CNTOP(ctz, CTZ, 0x68, 0x7a)

/* ------------------------------------------------------------------ */
/* setcond / movcond / brcond                                          */
/* ------------------------------------------------------------------ */

static void tgen_setcond(TCGContext *s, TCGType t, TCGCond c,
                         TCGReg r0, TCGReg r1, TCGReg r2)
{
    w64_ir_setcond(t, c, r0, r1, r2);
    w64_get(s, r1, t);
    w64_get(s, r2, t);
    w64_cmp_op(s, t, c);
    w64_set_i32(s, r0);      /* setcond result is always i32 */
}

static void tgen_setcondi(TCGContext *s, TCGType t, TCGCond c,
                          TCGReg r0, TCGReg r1, tcg_target_long i2)
{
    w64_ir_setcondi(t, c, r0, r1, i2);
    w64_get(s, r1, t);
    w64_const(s, t, i2);
    w64_cmp_op(s, t, c);
    w64_set_i32(s, r0);
}

static const TCGOutOpSetcond outop_setcond = {
    .base.static_constraint = C_O1_I2(r, r, rC),
    .out_rrr = tgen_setcond,
    .out_rri = tgen_setcondi,
};

static const TCGOutOpSetcond outop_negsetcond = {
    .base.static_constraint = C_NotImplemented,
};

static void tgen_movcond(TCGContext *s, TCGType t, TCGCond c, TCGReg ret,
                         TCGReg c1, TCGArg c2, bool const_c2,
                         TCGArg vt, bool const_vt, TCGArg vf, bool const_vf)
{
    w64_ir_movcond(t, c, ret, c1, c2, const_c2, vt, const_vt, vf, const_vf);
    w64_get(s, c1, t);
    if (const_c2) {
        w64_const(s, t, c2);
    } else {
        w64_get(s, c2, t);
    }
    w64_cmp_op(s, t, c);
    w64_u8(s, 0x04);                       /* if (result t) */
    w64_u8(s, t == TCG_TYPE_I32 ? 0x7f : 0x7e);
    if (const_vt) {
        w64_const(s, t, vt);
    } else {
        w64_get(s, vt, t);
    }
    w64_u8(s, 0x05);                       /* else */
    if (const_vf) {
        w64_const(s, t, vf);
    } else {
        w64_get(s, vf, t);
    }
    w64_u8(s, 0x0b);                       /* end */
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, ret);
    } else {
        w64_set_i64(s, ret);
    }
}

static const TCGOutOpMovcond outop_movcond = {
    .base.static_constraint = C_O1_I4(r, r, rC, rC, rC),
    .out = tgen_movcond,
};

static void tgen_brcond(TCGContext *s, TCGType t, TCGCond c,
                        TCGReg r1, TCGReg r2, TCGLabel *l)
{
    w64_ir_brcond(t, c, r1, r2, l);
    w64_get(s, r1, t);
    w64_get(s, r2, t);
    w64_cmp_op(s, t, c);
    w64_br_to_label(s, l, true);
}

static void tgen_brcondi(TCGContext *s, TCGType t, TCGCond c,
                         TCGReg r1, tcg_target_long i2, TCGLabel *l)
{
    w64_ir_brcondi(t, c, r1, i2, l);
    w64_get(s, r1, t);
    w64_const(s, t, i2);
    w64_cmp_op(s, t, c);
    w64_br_to_label(s, l, true);
}

static const TCGOutOpBrcond outop_brcond = {
    .base.static_constraint = C_O0_I2(r, rC),
    .out_rr = tgen_brcond,
    .out_ri = tgen_brcondi,
};

/* ------------------------------------------------------------------ */
/* mul2 / mul-high                                                     */
/* ------------------------------------------------------------------ */

static void tgen_mulu2(TCGContext *s, TCGType t, TCGReg a0, TCGReg a1,
                       TCGReg a2, TCGReg a3)
{
    w64_ir_mul2(WI_MULU2_32, a0, a1, a2, a3);
    tcg_debug_assert(t == TCG_TYPE_I32);
    w64_get_i64(s, a2);
    w64_get_i64(s, a3);
    w64_u8(s, 0x7e);                        /* i64.mul */
    w64_local_tee(s, W64_L_SCR0);
    w64_u8(s, 0xa7);                        /* i32.wrap_i64 */
    w64_set_i32(s, a0);
    w64_local_get(s, W64_L_SCR0);
    w64_const_i64(s, 32);
    w64_u8(s, 0x88);                        /* i64.shr_u */
    w64_u8(s, 0xa7);                        /* i32.wrap_i64 */
    w64_set_i32(s, a1);
}
static const TCGOutOpMul2 outop_mulu2 = {
    .base.static_constraint = C_O2_I2(r, r, r, r),
    .out_rrrr = tgen_mulu2,
};

static void tgen_muls2(TCGContext *s, TCGType t, TCGReg a0, TCGReg a1,
                       TCGReg a2, TCGReg a3)
{
    w64_ir_mul2(WI_MULS2_32, a0, a1, a2, a3);
    tcg_debug_assert(t == TCG_TYPE_I32);
    w64_get_i32(s, a2);
    w64_u8(s, 0xac);                        /* i64.extend_i32_s */
    w64_get_i32(s, a3);
    w64_u8(s, 0xac);                        /* i64.extend_i32_s */
    w64_u8(s, 0x7e);                        /* i64.mul */
    w64_local_tee(s, W64_L_SCR0);
    w64_u8(s, 0xa7);                        /* i32.wrap_i64 */
    w64_set_i32(s, a0);
    w64_local_get(s, W64_L_SCR0);
    w64_const_i64(s, 32);
    w64_u8(s, 0x88);                        /* i64.shr_u */
    w64_u8(s, 0xa7);                        /* i32.wrap_i64 */
    w64_set_i32(s, a1);
}
static const TCGOutOpMul2 outop_muls2 = {
    .base.static_constraint = C_O2_I2(r, r, r, r),
    .out_rrrr = tgen_muls2,
};

static void tgen_muluh(TCGContext *s, TCGType t, TCGReg a0, TCGReg a1, TCGReg a2)
{
    w64_ir_alu(t, WI_ALU_MULUH, a0, a1, a2);
    tcg_debug_assert(t == TCG_TYPE_I32);
    w64_get_i64(s, a1);
    w64_get_i64(s, a2);
    w64_u8(s, 0x7e);                        /* i64.mul */
    w64_const_i64(s, 32);
    w64_u8(s, 0x88);                        /* i64.shr_u */
    w64_u8(s, 0xa7);                        /* i32.wrap_i64 */
    w64_set_i32(s, a0);
}
static const TCGOutOpBinary outop_muluh = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_muluh,
};

static void tgen_mulsh(TCGContext *s, TCGType t, TCGReg a0, TCGReg a1, TCGReg a2)
{
    w64_ir_alu(t, WI_ALU_MULSH, a0, a1, a2);
    tcg_debug_assert(t == TCG_TYPE_I32);
    w64_get_i32(s, a1);
    w64_u8(s, 0xac);                        /* i64.extend_i32_s */
    w64_get_i32(s, a2);
    w64_u8(s, 0xac);                        /* i64.extend_i32_s */
    w64_u8(s, 0x7e);                        /* i64.mul */
    w64_const_i64(s, 32);
    w64_u8(s, 0x87);                        /* i64.shr_s */
    w64_u8(s, 0xa7);                        /* i32.wrap_i64 */
    w64_set_i32(s, a0);
}
static const TCGOutOpBinary outop_mulsh = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_mulsh,
};

/* ------------------------------------------------------------------ */
/* deposit / extract                                                   */
/* ------------------------------------------------------------------ */

static void w64_deposit_field(TCGContext *s, TCGType t,
                              TCGReg arg2, tcg_target_long c2, bool is_c,
                              unsigned ofs, unsigned len)
{
    uint64_t mask = (len >= 64 ? ~(uint64_t)0 : (UINT64_C(1) << len) - 1)
                   << ofs;
    tcg_target_long smask = (tcg_target_long)mask;

    if (is_c) {
        w64_const(s, t, (c2 << ofs) & smask);
        return;
    }
    w64_get(s, arg2, t);
    w64_const(s, t, ofs);
    w64_u8(s, t == TCG_TYPE_I32 ? 0x74 : 0x86);           /* shl */
    if (ofs + len < (unsigned)(t == TCG_TYPE_I32 ? 32 : 64)) {
        w64_const(s, t, smask);
        w64_u8(s, t == TCG_TYPE_I32 ? 0x71 : 0x83);       /* and */
    }
    /* ofs+len == width: the shift already placed the field, no mask */
}

static void tgen_deposit(TCGContext *s, TCGType t, TCGReg a0, TCGReg a1,
                         TCGReg a2, unsigned ofs, unsigned len)
{
    w64_ir_field(t == TCG_TYPE_I32 ? WI_DEPOSIT32 : WI_DEPOSIT64,
                 a0, a1, a2, ofs, len);
    uint64_t mask = (len >= 64 ? ~(uint64_t)0 : (UINT64_C(1) << len) - 1)
                   << ofs;

    w64_deposit_field(s, t, a2, 0, false, ofs, len);
    w64_local_set(s, w64_scr(t));
    w64_get(s, a1, t);
    w64_const(s, t, ~(tcg_target_long)mask);
    w64_u8(s, t == TCG_TYPE_I32 ? 0x71 : 0x83);           /* and */
    w64_local_get(s, w64_scr(t));       /* the typed scratch set above */
    w64_u8(s, t == TCG_TYPE_I32 ? 0x72 : 0x84);           /* or */
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, a0);
    } else {
        w64_set_i64(s, a0);
    }
}
static const TCGOutOpDeposit outop_deposit = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_deposit,
};

static void tgen_extract(TCGContext *s, TCGType t, TCGReg a0, TCGReg a1,
                         unsigned ofs, unsigned len)
{
    w64_ir_field(t == TCG_TYPE_I32 ? WI_EXTRACT32 : WI_EXTRACT64,
                 a0, a1, 0, ofs, len);
    unsigned width = t == TCG_TYPE_I32 ? 32 : 64;

    if (ofs == 0 && len == width) {
        tcg_out_mov(s, t, a0, a1);
        return;
    }
    w64_get(s, a1, t);
    if (ofs) {
        w64_const(s, t, ofs);
        w64_u8(s, t == TCG_TYPE_I32 ? 0x76 : 0x88);       /* shr_u */
    }
    if (ofs + len < width) {
        uint64_t mask = (UINT64_C(1) << len) - 1;
        w64_const(s, t, mask);
        w64_u8(s, t == TCG_TYPE_I32 ? 0x71 : 0x83);       /* and */
    }
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, a0);
    } else {
        w64_set_i64(s, a0);
    }
}
static const TCGOutOpExtract outop_extract = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_extract,
};

static void tgen_sextract(TCGContext *s, TCGType t, TCGReg a0, TCGReg a1,
                          unsigned ofs, unsigned len)
{
    w64_ir_field(t == TCG_TYPE_I32 ? WI_SEXTRACT32 : WI_SEXTRACT64,
                 a0, a1, 0, ofs, len);
    unsigned width = t == TCG_TYPE_I32 ? 32 : 64;

    w64_get(s, a1, t);
    if (ofs + len == width) {
        /* arithmetic shift alone sign-extends the field */
        w64_const(s, t, ofs);
        w64_u8(s, t == TCG_TYPE_I32 ? 0x75 : 0x87);       /* shr_s */
    } else {
        /* shift the field to the top, then arithmetic shift down */
        w64_const(s, t, width - ofs - len);
        w64_u8(s, t == TCG_TYPE_I32 ? 0x74 : 0x86);       /* shl */
        w64_const(s, t, width - len);
        w64_u8(s, t == TCG_TYPE_I32 ? 0x75 : 0x87);       /* shr_s */
    }
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, a0);
    } else {
        w64_set_i64(s, a0);
    }
}
static const TCGOutOpExtract outop_sextract = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_sextract,
};

static const TCGOutOpExtract2 outop_extract2 = {
    .base.static_constraint = C_NotImplemented,
};

/* ------------------------------------------------------------------ */
/* byte swaps                                                          */
/* ------------------------------------------------------------------ */

/* i32/i64 bswap16: swap the low 16 bits; flags govern the upper bits
 * (TCG_BSWAP_IZ is an input promise, not an output property). */
static void tgen_bswap16(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1,
                         unsigned flags)
{
    w64_ir_op(WI_BSWAP16, r0, r1, t != TCG_TYPE_I32);
    w64_ir_imm32(flags);
    unsigned top = t == TCG_TYPE_I32 ? 32 : 64;
    uint64_t lowmask = t == TCG_TYPE_I32 ? 0xffff : 0xffff;

    /* swapped low half */
    w64_get(s, r1, t);
    w64_const(s, t, 8);
    w64_u8(s, t == TCG_TYPE_I32 ? 0x76 : 0x88);           /* shr_u */
    w64_const(s, t, 0xff);
    w64_u8(s, t == TCG_TYPE_I32 ? 0x71 : 0x83);           /* and */
    w64_get(s, r1, t);
    w64_const(s, t, 0xff);
    w64_u8(s, t == TCG_TYPE_I32 ? 0x71 : 0x83);           /* and */
    w64_const(s, t, 8);
    w64_u8(s, t == TCG_TYPE_I32 ? 0x74 : 0x86);           /* shl */
    w64_u8(s, t == TCG_TYPE_I32 ? 0x72 : 0x84);           /* or */
    { int scr_ = w64_scr(t);
    w64_local_set(s, scr_);

    if (flags & TCG_BSWAP_OZ) {
        w64_local_get(s, scr_);
    } else if (flags & TCG_BSWAP_OS) {
        w64_local_get(s, scr_);
        w64_const(s, t, top - 16);
        w64_u8(s, t == TCG_TYPE_I32 ? 0x74 : 0x86);       /* shl */
        w64_const(s, t, top - 16);
        w64_u8(s, t == TCG_TYPE_I32 ? 0x75 : 0x87);       /* shr_s */
    } else {
        /* preserve input upper bits (default) */
        w64_local_get(s, scr_);
        w64_get(s, r1, t);
        w64_const(s, t, (tcg_target_long)~lowmask);
        w64_u8(s, t == TCG_TYPE_I32 ? 0x71 : 0x83);       /* and */
        w64_u8(s, t == TCG_TYPE_I32 ? 0x72 : 0x84);       /* or */
    }
    }
    if (t == TCG_TYPE_I32) {
        w64_set_i32(s, r0);
    } else {
        w64_set_i64(s, r0);
    }
}
static const TCGOutOpBswap outop_bswap16 = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_bswap16,
};

static void tgen_bswap32(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1,
                         unsigned flags)
{
    w64_ir_op(WI_BSWAP32, r0, r1, t != TCG_TYPE_I32);
    w64_ir_imm32(flags);
    unsigned top = t == TCG_TYPE_I32 ? 32 : 64;

    if (t == TCG_TYPE_I32) {
        /* t = rotr(x, 16); ret = (t & 0xff00ff00) >> 8 | (t & 0x00ff00ff) << 8 */
        w64_get_i32(s, r1);
        w64_const_i32(s, 16);
        w64_u8(s, 0x78);                    /* i32.rotr */
        w64_local_tee(s, W64_L_SCR32);
        w64_const_i32(s, (int32_t)0xff00ff00u);
        w64_u8(s, 0x71);                    /* and */
        w64_const_i32(s, 8);
        w64_u8(s, 0x76);                    /* shr_u */
        w64_local_get(s, W64_L_SCR32);
        w64_const_i32(s, 0x00ff00ff);
        w64_u8(s, 0x71);                    /* and */
        w64_const_i32(s, 8);
        w64_u8(s, 0x74);                    /* shl */
        w64_u8(s, 0x72);                    /* or */
        w64_set_i32(s, r0);
        return;
    }

    /* i64: swap the low 32 bits, upper 32 governed by flags.
     * bswap32(x) = adjacent-byte swap of the 16-bit-half swap, all in
     * the low 32 bits (kept as i64 ops; every intermediate stays
     * masked to 32 bits). */
    w64_get_i64(s, r1);
    w64_const_i64(s, 0x00000000ffffffffull);
    w64_u8(s, 0x83);                        /* and */
    w64_local_set(s, W64_L_SCR0);
    /* 16-bit half swap */
    w64_local_get(s, W64_L_SCR0);
    w64_const_i64(s, 16);
    w64_u8(s, 0x86);                        /* shl */
    w64_local_get(s, W64_L_SCR0);
    w64_const_i64(s, 16);
    w64_u8(s, 0x88);                        /* shr_u */
    w64_u8(s, 0x84);                        /* or */
    w64_const_i64(s, 0x00000000ffffffffull);
    w64_u8(s, 0x83);                        /* and */
    w64_local_set(s, W64_L_SCR0);
    /* two-mask adjacent-byte swap */
    w64_local_get(s, W64_L_SCR0);
    w64_const_i64(s, (int64_t)0xff00ff00u);
    w64_u8(s, 0x83);                        /* and */
    w64_const_i64(s, 8);
    w64_u8(s, 0x88);                        /* shr_u */
    w64_local_get(s, W64_L_SCR0);
    w64_const_i64(s, 0x00ff00ff);
    w64_u8(s, 0x83);                        /* and */
    w64_const_i64(s, 8);
    w64_u8(s, 0x86);                        /* shl */
    w64_u8(s, 0x84);                        /* or */
    w64_local_set(s, W64_L_SCR0);

    if (flags & TCG_BSWAP_OZ) {
        w64_local_get(s, W64_L_SCR0);
    } else if (flags & TCG_BSWAP_OS) {
        w64_local_get(s, W64_L_SCR0);
        w64_const_i64(s, top - 32);
        w64_u8(s, 0x86);                    /* shl */
        w64_const_i64(s, top - 32);
        w64_u8(s, 0x87);                    /* shr_s */
    } else {
        w64_local_get(s, W64_L_SCR0);
        w64_get_i64(s, r1);
        w64_const_i64(s, ~(int64_t)0xffffffffu);
        w64_u8(s, 0x83);                    /* and */
        w64_u8(s, 0x84);                    /* or */
    }
    w64_set_i64(s, r0);
}
static const TCGOutOpBswap outop_bswap32 = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_bswap32,
};

static void tgen_bswap64(TCGContext *s, TCGType t, TCGReg r0, TCGReg r1)
{
    w64_ir_op(WI_BSWAP64, r0, r1, 0);
    /* stage 1: bytes */
    w64_get_i64(s, r1);
    w64_const_i64(s, 0x00ff00ff00ff00ffull);
    w64_u8(s, 0x83);                        /* and */
    w64_const_i64(s, 8);
    w64_u8(s, 0x86);                        /* shl */
    w64_get_i64(s, r1);
    w64_const_i64(s, 8);
    w64_u8(s, 0x88);                        /* shr_u */
    w64_const_i64(s, 0x00ff00ff00ff00ffull);
    w64_u8(s, 0x83);                        /* and */
    w64_u8(s, 0x84);                        /* or */
    w64_local_set(s, W64_L_SCR0);

    /* stage 2: 16-bit halves */
    w64_local_get(s, W64_L_SCR0);
    w64_const_i64(s, 0x0000ffff0000ffffull);
    w64_u8(s, 0x83);                        /* and */
    w64_const_i64(s, 16);
    w64_u8(s, 0x86);                        /* shl */
    w64_local_get(s, W64_L_SCR0);
    w64_const_i64(s, 16);
    w64_u8(s, 0x88);                        /* shr_u */
    w64_const_i64(s, 0x0000ffff0000ffffull);
    w64_u8(s, 0x83);                        /* and */
    w64_u8(s, 0x84);                        /* or */
    w64_local_set(s, W64_L_SCR0);

    /* stage 3: 32-bit halves */
    w64_local_get(s, W64_L_SCR0);
    w64_const_i64(s, 32);
    w64_u8(s, 0x86);                        /* shl */
    w64_local_get(s, W64_L_SCR0);
    w64_const_i64(s, 32);
    w64_u8(s, 0x88);                        /* shr_u */
    w64_u8(s, 0x84);                        /* or */
    w64_set_i64(s, r0);
}
static const TCGOutOpUnary outop_bswap64 = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_bswap64,
};

/* ------------------------------------------------------------------ */
/* remaining outop tables                                              */
/* ------------------------------------------------------------------ */

static const TCGOutOpLoad outop_ld8u = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tcg_out_ld8u,
};
static const TCGOutOpLoad outop_ld8s = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tcg_out_ld8s,
};
static const TCGOutOpLoad outop_ld16u = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tcg_out_ld16u,
};
static const TCGOutOpLoad outop_ld16s = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tcg_out_ld16s,
};
static const TCGOutOpLoad outop_ld32u = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tcg_out_ld32u,
};
static const TCGOutOpLoad outop_ld32s = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tcg_out_ld32s,
};
static const TCGOutOpStore outop_st8 = {
    .base.static_constraint = C_O0_I2(r, r),
    .out_r = tcg_out_st8,
};
static const TCGOutOpStore outop_st16 = {
    .base.static_constraint = C_O0_I2(r, r),
    .out_r = tcg_out_st16,
};
static void tgen_sti(TCGContext *s, TCGType t, tcg_target_long v,
                     TCGReg b, intptr_t o)
{
    tcg_out_sti(s, t, v, b, o);
}

static const TCGOutOpStore outop_st = {
    .base.static_constraint = C_O0_I2(r, r),
    .out_r = tcg_out_st,
    .out_i = tgen_sti,
};

static void tgen_extrh_i64_i32(TCGContext *s, TCGType t, TCGReg a0, TCGReg a1)
{
    tcg_out_extrh_i64_i32(s, a0, a1);
}
static const TCGOutOpUnary outop_extrh_i64_i32 = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_extrh_i64_i32,
};

static const TCGOutOpBinary outop_addco = {
    .base.static_constraint = C_NotImplemented,
};
static const TCGOutOpBinary outop_addcio = {
    .base.static_constraint = C_NotImplemented,
};
static const TCGOutOpAddSubCarry outop_addci = {
    .base.static_constraint = C_NotImplemented,
};
static const TCGOutOpAddSubCarry outop_subbi = {
    .base.static_constraint = C_NotImplemented,
};
static const TCGOutOpAddSubCarry outop_subbio = {
    .base.static_constraint = C_NotImplemented,
};
static const TCGOutOpAddSubCarry outop_subbo = {
    .base.static_constraint = C_NotImplemented,
};
static const TCGOutOpDivRem outop_divs2 = {
    .base.static_constraint = C_NotImplemented,
};
static const TCGOutOpDivRem outop_divu2 = {
    .base.static_constraint = C_NotImplemented,
};

/* ------------------------------------------------------------------ */
/* module assembly                                                     */
/* ------------------------------------------------------------------ */

/*
 * True while something reads tb->icount as a statistic, charged once per
 * TB entry from the emitted prologue below.  A TB that can leave before
 * its last instruction over-charges those, so target/arm does not merge
 * a conditional branch's fall-through while any of them is armed.
 *
 * The lockstep fold is deliberately not in this set: it is a gate, and
 * excluding it would mean the gate never ran the shipping control flow.
 * Merged and unmerged 250 Mi lockstep runs return the same verdict, to
 * the digest -- the grid is TB-entry-keyed on both sides.
 */
bool w64_tb_icount_exact(void)
{
    return w64_tbhist_on() || w64_tbstats_inline() || icount2_enabled();
}

static void tcg_out_tb_start(TCGContext *s)
{
    static const uint8_t magic[8] = { 0, 'a', 's', 'm', 1, 0, 0, 0 };
    static const uint8_t p1[1] = { 0 };    /* (i32) */
    uint8_t sync_type = 0, sync_imp = 0, sync_uimp = 0;
    uint8_t ls_type = 0, ls_imp = 0, ls_uimp = 0;
    uint8_t pad_type = 0, pad_imp = 0, pad_uimp = 0;
    uint32_t tidx;
    unsigned nlabels = MIN((unsigned)s->nb_labels, W64_MAX_LABELS);

    /*
     * Only the counters and the live prefix of label_idx carry state
     * from the previous TB: fixup[], type[], imp[] and blk[] are all
     * bounded by their own counters and are written before they are
     * read.  Zeroing the whole struct was 11.6 KB of memset per
     * translation, against the ~10 label slots a TB actually uses.
     */
    memset(W.label_idx, 0, nlabels * sizeof(W.label_idx[0]));
    W.n_labels = 0;
    W.overflow = false;
    W.n_fixup = 0;
    W.n_types = 0;
    W.n_imp = 0;
    W.n_blk = 0;
    W.loop_idx = 0;
    W.nested = false;
    W.selfloop = false;
    W.rep = 0;
    n_cf = 0;
    w64_ir_reset(nlabels);
    w64_scan_labels(s);
    w64_batch_begin_tb();

    /* lazy one-time init of the inline-accounting address table
     * (translation of the first TB precedes w64_init/first exec) */
    if (w64_acct_addr[0] == 0) {
        w64_acct_init();
    }
    w64_acct_flags = (w64_tbhist_on() ? W64_ACCT_TBHIST : 0) |
                     (w64_tbstats_inline() ? W64_ACCT_TBSTATS : 0) |
                     (icount2_enabled() ? W64_ACCT_ICOUNT2 : 0) |
                     (w64_ls_armed() ? W64_ACCT_LS : 0);

    /* descriptor */
    stl_p(s->code_buf + W64_DESC_FIDX, 0);
    stl_p(s->code_buf + W64_DESC_MODLEN, 0);
    stl_p(s->code_buf + W64_DESC_ICOUNT, s->gen_tb->icount);
    stl_p(s->code_buf + W64_DESC_NIMP, 0);
    tidx = w64_alloc_tidx();
    stl_p(s->code_buf + W64_DESC_TIDX, tidx);

    /* module prelude reservation */
    memcpy(s->code_buf + W64_MOD_OFF, magic, sizeof(magic));

    /* type 0: the TB entry signature (i64, i64, i64) -> i32 — also the
     * call_indirect signature used by goto_tb chaining */
    {
        uint8_t p[3] = { 1, 1, 1 };
        w64_add_type(3, p, 0);
    }

    /* TB accounting (phase 3): the prologue performs the per-TB-entry
     * accounting INLINE and imports only the rare/conditional tails -
     * the icount2 deadline sync (opt-in ?icount=precise-clocks) and the
     * lockstep fold (W64_LOCKSTEP).  Registered first so their indices
     * are deterministic (the prologue's calls are the body's first
     * instructions). */
    if (icount2_enabled()) {
        sync_type = w64_add_type(0, (const uint8_t *)"", 0xff);  /* () -> () */
        sync_uimp = w64_union_type(0, NULL, 0xff);
        sync_imp = w64_import_idx(
            (uint32_t)(uintptr_t)w64_icount2_sync_now, sync_type);
        sync_uimp = w64_union_import(
            (uint32_t)(uintptr_t)w64_icount2_sync_now, sync_uimp);
    }
    if (w64_ls_armed()) {
        ls_type = w64_add_type(1, p1, 0xff);            /* (i32) -> () */
        ls_uimp = w64_union_type(1, p1, 0xff);
        ls_imp = w64_import_idx((uint32_t)(uintptr_t)w64_lockstep_account,
                                ls_type);
        ls_uimp = w64_union_import(
            (uint32_t)(uintptr_t)w64_lockstep_account, ls_uimp);
    }
    if (w64_call_pad()) {
        pad_type = w64_add_type(0, (const uint8_t *)"", 0xff);   /* () -> () */
        pad_uimp = w64_union_type(0, NULL, 0xff);
        pad_imp = w64_import_idx((uint32_t)(uintptr_t)w64_callpad_sink,
                                 pad_type);
        pad_uimp = w64_union_import(
            (uint32_t)(uintptr_t)w64_callpad_sink, pad_uimp);
    }

    /* body: [body-size LEB (patched)] [locals] [expr...] */
    s->code_ptr = s->code_buf + W64_BODY_OFF;
    w64_uleb32_p5(s, 0);
    w64_u8(s, w64_locals_pad() ? 0x05 : 0x04);   /* local runs */
    w64_uleb(s, TCG_TARGET_NB_REGS + 1);    /* i32: regs + $bp */
    w64_u8(s, 0x7f);
    w64_uleb(s, TCG_TARGET_NB_REGS);        /* i64: regs */
    w64_u8(s, 0x7e);
    w64_u8(s, 0x01); w64_u8(s, 0x7f);       /* i32: $scr32 */
    w64_u8(s, 0x03); w64_u8(s, 0x7e);       /* i64: $scr0..$scr2 */
    if (w64_locals_pad()) {
        w64_uleb(s, w64_locals_pad());
        w64_u8(s, 0x7e);
    }

    /* prologue: per-TB-entry accounting, inline */
    if (w64_tbhist_on()) {
        w64_tbhist_emit(s, tidx);
    }

    if (w64_tbstats_inline()) {
        /* wasm_tb_stats[0]++ and [1] += icount (plain RMWs — the fields
         * are only ever written by this vCPU thread) */
        w64_const_i64(s, (int64_t)w64_acct_addr[0]);
        w64_u8(s, 0x29);  w64_memarg(s, 3, 0);        /* i64.load */
        w64_const_i64(s, 1);
        w64_u8(s, 0x7c);                              /* i64.add */
        w64_local_set(s, W64_L_SCR0);
        w64_const_i64(s, (int64_t)w64_acct_addr[0]);
        w64_local_get(s, W64_L_SCR0);
        w64_u8(s, 0x37);  w64_memarg(s, 3, 0);        /* i64.store */

        w64_const_i64(s, (int64_t)w64_acct_addr[1]);
        w64_u8(s, 0x29);  w64_memarg(s, 3, 0);
        w64_const_i64(s, s->gen_tb->icount);
        w64_u8(s, 0x7c);
        w64_local_set(s, W64_L_SCR0);
        w64_const_i64(s, (int64_t)w64_acct_addr[1]);
        w64_local_get(s, W64_L_SCR0);
        w64_u8(s, 0x37);  w64_memarg(s, 3, 0);
    }

    if (icount2_enabled()) {
        /* icount2_advance fast path: ticks += icount — atomic accesses,
         * mirroring the qatomic_read/qatomic_set the C function emits
         * (timers_state is read/written cross-thread).  Only for the
         * opt-in icount2 (?icount=precise-clocks) timing model: the
         * default stock-icount boot never sets icount2_deadline, so an
         * atomic RMW + a deadline compare per TB entry (~5M/s) would be
         * pure overhead there.  icount2 is fixed at command-line parse
         * time, before the first translation. */
        w64_const_i64(s, (int64_t)w64_acct_addr[2]);
        w64_u8(s, 0xfe); w64_u8(s, 0x11);
        w64_memarg(s, 3, 0);                          /* i64.atomic.load */
        w64_const_i64(s, s->gen_tb->icount);
        w64_u8(s, 0x7c);
        w64_local_set(s, W64_L_SCR0);                /* new ticks */
        w64_const_i64(s, (int64_t)w64_acct_addr[2]);
        w64_local_get(s, W64_L_SCR0);
        w64_u8(s, 0xfe); w64_u8(s, 0x18);
        w64_memarg(s, 3, 0);                          /* i64.atomic.store */

        /* if (deadline > 0 && new_ticks >= deadline)
         *     w64_icount2_sync_now()   — the rare slow path */
        w64_const_i64(s, (int64_t)w64_acct_addr[3]);
        w64_u8(s, 0xfe); w64_u8(s, 0x11);
        w64_memarg(s, 3, 0);
        w64_local_tee(s, W64_L_SCR1);                /* deadline */
        w64_const_i64(s, 0);
        w64_u8(s, 0x55);                              /* i64.gt_s */
        w64_local_get(s, W64_L_SCR0);
        w64_local_get(s, W64_L_SCR1);
        w64_u8(s, 0x59);                              /* i64.ge_s */
        w64_u8(s, 0x71);                              /* i32.and */
        w64_blk_push(s, W64_BLK_IF, -1);
        w64_u8(s, 0x10);                              /* call */
        w64_callidx(s, sync_imp, sync_uimp);
        w64_blk_pop(s);
    }

    if (w64_ls_armed()) {
        /* if (w64_ls_on) w64_lockstep_account(icount) — the lockstep
         * fold import (w64_ls_on stays 0 when the log could not be
         * opened) */
        w64_const_i64(s, (int64_t)w64_acct_addr[4]);
        w64_u8(s, 0x28);  w64_memarg(s, 2, 0);        /* i32.load */
        w64_blk_push(s, W64_BLK_IF, -1);
        w64_const_i32(s, s->gen_tb->icount);
        w64_u8(s, 0x10);                              /* call */
        w64_callidx(s, ls_imp, ls_uimp);
        w64_blk_pop(s);
    }

    if (w64_call_pad()) {
        unsigned i;

        for (i = 0; i < w64_call_pad(); i++) {
            w64_u8(s, 0x10);                          /* call */
            w64_callidx(s, pad_imp, pad_uimp);
        }
    }

    if (w64_byte_pad()) {
        unsigned i;

        w64_local_get(s, W64_L_SCR32);
        w64_blk_push(s, W64_BLK_IF, -1);
        for (i = 0; i < w64_byte_pad(); i++) {
            w64_local_get(s, W64_L_SCR0);
            w64_const_i64(s, (int64_t)i);
            w64_u8(s, 0x7c);                          /* i64.add */
            w64_local_set(s, W64_L_SCR0);
        }
        w64_blk_pop(s);
    }

    w64_emit_loop_head(s);
}

/* small growable buffer for the prelude sections */
struct w64_buf {
    uint8_t b[W64_PRELUDE + 16];
    unsigned n;
    bool overflow;
};

static void wb_u8(struct w64_buf *w, uint8_t v)
{
    if (w->n >= sizeof(w->b)) {
        w->overflow = true;
        return;
    }
    w->b[w->n++] = v;
}

static void wb_bytes(struct w64_buf *w, const void *p, unsigned n)
{
    if (w->n + n > sizeof(w->b)) {
        w->overflow = true;
        return;
    }
    memcpy(w->b + w->n, p, n);
    w->n += n;
}

static void wb_uleb(struct w64_buf *w, uint64_t v)
{
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        wb_u8(w, v ? b | 0x80 : b);
    } while (v);
}

static int tcg_out_tb_finalize(TCGContext *s)
{
    struct w64_buf w;
    size_t body_total, body_size;
    unsigned i;

    if (W.overflow) {
        /* an emitter table filled up mid-translation: report "TB too
         * large" so tb_gen_code retries with half the insns instead of
         * emitting a corrupt module */
        return -2;
    }

    /* close the open label region, the dispatch loop, and the function:
     * the loop is void — falling off its end never happens (every region
     * ends in a branch or return), but the validator wants the function's
     * i32 result on the stack after it. */
    if (W.nested) {
        if (W.n_blk != 0) {
            return -2;
        }
    } else {
        if (W.n_blk != 2) {
            return -2;
        }
        w64_blk_pop(s);                    /* end: last region's if */
        w64_blk_pop(s);                    /* end: the loop */
    }
    w64_const_i32(s, 0);
    w64_u8(s, 0x0b);                       /* end: function expr */

    body_total = s->code_ptr - (s->code_buf + W64_BODY_OFF);
    body_size = body_total - 5;
    wasm_diag_stat[WASM_DIAG_TB_BYTES] += body_size;

    /* every forward branch must have been resolved by now */
    if (W.n_fixup != 0) {
        return -2;
    }

    /* patch the body-size LEB */
    {
        tcg_insn_unit *p = s->code_buf + W64_BODY_OFF;
        uint32_t v = body_size;
        int b;
        for (b = 0; b < 4; b++) {
            p[b] = (v & 0x7f) | 0x80;
            v >>= 7;
        }
        p[4] = v & 0x7f;
    }

    /* sections */
    w.n = 0;
    w.overflow = false;
    wb_bytes(&w, s->code_buf + W64_MOD_OFF, 8);         /* magic */

    /* type section */
    {
        unsigned start;
        wb_u8(&w, 0x01);                       /* section id */
        start = ++w.n;                         /* size slot */
        wb_uleb(&w, W.n_types);
        for (i = 0; i < W.n_types; i++) {
            unsigned j;
            wb_u8(&w, 0x60);
            wb_uleb(&w, W.type[i].np);
            for (j = 0; j < W.type[i].np; j++) {
                wb_u8(&w, W.type[i].p[j] ? 0x7e : 0x7f);
            }
            if (W.type[i].ret == 0xff) {
                wb_uleb(&w, 0);
            } else {
                wb_uleb(&w, 1);
                wb_u8(&w, W.type[i].ret ? 0x7e : 0x7f);
            }
        }
        w.b[start - 1] = w.n - start;
        if (w.n - start >= 128) {
            return -2;                  /* 1-byte section size overflow */
        }
    }

    /* import section: memory + shared chain table + helpers */
    {
        unsigned start;
        wb_u8(&w, 0x02);                       /* section id */
        start = ++w.n;                         /* size slot */
        wb_uleb(&w, W.n_imp + 2);
        wb_u8(&w, 1); wb_u8(&w, 'e');       /* module "e" */
        wb_u8(&w, 1); wb_u8(&w, 'm');       /* field  "m" */
        wb_u8(&w, 0x02);                    /* memory */
        wb_u8(&w, 0x07);                    /* 64-bit | shared | max */
        wb_uleb(&w, W64_MEM_PAGES);
        wb_uleb(&w, W64_MEM_PAGES);
        /* the shared chain table (goto_tb targets); min must not exceed
         * the actual table size at instantiation time */
        wb_u8(&w, 1); wb_u8(&w, 'e');       /* module "e" */
        wb_u8(&w, 1); wb_u8(&w, 't');       /* field  "t" */
        wb_u8(&w, 0x01);                    /* table (import kind) */
        wb_u8(&w, 0x70);                    /* funcref */
        wb_u8(&w, 0x00);                    /* limits: min only */
        wb_uleb(&w, 1);
        for (i = 0; i < W.n_imp; i++) {
            char name[8];
            int len = snprintf(name, sizeof(name), "f%u", i);
            wb_u8(&w, 1); wb_u8(&w, 'e');
            wb_u8(&w, len);
            wb_bytes(&w, name, len);
            wb_u8(&w, 0x00);                /* function */
            wb_uleb(&w, W.imp[i].type);
        }
        w.b[start - 1] = w.n - start;
        if (w.n - start >= 128) {
            return -2;                  /* 1-byte section size overflow */
        }
    }

    /* function section: one defined function, type 0 */
    wb_u8(&w, 0x03); wb_u8(&w, 0x02); wb_u8(&w, 0x01); wb_u8(&w, 0x00);

    /* export section: "tb" -> function index n_imp */
    {
        unsigned start;
        wb_u8(&w, 0x07);                       /* section id */
        start = ++w.n;                         /* size slot */
        wb_uleb(&w, 1);
        wb_u8(&w, 2); wb_u8(&w, 't'); wb_u8(&w, 'b');
        wb_u8(&w, 0x00);
        wb_uleb(&w, W.n_imp);
        w.b[start - 1] = w.n - start;
        if (w.n - start >= 128) {
            return -2;                  /* 1-byte section size overflow */
        }
    }

    /* custom-section filler + code section header, ending at BODY_OFF */
    {
        unsigned payload = 1 + body_total;  /* count + body */
        unsigned trailer = 1 + 5 + 1;       /* id + size LEB + count */
        unsigned target = W64_PRELUDE - trailer;
        unsigned room;

        if (w.n > target || target - w.n == 1 || target - w.n == 2) {
            /* cannot fill the gap with a well-formed custom section */
            return -2;
        }
        room = target - w.n;

        if (room > 0) {
            /* one custom section: id 0, size LEB, content = name + payload.
             * The size counts the content only (name length + payload).
             * NOTE: bytes are computed into locals BEFORE writing — an
             * emscripten -O3 inlining artifact was observed mangling the
             * reassigned `content` when the else-branch lands exactly on
             * content==127 (the emitted LEB said 255), so keep the value
             * flow trivially simple for the optimizer. */
            unsigned content, nleb;
            uint8_t b0, b1 = 0;
            if (room - 2 <= 127) {
                content = room - 2;
                b0 = (uint8_t)content;
                nleb = 1;
            } else {
                content = room - 3;
                if (content < 127 || content >= 16384) {
                    return -2;
                }
                b0 = (uint8_t)((content & 0x7f) | 0x80);
                b1 = (uint8_t)(content >> 7);
                nleb = 2;
            }
            wb_u8(&w, 0x00);               /* section id */
            wb_u8(&w, b0);
            if (nleb == 2) {
                wb_u8(&w, b1);
            }
            wb_u8(&w, 0);                  /* name length 0 */
            room -= 2 + nleb;
            while (room--) {
                wb_u8(&w, 0);
            }
        }
        if (w.n != target) {
            return -2;
        }

        wb_u8(&w, 0x0a);                    /* code section */
        w.n += 5;                           /* patched below */
        wb_u8(&w, 0x01);                    /* one body */

        if (w.overflow) {
            return -2;
        }
        memcpy(s->code_buf + W64_MOD_OFF, w.b, W64_PRELUDE);

        /* code section size (5-byte padded LEB) */
        {
            uint8_t *p = s->code_buf + W64_MOD_OFF + target + 1;
            uint32_t v = payload;
            int b;
            for (b = 0; b < 4; b++) {
                p[b] = (v & 0x7f) | 0x80;
                v >>= 7;
            }
            p[4] = v & 0x7f;
        }
    }

    /* import table follows the module */
    for (i = 0; i < W.n_imp; i++) {
        stl_p(s->code_ptr, W.imp[i].fptr);
        s->code_ptr += 4;
    }

    /* descriptor */
    stl_p(s->code_buf + W64_DESC_MODLEN, W64_PRELUDE + body_total);
    stl_p(s->code_buf + W64_DESC_NIMP, W.n_imp);

    /* join the open batch: may assemble + instantiate the batch module
     * right here (fill or union-table pressure) and flip this TB's
     * descriptor to the batch thunk (desc+4 = batch id, so the
     * dispatcher calls run(env, sp, tp, tidx) instead of the entry) */
    {
        /* W64_DUMPTB=N: hex of the Nth TB's complete temp module, so the
         * emitted bytes can be disassembled (wasm-dis) and counted. */
        static int dump = -2;
        if (dump == -2) {
            const char *e = getenv("W64_DUMPTB");
            dump = e ? atoi(e) : -1;
        }
        if (dump >= 0 &&
            wasm_diag_stat[WASM_DIAG_TB_GEN] == (uint64_t)dump) {
            uint32_t n = W64_PRELUDE + body_total;
            const uint8_t *b = s->code_buf + W64_MOD_OFF;
            char *h = g_malloc(2 * (size_t)n + 1);
            uint32_t i;

            for (i = 0; i < n; i++) {
                snprintf(h + 2 * i, 3, "%02x", b[i]);
            }
            fprintf(stderr, "W64TBDUMP icount=%u n=%u %s\n",
                    s->gen_tb->icount, n, h);
            g_free(h);
        }
    }

    w64_ir_finish(ldl_p(s->code_buf + W64_DESC_TIDX));
    w64_batch_member((uintptr_t)s->code_buf, body_total, CF, n_cf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* target boilerplate                                                  */
/* ------------------------------------------------------------------ */

static const int tcg_target_reg_alloc_order[] = {
    TCG_REG_R2, TCG_REG_R3, TCG_REG_R4, TCG_REG_R5,
    TCG_REG_R6, TCG_REG_R7, TCG_REG_R8, TCG_REG_R9,
    TCG_REG_R10, TCG_REG_R11, TCG_REG_R12,
    /* call-clobbered (helper results land here) — use last */
    TCG_REG_R0, TCG_REG_R1,
};

static const int tcg_target_call_iarg_regs[] = { };

static TCGReg tcg_target_call_oarg_reg(TCGCallReturnKind kind, int slot)
{
    tcg_debug_assert(kind == TCG_CALL_RET_NORMAL);
    tcg_debug_assert(slot >= 0 && slot < 128 / TCG_TARGET_REG_BITS);
    return TCG_REG_R0 + slot;
}

#ifdef CONFIG_DEBUG_TCG
static const char *const tcg_target_reg_names[TCG_TARGET_NB_REGS] = {
    "r00", "r01", "r02", "r03", "r04", "r05", "r06", "r07",
    "r08", "r09", "r10", "r11", "r12", "tmp", "env", "sp",
};
#endif

static bool tcg_target_const_match(int64_t val, int ct, TCGType type,
                                   TCGCond cond, int vece)
{
    return ct & TCG_CT_CONST;
}

static bool patch_reloc(tcg_insn_unit *code_ptr, int type,
                        intptr_t value, intptr_t addend)
{
    /* the backend never creates relocations */
    g_assert_not_reached();
}

static void tcg_out_nop_fill(tcg_insn_unit *p, int count)
{
    memset(p, 0x01, count);
}

static TCGConstraintSetIndex tcg_target_op_def(TCGOpcode op, TCGType type,
                                               unsigned flags)
{
    return C_NotImplemented;
}

bool tcg_target_has_memory_bswap(MemOp memop)
{
    return false;
}

static bool tcg_out_qemu_ld_slow_path(TCGContext *s, TCGLabelQemuLdst *l)
{
    g_assert_not_reached();
}

static bool tcg_out_qemu_st_slow_path(TCGContext *s, TCGLabelQemuLdst *l)
{
    g_assert_not_reached();
}

static void tcg_out_set_carry(TCGContext *s)
{
    g_assert_not_reached();
}

static void tcg_out_set_borrow(TCGContext *s)
{
    g_assert_not_reached();
}

static void tcg_target_init(TCGContext *s)
{
    w64_interp_init();
    tcg_target_available_regs[TCG_TYPE_I32] = MAKE_64BIT_MASK(0, 13);
    tcg_target_available_regs[TCG_TYPE_I64] = MAKE_64BIT_MASK(0, 13);

    /* wasm calls preserve locals; only the result registers change */
    tcg_target_call_clobber_regs = MAKE_64BIT_MASK(TCG_REG_R0, 2);

    s->reserved_regs = 0;
    tcg_regset_set_reg(s->reserved_regs, TCG_REG_TMP);
    tcg_regset_set_reg(s->reserved_regs, TCG_AREG0);
    tcg_regset_set_reg(s->reserved_regs, TCG_REG_CALL_STACK);

    /* call arguments come first, temp storage after */
    tcg_set_frame(s, TCG_REG_CALL_STACK, TCG_STATIC_CALL_ARGS_SIZE,
                  TCG_STATIC_FRAME_SIZE);
}

/* No emitted prologue: tcg_qemu_tb_exec is a C function (wasm64.c). */
static inline void tcg_target_qemu_prologue(TCGContext *s)
{
}
