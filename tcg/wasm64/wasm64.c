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
#include "qemu/main-loop.h"             /* bql_release_lazy */
#include "exec/cpu-common.h"
#include "exec/gdbstub.h"
#include "hw/core/cpu.h"
#include <emscripten.h>
#include "exec/translation-block.h"
#include "wasm64.h"
#include "w64-interp.h"

QEMU_BUILD_BUG_ON(W64_TCP_FIDX != W64_DESC_FIDX);
QEMU_BUILD_BUG_ON(W64_TCP_TIDX != W64_DESC_TIDX);

__thread uintptr_t w64_tb_ptr;

/* per-thread state: frame + slot layout
 *   frame + 0 .. 8    goto_ptr handoff slot ([sp-8])
 *   frame + 16 ...    call frame ($sp = frame + 16)
 * One frame per nesting level: the pmb887x DSP JIT runs its blocks
 * through this dispatcher from inside an ARM device access (dsp_sync),
 * while the ARM TB that made the access still has its frame live. */
#define W64_EXEC_NEST_MAX 4
static __thread uint8_t *w64_frames[W64_EXEC_NEST_MAX];
static __thread unsigned w64_exec_depth;

typedef uint32_t w64_run_fn(uintptr_t env, uintptr_t sp, uintptr_t tp,
                            uint32_t tidx);

static void w64_ls_init(void);

