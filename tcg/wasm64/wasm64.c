/*
 * TCG wasm64 backend — runtime dispatch (tcg_qemu_tb_exec).
 *
 * Owns the per-vCPU call frame, the batch modules the translated TBs are
 * compiled into (see wasm64.h), and the dispatch loop that enters them.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "tcg/tcg.h"
#include "system/cpu-timers.h"
#include "qemu/main-loop.h"             /* bql_lock (w64_icount2_sync_now) */
#include "exec/cpu-common.h"
#include "exec/gdbstub.h"
#include "hw/core/cpu.h"
#include <emscripten.h>
#include "qemu/timer.h"
#include "exec/translation-block.h"
#include "wasm64.h"
#include "w64-interp.h"

QEMU_BUILD_BUG_ON(W64_TCP_FIDX != W64_DESC_FIDX);
QEMU_BUILD_BUG_ON(W64_TCP_TIDX != W64_DESC_TIDX);

__thread uintptr_t w64_tb_ptr;

/* per-thread state: frame + slot layout
 *   frame + 0 .. 8    goto_ptr handoff slot ([sp-8])
 *   frame + 16 ...    call frame ($sp = frame + 16)
 * Per thread because the pmb887x DSP JIT runs TBs through this dispatcher
 * on a thread of its own. */
static __thread uint8_t *w64_frame;

typedef uint32_t w64_run_fn(uintptr_t env, uintptr_t sp, uintptr_t tp,
                            uint32_t tidx);

static void w64_ls_init(void);

/* Rare slow path for the inline TB-prologue accounting: the icount2
 * virtual-clock deadline was crossed.  Mirrors icount2_advance()'s
 * tail exactly — the ticks themselves were already advanced inline. */
void w64_icount2_sync_now(void)
{
    bql_lock();
    icount2_sync();
    bql_unlock();
}

static void w64_init(void)
{
    static bool ls_armed;

    if (!w64_frame) {
        w64_frame = g_malloc0(16 + TCG_STATIC_CALL_ARGS_SIZE +
                              TCG_STATIC_FRAME_SIZE);
    }
    if (!ls_armed) {
        ls_armed = true;
        /* Eager lockstep init: an armed prologue tests w64_ls_on before
         * its import call, so the fold must be armed before the first
         * executed TB's prologue runs. */
        w64_ls_init();
    }
}

/* Addresses for the inline TB-prologue accounting (tcg_out_tb_start
 * reads these at translation time; see wasm64.h).  Filled lazily by
 * the emitter's first tcg_out_tb_start — wasm cannot fold integer casts
 * of addresses into static initializers, and translation of the first
 * TB happens before w64_init()/first exec. */
uint64_t w64_acct_addr[W64_ACCT_N];
uint32_t w64_acct_flags;

void w64_acct_init(void)
{
    uintptr_t ticks, deadline;

    icount2_w64_acct_addrs(&ticks, &deadline);
    w64_acct_addr[W64_ACCT_INSNS] = (uint64_t)(uintptr_t)&wasm_guest_insns;
    w64_acct_addr[W64_ACCT_TICKS] = (uint64_t)ticks;
    w64_acct_addr[W64_ACCT_DEADLINE] = (uint64_t)deadline;
    w64_acct_addr[W64_ACCT_LS_ON] = (uint64_t)(uintptr_t)&w64_ls_on;
}

/* ------------------------------------------------------------------ */
/* lockstep fold
 *
 * The wasm build cannot dlopen plugins, so the phase-0b fold
 * (tests/lockstep.c) is compiled in and driven by env vars instead of
 * plugin args.  With -accel tcg,one-insn-per-tb (how the gate boots both
 * legs) TB boundaries are insn boundaries, so counting in the dispatcher
 * before each TB executes produces exactly the plugin's executed-insn
 * grid — and samples see the same architectural state the plugin's
 * pre-insn callbacks see (globals synced back into env at TB end).
 * Log format is byte-identical to tests/lockstep.c so tools/lockstep.mjs
 * comparisons work unmodified. */

#define LS_FNV0     0xcbf29ce484222325ull
#define LS_MAX_MEM  8
#define LS_MAX_REGS 17

struct w64_ls_state {
    int stop;                   /* budget crossed: run this insn, then halt */
    uint64_t period, epoch, meminsns, limit;
    uint64_t from, to;          /* dense T-line window */
    uint64_t count;             /* executed insns, residual-kept */
    uint64_t samples;
    uint64_t regs, regs_all;
    struct { uint64_t addr, len; } mem[LS_MAX_MEM];
    int n_mem;
    FILE *f;
};
static struct w64_ls_state LS;

/* Lockstep armed (set once by w64_ls_init); the emitted TB prologues
 * test it through w64_acct_addr[W64_ACCT_LS_ON]. */
uint32_t w64_ls_on;

static uint64_t w64_ls_fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--) {
        h ^= *b++;
        h *= 0x100000001b3ull;
    }
    return h;
}

static void w64_ls_parse_mem(const char *spec)
{
    char *dup = g_strdup(spec), *p = dup;
    while (p && *p && LS.n_mem < LS_MAX_MEM) {
        char *colon = strchr(p, ':');
        char *plus;
        if (colon) {
            *colon = 0;
        }
        plus = strchr(p, '+');
        if (!plus) {
            break;
        }
        *plus = 0;
        LS.mem[LS.n_mem].addr = strtoull(p, NULL, 16);
        LS.mem[LS.n_mem].len = strtoull(plus + 1, NULL, 16);
        if (LS.mem[LS.n_mem].len) {
            LS.n_mem++;
        }
        p = colon ? colon + 1 : NULL;
    }
    g_free(dup);
}

