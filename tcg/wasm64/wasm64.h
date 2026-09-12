/*
 * TCG wasm64 backend — shared definitions (tcg-target.c.inc + wasm64.c).
 *
 * Batching (phase 2): each translated TB stages its module bytes in the
 * code generation buffer behind a small descriptor:
 *
 *   tb_ptr +  0  u32  fidx    addFunction() index of the compiled entry
 *                              ("tb" export of the TB's temp module, or
 *                              the owning batch's "run" thunk once the
 *                              batch has landed), 0 = not yet compiled
 *   tb_ptr +  4  u32  mod_len module byte count starting at tb_ptr+W64_MOD_OFF;
 *                              the dispatcher clears it after the temp
 *                              module is instantiated, and the batch
 *                              landing overwrites it with the batch id
 *                              (non-zero = entry is the batch thunk)
 *   tb_ptr +  8  u32  icount  guest insns (w64_tb_account / icount2)
 *   tb_ptr + 12  u32  n_imp   import table entries following the module
 *   tb_ptr + 16  u32  tidx    index of this TB in the shared chain table
 *                              (assigned at translation; the entry is set
 *                              when the module is instantiated)
 *   tb_ptr + 20       module: [prelude (fixed W64_PRELUDE bytes)][body]
 *
 * A TB executes immediately after translation through a single-member
 * "temp" module (its fidx cached in the descriptor).  Simultaneously the
 * TB joins the open batch: its body bytes and per-TB import/type tables
 * are staged so that every N TBs (W64_BATCH_N, default 128) ONE batch
 * module can be assembled — union type/import tables (dedup across
 * members; each `call <imp>` operand is a fixed-width 2-byte LEB so the
 * batch assembler rewrites per-TB indices to union indices in place),
 * one function per member, one `run(env, sp, tp, tidx)` thunk exported
 * and addFunction'd, and one active element segment per member that
 * registers its function into the shared chain table at the member's
 * tidx.  When the batch lands: the thunk's fidx replaces the members'
 * temp fidxes in their descriptors (batch id in desc+4 tells the
 * dispatcher to call `run(env, sp, tp, tidx)` instead of the entry
 * directly), the temp modules are dropped (removeFunction + the element
 * segments overwrite their chain-table entries), and the per-TB
 * instances/modules become garbage-collectable.  Steady state holds
 * ~live_TBs/N instances instead of one per TB — the fix for the
 * renderer OOM wall at ~800k live TBs — and the per-TB instantiate
 * round-trip amortizes 128x.
 *
 * goto_tb chaining (phase 2): the chain slot is tb->jmp_target_addr[n]
 * itself — qemu core keeps it linked (target tb->tc.ptr) or reset
 * (tb->tc.ptr + jmp_reset_offset[n]), TCI-style; the emitted code reads
 * it at runtime and tail-calls the target through the shared funcref
 * table ("e"/"t") when linked, else falls through to the exit_tb that
 * follows.  TBs entered via a chain still run their prologue's TB
 * accounting (inline by default, phase 3: wasm_tb_stats + the icount2
 * fast path emitted directly, imports only for the deadline-crossed
 * sync and the lockstep fold; W64_NOACCTINLINE=1 reverts to the
 * imported w64_tb_account(icount)), so accounting and the lockstep
 * fold stay per-TB-entry exact.  The chain also refuses targets whose descriptor
 * fidx is 0 (evicted batch member — v1 only happens across tb_flush,
 * the check keeps a future LRU cap sound).
 *
 * The prelude holds the temp module's wasm sections (type/import/
 * function/export plus a custom-section filler) and ends in the code-
 * section header + body size, all at fixed offsets so that
 * gen_insn_end_off[] values (raw code offsets) line up with the
 * emitted stream.  The body starts at the fixed offset W64_BODY_OFF so
 * that qemu_ld/st retaddr values are final at emission time.
 *
 * The import table after the module holds one u32 per import: the
 * emscripten function-table index (= C function pointer) of the helper.
 * Module/field names for all imports are "e"/"m" (memory) and "e"/"fN".
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TCG_WASM64_H
#define TCG_WASM64_H

#define W64_DESC_FIDX     0
#define W64_DESC_MODLEN   4
#define W64_DESC_BATCH    4    /* alias: once the batch landed this holds
                                * W64_BATCH_TAG | batch id; until then it is
                                * mod_len (clearing W64_BATCH_TAG) */