static void w64_init(void)
{
    static bool ls_armed;

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
    w64_acct_addr[W64_ACCT_INSNS] = (uint64_t)(uintptr_t)&wasm_guest_insns;
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
    int64_t count;              /* executed insns, residual-kept */
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
    while (LS.count >= (int64_t)LS.period) {
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

int64_t *w64_ls_count(void)
{
    static int armed = -1;

    if (armed < 0) {
        armed = getenv("W64_LOCKSTEP") != NULL;
    }
    return armed ? &LS.count : NULL;
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

/* A batch as the assembler sees it (the open batch, B); a landed batch
 * keeps only its members and id in one. */
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
    uint32_t *tidx;        /* member chain-table indices (eviction) */
    struct w64_landed *next; /* live FIFO */
    uint32_t thunk;        /* the run thunk, to reach the rebinder */
    uint32_t rtidx;        /* the rebinder's chain-table index, 0 = none */
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
 * element segments register every member into TAB at its tidx, which is
 * all that keeps the instance alive.  TAB is the shared chain table every
 * module imports as "e"/"t"; index 0 is never assigned.
 *
 * Returns the run thunk, (i64 env, i64 sp, i64 tp, i32 tidx) -> i32,
 * which tail-calls TAB[tidx] with (env, sp, tp).  It is one function for
 * every batch, made with the first: a thunk per module cost an
 * addFunction/removeFunction pair and a lazy compile per module, and the
 * dispatcher's indirect call met a different target for every batch. */
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
    if (globalThis.__w64run === undefined) {
        const run = new Uint8Array([
            0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
            /* types: 0 the TB signature, 1 the thunk's */
            0x01, 0x10, 0x02,
            0x60, 0x03, 0x7e, 0x7e, 0x7e, 0x01, 0x7f,
            0x60, 0x04, 0x7e, 0x7e, 0x7e, 0x7f, 0x01, 0x7f,
            /* import e.t: funcref table, min 1 */
            0x02, 0x09, 0x01, 0x01, 0x65, 0x01, 0x74, 0x01, 0x70, 0x00, 0x01,
            /* one function of type 1, exported as "run" */
            0x03, 0x02, 0x01, 0x01,
            0x07, 0x07, 0x01, 0x03, 0x72, 0x75, 0x6e, 0x00, 0x00,
            /* local.get 0..3; return_call_indirect type 0, table 0 */
            0x0a, 0x0f, 0x01, 0x0d, 0x00,
            0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0x20, 0x03,
            0x13, 0x00, 0x00, 0x0b,
        ]);
        globalThis.__w64run = addFunction(new WebAssembly.Instance(
            new WebAssembly.Module(run), { e: { t: TAB } }).exports.run,
            'jjjii');
    }
    return globalThis.__w64run;
});

/*
 * The batch's pc-cache probe: helper_lookup_tb_ptr_lc's hit path
 * (accel/tcg/cpu-exec.c w64_pcc_get, w64_lc_fill) as a function of the
 * module, which the members' goto_ptr exits call in the helper's place
 * (tcg-target.c.inc w64_pcc_call).  V8 inlines a callee of the same
 * module, never an import, so TurboFan code pays no call for a hit, while
 * a member's body grows by three bytes, not by the probe: emitted at
 * every exit, that cost the EL71 boot ~8-10 % in translation and Liftoff
 * compile time.  It reads the key where the helper does (W64PccShape),
 * at constant addresses when the CPU is alone (see w64_tb_env), and on a
 * miss calls the helper, union import @miss.
 *
 *   (func (param $env i64) (param $lc i64) (result i64))
 */
enum {
    PB_ENV, PB_LC,                                          /* params */
    PB_ENVW, PB_PC, PB_E, PB_GEN, PB_K0, PB_LCW = PB_K0 + 3, /* i32 */
    PB_TC,                                                   /* i64 */
};
#define W64_PB_HINTS 8

static void pb_local(struct w64_mb *b, uint8_t op, unsigned l)
{
    mb_u8(b, op);
    mb_uleb(b, l);
}

static void pb_i32(struct w64_mb *b, int32_t v)
{
    mb_u8(b, 0x41);
    mb_sleb32(b, v);
}

static void pb_mem(struct w64_mb *b, uint8_t op, unsigned align, uint32_t off)
{
    mb_u8(b, op);
    mb_uleb(b, align);
    mb_uleb(b, off);
}

/* push the u32, or the zero-extended u8, at env-relative @off */
static void pb_env_load(struct w64_mb *b, uintptr_t env, int32_t off,
                        unsigned size)
{
    uint32_t moff = 0;

    if (env) {
        pb_i32(b, (int32_t)(uint32_t)(env + off));
    } else if (off < 0) {
        pb_local(b, 0x20, PB_ENVW);
        pb_i32(b, off);
        mb_u8(b, 0x6a);                         /* i32.add */
    } else {
        pb_local(b, 0x20, PB_ENVW);
        moff = off;
    }
    if (size == 1) {
        pb_mem(b, 0x2d, 0, moff);               /* i32.load8_u */
    } else {
        pb_mem(b, 0x28, 2, moff);               /* i32.load */
    }
}

static void pb_hint(struct w64_mb *b, struct w64_hint *h, unsigned *nh,
                    bool likely)
{
    tcg_debug_assert(*nh < W64_PB_HINTS);
    h[*nh].pos = b->n;
    h[*nh].likely = likely;
    (*nh)++;
}

/* the entry's u32 at @off, against the top of the stack: a miss if not equal */
static void pb_ent_cmp(struct w64_mb *b, uint32_t off, struct w64_hint *h,
                       unsigned *nh)
{
    pb_local(b, 0x20, PB_E);
    pb_mem(b, 0x28, 2, off);                    /* i32.load */
    mb_u8(b, 0x47);                             /* i32.ne */
    pb_hint(b, h, nh, false);
    mb_u8(b, 0x0d);                             /* br_if: the miss */
    mb_uleb(b, 0);
}

/* the probe's [locals][expr] into @b; @h gets its hints, as offsets in @b */
static void w64_pcc_probe(struct w64_mb *b, uint32_t miss,
                          struct w64_hint *h, unsigned *nh)
{
    const struct W64PccShape *sh = w64_pcc_shape();
    uintptr_t env = CPU_NEXT(first_cpu) ? 0 : (uintptr_t)cpu_env(first_cpu);
    int i;

    *nh = 0;
    mb_u8(b, 2);                                /* local runs */
    mb_uleb(b, PB_TC - PB_ENVW);
    mb_u8(b, 0x7f);
    mb_uleb(b, 1);
    mb_u8(b, 0x7e);

    if (!env) {
        pb_local(b, 0x20, PB_ENV);
        mb_u8(b, 0xa7);                         /* i32.wrap_i64 */
        pb_local(b, 0x21, PB_ENVW);
    }

    /* the entry: tab + (w64_pcc_hash(pc) << 5) */
    QEMU_BUILD_BUG_ON(sizeof(struct W64PccEnt) != 1 << 5);
    pb_env_load(b, env, sh->env_pc, 4);
    pb_local(b, 0x22, PB_PC);
    pb_i32(b, (int32_t)sh->mul);
    mb_u8(b, 0x6c);                             /* i32.mul */
    pb_i32(b, 32 - sh->bits - 5);
    mb_u8(b, 0x76);                             /* i32.shr_u */
    pb_i32(b, ((1u << sh->bits) - 1) << 5);
    mb_u8(b, 0x71);                             /* i32.and */
    pb_i32(b, (int32_t)(uintptr_t)sh->tab);
    mb_u8(b, 0x6a);                             /* i32.add */
    pb_local(b, 0x21, PB_E);

    mb_u8(b, 0x02);                             /* block: the miss */
    mb_u8(b, 0x40);
    pb_local(b, 0x20, PB_PC);
    pb_ent_cmp(b, offsetof(struct W64PccEnt, pc), h, nh);
    pb_env_load(b, env, sh->env_gen, 4);
    pb_local(b, 0x22, PB_GEN);
    pb_ent_cmp(b, offsetof(struct W64PccEnt, gen), h, nh);
    pb_env_load(b, env, sh->env_cpu_index, 4);
    pb_ent_cmp(b, offsetof(struct W64PccEnt, cpu_index), h, nh);
    for (i = 0; i < 3; i++) {
        pb_env_load(b, env, sh->env_key[i], sh->key_size[i]);
        pb_local(b, 0x22, PB_K0 + i);
        pb_ent_cmp(b, offsetof(struct W64PccEnt, key32[i]), h, nh);
    }
    pb_local(b, 0x20, PB_E);
    pb_mem(b, 0x29, 3, offsetof(struct W64PccEnt, tc));  /* i64.load */
    pb_local(b, 0x21, PB_TC);

    /*
     * w64_lc_fill: a slot is filled when every key word it does not
     * hold dynamically is the CPU's.  Writing those back is a no-op, so
     * all three are stored.
     */
    pb_local(b, 0x20, PB_LC);
    mb_u8(b, 0x50);                             /* i64.eqz */
    mb_u8(b, 0x45);                             /* i32.eqz */
    mb_u8(b, 0x04);                             /* if: a slot */
    mb_u8(b, 0x40);
    pb_local(b, 0x20, PB_LC);
    mb_u8(b, 0xa7);                             /* i32.wrap_i64 */
    pb_local(b, 0x21, PB_LCW);
    for (i = 0; i < 3; i++) {
        pb_local(b, 0x20, PB_LCW);
        pb_mem(b, 0x28, 2, offsetof(struct W64LookupCache, key32[i]));
        pb_local(b, 0x20, PB_K0 + i);
        mb_u8(b, 0x47);                         /* i32.ne */
        if (i) {
            pb_i32(b, i);
            mb_u8(b, 0x74);                     /* i32.shl */
            mb_u8(b, 0x72);                     /* i32.or */
        }
    }
    pb_local(b, 0x20, PB_LCW);
    pb_mem(b, 0x2d, 0, offsetof(struct W64LookupCache, dynmask));
    pb_i32(b, -1);
    mb_u8(b, 0x73);                             /* i32.xor */
    mb_u8(b, 0x71);                             /* i32.and */
    mb_u8(b, 0x45);                             /* i32.eqz */
    pb_hint(b, h, nh, true);
    mb_u8(b, 0x04);                             /* if: fill */
    mb_u8(b, 0x40);
    pb_local(b, 0x20, PB_LCW);
    pb_local(b, 0x20, PB_PC);
    pb_mem(b, 0x36, 2, offsetof(struct W64LookupCache, pc));  /* i32.store */
    for (i = 0; i < 3; i++) {
        pb_local(b, 0x20, PB_LCW);
        pb_local(b, 0x20, PB_K0 + i);
        pb_mem(b, 0x36, 2, offsetof(struct W64LookupCache, key32[i]));
    }
    pb_local(b, 0x20, PB_LCW);
    pb_local(b, 0x20, PB_TC);
    pb_mem(b, 0x37, 3, offsetof(struct W64LookupCache, tc));  /* i64.store */
    pb_local(b, 0x20, PB_LCW);
    pb_local(b, 0x20, PB_GEN);
    pb_mem(b, 0x36, 2, offsetof(struct W64LookupCache, gen));
    mb_u8(b, 0x0b);                             /* end: fill */
    mb_u8(b, 0x0b);                             /* end: a slot */
    pb_local(b, 0x20, PB_TC);
    mb_u8(b, 0x0f);                             /* return */
    mb_u8(b, 0x0b);                             /* end: the miss */

    pb_local(b, 0x20, PB_ENV);
    pb_local(b, 0x20, PB_LC);
    mb_u8(b, 0x10);                             /* call: the helper */
    mb_uleb(b, miss);
    mb_u8(b, 0x0b);                             /* end */
}

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

/*
 * SpiderMonkey binds a table slot to the code a function has when the slot
 * is written, and never patches it: a member registered by an active
 * segment is entered through its baseline code for good, which hops to the
 * optimized code through a jump table once there is some (the comment on
 * tiering in js/src/wasm/WasmCompile.cpp).  Chained TBs enter each other
 * through the table and nowhere else, so that is every TB transition.
 * table.init binds a slot again, to the best tier of that moment
 * (Instance::initElems), so a batch for Firefox carries its members a
 * second time in a passive segment, and a function of the TB signature,
 * (dst, src, n) -> 0, that copies a run of them into the table.  It sits
 * in the chain table like a member; w64_rebind_live() calls it through
 * the run thunk.  V8 patches its dispatch tables when a function tiers
 * up, so nothing of this is emitted for it.
 */
EM_JS(int, w64_rebind_wanted, (void), {
    return navigator.userAgent.indexOf('Firefox') >= 0 ? 1 : 0;
});

static int w64_rebind = -1;

static void w64_rebind_body(struct w64_mb *b, unsigned segment)
{
    static const uint8_t args[] = {
        0x00,                               /* no locals */
        0x20, 0x00, 0xa7,                   /* local.get 0, i32.wrap_i64 */
        0x20, 0x01, 0xa7,
        0x20, 0x02, 0xa7,
        0xfc, 0x0c,                         /* table.init */
    };

    mb_put(b, args, sizeof(args));
    mb_uleb(b, segment);
    mb_u8(b, 0x00);                         /* table 0 */
    mb_u8(b, 0x41); mb_u8(b, 0x00);         /* i32.const 0 */
    mb_u8(b, 0x0b);
}

/* Assemble the batch module from @src and instantiate it; returns the
 * run thunk's fidx.  A nonzero @rtidx asks for a rebinder there. */
static uint32_t w64_assemble_instantiate(const struct w64_bsrc *src,
                                         uint32_t rtidx)
{
    static const uint8_t magic[8] = { 0, 'a', 's', 'm', 1, 0, 0, 0 };
    struct w64_mb mod = { 0 }, sec = { 0 }, probe = { 0 }, rebind = { 0 };
    struct w64_hint ph[W64_PB_HINTS];
    uint8_t name[8];
    uint32_t maxtidx = 0;
    uint32_t *ip;
    uint32_t thunk;
    unsigned m, i, nph = 0;
    unsigned nfn = src->n_member;       /* members, the probe, the rebinder */
    int miss = -1;

    /* the pc-cache probe, when a member calls it: the last function */
    for (i = 0; i < src->n_hint; i++) {
        if (src->hint[i].pcc_call) {
            uint32_t helper = (uint32_t)(uintptr_t)w64_pcc_shape()->helper;

            for (m = 0; m < src->n_uimp; m++) {
                if (src->uimp[m].fptr == helper) {
                    miss = m;
                    break;
                }
            }
            tcg_debug_assert(miss >= 0);
            w64_pcc_probe(&probe, miss, ph, &nph);
            nfn++;
            break;
        }
    }
    for (m = 0; miss >= 0 && m < src->n_member; m++) {
        uint32_t hs = m ? src->member[m - 1].hint_end : 0;

        for (i = hs; i < src->member[m].hint_end; i++) {
            if (src->hint[i].pcc_call) {
                uint8_t *p = src->member[m].body + src->hint[i].pos -
                             W64_BODY_OFF;
                uint32_t v = src->n_uimp + src->n_member;
                int b;

                for (b = 0; b < 4; b++) {
                    p[b] = (v & 0x7f) | 0x80;
                    v >>= 7;
                }
                p[4] = v & 0x7f;
            }
        }
    }
    if (rtidx) {
        /* segments: one per member, the rebinder's, the passive one */
        w64_rebind_body(&rebind, src->n_member + 1);
    }
    mb_put(&mod, magic, sizeof(magic));

    /* type section: union types */
    mb_uleb(&sec, src->n_utypes);
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
    mb_uleb(&sec, nfn + (rtidx != 0));
    for (i = 0; i < src->n_member; i++) {
        mb_uleb(&sec, 0);
    }
    if (miss >= 0) {
        mb_uleb(&sec, src->uimp[miss].type); /* the probe: the helper's */
    }
    if (rtidx) {
        mb_uleb(&sec, 0);
        nfn++;
    }
    mb_sec(&mod, 3, &sec);

    /* element section: one active segment per member, TAB[tidx] = m */
    sec.n = 0;
    mb_uleb(&sec, src->n_member + (rtidx ? 2 : 0));
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
    if (rtidx) {
        maxtidx = MAX(maxtidx, rtidx);
        mb_u8(&sec, 0x00);                /* active: TAB[rtidx] = rebinder */
        mb_u8(&sec, 0x41);
        mb_sleb32(&sec, (int32_t)rtidx);
        mb_u8(&sec, 0x0b);
        mb_uleb(&sec, 1);
        mb_uleb(&sec, src->n_uimp + nfn - 1);
        mb_u8(&sec, 0x01);                /* passive, function indices */
        mb_u8(&sec, 0x00);
        mb_uleb(&sec, src->n_member);
        for (m = 0; m < src->n_member; m++) {
            mb_uleb(&sec, src->n_uimp + m);
        }
    }
    mb_sec(&mod, 9, &sec);

    /*
     * metadata.code.branch_hint (must precede the code section): per
     * member, the hinted if / br_if opcodes, as offsets from the start
     * of the function body — the byte after its size LEB.
     */
    {
        static const char hname[] = "metadata.code.branch_hint";
        unsigned nf = nph > 0, nh;

        sec.n = 0;
        mb_uleb(&sec, sizeof(hname) - 1);
        mb_put(&sec, hname, sizeof(hname) - 1);
        for (m = 0; m < src->n_member; m++) {
            uint32_t hs = m ? src->member[m - 1].hint_end : 0;

            for (i = hs; i < src->member[m].hint_end; i++) {
                if (!src->hint[i].pcc_call) {
                    nf++;
                    break;
                }
            }
        }
        mb_uleb(&sec, nf);
        for (m = 0; nf && m < src->n_member; m++) {
            uint32_t hs = m ? src->member[m - 1].hint_end : 0;

            nh = 0;
            for (i = hs; i < src->member[m].hint_end; i++) {
                nh += !src->hint[i].pcc_call;
            }
            if (nh == 0) {
                continue;
            }
            mb_uleb(&sec, src->n_uimp + m);
            mb_uleb(&sec, nh);
            for (i = hs; i < src->member[m].hint_end; i++) {
                if (src->hint[i].pcc_call) {
                    continue;
                }
                mb_uleb(&sec, src->hint[i].pos - W64_BODY_OFF - 5);
                mb_u8(&sec, 1);
                mb_u8(&sec, src->hint[i].likely);
            }
        }
        if (nph) {
            mb_uleb(&sec, src->n_uimp + src->n_member);
            mb_uleb(&sec, nph);
            for (i = 0; i < nph; i++) {
                mb_uleb(&sec, ph[i].pos);
                mb_u8(&sec, 1);
                mb_u8(&sec, ph[i].likely);
            }
        }
        if (nf) {
            mb_sec(&mod, 0, &sec);
        }
    }

    /* code section: the staged bodies, the probe and the rebinder; the
     * total size is a padded 5-byte LEB so it never shifts when the body
     * sum crosses a LEB width boundary */
    {
        uint64_t total = nfn < 128 ? 1 : 2;

        for (m = 0; m < src->n_member; m++) {
            total += src->member[m].body_len;
        }
        if (miss >= 0) {
            total += 5 + probe.n;
        }
        if (rtidx) {
            total += 1 + rebind.n;
        }
        mb_u8(&mod, 10);
        mb_uleb_p5(&mod, (uint32_t)total);
        mb_uleb(&mod, nfn);
        for (m = 0; m < src->n_member; m++) {
            mb_put(&mod, src->member[m].body, src->member[m].body_len);
        }
        if (miss >= 0) {
            mb_uleb_p5(&mod, probe.n);
            mb_put(&mod, probe.b, probe.n);
        }
        if (rtidx) {
            tcg_debug_assert(rebind.n < 128);
            mb_u8(&mod, rebind.n);
            mb_put(&mod, rebind.b, rebind.n);
        }
    }

#ifdef W64_NAME_TBS
    /* debug: name each TB function tb_<pc>_<icount>_<flags>, so V8 CPU
     * profiles show which guest code the time goes to */
    {
        static const char nname[] = "name";
        struct w64_mb sub = { 0 };
        char buf[40];

        sec.n = 0;
        mb_uleb(&sec, sizeof(nname) - 1);
        mb_put(&sec, nname, sizeof(nname) - 1);
        mb_uleb(&sub, src->n_member);
        for (m = 0; m < src->n_member; m++) {
            TranslationBlock *tb = tcg_tb_lookup(src->member[m].tcptr);
            int len = tb ? snprintf(buf, sizeof(buf), "tb_%08x_%u_%x",
                                    (unsigned)tb->pc, tb->icount, tb->flags)
                         : snprintf(buf, sizeof(buf), "x_%x",
                                    src->member[m].tcptr);
            mb_uleb(&sub, src->n_uimp + m);
            mb_uleb(&sub, len);
            mb_put(&sub, buf, len);
        }
        mb_u8(&sec, 1);
        mb_uleb(&sec, sub.n);
        mb_put(&sec, sub.b, sub.n);
        mb_sec(&mod, 0, &sec);
        g_free(sub.b);
    }
#endif

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
    g_free(probe.b);
    g_free(rebind.b);
    g_free(ip);
    return thunk;
}

uintptr_t w64_tb_env;

/* include/qemu/w64-low.h */
extern char __global_base;
static uintptr_t w64_low_given[4];

void *w64_low_place(uintptr_t at, size_t size)
{
    unsigned i, free = ARRAY_SIZE(w64_low_given);

    for (i = 0; i < ARRAY_SIZE(w64_low_given); i++) {
        if (w64_low_given[i] == at) {
            return NULL;
        }
        if (w64_low_given[i] == 0) {
            free = i;
        }
    }
    if (free == ARRAY_SIZE(w64_low_given) ||
        at + size > (uintptr_t)&__global_base) {
        return NULL;
    }
    w64_low_given[free] = at;
    return memset((void *)at, 0, size);
}

bool w64_low_release(void *p)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(w64_low_given); i++) {
        if (p && w64_low_given[i] == (uintptr_t)p) {
            w64_low_given[i] = 0;
            return true;
        }
    }
    return false;
}