/* gdb core-feature indices for the plugin's register list
 * (r0..r12, sp, lr, pc, cpsr) */
static const int w64_ls_regidx[LS_MAX_REGS] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 25,
};

static uint64_t w64_ls_mem_digest(int r)
{
    CPUState *cpu = current_cpu;
    static uint8_t *buf;
    static size_t bufcap;
    uint64_t h = w64_ls_fnv(LS_FNV0, &LS.mem[r].addr,
                            sizeof LS.mem[r].addr);
    uint64_t remaining = LS.mem[r].len;
    uint64_t addr = LS.mem[r].addr;
    bool ok = true;
    if (bufcap == 0) {
        bufcap = 1 << 20;
        buf = g_malloc(bufcap);
    }
    /* Read in 1 MB chunks: FNV-1a is streaming, so chunk boundaries do not
     * change the digest, and the read never exceeds the buffer. */
    while (remaining > 0) {
        size_t chunk = (remaining < bufcap) ? (size_t)remaining : bufcap;
        if (cpu_memory_rw_debug(cpu, addr, buf, chunk, 0) != 0) {
            ok = false;
            break;
        }
        h = w64_ls_fnv(h, buf, chunk);
        addr += chunk;
        remaining -= chunk;
    }
    if (!ok) {
        h = w64_ls_fnv(h, "UNMAPPED", 8);
    }
    return h;
}

static void w64_ls_sample(void)
{
    CPUState *cpu = current_cpu;
    uint64_t vals[LS_MAX_REGS];
    uint64_t idx = ++LS.samples;
    uint64_t insn_total = idx * LS.period;
    int i;

    for (i = 0; i < LS_MAX_REGS; i++) {
        /* gdb_get_reg32 appends — truncate between reads (see
         * tests/lockstep.c read_regs) */
        GByteArray *buf = g_byte_array_sized_new(8);
        uint64_t v = 0;
        if (!gdb_read_register(cpu, buf, w64_ls_regidx[i])) {
            v = 0xdeadbeefdeadbeefull;
        } else {
            memcpy(&v, buf->data, buf->len < 8 ? buf->len : 8);
        }
        g_byte_array_free(buf, TRUE);
        vals[i] = v;
    }

    {
        uint64_t h = w64_ls_fnv(LS.regs, &idx, sizeof idx);
        LS.regs = w64_ls_fnv(h, vals, sizeof vals);
    }

    if (LS.to > LS.from && idx >= LS.from && idx < LS.to) {
        fprintf(LS.f, "T %u %llu %016llx", cpu ? cpu->cpu_index : 0,
                (unsigned long long)insn_total,
                (unsigned long long)LS.regs);
        for (i = 0; i < LS_MAX_REGS; i++) {
            fprintf(LS.f, " %016llx", (unsigned long long)vals[i]);
        }
        fputc('\n', LS.f);
    }

    if (LS.epoch && insn_total % LS.epoch == 0) {
        /* Point-in-time hash of the register vector (matches tests/lockstep.c
         * E-line): robust to cross-backend transient hash fragility. */
        uint64_t ehash = w64_ls_fnv(LS_FNV0, &idx, sizeof idx);
        ehash = w64_ls_fnv(ehash, vals, sizeof vals);
        LS.regs_all = w64_ls_fnv(LS.regs_all, &ehash, sizeof ehash);
        fprintf(LS.f, "E %u %llu %llu %016llx\n",
                cpu ? cpu->cpu_index : 0,
                (unsigned long long)(insn_total / LS.epoch),
                (unsigned long long)insn_total,
                (unsigned long long)ehash);
        fflush(LS.f);
        LS.regs = LS_FNV0;
    }
    if (LS.meminsns && insn_total % LS.meminsns == 0) {
        fprintf(LS.f, "M %u %llu", cpu ? cpu->cpu_index : 0,
                (unsigned long long)insn_total);
        for (i = 0; i < LS.n_mem; i++) {
            fprintf(LS.f, " %016llx",
                    (unsigned long long)w64_ls_mem_digest(i));
        }
        fputc('\n', LS.f);
        fflush(LS.f);
    }

    if (LS.limit && insn_total >= LS.limit) {
        /* mirror the plugin's stop_at=: the in-flight insn still runs,
         * then the main loop exits cleanly (no emscripten exit() from
         * deep vCPU context — that trips the asyncify teardown). */
        fprintf(LS.f, "X %u %llu %llu %016llx\n",
                cpu ? cpu->cpu_index : 0,
                (unsigned long long)(LS.samples * LS.period),
                (unsigned long long)LS.samples,
                (unsigned long long)LS.regs_all);
        fflush(LS.f);
        LS.stop = 1;
    }
}