#define W64_DESC_ICOUNT   8
#define W64_DESC_NIMP    12
#define W64_DESC_TIDX    16

#define W64_MOD_OFF      20
#define W64_PRELUDE     256
#define W64_BODY_OFF    (W64_MOD_OFF + W64_PRELUDE)

/* Exit-code flag: goto_ptr handoff.  The next TB's code pointer was
 * stored at [sp-8]; bit 31 is clear in every exit_tb value because the
 * wasm64 heap is 2GB. */
#define W64_EXIT_GOTOPTR 0x80000000u

/* desc+4 bit 31 once a batch landed: distinguishes the batch id from
 * the mod_len a not-yet-compiled temp module still carries there. */
#define W64_BATCH_TAG    0x80000000u

/* main linear memory: shared, memory64, 2GB (32768 pages), fixed */
#define W64_MEM_PAGES   32768

/* per-TB (temp module) table capacities */
#define W64_MAX_TYPES   12
#define W64_MAX_IMPORTS 24

/* per-batch union table capacities.  A batch closes early when either
 * union is within one TB's worth of new entries (W64_MAX_*) of full —
 * a single TB can add at most that many entries. */
#define W64_UMAX_TYPES   64
#define W64_UMAX_IMPORTS 192

/* default members per batch module; W64_BATCH_N env overrides
 * (clamped to 1..W64_BATCH_N_MAX; setting it to 1 exercises the whole
 * batch path with minimal batching), W64_NOBATCH=1 disables batching
 * entirely (pure per-TB temp modules). */
#define W64_BATCH_N_DEF 128
#define W64_BATCH_N_MAX 256

struct w64_type {
    uint8_t np;             /* number of params */
    uint8_t p[8];           /* 0 = i32, 1 = i64 */
    uint8_t ret;            /* 0xff = void */
};

struct w64_import {
    uint32_t fptr;          /* emscripten table index (C fn pointer) */
    uint8_t type;           /* type section index */
};

/* one `call <funcidx>` site: byte offset of the fixed-width 2-byte
 * index LEB within the TB's code buffer, and the call target's index
 * in the batch union import table (written at batch assembly). */
struct w64_cfix {
    uint32_t pos;
    uint16_t uimp;
};

/* runtime (wasm64.c), used by the emitters */
uint32_t w64_alloc_tidx(void);   /* next shared-chain-table index (>=1) */
void w64_tb_account(unsigned insns); /* per-TB-entry accounting import
                                       * (W64_NOACCTINLINE=1 fallback) */
extern uint32_t w64_chain_stop;  /* lockstep budget reached: stop chaining */

/* Inline TB-prologue accounting (phase 3): addresses of the state the
 * emitted prologue touches directly, read at translation time.
 * Order: [0]=wasm_tb_stats[0] (tbs), [1]=wasm_tb_stats[1] (insns),
 * [2]=&timers_state.icount2_ticks, [3]=&timers_state.icount2_deadline,
 * [4]=&w64_ls_on. */
extern uint64_t w64_acct_addr[5];

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
 * tests it via w64_acct_addr[4] to skip the import entirely when off. */
extern uint32_t w64_ls_on;

/* wasm_tb_stats as [tbs, insns] (ui/wasm.c); see w64_acct_addr above. */
extern uint64_t wasm_tb_stats[2];

/* batching API (wasm64.c) — called from tcg-target.c.inc */
uint8_t w64_union_type(unsigned np, const uint8_t *p, uint8_t ret);
uint8_t w64_union_import(uint32_t fptr, uint8_t utype);
void w64_batch_member(uintptr_t tcptr, uint32_t body_len,
                      const struct w64_cfix *cf, uint32_t ncf);
void w64_batch_flush(void);     /* tb_flush teardown */

#endif /* TCG_WASM64_H */