/* Drop the oldest live batch: its members fall back to the dispatcher
 * (fidx 0 + batch tag), which retires them. */

bool w64_tc_evicted(const void *tc_ptr)
{
    const uint32_t *desc = tc_ptr;

    return desc[W64_DESC_FIDX / 4] == 0 &&
           (desc[W64_DESC_BATCH / 4] & W64_BATCH_TAG);
}

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
    w64_evict_epoch++;

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
    if (l->rtidx) {
        w64_tab_unset((uintptr_t)&l->rtidx, 1);
    }

    /*
     * A goto_ptr inline-cache slot holds a table index too, and has no
     * back-reference at all, so retire every slot at once — that is what
     * the generation is for.  A boot stays an order of magnitude below
     * W64_LIVE_MAX (~450-760 live modules against 6144); a Java game that
     * keeps compiling lands two or three batches a second and can get
     * there in half an hour.  Nothing short of that runs this path, so
     * test it with the cap lowered (256 under tools/lockstep-wasm.mjs).
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
            g_free((void *)l->src.member);
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

/*
 * Bind the chain-table slots of every live batch again.  A member that has
 * been optimized since the last pass is entered directly from here on.
 * Indices are handed out in translation order, so a batch is a few runs.
 */
static void w64_rebind_live(void)
{
    struct w64_landed *l;

    for (l = w64_live_head; l; l = l->next) {
        w64_run_fn *run = (w64_run_fn *)(uintptr_t)l->thunk;
        unsigned m, n;

        for (m = 0; l->rtidx && m < l->src.n_member; m += n) {
            for (n = 1; m + n < l->src.n_member &&
                        l->tidx[m + n] == l->tidx[m] + n; n++) {
            }
            run(l->tidx[m], m, n, l->rtidx);
        }
    }
}