static void w64_ls_init(void)
{
    const char *e;

    if (!getenv("W64_LOCKSTEP")) {
        return;
    }
    w64_ls_on = 1;      /* emitted prologues: route to w64_lockstep_account */
    LS.period = 1ull << 16;
    LS.epoch = 1ull << 20;
    LS.meminsns = 1ull << 23;
    if ((e = getenv("W64_LOCKSTEP_PERIOD"))) {
        LS.period = strtoull(e, NULL, 0);
    }
    if ((e = getenv("W64_LOCKSTEP_EPOCH"))) {
        LS.epoch = strtoull(e, NULL, 0);
    }
    if ((e = getenv("W64_LOCKSTEP_MEMINSNS"))) {
        LS.meminsns = strtoull(e, NULL, 0);
    }
    if ((e = getenv("W64_LOCKSTEP_INSNS"))) {
        LS.limit = strtoull(e, NULL, 0);
    }
    if ((e = getenv("W64_LOCKSTEP_FROM"))) {
        LS.from = strtoull(e, NULL, 0);
    }
    if ((e = getenv("W64_LOCKSTEP_TO"))) {
        LS.to = strtoull(e, NULL, 0);
    }
    if (LS.to > LS.from) {
        LS.period = 1;         /* dense mode: sample every insn */
    }
    if ((e = getenv("W64_LOCKSTEP_MEM"))) {
        w64_ls_parse_mem(e);
    }
    if (!LS.n_mem) {
        LS.mem[0].addr = 0x800000;    /* pmb887x internal SRAM, 96k */
        LS.mem[0].len = 0x18000;
        LS.mem[1].addr = 0xa8000000;  /* SDRAM via EBU CS1, 16M */
        LS.mem[1].len = 0x1000000;
        LS.n_mem = 2;
    }
    LS.regs = LS_FNV0;
    LS.regs_all = LS_FNV0;
    LS.f = fopen("/lockstep.log", "w");
    if (!LS.f) {
        fprintf(stderr, "w64 lockstep: cannot open /lockstep.log\n");
        w64_ls_on = 0;
        return;
    }
    fprintf(LS.f, "C period=%llu epoch=%llu meminsns=%llu from=%llu to=%llu mem=",
            (unsigned long long)LS.period, (unsigned long long)LS.epoch,
            (unsigned long long)LS.meminsns,
            (unsigned long long)LS.from, (unsigned long long)LS.to);
    for (int i = 0; i < LS.n_mem; i++) {
        fprintf(LS.f, "%s%llx+%llx", i ? ":" : "",
                (unsigned long long)LS.mem[i].addr,
                (unsigned long long)LS.mem[i].len);
    }
    fputc('\n', LS.f);
    fflush(LS.f);
}

/* called from the emitted TB prologue while w64_ls_on */
void w64_lockstep_account(unsigned insns)
{
    LS.count += insns;
    while (LS.count >= LS.period) {
        LS.count -= LS.period;
        w64_ls_sample();
        if (LS.stop) {
            break;
        }
    }
    if (LS.stop) {
        /* budget crossed: suppress further chaining so the chain tail
         * unwinds to the dispatcher (which performs the clean exit)
         * instead of running guest insns past the budget — under
         * one-insn-per-tb this stops the leg exactly like the native
         * plugin's stop_at */
        w64_chain_stop = 1;
    }
}

/* Shared-chain-table index allocator: one index per translated TB,
 * recycled only at tb_flush (bounded by the live TB set; index 0 stays
 * unused so a zero/garbage read can never select a live entry). */
static uint32_t w64_next_tidx = 1;

uint32_t w64_alloc_tidx(void)
{
    return w64_next_tidx++;
}

/*
 * tb_gen_code() treats a nearly full chain table like a full code buffer:
 * the indices are recycled by tb_flush, and now that a TB costs the buffer
 * a few hundred bytes the table would otherwise run out first (w64-interp.c
 * drops the records of an index past W64_TIDX_N, which silently turns the
 * interpreter tier off).  A translation takes one index, a restarted one a
 * few more.
 */
bool w64_tidx_left(void)
{
    return w64_next_tidx < W64_TIDX_N - 16;
}

/* ------------------------------------------------------------------ */
/* batching                                                           */
/* ------------------------------------------------------------------ */

struct w64_member {
    uint32_t tcptr;         /* descriptor address (code buffer) */
    uint32_t body_len;      /* [size LEB][locals][expr] bytes */
    uint32_t hint_end;      /* hint arena entries below this are ours */
    uint8_t *body;          /* staged copy; freed when the batch lands */
};

static __thread struct {
    struct w64_type utype[W64_UMAX_TYPES];
    uint8_t n_utypes;
    struct w64_import uimp[W64_UMAX_IMPORTS];
    uint8_t n_uimp;
    struct w64_member member[W64_BATCH_N];
    uint16_t n_member;
    struct w64_hint *hint;  /* per-member branch hints, in member order */
    uint32_t n_hint, cap_hint;
    uint32_t id;            /* 0 = no batch open */
    /* union entries when the current translation started */
    uint8_t tb_utypes0, tb_uimp0;
    bool tb_overflow;       /* the translation added too many */
} B;

/* A batch as the assembler sees it: the open batch (B) or the copy kept
 * for a landed one. */
struct w64_bsrc {
    const struct w64_type *utype;
    const struct w64_import *uimp;
    const struct w64_member *member;
    const struct w64_hint *hint;
    unsigned n_utypes, n_uimp, n_member, n_hint;
    uint32_t id;
};

/* Landed batches, by id.  A landed batch keeps only its records — which
 * descriptors and chain-table entries are its members — for eviction.
 * The staged bodies are freed at landing, so an evicted member cannot be
 * re-instantiated: the dispatcher retires it and the TB is translated
 * afresh (w64_batch_retire).  The live set (instantiated modules) is a
 * FIFO capped at W64_LIVE_MAX — Firefox caps a process at ~16k live wasm
 * modules (64 KB of executable address space each). */
struct w64_landed {
    struct w64_bsrc src;
    uint32_t thunk;         /* run-thunk fidx; 0 = evicted */
    uint32_t *tidx;         /* member chain-table indices (eviction) */
    struct w64_landed *next; /* live FIFO */
};
static struct w64_landed **w64_landed_by_id;
static unsigned w64_landed_cap;
static struct w64_landed *w64_live_head, *w64_live_tail;
static unsigned w64_live_n;
static uint32_t w64_next_batch_id;

#define W64_LIVE_MAX        6144

