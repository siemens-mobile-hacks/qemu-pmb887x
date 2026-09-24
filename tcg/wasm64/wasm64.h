/*
 * TCG wasm64 backend — shared definitions (tcg-target.c.inc + wasm64.c).
 *
 * Each translated TB is a descriptor followed by one wasm function body
 * in the code generation buffer:
 *
 *   tb_ptr +  0  u32  fidx    addFunction() index of the owning batch's
 *                              "run" thunk, 0 = not compiled (or evicted)
 *   tb_ptr +  4  u32  batch   W64_BATCH_TAG | batch id once the batch
 *                              module landed, else 0
 *   tb_ptr +  8  u32  icount  guest insns (prologue accounting)
 *   tb_ptr + 12  u32  tidx    index of this TB in the shared chain table
 *   tb_ptr + 16       body:   [size LEB (5 bytes)][locals][expr]
 *
 * The TB joins its thread's open batch at finalize.  A batch becomes one
 * module - union type and import tables, one function per member, one
 * `run(env, sp, tp, tidx)` thunk, one active element segment per member
 * registering it in the shared chain table at its tidx - when it fills,
 * when a union table is nearly full, or when one of its members is
 * executed as compiled code for the first time (until then the
 * interpreter tier runs it, w64-interp.c).
 *
 * goto_tb chaining: the chain slot is tb->jmp_target_addr[n] itself,
 * linked or reset by qemu core TCI-style; emitted code reads it at
 * runtime and tail-calls the target through the shared funcref table
 * ("e"/"t") when linked, else falls through to the exit_tb after it.
 * Chained entries run the target's prologue accounting like any other.
 *
 * Every `call <imp>` operand is an index into the batch's union import
 * table, whose entries are the helpers' emscripten function-table
 * indices (= C function pointers).  Module/field names are "e"/"m"
 * (memory), "e"/"t" (chain table) and "e"/"fN" (helpers).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TCG_WASM64_H
#define TCG_WASM64_H

#define W64_DESC_FIDX     0
#define W64_DESC_BATCH    4
#define W64_DESC_ICOUNT   8
#define W64_DESC_TIDX    12
/* accel/tcg builds a goto_ptr dispatch target out of these two, through
 * W64_TCP_FIDX / W64_TCP_TIDX in exec/translation-block.h (where the
 * encoding is documented); wasm64.c asserts the pairs agree. */

#define W64_BODY_OFF     16

/* Exit-code flag: goto_ptr handoff.  The next TB's code pointer was
 * stored at [sp-8]; bit 31 is clear in every exit_tb value because the
 * wasm64 heap is 2GB. */
#define W64_EXIT_GOTOPTR 0x80000000u

/* set in the batch word once the TB's batch module landed */
#define W64_BATCH_TAG    0x80000000u

/* main linear memory: shared, 2GB (32768 pages), fixed */
#define W64_MEM_PAGES   32768

/*
 * The emitted modules import the main module's memory, so they declare
 * the type emscripten linked it with: -sMEMORY64=2 (configure requires
 * --wasm64-32bit-address-limit) lowers it to a 32-bit index.
 */
#define W64_MEM_LIMITS  0x03            /* shared | max */

/* the most union types / imports a single TB may add to its batch */
#define W64_MAX_TYPES   12
#define W64_MAX_IMPORTS 24

/* per-batch union table capacities.  A batch closes early when either
 * union is within one TB's worth of new entries (W64_MAX_*) of full —
 * a single TB can add at most that many entries. */
#define W64_UMAX_TYPES   64
#define W64_UMAX_IMPORTS 192

/* Members per batch module.  With the interpreter tier a batch is closed
 * by a member reaching its promotion threshold long before it fills, so
 * the cap only ever cuts a batch short. */
#define W64_BATCH_N 1024

struct w64_type {
    uint8_t np;             /* number of params */
    uint8_t p[8];           /* 0 = i32, 1 = i64 */
    uint8_t ret;            /* 0xff = void */
};

struct w64_import {
    uint32_t fptr;          /* emscripten table index (C fn pointer) */
    uint8_t type;           /* type section index */
};

/*
 * A branch hint: @pos is the offset of an `if` / `br_if` opcode within
 * the TB's code buffer, and the batch module's metadata.code.branch_hint
 * section tells the engine which way it goes.  TurboFan then compiles
 * the other arm as deferred code, which is where its register allocator
 * puts the spills and reloads a call forces.
 */
struct w64_hint {
    uint32_t pos;
    bool likely;
};

/* shared chain table capacity: tidx is below this */
#define W64_TIDX_N   (1u << 21)

/* runtime (wasm64.c), used by the emitters */
uint32_t w64_alloc_tidx(void);   /* next shared-chain-table index (>=1) */
extern uint32_t w64_chain_stop;  /* lockstep budget reached: stop chaining */

/* Inline TB-prologue accounting: addresses of the state the emitted
 * prologue touches directly, read at translation time. */
enum {
    W64_ACCT_INSNS,         /* &wasm_guest_insns */
    W64_ACCT_TICKS,         /* &timers_state.icount2_ticks */
    W64_ACCT_DEADLINE,      /* &timers_state.icount2_deadline */
    W64_ACCT_LS_ON,         /* &w64_ls_on */
    W64_ACCT_N,
};
extern uint64_t w64_acct_addr[W64_ACCT_N];

/* One-time lazy fill of w64_acct_addr (called from the emitter's first
 * tcg_out_tb_start — translation of the first TB precedes first exec). */
void w64_acct_init(void);

/* Rare slow path from the inline prologue: the icount2 virtual-clock
 * deadline was crossed (mirrors icount2_advance's tail — BQL + sync). */
void w64_icount2_sync_now(void);

/* Lockstep fold accounting (emitted prologue calls it only when armed;
 * exported for the emitter's import registration). */
void w64_lockstep_account(unsigned insns);

/* 1 once w64_ls_init() armed the lockstep fold; the emitted prologue
 * tests it via w64_acct_addr[W64_ACCT_LS_ON] to skip the import when off. */
extern uint32_t w64_ls_on;

/* What the emitted TB prologue accounts for, published once by the
 * emitter so the interpreter tier can do the same work (w64-interp.c). */
#define W64_ACCT_GUEST_INSNS  2
#define W64_ACCT_ICOUNT2  4
#define W64_ACCT_LS       8
extern uint32_t w64_acct_flags;

/* batching API (wasm64.c) — called from tcg-target.c.inc */
void w64_batch_begin_tb(void);
uint8_t w64_union_type(unsigned np, const uint8_t *p, uint8_t ret);
uint8_t w64_union_import(uint32_t fptr, uint8_t utype);
bool w64_batch_tb_overflow(void);
void w64_batch_member(uintptr_t tcptr, const uint8_t *body,
                      uint32_t body_len, const struct w64_hint *h,
                      uint32_t nh);
void w64_batch_flush(void);     /* tb_flush teardown */

#endif /* TCG_WASM64_H */