#define W64_REBIND_MS   1000.0

static void w64_rebind_poll(void)
{
    static double next;
    double now = emscripten_get_now();

    if (now >= next) {
        w64_rebind_live();
        next = now + W64_REBIND_MS;
    }
}

static void w64_batch_close(void)
{
    struct w64_landed *l;
    uint32_t thunk, rtidx = 0;
    unsigned m;

    if (w64_rebind < 0) {
        w64_rebind = w64_rebind_wanted();
    }
    if (w64_rebind && w64_tidx_left()) {
        rtidx = w64_alloc_tidx();
    }
    w64_bsrc_of_open();
    thunk = w64_assemble_instantiate(&B_src, rtidx);

    /* the module holds the code now */
    for (m = 0; m < B.n_member; m++) {
        g_free(B.member[m].body);
        B.member[m].body = NULL;
    }

    /* landed record: the members, for eviction */
    l = g_new0(struct w64_landed, 1);
    l->src.member = g_memdup2(B.member, B.n_member * sizeof(*B.member));
    l->src.n_member = B.n_member;
    l->src.id = B.id;
    l->tidx = g_new(uint32_t, B.n_member);
    l->thunk = thunk;
    l->rtidx = rtidx;

    /* land: point every member at the run thunk */
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

/* tb_flush teardown: drop every landed batch and the open batch's staged
 * bodies, clear the chain table and recycle the tidx space (the code
 * buffer — the descriptors — is freed by the caller). */
void w64_batch_flush(void)
{
    unsigned m;

    w64_irec_flush();
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

static uintptr_t QEMU_DISABLE_CFI w64_exec(CPUArchState *env, uintptr_t tb,
                                            uint8_t *frame)
{
    static uint32_t entries;

    if (w64_rebind > 0 && (++entries & 0x3ff) == 0) {
        w64_rebind_poll();
    }
    for (;;) {
        uint32_t *desc = (uint32_t *)tb;
        uint32_t fidx, res;

        if (desc[W64_DESC_FIDX / 4] == 0 &&
            w64_interp_try(desc[W64_DESC_TIDX / 4],
                           desc[W64_DESC_ICOUNT / 4], (uintptr_t)env,
                           (uintptr_t)(frame + 16),
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

        /* the run thunk tail-calls TAB[tidx] with (env, sp, tp) */
        res = ((w64_run_fn *)(uintptr_t)fidx)(
            (uintptr_t)env, (uintptr_t)(frame + 16),
            (uintptr_t)&w64_tb_ptr, desc[W64_DESC_TIDX / 4]);

    exited:
        if (LS.stop) {
            /* budget crossed: this insn ran; halt before the next TB.
             * qemu_system_shutdown_request trips an uninitialized
             * main-loop mutex on the wasm build and exit() from the
             * main loop never happens — emscripten exit() from the vCPU
             * is the stop path (flushes stdio, fires onExit).  Only with
             * the BQL free: DSP code runs here too, nested in an ARM MMIO
             * access or from a run_on_cpu work item, holding the BQL for
             * real, and exiting with it deadlocks.  The next ARM TB that
             * runs without it stops instead. */
            bql_release_lazy();
            if (!bql_locked()) {
                fflush(LS.f);
                exit(0);
            }
        }

        if (res & W64_EXIT_GOTOPTR) {
            /* the handoff slot is [sp-8] = frame+8 (sp = frame+16); the
             * dispatcher read frame+0 — always 0 — so every goto_ptr
             * unwound to cpu_exec_loop (14.9M of 16.8M exits per boot) */
            uintptr_t next = *(uintptr_t *)(frame + 8);
            if (next == 0 || next == (uintptr_t)tcg_code_gen_epilogue) {
                return 0;
            }
            tb = next;
            continue;
        }
        return res;
    }
}

/* cpu_loop_exit() longjmped out of every TB this thread was running. */
void w64_exec_unwound(void)
{
    w64_exec_depth = 0;
    w64_interp_unwound();
}

uintptr_t tcg_qemu_tb_exec(CPUArchState *env, const void *v_tb_ptr)
{
    unsigned depth = w64_exec_depth;
    uintptr_t outer_tb_ptr = w64_tb_ptr;
    uintptr_t res;

    w64_init();
    g_assert(depth < W64_EXEC_NEST_MAX);
    if (!w64_frames[depth]) {
        w64_frames[depth] = g_malloc0(16 + TCG_STATIC_CALL_ARGS_SIZE +
                                      TCG_STATIC_FRAME_SIZE);
    }
    w64_exec_depth = depth + 1;
    res = w64_exec(env, (uintptr_t)v_tb_ptr, w64_frames[depth]);
    w64_exec_depth = depth;
    /* the outer TB's helper may still fault and unwind through it */
    w64_tb_ptr = outer_tb_ptr;
    return res;
}