static void w64_batch_open(void)
{
    B.n_utypes = 0;
    B.n_uimp = 0;
    B.n_member = 0;
    B.n_hint = 0;
    B.id = ++w64_next_batch_id;

    /* union type 0: the TB entry signature (i64, i64, i64) -> i32 —
     * goto_tb chaining call_indirects through it */
    B.utype[0].np = 3;
    B.utype[0].p[0] = 1;
    B.utype[0].p[1] = 1;
    B.utype[0].p[2] = 1;
    B.utype[0].ret = 0;
    B.n_utypes = 1;
}

static void w64_batch_close(void);

/*
 * Called at the start of every translation.  A batch that cannot take
 * another TB's worth of union entries closes first; an earlier attempt at
 * this translation (tb_gen_code retries with fewer insns) may have left
 * entries behind that no member uses.
 */
void w64_batch_begin_tb(void)
{
    if (B.id != 0 &&
        (B.n_uimp > W64_UMAX_IMPORTS - W64_MAX_IMPORTS - 1 ||
         B.n_utypes > W64_UMAX_TYPES - W64_MAX_TYPES - 1)) {
        if (B.n_member) {
            w64_batch_close();
        } else {
            B.id = 0;
        }
    }
    if (B.id == 0) {
        w64_batch_open();
    }
    B.tb_utypes0 = B.n_utypes;
    B.tb_uimp0 = B.n_uimp;
    B.tb_overflow = false;
}

bool w64_batch_tb_overflow(void)
{
    return B.tb_overflow;
}

uint8_t w64_union_type(unsigned np, const uint8_t *p, uint8_t ret)
{
    int i, j;

    for (i = 0; i < B.n_utypes; i++) {
        if (B.utype[i].np != np || B.utype[i].ret != ret) {
            continue;
        }
        for (j = 0; j < (int)np; j++) {
            if (B.utype[i].p[j] != p[j]) {
                break;
            }
        }
        if (j == (int)np) {
            return i;
        }
    }
    if (B.n_utypes - B.tb_utypes0 >= W64_MAX_TYPES) {
        B.tb_overflow = true;
        return 0;
    }
    B.utype[B.n_utypes].np = np;
    memcpy(B.utype[B.n_utypes].p, p, np);
    B.utype[B.n_utypes].ret = ret;
    return B.n_utypes++;
}

uint8_t w64_union_import(uint32_t fptr, uint8_t utype)
{
    int i;

    for (i = 0; i < B.n_uimp; i++) {
        if (B.uimp[i].fptr == fptr && B.uimp[i].type == utype) {
            return i;
        }
    }
    if (B.n_uimp - B.tb_uimp0 >= W64_MAX_IMPORTS) {
        B.tb_overflow = true;
        return 0;
    }
    B.uimp[B.n_uimp].fptr = fptr;
    B.uimp[B.n_uimp].type = utype;
    return B.n_uimp++;
}

/* growable byte buffer for module assembly */
struct w64_mb {
    uint8_t *b;
    size_t n, cap;
};

static void mb_put(struct w64_mb *m, const void *p, size_t n)
{
    if (m->n + n > m->cap) {
        m->cap = MAX(m->cap * 2, m->n + n + 256);
        m->b = g_realloc(m->b, m->cap);
    }
    memcpy(m->b + m->n, p, n);
    m->n += n;
}

static void mb_u8(struct w64_mb *m, uint8_t v)
{
    mb_put(m, &v, 1);
}

static void mb_uleb(struct w64_mb *m, uint64_t v)
{
    uint8_t t[10];
    unsigned k = 0;

    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        t[k++] = v ? (b | 0x80) : b;
    } while (v);
    mb_put(m, t, k);
}

static void mb_sleb32(struct w64_mb *m, int32_t v)
{
    uint8_t t[6];
    unsigned k = 0;
    bool more = true;

    while (more) {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) {
            more = false;
        } else {
            b |= 0x80;
        }
        t[k++] = b;
    }
    mb_put(m, t, k);
}

/* unsigned LEB padded to 5 bytes (patch-free sizing; value fits u32) */
static void mb_uleb_p5(struct w64_mb *m, uint32_t v)
{
    uint8_t t[5];
    int i;

    for (i = 0; i < 4; i++) {
        t[i] = (v & 0x7f) | 0x80;
        v >>= 7;
    }
    t[4] = v & 0x7f;
    mb_put(m, t, 5);
}

static void mb_sec(struct w64_mb *mod, uint8_t id, struct w64_mb *sec)
{
    mb_u8(mod, id);
    mb_uleb(mod, sec->n);
    mb_put(mod, sec->b, sec->n);
}

EM_JS(void, w64_remove, (int fidx), {
    removeFunction(fidx);
});

EM_JS(void, w64_tab_unset, (uintptr_t arr, uint32_t n), {
    const TAB = globalThis.__w64tab;
    const dv = new DataView(HEAPU8.buffer);
    const a = Number(arr);
    for (let i = 0; i < n; i++) {
        TAB.set(dv.getUint32(a + i * 4, true), null);
    }
});

EM_JS(void, w64_tab_clear, (void), {
    const TAB = globalThis.__w64tab;
    if (TAB) {
        for (let i = 0; i < TAB.length; i++) {
            TAB.set(i, null);
        }
    }
});

/* Instantiate a batch module.  The bytes are a full copy, not a view
 * (Firefox, https://bugzilla.mozilla.org/show_bug.cgi?id=1965217);
 * `ipp` points at the union import table (u32 C function pointers,
 * resolved through the main module's table).  The instance's active
 * element segments register every member into TAB at its tidx before
 * addFunction returns.  TAB is the shared chain table every module
 * imports as "e"/"t"; index 0 is never assigned. */
EM_JS(int, w64_batch_instantiate,
      (uintptr_t modp, uint32_t modlen, uintptr_t ipp, uint32_t nimp,
       uint32_t maxtidx), {
    /*
     * A DataView over HEAPU8.buffer, cached until the buffer identity
     * changes (emscripten replaces HEAPU8 on a memory growth, the only
     * event that can invalidate it).
     */
    let __v = globalThis.__w64v;
    if (__v === undefined || __v.b !== HEAPU8.buffer) {
        __v = globalThis.__w64v = { b: HEAPU8.buffer, dv: new DataView(HEAPU8.buffer) };
    }
    const dv = __v.dv;
    const mod_bytes = HEAPU8.slice(Number(modp), Number(modp) + Number(modlen));
    if (!globalThis.__w64tab) {
        globalThis.__w64tab = new WebAssembly.Table({ element: 'anyfunc', initial: 1 << 14 });
    }
    const TAB = globalThis.__w64tab;
    if (maxtidx >= TAB.length) {
        TAB.grow(Math.max(4096, maxtidx + 1 - TAB.length));
    }
    const ip = Number(ipp);
    const imports = { e: { m: wasmMemory, t: TAB } };
    for (let i = 0; i < nimp; i++) {
        imports.e['f' + i] = wasmTable.get(dv.getUint32(ip + i * 4, true));
    }
    let inst;
    try {
        inst = new WebAssembly.Instance(new WebAssembly.Module(mod_bytes), imports);
    } catch (e) {
        console.log('W64BATCHFAIL nimp=' + nimp + ' len=' + modlen + ': ' + e);
        throw e;
    }
    /*
     * GC nudge: SpiderMonkey does not count a module's executable memory
     * as GC pressure, so in a worker that never yields, dropped (evicted)
     * modules pile up until the ~16k-module executable budget is exhausted.
     * A throwaway 32 MB buffer every 256 instantiations is that pressure.
     * V8 does not need it, so only Firefox pays for it.
     */
    globalThis.__w64ninst = (globalThis.__w64ninst || 0) + 1;
    if (globalThis.__w64gc === undefined) {
        globalThis.__w64gc = navigator.userAgent.indexOf('Firefox') >= 0;
    }
    if (globalThis.__w64gc && (globalThis.__w64ninst & 255) === 0) {
        const junk = new ArrayBuffer(32 << 20);
        new Uint8Array(junk)[0] = 1;
    }
    return addFunction(inst.exports.run, 'jjjii');
});

/* the open batch, viewed as a source (B's arrays are inline) */
static struct w64_bsrc B_src;

static void w64_bsrc_of_open(void)
{
    B_src.utype = B.utype;
    B_src.uimp = B.uimp;
    B_src.member = B.member;
    B_src.hint = B.hint;
    B_src.n_utypes = B.n_utypes;
    B_src.n_uimp = B.n_uimp;
    B_src.n_member = B.n_member;
    B_src.n_hint = B.n_hint;
    B_src.id = B.id;
}

/* Assemble the batch module from @src and instantiate it; returns the
 * run-thunk fidx. */
static uint32_t w64_assemble_instantiate(const struct w64_bsrc *src)
{
    static const uint8_t magic[8] = { 0, 'a', 's', 'm', 1, 0, 0, 0 };
    /* thunk body: return_call_indirect TAB[tidx] with (env, sp, tp) */
    static const uint8_t thunk_body[14] = {
        13, 0,                                    /* size, no locals */
        0x20, 0, 0x20, 1, 0x20, 2, 0x20, 3,       /* local.get 0..3 */
        0x13, 0, 0,            /* return_call_indirect type 0, table 0 */
        0x0b,                                     /* end */
    };
    struct w64_mb mod = { 0 }, sec = { 0 };
    uint8_t name[8];
    uint32_t maxtidx = 0;
    uint32_t *ip;
    uint32_t thunk;
    unsigned m, i;
    unsigned nfn = src->n_member + 1;   /* members, then the thunk */
    mb_put(&mod, magic, sizeof(magic));

    /* type section: union types + the thunk signature */
    mb_uleb(&sec, src->n_utypes + 1);
    for (i = 0; i < src->n_utypes; i++) {
        unsigned j;
        mb_u8(&sec, 0x60);
        mb_uleb(&sec, src->utype[i].np);
        for (j = 0; j < src->utype[i].np; j++) {
            mb_u8(&sec, src->utype[i].p[j] ? 0x7e : 0x7f);
        }
        if (src->utype[i].ret == 0xff) {
            mb_uleb(&sec, 0);
        } else {
            mb_uleb(&sec, 1);
            mb_u8(&sec, src->utype[i].ret ? 0x7e : 0x7f);
        }
    }
    /* thunk: (i64 env, i64 sp, i64 tp, i32 tidx) -> i32 */
    mb_u8(&sec, 0x60);
    mb_uleb(&sec, 4);
    mb_u8(&sec, 0x7e);
    mb_u8(&sec, 0x7e);
    mb_u8(&sec, 0x7e);
    mb_u8(&sec, 0x7f);
    mb_uleb(&sec, 1);
    mb_u8(&sec, 0x7f);
    mb_sec(&mod, 1, &sec);

    /* import section: memory + shared chain table + union helpers */
    sec.n = 0;
    mb_uleb(&sec, src->n_uimp + 2);
    mb_u8(&sec, 1); mb_u8(&sec, 'e');
    mb_u8(&sec, 1); mb_u8(&sec, 'm');
    mb_u8(&sec, 0x02);                    /* memory */
    mb_u8(&sec, W64_MEM_LIMITS);
    mb_uleb(&sec, W64_MEM_PAGES);
    mb_uleb(&sec, W64_MEM_PAGES);
    mb_u8(&sec, 1); mb_u8(&sec, 'e');
    mb_u8(&sec, 1); mb_u8(&sec, 't');
    mb_u8(&sec, 0x01);                    /* table */
    mb_u8(&sec, 0x70);                    /* funcref */
    mb_u8(&sec, 0x00);                    /* limits: min only */
    mb_uleb(&sec, 1);
    for (i = 0; i < src->n_uimp; i++) {
        int len = snprintf((char *)name, sizeof(name), "f%u", i);
        mb_u8(&sec, 1); mb_u8(&sec, 'e');
        mb_u8(&sec, len);
        mb_put(&sec, name, len);
        mb_u8(&sec, 0x00);                /* function */
        mb_uleb(&sec, src->uimp[i].type);
    }
    mb_sec(&mod, 2, &sec);

    /* function section: every member has type 0, the TB signature */
    sec.n = 0;
    mb_uleb(&sec, nfn);
    for (i = 0; i + 1 < nfn; i++) {
        mb_uleb(&sec, 0);
    }
    mb_uleb(&sec, src->n_utypes);            /* the thunk's type index */
    mb_sec(&mod, 3, &sec);

    /* export section: "run" -> the thunk, which is always the last
     * defined function (indices count only the helper imports) */
    sec.n = 0;
    mb_uleb(&sec, 1);
    mb_u8(&sec, 3); mb_u8(&sec, 'r'); mb_u8(&sec, 'u'); mb_u8(&sec, 'n');
    mb_u8(&sec, 0x00);
    mb_uleb(&sec, src->n_uimp + nfn - 1);
    mb_sec(&mod, 7, &sec);

    /* element section: one active segment per member, TAB[tidx] = m */
    sec.n = 0;
    mb_uleb(&sec, src->n_member);
    for (m = 0; m < src->n_member; m++) {
        uint32_t tidx = ((const uint32_t *)(uintptr_t)src->member[m].tcptr)
                        [W64_DESC_TIDX / 4];
        maxtidx = MAX(maxtidx, tidx);
        mb_u8(&sec, 0x00);                /* active, table 0 */
        mb_u8(&sec, 0x41);                /* i32.const */
        mb_sleb32(&sec, (int32_t)tidx);
        mb_u8(&sec, 0x0b);                /* end */
        mb_uleb(&sec, 1);
        mb_uleb(&sec, src->n_uimp + m);
    }
    mb_sec(&mod, 9, &sec);

    /*
     * metadata.code.branch_hint (must precede the code section): per
     * member, the hinted if / br_if opcodes, as offsets from the start
     * of the function body — the byte after its size LEB.
     */
    {
        static const char hname[] = "metadata.code.branch_hint";
        unsigned nf = 0;

        sec.n = 0;
        mb_uleb(&sec, sizeof(hname) - 1);
        mb_put(&sec, hname, sizeof(hname) - 1);
        for (m = 0; m < src->n_member; m++) {
            uint32_t hs = m ? src->member[m - 1].hint_end : 0;
            nf += src->member[m].hint_end > hs;
        }
        mb_uleb(&sec, nf);
        for (m = 0; nf && m < src->n_member; m++) {
            uint32_t hs = m ? src->member[m - 1].hint_end : 0;

            if (src->member[m].hint_end == hs) {
                continue;
            }
            mb_uleb(&sec, src->n_uimp + m);
            mb_uleb(&sec, src->member[m].hint_end - hs);
            for (i = hs; i < src->member[m].hint_end; i++) {
                mb_uleb(&sec, src->hint[i].pos - W64_BODY_OFF - 5);
                mb_u8(&sec, 1);
                mb_u8(&sec, src->hint[i].likely);
            }
        }
        if (nf) {
            mb_sec(&mod, 0, &sec);
        }
    }

    /* code section: the staged bodies + the thunk; the total size is a
     * padded 5-byte LEB so it never shifts when the body sum crosses a
     * LEB width boundary */
    {
        uint64_t total = (nfn < 128 ? 1 : 2) + sizeof(thunk_body);

        for (m = 0; m < src->n_member; m++) {
            total += src->member[m].body_len;
        }
        mb_u8(&mod, 10);
        mb_uleb_p5(&mod, (uint32_t)total);
        mb_uleb(&mod, nfn);
        for (m = 0; m < src->n_member; m++) {
            mb_put(&mod, src->member[m].body, src->member[m].body_len);
        }
        mb_put(&mod, thunk_body, sizeof(thunk_body));
    }

    /* union import table for the JS resolver */
    ip = g_malloc_n(src->n_uimp ? src->n_uimp : 1, sizeof(uint32_t));
    for (i = 0; i < src->n_uimp; i++) {
        ip[i] = src->uimp[i].fptr;
    }

    thunk = w64_batch_instantiate((uintptr_t)mod.b, mod.n, (uintptr_t)ip,
                                  src->n_uimp, maxtidx);
    tcg_debug_assert(thunk != 0);
    g_free(mod.b);
    g_free(sec.b);
    g_free(ip);
    return thunk;
}

/* Drop the oldest live batch: its members fall back to the dispatcher
 * (fidx 0 + batch tag), which retires them. */
static void w64_batch_evict_oldest(void)
{
    struct w64_landed *l = w64_live_head;
    CPUState *cpu;
    unsigned m;

    if (!l) {
        return;
    }
    w64_live_head = l->next;
    if (!w64_live_head) {
        w64_live_tail = NULL;
    }
    l->next = NULL;
    w64_live_n--;

    for (m = 0; m < l->src.n_member; m++) {
        uint32_t *desc = (uint32_t *)(uintptr_t)l->src.member[m].tcptr;
        TranslationBlock *tb;

        /* a descriptor re-used by a later translation carries another
         * batch id by now: leave it alone */
        if (desc[W64_DESC_BATCH / 4] != (W64_BATCH_TAG | l->src.id)) {
            continue;
        }
        desc[W64_DESC_FIDX / 4] = 0;
        /*
         * A linked goto_tb slot holds this member's chain-table index and
         * nothing else (exec/translation-block.h), so clearing fidx no
         * longer stops the chain: drop the incoming jumps instead.  They
         * are re-linked the next time each source runs.
         */
        tb = tcg_tb_lookup(l->src.member[m].tcptr);
        if (tb) {
            tb_w64_unlink_incoming(tb);
        }
    }
    w64_tab_unset((uintptr_t)l->tidx, l->src.n_member);
    w64_remove((int)l->thunk);
    l->thunk = 0;

    /*
     * A goto_ptr inline-cache slot holds a table index too, and has no
     * back-reference at all, so retire every slot at once — that is what
     * the generation is for.  A boot stays an order of magnitude below
     * W64_LIVE_MAX (~450-760 live modules against 6144), so neither this
     * nor the unlink above has been observed to run.
     */
    CPU_FOREACH(cpu) {
        cpu_tb_key_gen_bump(cpu);
    }
}

static void w64_live_push(struct w64_landed *l)
{
    l->next = NULL;
    if (w64_live_tail) {
        w64_live_tail->next = l;
    } else {
        w64_live_head = l;
    }
    w64_live_tail = l;
    w64_live_n++;
    while (w64_live_n > W64_LIVE_MAX) {
        w64_batch_evict_oldest();
    }
}

/*
 * Evicted member executed again.  Its module is gone and so is the staged
 * body, so the TB is retired and the loop translates the address afresh,
 * the way cpu_io_recompile drops a TB.  Nothing can still reach the old
 * one: eviction unlinked its incoming chains and retired the goto_ptr
 * inline caches, and the dispatcher is the only other way in.
 */
static G_NORETURN void w64_batch_retire(CPUArchState *env, uintptr_t tcptr)
{
    TranslationBlock *tb = tcg_tb_lookup(tcptr);

    if (!tb) {
        fprintf(stderr, "w64: evicted TB %#x is not known\n",
                (unsigned)tcptr);
        abort();
    }
    tb_w64_retire(env_cpu(env), tb);
}

static void w64_landed_free_all(void)
{
    unsigned i;
    for (i = 0; i < w64_landed_cap; i++) {
        struct w64_landed *l = w64_landed_by_id[i];
        if (l) {
            g_free((void *)l->src.utype);
            g_free((void *)l->src.uimp);
            g_free((void *)l->src.member);
            g_free((void *)l->src.hint);
            g_free(l->tidx);
            g_free(l);
            w64_landed_by_id[i] = NULL;
        }
    }
    w64_live_head = w64_live_tail = NULL;
    w64_live_n = 0;
}

static void w64_landed_register(struct w64_landed *l)
{
    if (l->src.id >= w64_landed_cap) {
        unsigned ncap = MAX(w64_landed_cap * 2, l->src.id + 1024);
        w64_landed_by_id = g_renew(struct w64_landed *, w64_landed_by_id, ncap);
        memset(w64_landed_by_id + w64_landed_cap, 0,
               (ncap - w64_landed_cap) * sizeof(*w64_landed_by_id));
        w64_landed_cap = ncap;
    }
    w64_landed_by_id[l->src.id] = l;
}

static void w64_batch_close(void)
{
    struct w64_landed *l;
    uint32_t thunk;
    unsigned m;

    w64_bsrc_of_open();
    thunk = w64_assemble_instantiate(&B_src);

    /* the module holds the code now */
    for (m = 0; m < B.n_member; m++) {
        g_free(B.member[m].body);
        B.member[m].body = NULL;
    }

    /* landed record: compact copies of the open batch's tables */
    l = g_new0(struct w64_landed, 1);
    l->src.utype = g_memdup2(B.utype, B.n_utypes * sizeof(*B.utype));
    l->src.uimp = g_memdup2(B.uimp, B.n_uimp * sizeof(*B.uimp));
    l->src.member = g_memdup2(B.member, B.n_member * sizeof(*B.member));
    l->src.hint = g_memdup2(B.hint, B.n_hint * sizeof(*B.hint));
    l->src.n_utypes = B.n_utypes;
    l->src.n_uimp = B.n_uimp;
    l->src.n_member = B.n_member;
    l->src.n_hint = B.n_hint;
    l->src.id = B.id;
    l->thunk = thunk;
    l->tidx = g_new(uint32_t, B.n_member);

    /* land: point every member at the batch thunk */
    for (m = 0; m < B.n_member; m++) {
        uint32_t *desc = (uint32_t *)(uintptr_t)B.member[m].tcptr;
        desc[W64_DESC_FIDX / 4] = thunk;
        desc[W64_DESC_BATCH / 4] = W64_BATCH_TAG | B.id;
        l->tidx[m] = desc[W64_DESC_TIDX / 4];
        w64_irec_drop(l->tidx[m]);
    }

    w64_landed_register(l);
    w64_live_push(l);
    B.id = 0;
}

void w64_batch_member(uintptr_t tcptr, const uint8_t *body,
                      uint32_t body_len, const struct w64_hint *h,
                      uint32_t nh)
{
    tcg_debug_assert(B.id != 0 && B.n_member < W64_BATCH_N);

    if (B.n_hint + nh > B.cap_hint) {
        B.cap_hint = MAX(B.cap_hint * 2, B.n_hint + nh + 64);
        B.hint = g_realloc(B.hint, (size_t)B.cap_hint * sizeof(*B.hint));
    }
    memcpy(B.hint + B.n_hint, h, (size_t)nh * sizeof(*h));
    B.n_hint += nh;

    B.member[B.n_member].tcptr = (uint32_t)tcptr;
    B.member[B.n_member].body_len = body_len;
    B.member[B.n_member].hint_end = B.n_hint;
    B.member[B.n_member].body = g_memdup2(body, body_len);
    B.n_member++;

    /* close on fill, or when a union table is within one TB's worth of
     * new entries (a TB adds at most W64_MAX_* of each) of full */
    if (B.n_member >= W64_BATCH_N ||
        B.n_uimp > W64_UMAX_IMPORTS - W64_MAX_IMPORTS - 1 ||
        B.n_utypes > W64_UMAX_TYPES - W64_MAX_TYPES - 1) {
        w64_batch_close();
    }
}

/*
 * Withdraw the member staged for @tcptr: tb_gen_code() generated the TB
 * (so tcg_out_tb_finalize already staged it) and then abandoned it
 * without advancing code_gen_ptr — encode_search() running past the
 * region's highwater does exactly that.  The next tcg_tb_alloc() then
 * carves a TranslationBlock out of the very bytes this member's
 * descriptor address names, and landing the batch would write that TB's
 * fidx into it.
 *
 * Staging is synchronous, so the abandoned TB is always the last member.
 * If staging already closed the batch, its landed record names a
 * descriptor another TB now owns; eviction checks the batch word before
 * touching one.
 */
void w64_batch_unstage(uintptr_t tcptr)
{
    if (B.id == 0 || B.n_member == 0 ||
        B.member[B.n_member - 1].tcptr != (uint32_t)tcptr) {
        return;
    }
    B.n_hint = B.n_member > 1 ? B.member[B.n_member - 2].hint_end : 0;
    B.n_member--;
    g_free(B.member[B.n_member].body);
    B.member[B.n_member].body = NULL;
}

/*
 * First execution of a TB as compiled code: it is still staged in this
 * thread's open batch (every TB is translated and run by one thread), so
 * assemble + compile the whole batch now.
 */
static void w64_batch_close_pending(uintptr_t tcptr)
{
    unsigned m;

    for (m = 0; B.id != 0 && m < B.n_member; m++) {
        if (B.member[m].tcptr == (uint32_t)tcptr) {
            w64_batch_close();
            return;
        }
    }
    fprintf(stderr, "w64: TB %#x is neither compiled nor staged\n",
            (unsigned)tcptr);
    abort();
}

/* tb_flush teardown: drop every landed batch thunk and the open batch's
 * staged bodies, clear the chain table and recycle the tidx space (the
 * code buffer — the descriptors — is freed by the caller). */
void w64_batch_flush(void)
{
    struct w64_landed *l;
    unsigned m;

    w64_irec_flush();
    for (l = w64_live_head; l; l = l->next) {
        w64_remove((int)l->thunk);
        l->thunk = 0;
    }
    w64_landed_free_all();
    w64_tab_clear();
    w64_next_tidx = 1;
    for (m = 0; m < B.n_member; m++) {
        g_free(B.member[m].body);
        B.member[m].body = NULL;
    }
    B.n_member = 0;
    B.id = 0;
}

/* Set when the lockstep budget is crossed: emitted goto_tb code re-reads
 * this at runtime and refuses to chain, unwinding to the dispatcher. */
uint32_t w64_chain_stop;

uintptr_t QEMU_DISABLE_CFI tcg_qemu_tb_exec(CPUArchState *env,
                                            const void *v_tb_ptr)
{
    uintptr_t tb = (uintptr_t)v_tb_ptr;

    w64_init();

    for (;;) {
        uint32_t *desc = (uint32_t *)tb;
        uint32_t fidx, res;

        if (desc[W64_DESC_FIDX / 4] == 0 &&
            w64_interp_try(desc[W64_DESC_TIDX / 4],
                           desc[W64_DESC_ICOUNT / 4], (uintptr_t)env,
                           (uintptr_t)(w64_frame + 16),
                           (uintptr_t)&w64_tb_ptr, &res)) {
            goto exited;
        }

        fidx = desc[W64_DESC_FIDX / 4];
        if (fidx == 0) {
            if (desc[W64_DESC_BATCH / 4] & W64_BATCH_TAG) {
                w64_batch_retire(env, tb);      /* evicted: does not return */
            }
            w64_batch_close_pending(tb);
            fidx = desc[W64_DESC_FIDX / 4];
            tcg_debug_assert(fidx != 0);
        }

        /* the batch's run thunk tail-calls TAB[tidx] with (env, sp, tp) */
        res = ((w64_run_fn *)(uintptr_t)fidx)(
            (uintptr_t)env, (uintptr_t)(w64_frame + 16),
            (uintptr_t)&w64_tb_ptr, desc[W64_DESC_TIDX / 4]);

    exited:
        if (LS.stop) {
            /* budget crossed: this insn ran; halt before the next TB.
             * qemu_system_shutdown_request trips an uninitialized
             * main-loop mutex on the wasm build and exit() from the
             * main loop never happens — emscripten exit() from the vCPU
             * is the stop path (flushes stdio, fires onExit). */
            fflush(LS.f);
            bql_release_lazy();
            exit(0);
        }

        if (res & W64_EXIT_GOTOPTR) {
            /* the handoff slot is [sp-8] = frame+8 (sp = frame+16); the
             * dispatcher read frame+0 — always 0 — so every goto_ptr
             * unwound to cpu_exec_loop (14.9M of 16.8M exits per boot) */
            uintptr_t next = *(uintptr_t *)(w64_frame + 8);
            if (next == 0 || next == (uintptr_t)tcg_code_gen_epilogue) {
                return 0;
            }
            tb = next;
            continue;
        }
        return res;
    }
}
