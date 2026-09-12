/*
 * TCG wasm64 backend — runtime dispatch (tcg_qemu_tb_exec).
 *
 * Owns the per-vCPU call frame and the lazy compilation of TB modules
 * through the browser WebAssembly API.  Each TB's module bytes and its
 * helper-import table live in the code generation buffer right behind
 * the descriptor at tb->tc.ptr (see wasm64.h); compilation happens at
 * first execution and the resulting emscripten table index is cached
 * in the descriptor.
 *
 * Phase 1 (doc/wasm-tcg-backend-plan.md §5): no chaining — every TB
 * returns to this loop (exit codes, or the goto_ptr handoff slot).
 * TB accounting (wasm_tb_account / icount2_advance) mirrors the TCI
 * build's INDEX_op_tci_tbhdr so the two engines stay observable in
 * the same units.
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
#include "wasm64.h"

/* icount2.c: addresses of the fields emitted TB prologues touch
 * (cpu-timers-internal.h needs too many prerequisites to include here) */
void icount2_w64_acct_addrs(uintptr_t *ticks, uintptr_t *deadline);

__thread uintptr_t w64_tb_ptr;

/*
 * Compile + instantiate the TB module at tb_ptr and insert its entry
 * function into the emscripten table.  Returns the table index.
 *
 * The JS side reads the descriptor, copies the module bytes (a full
 * copy, not a view — Firefox compatibility,
 * https://bugzilla.mozilla.org/show_bug.cgi?id=1965217), resolves the
 * helper imports through wasmTable.get() on the main module's table
 * (C function pointers == table indices), and registers the instance
 * entry with addFunction().
 */
EM_JS(int, w64_instantiate, (uintptr_t tb_ptr), {
    const p = Number(tb_ptr);
    const dv = new DataView(HEAPU8.buffer);
    const mod_len = dv.getUint32(p + 4, true);
    const n_imp = dv.getUint32(p + 12, true);

    const mod_bytes = new Uint8Array(HEAPU8.slice(p + 20, p + 20 + mod_len));
    const tidx = dv.getUint32(p + 16, true);
    /* the shared chain table: every TB module imports it as "e"/"t";
    * entries are registered here, at instantiation (a TB can only be
    * linked after it has executed once, so the entry always precedes
    * any goto_tb that references the index).  Grown in chunks; index 0
    * is never assigned. */
    if (!globalThis.__w64tab) {
      globalThis.__w64tab = new WebAssembly.Table({ element: 'anyfunc', initial: 1 << 14 });
    }
    const TAB = globalThis.__w64tab;
    if (tidx >= TAB.length) {
      TAB.grow(Math.max(4096, tidx + 1 - TAB.length));
    }
    const imports = { e: { m: wasmMemory, t: TAB } };
    const tbl = p + 20 + mod_len;
    for (let i = 0; i < n_imp; i++) {
      /* memory64: the emscripten table is i64-indexed */
      imports.e['f' + i] = wasmTable.get(BigInt(dv.getUint32(tbl + i * 4, true)));
    }

    let mod;
    try {
        mod = new WebAssembly.Module(mod_bytes);
    } catch (e) {
        /* dump for offline analysis (W64DUMP<i>:<base64> lines) */
        let bin = '';
        const CH = 0x8000;
        for (let i = 0; i < mod_bytes.length; i += CH) {
            bin += String.fromCharCode.apply(null, mod_bytes.subarray(i, i + CH));
        }
        const b64 = btoa(bin);
        for (let i = 0; i < b64.length; i += 3000) {
            console.log('W64DUMPC' + (i / 3000) + ':' + b64.substr(i, 3000));
        }
        console.log('W64DUMPE:nimp=' + n_imp + ' len=' + mod_len);
        throw e;
    }
    let inst;
    try {
        inst = new WebAssembly.Instance(mod, imports);
    } catch (e) {
        if (String(e).includes('does not match')) {
            const idxs = [];
            for (let i = 0; i < n_imp; i++) {
                idxs.push(dv.getUint32(tbl + i * 4, true));
            }
            console.log('W64DBG link fail: ' + e + ' imps=' + n_imp +
                        ' idx=[' + idxs.join(',') + ']');
            /* decode our own type + import sections to show declared sigs */
            try {
                let q = 8;
                const rd = () => mod_bytes[q++];
                const ul = () => { let v = 0, s = 0, b; do { b = rd(); v |= (b & 0x7f) << s; s += 7; } while (b & 0x80); return v >>> 0; };
                let types = null, imptype = [];
                while (q < mod_bytes.length) {
                    const id = rd(); const sz = ul(); const end = q + sz;
                    if (id === 1) {
                        const nt = ul(); types = [];
                        for (let t = 0; t < nt; t++) {
                            if (rd() !== 0x60) throw new Error('bad type');
                            const np = ul(); const ps = [];
                            for (let x = 0; x < np; x++) ps.push(rd());
                            const nr = ul(); let r = null;
                            if (nr) r = rd();
                            types.push([ps, r]);
                        }
                    } else if (id === 2) {
                        const ni = ul();
                        for (let x = 0; x < ni; x++) {
                            const ml = ul(); q += ml;
                            const fl = ul(); q += fl;
                            const kind = rd();
                            if (kind === 0) imptype.push(ul());
                            else if (kind === 2) { const flags = rd(); ul(); if (flags & 1) ul(); }
                            else throw new Error('bad import kind ' + kind);
                        }
                    }
                    q = end;
                }
                const hex0 = (x) => 'x' + x.toString(16);
            const ts = (t) => '(' + t[0].map(x => x === 0x7e ? 'j' : x === 0x7f ? 'i' : hex0(x)).join(',') +
                            ')->' + (t[1] === null ? 'void' : t[1] === 0x7e ? 'j' : t[1] === 0x7f ? 'i' : hex0(t[1]));
                console.log('W64DEC types=' + types.map(ts).join(' ') +
                            ' imports=[' + imptype.join(',') + ']');
            } catch (e2) {
                console.log('W64DEC parse failed: ' + e2);
            }
            /* probe the actual wasm type of each helper funcref */
            const leb = (v) => {
                const out = [];
                do { let b = v & 0x7f; v >>>= 7; if (v) b |= 0x80; out.push(b); } while (v);
                return out;
            };
            const sig = (params, ret) => {
                const ft = [0x60].concat(leb(params.length), params,
                                        ret ? [1, ret] : [0]);
                const type = leb(1).concat(ft);   /* one type */
                const entry = [1, 't'.charCodeAt(0), 1, 'f'.charCodeAt(0), 0]
                    .concat(leb(0));
                const content = leb(1).concat(entry);   /* count + entry */
                const impsec = [2].concat(leb(content.length), content);
                const bytes = [0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]
                    .concat([1], leb(type.length), type, impsec);
                return bytes;
            };
            for (let k = 0; k < n_imp; k++) {
                const fr = wasmTable.get(BigInt(idxs[k]));
                const cands = [];
                const J = 0x7e, I = 0x7f;
                const F = 0x7d, D = 0x7c;
                for (const ps of [[J,J,I,J], [J,J,I,I,J], [J,J,J,I,J],
                                  [J,I,I,J], [J,J,I], [J,I], [J], [J,J],
                                  [J,J,J,J], [I,I,I,I], [J,J,J], [I], [],
                                  [J,F], [J,D], [J,J,D], [J,I,J], [J,J,F],
                                  [J,I,I], [J,J,I,I], [I,I], [J,I,I,J,J]]) {
                    for (const r of [J, I, D, F, null]) {
                        try {
                            new WebAssembly.Instance(
                                new WebAssembly.Module(new Uint8Array(sig(ps, r))),
                                { t: { f: fr } });
                            cands.push('(' + ps.map(x => x === J ? 'j' : 'i').join(',') +
                                       ')->' + (r === J ? 'j' : r === I ? 'i' : 'void'));
                        } catch (_) { /* keep trying */ }
                    }
                }
                console.log('W64SIG f' + k + ' idx=' + idxs[k] + ': ' +
                            (cands.join(' | ') || 'none of the candidates'));
                if (cands.length) {
                    /* also dump the names the sig-printer can't: none */
                }
            }
        }
        throw e;
    }
    /* register the entry for chaining before addFunction: a concurrent
     * (single-threaded here) tb_add_jump can only follow this TB's first
     * execution, which returns through the dispatcher below */
    TAB.set(tidx, inst.exports.tb);
    return addFunction(inst.exports.tb, 'jjji');
});

/* per-vCPU state: frame + slot layout
 *   frame + 0 .. 8    goto_ptr handoff slot ([sp-8])
 *   frame + 16 ...    call frame ($sp = frame + 16) */
static bool w64_inited;
static uint8_t *w64_frame;

typedef uint32_t w64_tb_fn(uintptr_t env, uintptr_t sp, uintptr_t tp);
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
    if (!w64_inited) {
        size_t sz = 16 + TCG_STATIC_CALL_ARGS_SIZE + TCG_STATIC_FRAME_SIZE;
        w64_frame = g_malloc0(sz);
        w64_inited = true;
        /* Eager lockstep init: the inline TB accounting (below) tests
         * w64_ls_on before any import call, so the fold must be armed
         * before the first executed TB's prologue runs — this is the
         * same point where the old lazy init (first w64_tb_account)
         * fired, one TB earlier than execution. */
        w64_ls_init();
    }
}

/* Addresses for the inline TB-prologue accounting (tcg_out_tb_start
 * reads these at translation time; see wasm64.h).  Filled lazily by
 * the emitter's first tcg_out_tb_start — wasm cannot fold integer casts
 * of addresses into static initializers, and translation of the first
 * TB happens before w64_init()/first exec. */
uint64_t w64_acct_addr[5];

void w64_acct_init(void)
{
    uintptr_t ticks, deadline;

    icount2_w64_acct_addrs(&ticks, &deadline);
    w64_acct_addr[0] = (uint64_t)(uintptr_t)&wasm_tb_stats[0];
    w64_acct_addr[1] = (uint64_t)(uintptr_t)&wasm_tb_stats[1];
    w64_acct_addr[2] = (uint64_t)ticks;
    w64_acct_addr[3] = (uint64_t)deadline;
    w64_acct_addr[4] = (uint64_t)(uintptr_t)&w64_ls_on;
}

/* ------------------------------------------------------------------ */
/* lockstep fold (phase-1 gate, doc/wasm-tcg-backend-plan.md §5)
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
    int on, inited;
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

/* Mirror of LS.on for the emitted TB prologues (stable address, set
 * once by w64_ls_init; the emitter embeds &w64_ls_on via w64_acct_addr). */
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

    LS.inited = 1;
    if (!getenv("W64_LOCKSTEP")) {
        return;
    }
    LS.on = 1;
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
        LS.on = 0;
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

/* called from the emitted TB prologue (import, only when w64_ls_on) —
 * and from w64_tb_account (the W64_NOACCTINLINE fallback) */
void w64_lockstep_account(unsigned insns)
{
    if (!LS.inited) {
        w64_ls_init();
    }
    if (!LS.on) {
        return;
    }
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

/* ------------------------------------------------------------------ */
/* batching (phase 2, doc/wasm-tcg-backend-plan.md §4.1)              */
/*                                                                    */
/* Every translated TB joins the open batch at finalize; every        */
/* W64_BATCH_N members one batch module is assembled (union type and  */
/* import tables deduped across members, one function per member,     */
/* one `run(env, sp, tp, tidx)` thunk) and instantiated.  Its active  */
/* element segments register the member functions into the shared     */
/* chain table at their tidx — replacing the temp-module entries —    */
/* and the members' descriptors flip to the thunk (batch id in        */
/* desc+4).  The temp modules are dropped, so steady state holds      */
/* ~live_TBs/N instances instead of one per TB.                       */
/* ------------------------------------------------------------------ */

struct w64_member {
    uint32_t tcptr;         /* descriptor address (code buffer) */
    uint32_t body_len;      /* [size LEB][locals][expr] bytes */
    uint32_t fix_end;       /* fixup arena entries below this are ours */
    uint32_t sum;           /* FNV-1a of the staged body bytes, taken at
                             * add time — re-verified at close to split
                             * "source corrupted in place between staging
                             * and close" (foreign writer into the code
                             * buffer) from record/assembly bugs */
};

static __thread struct {
    struct w64_type utype[W64_UMAX_TYPES];
    uint8_t n_utypes;
    struct w64_import uimp[W64_UMAX_IMPORTS];
    uint8_t n_uimp;
    struct w64_member member[W64_BATCH_N_MAX];
    uint16_t n_member;
    struct w64_cfix *fix;   /* per-member call fixups, in member order */
    uint32_t n_fix, cap_fix;
    uint32_t id;            /* 0 = no batch open */
} B;

/* A batch as the assembler sees it: the open batch (B) or the compact
 * copy kept for a landed one. */
struct w64_bsrc {
    const struct w64_type *utype;
    const struct w64_import *uimp;
    const struct w64_member *member;
    const struct w64_cfix *fix;
    unsigned n_utypes, n_uimp, n_member, n_fix;
    uint32_t id;
};

/* Landed batches, by id.  The staged bodies stay in the code buffer
 * until tb_flush, so a landed batch keeps only its records: enough to
 * re-assemble the identical module after an eviction.  The live set
 * (instantiated modules) is a FIFO capped at w64_live_max — Firefox
 * caps a process at ~16k live wasm modules (64 KB of executable
 * address space each), and a boot translates far more TBs than that. */
struct w64_landed {
    struct w64_bsrc src;
    uint32_t thunk;         /* run-thunk fidx; 0 = evicted */
    uint32_t maxtidx;
    uint32_t *tidx;         /* member chain-table indices (eviction) */
    struct w64_landed *next; /* live FIFO */
    bool small;             /* not yet compacted (see w64_compact) */
};
static struct w64_landed **w64_landed_by_id;
static unsigned w64_landed_cap;
static struct w64_landed *w64_live_head, *w64_live_tail;
static unsigned w64_live_n, w64_landed_n;
static unsigned w64_small_n, w64_small_members;
static uint32_t w64_next_batch_id;

/* compaction thresholds: merge the live small batches into one module
 * once this many of them / members have accumulated */
#define W64_COMPACT_BATCHES 256
#define W64_COMPACT_MEMBERS 1024

static unsigned w64_live_max(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("W64_LIVE_MAX");
        v = e ? atoi(e) : 6144;
        v = MAX(v, 16);
    }
    return v;
}

/* batching mode: 0 = disabled (W64_NOBATCH), else members per batch
 * (W64_BATCH_N env override, default W64_BATCH_N_DEF) */
static int w64_batch_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e;
        mode = W64_BATCH_N_DEF;
        if ((e = getenv("W64_NOBATCH")) != NULL && *e) {
            mode = 0;
        } else if ((e = getenv("W64_BATCH_N")) != NULL) {
            int v = atoi(e);
            if (v >= 1 && v <= W64_BATCH_N_MAX) {
                mode = v;
            }
        }
    }
    return mode;
}

static void w64_batch_open(void)
{
    B.n_utypes = 0;
    B.n_uimp = 0;
    B.n_member = 0;
    B.n_fix = 0;
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

uint8_t w64_union_type(unsigned np, const uint8_t *p, uint8_t ret)
{
    int i, j;

    if (!w64_batch_mode()) {
        return 0;
    }
    if (B.id == 0) {
        w64_batch_open();
    }
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
    B.utype[B.n_utypes].np = np;
    memcpy(B.utype[B.n_utypes].p, p, np);
    B.utype[B.n_utypes].ret = ret;
    return B.n_utypes++;
}

uint8_t w64_union_import(uint32_t fptr, uint8_t utype)
{
    int i;

    if (!w64_batch_mode()) {
        return 0;
    }
    for (i = 0; i < B.n_uimp; i++) {
        if (B.uimp[i].fptr == fptr && B.uimp[i].type == utype) {
            return i;
        }
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

/* Instantiate a batch module.  The bytes are a full copy on the JS
 * side (Firefox compat, see w64_instantiate); `ipp` points at the
 * union import table (u32 C function pointers).  The instance's
 * active element segments register every member into TAB at its tidx
 * (replacing the temp-module entries) before addFunction returns. */
EM_JS(int, w64_batch_instantiate,
      (uintptr_t modp, uint32_t modlen, uintptr_t ipp, uint32_t nimp,
       uint32_t maxtidx), {
    const dv = new DataView(HEAPU8.buffer);
    const mod_bytes =
        new Uint8Array(HEAPU8.slice(Number(modp), Number(modp) + Number(modlen)));
    if (!globalThis.__w64tab) {
        globalThis.__w64tab = new WebAssembly.Table({ element: 'anyfunc', initial: 1 << 14 });
    }
    const TAB = globalThis.__w64tab;
    if (maxtidx >= TAB.length) {
        TAB.grow(Math.max(4096, maxtidx + 1 - TAB.length));
    }
    const imports = { e: { m: wasmMemory, t: TAB } };
    const ip = Number(ipp);
    for (let i = 0; i < nimp; i++) {
        /* memory64: the emscripten table is i64-indexed */
        imports.e['f' + i] = wasmTable.get(BigInt(dv.getUint32(ip + i * 4, true)));
    }
    let inst;
    try {
        inst = new WebAssembly.Instance(new WebAssembly.Module(mod_bytes), imports);
    } catch (e) {
        console.log('W64BATCHFAIL nimp=' + nimp + ' len=' + modlen + ': ' + e);
        /* stash the failing module for post-mortem: the page FS survives
         * the worker crash, unlike queued console messages (the lockstep
         * driver salvages logs the same way) */
        try {
            globalThis.__w64nf = (globalThis.__w64nf || 0) + 1;
            FS.writeFile('/w64fail-' + globalThis.__w64nf + '.wasm', mod_bytes);
            console.log('W64FAILSAVED /w64fail-' + globalThis.__w64nf + '.wasm');
        } catch (e2) { console.log('W64FAILSAVE-ERR ' + e2); }
        throw e;
    }
    /* GC nudge: SpiderMonkey does not count a module's executable
     * memory as GC pressure, so in a worker that never yields, dropped
     * (evicted) modules pile up until the ~16k-module executable
     * budget is exhausted (measured: a 6000-module FIFO OOMs at 16.3k
     * created; ~80 KB of ordinary allocation per module keeps it
     * collected).  A throwaway 32 MB buffer every 256 instantiations
     * is that pressure; V8 is indifferent to it. */
    globalThis.__w64ninst = (globalThis.__w64ninst || 0) + 1;
    if ((globalThis.__w64ninst & 255) === 0) {
        const junk = new ArrayBuffer(32 << 20);
        new Uint8Array(junk)[0] = 1;
    }
    return addFunction(inst.exports.run, 'jjjii');
});

/* flaky-batch-corruption forensics: stage-time vs close-time source
 * comparison.  Every member checksums its staged body bytes at add
 * time; w64_batch_close re-reads the source (LEB + checksum) before
 * assembling.  A mismatch means the CODE BUFFER was overwritten in
 * place between this member's staging and the batch close — nothing
 * in the batch machinery writes there, so that would be a foreign
 * writer.  On any failure the full evidence set is written to the
 * page FS as /w64bad-<id>.bin (all member sources + records + the
 * assembled module if any) — the console drops multi-line output.
 * All checks are O(bytes) per close (~60KB), amortized over a
 * WebAssembly.Module compile, and free when nothing fails. */

#define W64_FNV0 0x811c9dc5u

static uint32_t w64_sum(const uint8_t *p, uint32_t n)
{
    uint32_t h = W64_FNV0;
    while (n--) {
        h = (h ^ *p++) * 0x01000193u;
    }
    return h;
}

/* read the emitter's 5-byte padded size LEB (values < 2^32, so 5
 * bytes is the cap; the emitter always writes exactly 5) */
static uint32_t w64_read_leb5(const uint8_t *p, uint32_t *nbytes)
{
    uint32_t v = 0, shift = 0, k = 0;
    do {
        uint8_t b = p[k++];
        v |= (uint32_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            break;
        }
    } while (k < 5);
    *nbytes = k;
    return v;
}

/* append a forensic record to the open /w64bad file, if any */
static FILE *w64_badf;
static unsigned w64_badn;

static void w64_bad_open(void)
{
    char name[32];
    if (w64_badf) {
        return;
    }
    snprintf(name, sizeof(name), "/w64bad-%u.bin", ++w64_badn);
    w64_badf = fopen(name, "wb");
    fprintf(stderr, "W64BADFILE %s\n", name);
}

static void w64_bad_member(unsigned m, const char *why)
{
    const uint8_t *src = (const uint8_t *)(uintptr_t)B.member[m].tcptr;
    uint32_t fs = m ? B.member[m - 1].fix_end : 0;
    unsigned i;

    fprintf(stderr, "W64BADSRC member %u tcptr=%#x len=%u sum=%08x: %s\n",
            m, B.member[m].tcptr, B.member[m].body_len, B.member[m].sum,
            why);
    if (!w64_badf) {
        return;
    }
    /* records + full source region: descriptor, prelude, body and the
     * following 64 bytes of slack (import table + next TB head) */
    fprintf(w64_badf, "M %u tcptr=%#x body_len=%u fix_end=%u sum=%08x"
            " why=%s fixes=%u\n",
            m, B.member[m].tcptr, B.member[m].body_len,
            B.member[m].fix_end, B.member[m].sum, why,
            B.member[m].fix_end - fs);
    for (i = fs; i < B.member[m].fix_end; i++) {
        fprintf(w64_badf, "F pos=%u uimp=%u\n", B.fix[i].pos, B.fix[i].uimp);
    }
    fwrite(src, 1, W64_BODY_OFF + B.member[m].body_len + 64, w64_badf);
    fprintf(w64_badf, "\nEOM %u\n", m);
}

static void w64_bad_close(void)
{
    if (w64_badf) {
        unsigned i;
        fprintf(w64_badf, "B id=%u members=%u utypes=%u uimps=%u"
                " next_tidx=%u\n",
                B.id, B.n_member, B.n_utypes, B.n_uimp, w64_next_tidx);
        for (i = 0; i < B.n_uimp; i++) {
            fprintf(w64_badf, "I fptr=%u type=%u\n",
                    B.uimp[i].fptr, B.uimp[i].type);
        }
        fclose(w64_badf);
        w64_badf = NULL;
    }
}

/* the open batch, viewed as a source (B's arrays are inline) */
static struct w64_bsrc B_src;

static void w64_bsrc_of_open(void)
{
    B_src.utype = B.utype;
    B_src.uimp = B.uimp;
    B_src.member = B.member;
    B_src.fix = B.fix;
    B_src.n_utypes = B.n_utypes;
    B_src.n_uimp = B.n_uimp;
    B_src.n_member = B.n_member;
    B_src.n_fix = B.n_fix;
    B_src.id = B.id;
}

/* Assemble the batch module from @src (staged bodies re-read from the
 * code buffer) and instantiate it; returns the run-thunk fidx, 0 when
 * the assembled module failed validation (the members then stay on
 * temp modules — only possible for the open batch). */
static uint32_t w64_assemble_instantiate(const struct w64_bsrc *src,
                                         uint32_t *pmaxtidx)
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
    size_t code_sec = 0;
    uint64_t code_total = 0;

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
    mb_u8(&sec, 0x07);                    /* 64-bit | shared | max */
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

    /* function section: members (type 0) + thunk */
    sec.n = 0;
    mb_uleb(&sec, src->n_member + 1);
    for (m = 0; m < src->n_member; m++) {
        mb_uleb(&sec, 0);
    }
    mb_uleb(&sec, src->n_utypes);            /* the thunk's type index */
    mb_sec(&mod, 3, &sec);

    /* export section: "run" -> the thunk (funcidx n_uimp + n_member;
     * function indices count only the helper imports) */
    sec.n = 0;
    mb_uleb(&sec, 1);
    mb_u8(&sec, 3); mb_u8(&sec, 'r'); mb_u8(&sec, 'u'); mb_u8(&sec, 'n');
    mb_u8(&sec, 0x00);
    mb_uleb(&sec, src->n_uimp + src->n_member);
    mb_sec(&mod, 7, &sec);

    /* element section: one active segment per member —
     * TAB[tidx] = member function n_uimp + m */
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

    /* code section: staged bodies (rewritten to union import indices)
     * + the thunk; the total size is a padded 5-byte LEB so it never
     * shifts when the body sum crosses a LEB width boundary */
    code_sec = mod.n;
    {
        uint64_t total = (src->n_member + 1) < 128 ? 1 : 2;
        total += sizeof(thunk_body);
        for (m = 0; m < src->n_member; m++) {
            total += src->member[m].body_len;
        }
        code_total = total;
        mb_u8(&mod, 10);
        mb_uleb_p5(&mod, (uint32_t)total);
        mb_uleb(&mod, src->n_member + 1);
    }
    for (m = 0; m < src->n_member; m++) {
        const uint8_t *body = (const uint8_t *)(uintptr_t)src->member[m].tcptr;
        uint32_t fs = m ? src->member[m - 1].fix_end : 0;
        size_t off = mod.n;

        mb_put(&mod, body + W64_BODY_OFF, src->member[m].body_len);
        for (i = fs; i < src->member[m].fix_end; i++) {
            uint8_t *p = mod.b + off + (src->fix[i].pos - W64_BODY_OFF);
            p[0] = (uint8_t)((src->fix[i].uimp & 0x7f) | 0x80);
            p[1] = (uint8_t)(src->fix[i].uimp >> 7);
        }
    }
    mb_put(&mod, thunk_body, sizeof(thunk_body));

    /* Validate the assembled code section exactly the way the wasm
     * decoder will walk it (body-size LEBs, section total) plus the
     * call-fixup rewrites' bounds.  A failed batch used to kill the
     * vCPU worker outright (WebAssembly.Module CompileError); instead:
     * print forensics and skip the landing — every member keeps its
     * temp module, so execution continues correctly, just unbatched. */
    {
        size_t k = code_sec + 1 + 5;    /* skip section id + padded LEB */
        uint64_t v = 0;
        unsigned shift = 0, count;
        bool bad = false;

        for (;;) {                      /* count LEB (minimal) */
            uint8_t b = mod.b[k++];
            v |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
        }
        count = (unsigned)v;
        if (count != src->n_member + 1) {
            fprintf(stderr, "W64BATCHBAD count LEB=%u members+1=%u\n",
                    count, src->n_member + 1);
            bad = true;
        }
        for (m = 0; m < src->n_member && !bad; m++) {
            uint32_t fs = m ? src->member[m - 1].fix_end : 0;
            const uint8_t *srcp =
                (const uint8_t *)(uintptr_t)src->member[m].tcptr;
            v = 0; shift = 0;
            for (;;) {
                uint8_t b = mod.b[k++];
                v |= (uint64_t)(b & 0x7f) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
                if (shift > 35) break;
            }
            if (v != src->member[m].body_len - 5) {
                fprintf(stderr, "W64BATCHBAD member %u: walked size %llu"
                        " != staged %u (mod+0x%zx)\n",
                        m, (unsigned long long)v,
                        src->member[m].body_len - 5, k);
                fprintf(stderr, "  staged hdr:");
                for (i = 0; i < 12; i++) {
                    fprintf(stderr, " %02x", srcp[W64_BODY_OFF + i]);
                }
                fprintf(stderr, "\n  body_len=%u fixes=%u\n",
                        src->member[m].body_len,
                        src->member[m].fix_end - fs);
                /* source-vs-copy comparison around the desync, plus the
                 * previous member's tail: pins whether the bytes were
                 * already garbage in the code buffer or got mangled in
                 * the assembled module */
                {
                    size_t mo = k - 4;   /* a few bytes back */
                    fprintf(stderr, "  mod  @0x%zx:", mo);
                    for (i = 0; i < 16; i++) {
                        fprintf(stderr, " %02x", mod.b[mo + i]);
                    }
                    fprintf(stderr, "\n  src  @bodyoff-%u:",
                            (unsigned)(4));
                    for (i = 0; i < 16; i++) {
                        fprintf(stderr, " %02x", srcp[W64_BODY_OFF - 4 + i]);
                    }
                    if (m > 0) {
                        const uint8_t *pv =
                            (const uint8_t *)(uintptr_t)src->member[m - 1].tcptr;
                        uint32_t pl = src->member[m - 1].body_len;
                        fprintf(stderr, "\n  prev tail (len %u):", pl);
                        for (i = 0; i < 12; i++) {
                            fprintf(stderr, " %02x",
                                    pv[W64_BODY_OFF + pl - 12 + i]);
                        }
                    }
                    fprintf(stderr, "\n");
                }
                bad = true;
                break;
            }
            k += v;
        }
        if (!bad) {
            /* thunk body: size LEB 13 */
            if (mod.b[k] != 13) {
                fprintf(stderr, "W64BATCHBAD thunk size byte %02x @0x%zx\n",
                        mod.b[k], k);
                bad = true;
            } else {
                k += 1 + 13;
            }
        }
        if (!bad && k - (code_sec + 1 + 5) != code_total) {
            fprintf(stderr, "W64BATCHBAD section walk %zu != total %llu\n",
                    k - (code_sec + 1 + 5), (unsigned long long)code_total);
            bad = true;
        }
        if (!bad) {
            for (m = 0; m < src->n_member; m++) {
                uint32_t fs = m ? src->member[m - 1].fix_end : 0;
                uint32_t bl = src->member[m].body_len;
                for (i = fs; i < src->member[m].fix_end; i++) {
                    if (src->fix[i].pos < W64_BODY_OFF + 5 ||
                        src->fix[i].pos + 2 > W64_BODY_OFF + bl) {
                        fprintf(stderr, "W64BATCHBAD fixup oob member %u:"
                                " pos %u body [%u,%u)\n",
                                m, src->fix[i].pos, W64_BODY_OFF,
                                W64_BODY_OFF + bl);
                        bad = true;
                    }
                }
            }
        }
        if (bad) {
            fprintf(stderr, "W64BATCHSKIP id=%u members=%u bytes=%zu "
                    "(members stay on temp modules)\n",
                    src->id, src->n_member, mod.n);
            /* full evidence: all member sources + the assembled module
             * bytes that failed the walk */
            if (src == &B_src) {
                w64_bad_open();
                if (w64_badf) {
                    fwrite(mod.b, 1, mod.n, w64_badf);
                    fprintf(w64_badf, "\nEOMOD %zu\n", mod.n);
                    for (m = 0; m < src->n_member; m++) {
                        w64_bad_member(m, "assembled walk failed");
                    }
                    w64_bad_close();
                }
            }
            g_free(mod.b);
            g_free(sec.b);
            return 0;
        }
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
    *pmaxtidx = maxtidx;

    {   /* batch diagnostics (W64_DEBUG=1) */
        static int debug = -1;
        if (debug < 0) {
            debug = getenv("W64_DEBUG") != NULL;
        }
        if (debug) {
            static uint32_t n_closed;
            fprintf(stderr, "W64BATCH close#%u id=%u members=%u utypes=%u "
                    "uimps=%u bytes=%zu fixes=%u maxtidx=%u live=%u\n",
                    ++n_closed, src->id, src->n_member, src->n_utypes,
                    src->n_uimp, mod.n, src->n_fix, maxtidx, w64_live_n);
        }
    }

    return thunk;
}

/* Drop the oldest live batch: its members fall back to the dispatcher
 * (fidx 0 + batch tag), which re-assembles the module on demand. */
static void w64_batch_evict_oldest(void)
{
    struct w64_landed *l = w64_live_head;
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
    if (l->small) {
        w64_small_n--;
        w64_small_members -= l->src.n_member;
    }

    for (m = 0; m < l->src.n_member; m++) {
        uint32_t *desc = (uint32_t *)(uintptr_t)l->src.member[m].tcptr;
        /* a descriptor re-used by a later translation carries another
         * batch id (or a mod_len) by now: leave it alone */
        if (desc[W64_DESC_BATCH / 4] == (W64_BATCH_TAG | l->src.id)) {
            desc[W64_DESC_FIDX / 4] = 0;
        }
    }
    w64_tab_unset((uintptr_t)l->tidx, l->src.n_member);
    w64_remove((int)l->thunk);
    l->thunk = 0;
}

static void w64_live_unlink(struct w64_landed *l)
{
    struct w64_landed **pp = &w64_live_head, *prev = NULL;
    while (*pp && *pp != l) {
        prev = *pp;
        pp = &(*pp)->next;
    }
    if (*pp) {
        *pp = l->next;
        if (w64_live_tail == l) {
            w64_live_tail = prev;
        }
        w64_live_n--;
        if (l->small) {
            w64_small_n--;
            w64_small_members -= l->src.n_member;
        }
    }
    l->next = NULL;
}

static void w64_compact(void);

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
    if (l->small) {
        w64_small_n++;
        w64_small_members += l->src.n_member;
        if (w64_small_n >= W64_COMPACT_BATCHES ||
            w64_small_members >= W64_COMPACT_MEMBERS) {
            w64_compact();
        }
    }
    while (w64_live_n > w64_live_max()) {
        w64_batch_evict_oldest();
    }
}

static void w64_landed_flip(struct w64_landed *l)
{
    unsigned m;
    for (m = 0; m < l->src.n_member; m++) {
        uint32_t *desc = (uint32_t *)(uintptr_t)l->src.member[m].tcptr;
        if (desc[W64_DESC_BATCH / 4] == (W64_BATCH_TAG | l->src.id)) {
            desc[W64_DESC_FIDX / 4] = l->thunk;
        }
    }
}

/* Evicted member executed again: re-instantiate its batch. */
static bool w64_batch_ensure(uint32_t id)
{
    struct w64_landed *l;
    static unsigned n_reensure;

    if (id == 0 || id >= w64_landed_cap || !(l = w64_landed_by_id[id])) {
        return false;
    }
    if (l->thunk) {
        return true;
    }
    l->thunk = w64_assemble_instantiate(&l->src, &l->maxtidx);
    if (!l->thunk) {
        return false;
    }
    n_reensure++;
    if ((n_reensure & 1023) == 0 && getenv("W64_DEBUG")) {
        fprintf(stderr, "W64BATCH re-ensure #%u (live=%u landed=%u)\n",
                n_reensure, w64_live_n, w64_landed_n);
    }
    w64_landed_flip(l);
    w64_live_push(l);
    return true;
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
            g_free((void *)l->src.fix);
            g_free(l->tidx);
            g_free(l);
            w64_landed_by_id[i] = NULL;
        }
    }
    w64_live_head = w64_live_tail = NULL;
    w64_live_n = 0;
    w64_landed_n = 0;
    w64_small_n = 0;
    w64_small_members = 0;
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
    w64_landed_n++;
}

static void w64_landed_free(struct w64_landed *l)
{
    w64_landed_by_id[l->src.id] = NULL;
    w64_landed_n--;
    g_free((void *)l->src.utype);
    g_free((void *)l->src.uimp);
    g_free((void *)l->src.member);
    g_free((void *)l->src.fix);
    g_free(l->tidx);
    g_free(l);
}

/*
 * Compaction: merge the live small batches (one per lookup miss, ~3
 * members each) into a single module.  Every member's staged body is
 * still in the code buffer; only the union tables are rebuilt (types
 * and helper imports deduplicated across the batches, the members'
 * 2-byte call-fixup LEBs remapped) — the identical per-member code
 * compiles once more, at the engine's bulk rate (per-byte cost is
 * small next to the per-module fixed cost that the small batches pay).
 * Keeps the live module count near (members / 1024) instead of one
 * module per miss: Firefox's executable-memory budget, and no
 * eviction churn of hot code.
 */
static void w64_compact(void)
{
    struct w64_landed *l, *merged;
    struct w64_landed **smalls;
    unsigned n_smalls = 0, cap_smalls = w64_small_n;
    struct w64_type *utype;
    struct w64_import *uimp;
    struct w64_member *member;
    struct w64_cfix *fix;
    uint32_t *tidx;
    unsigned n_utypes = 0, n_uimp = 0, n_member = 0, n_fix = 0;
    unsigned cap_utypes = 0, cap_uimp = 0, cap_member = 0, cap_fix = 0;
    unsigned i, m;
    uint32_t maxtidx = 0, thunk;
    static int debug = -1;

    if (debug < 0) {
        debug = getenv("W64_DEBUG") != NULL;
    }
    if (cap_smalls == 0) {
        return;
    }
    smalls = g_new(struct w64_landed *, cap_smalls);
    for (l = w64_live_head; l && n_smalls < cap_smalls; l = l->next) {
        if (l->small && l->thunk) {
            smalls[n_smalls++] = l;
            cap_utypes += l->src.n_utypes;
            cap_uimp += l->src.n_uimp;
            cap_member += l->src.n_member;
            cap_fix += l->src.n_fix;
        }
    }
    if (n_smalls < 2) {
        g_free(smalls);
        return;
    }
    utype = g_new(struct w64_type, cap_utypes);
    uimp = g_new(struct w64_import, cap_uimp);
    member = g_new(struct w64_member, cap_member);
    fix = g_new(struct w64_cfix, cap_fix ? cap_fix : 1);
    tidx = g_new(uint32_t, cap_member);

    for (i = 0; i < n_smalls; i++) {
        const struct w64_bsrc *src = &smalls[i]->src;
        uint32_t tmap[W64_UMAX_TYPES], imap[W64_UMAX_IMPORTS];
        unsigned t, k, f;

        for (t = 0; t < src->n_utypes; t++) {
            const struct w64_type *ty = &src->utype[t];
            for (k = 0; k < n_utypes; k++) {
                if (utype[k].np == ty->np && utype[k].ret == ty->ret &&
                    memcmp(utype[k].p, ty->p, ty->np) == 0) {
                    break;
                }
            }
            if (k == n_utypes) {
                utype[n_utypes++] = *ty;
            }
            tmap[t] = k;
        }
        for (t = 0; t < src->n_uimp; t++) {
            uint32_t type = tmap[src->uimp[t].type];
            for (k = 0; k < n_uimp; k++) {
                if (uimp[k].fptr == src->uimp[t].fptr && uimp[k].type == type) {
                    break;
                }
            }
            if (k == n_uimp) {
                uimp[n_uimp].fptr = src->uimp[t].fptr;
                uimp[n_uimp].type = type;
                n_uimp++;
            }
            imap[t] = k;
        }
        for (m = 0; m < src->n_member; m++) {
            uint32_t fs = m ? src->member[m - 1].fix_end : 0;
            for (f = fs; f < src->member[m].fix_end; f++) {
                fix[n_fix].pos = src->fix[f].pos;
                fix[n_fix].uimp = imap[src->fix[f].uimp];
                n_fix++;
            }
            member[n_member] = src->member[m];
            member[n_member].fix_end = n_fix;
            tidx[n_member] = smalls[i]->tidx[m];
            maxtidx = MAX(maxtidx, tidx[n_member]);
            n_member++;
        }
    }
    tcg_debug_assert(n_uimp < (1u << 14));   /* 2-byte call-index LEBs */

    merged = g_new0(struct w64_landed, 1);
    merged->src.utype = utype;
    merged->src.uimp = uimp;
    merged->src.member = member;
    merged->src.fix = fix;
    merged->src.n_utypes = n_utypes;
    merged->src.n_uimp = n_uimp;
    merged->src.n_member = n_member;
    merged->src.n_fix = n_fix;
    merged->src.id = ++w64_next_batch_id;
    merged->tidx = tidx;
    merged->small = false;

    thunk = w64_assemble_instantiate(&merged->src, &merged->maxtidx);
    if (!thunk) {
        fprintf(stderr, "W64COMPACT id=%u FAILED (%u batches, %u members)\n",
                merged->src.id, n_smalls, n_member);
        g_free(utype);
        g_free(uimp);
        g_free(member);
        g_free(fix);
        g_free(tidx);
        g_free(merged);
        g_free(smalls);
        /* do not retry on every landing: age the smalls out */
        for (l = w64_live_head; l; l = l->next) {
            if (l->small) {
                l->small = false;
            }
        }
        w64_small_n = 0;
        w64_small_members = 0;
        return;
    }
    merged->thunk = thunk;

    /* re-point every member still owned by its small batch (a
     * descriptor re-used since carries another id or a mod_len) */
    for (i = 0; i < n_smalls; i++) {
        const struct w64_bsrc *src = &smalls[i]->src;
        for (m = 0; m < src->n_member; m++) {
            uint32_t *desc = (uint32_t *)(uintptr_t)src->member[m].tcptr;
            if (desc[W64_DESC_BATCH / 4] == (W64_BATCH_TAG | src->id)) {
                desc[W64_DESC_FIDX / 4] = thunk;
                desc[W64_DESC_BATCH / 4] = W64_BATCH_TAG | merged->src.id;
            }
        }
        /* the merged element segments replaced every chain-table
         * entry; the small module is unreferenced once its thunk goes */
        w64_live_unlink(smalls[i]);
        w64_remove((int)smalls[i]->thunk);
        w64_landed_free(smalls[i]);
    }
    g_free(smalls);

    w64_landed_register(merged);
    w64_live_push(merged);
    if (debug) {
        static unsigned n_compact;
        fprintf(stderr, "W64COMPACT #%u id=%u batches=%u members=%u uimps=%u "
                "utypes=%u fixes=%u live=%u landed=%u\n",
                ++n_compact, merged->src.id, n_smalls, n_member, n_uimp,
                n_utypes, n_fix, w64_live_n, w64_landed_n);
    }
}

static void w64_batch_close(void)
{
    struct w64_landed *l;
    uint32_t maxtidx = 0;
    uint32_t thunk;
    unsigned m;

    /* pre-assembly source re-validation: every member's staged bytes
     * are re-read and compared against the records taken at add time.
     * A mismatch here is the split we need: the code buffer changed
     * under us (or the records were never right) — independent of the
     * module assembly below. */
    {
        bool badsrc = false;
        for (m = 0; m < B.n_member; m++) {
            const uint8_t *src =
                (const uint8_t *)(uintptr_t)B.member[m].tcptr;
            uint32_t nbytes, leb;
            leb = w64_read_leb5(src + W64_BODY_OFF, &nbytes);
            if (nbytes != 5 || leb != B.member[m].body_len - 5) {
                w64_bad_open();
                w64_bad_member(m, "size LEB changed since staging");
                badsrc = true;
                break;
            }
            if (w64_sum(src + W64_BODY_OFF, B.member[m].body_len)
                != B.member[m].sum) {
                w64_bad_open();
                w64_bad_member(m, "body bytes changed since staging");
                badsrc = true;
                break;
            }
        }
        if (badsrc) {
            fprintf(stderr, "W64BATCHSKIP id=%u members=%u SOURCE-CORRUPT"
                    " (members stay on temp modules)\n", B.id, B.n_member);
            w64_bad_close();
            B.id = 0;
            B.n_utypes = 0;
            B.n_uimp = 0;
            B.n_member = 0;
            B.n_fix = 0;
            return;
        }
    }

    w64_bsrc_of_open();
    thunk = w64_assemble_instantiate(&B_src, &maxtidx);
    if (!thunk) {
        fprintf(stderr, "W64BATCHSKIP id=%u members=%u "
                "(members stay on temp modules)\n", B.id, B.n_member);
        B.id = 0;
        B.n_utypes = 0;
        B.n_uimp = 0;
        B.n_member = 0;
        B.n_fix = 0;
        return;
    }

    /* landed record: compact copies of the open batch's tables */
    l = g_new0(struct w64_landed, 1);
    l->src.utype = g_memdup2(B.utype, B.n_utypes * sizeof(*B.utype));
    l->src.uimp = g_memdup2(B.uimp, B.n_uimp * sizeof(*B.uimp));
    l->src.member = g_memdup2(B.member, B.n_member * sizeof(*B.member));
    l->src.fix = g_memdup2(B.fix, B.n_fix * sizeof(*B.fix));
    l->src.n_utypes = B.n_utypes;
    l->src.n_uimp = B.n_uimp;
    l->src.n_member = B.n_member;
    l->src.n_fix = B.n_fix;
    l->src.id = B.id;
    l->thunk = thunk;
    l->maxtidx = maxtidx;
    l->tidx = g_new(uint32_t, B.n_member);
    l->small = true;

    /* land: flip every member to the batch thunk, drop its temp */
    for (m = 0; m < B.n_member; m++) {
        uint32_t *desc = (uint32_t *)(uintptr_t)B.member[m].tcptr;
        if (desc[W64_DESC_FIDX / 4]) {
            w64_remove((int)desc[W64_DESC_FIDX / 4]);
        }
        desc[W64_DESC_FIDX / 4] = thunk;
        desc[W64_DESC_BATCH / 4] = W64_BATCH_TAG | B.id;
        l->tidx[m] = desc[W64_DESC_TIDX / 4];
    }

    w64_landed_register(l);
    w64_live_push(l);

    B.id = 0;
    B.n_utypes = 0;
    B.n_uimp = 0;
    B.n_member = 0;
    B.n_fix = 0;
}

void w64_batch_member(uintptr_t tcptr, uint32_t body_len,
                      const struct w64_cfix *cf, uint32_t ncf)
{
    int mode = w64_batch_mode();

    if (!mode || B.id == 0) {
        return;
    }
    tcg_debug_assert(B.n_member < W64_BATCH_N_MAX);

    /* TB-overflow retry in the same code buffer: the failed attempt's
     * finalize already staged a member (with the attempt's longer
     * body_len and fixup positions); the retry re-translates into the
     * SAME buffer and re-adds.  The stale entry's recorded body_len no
     * longer matches the re-patched (shorter) body-size LEB, and its
     * fixup positions may point beyond the new body — the assembled
     * batch would be corrupt exactly there.  Retries are synchronous,
     * so the stale member is always the last one: truncate it. */
    if (B.n_member > 0 && B.member[B.n_member - 1].tcptr == (uint32_t)tcptr) {
        uint32_t stale_fix_start =
            B.n_member > 1 ? B.member[B.n_member - 2].fix_end : 0;
        fprintf(stderr, "W64BATCHRETRY dup tcptr=%#x old_len=%u new_len=%u"
                " (stale member dropped)\n",
                (unsigned)tcptr, B.member[B.n_member - 1].body_len, body_len);
        B.n_fix = stale_fix_start;
        B.n_member--;
    }

    /* take this TB's call fixups into the batch arena (per-member
     * ranges are closed here, at the success point of translation) */
    if (B.n_fix + ncf > B.cap_fix) {
        B.cap_fix = MAX(B.cap_fix * 2, B.n_fix + ncf + 64);
        B.fix = g_realloc(B.fix, (size_t)B.cap_fix * sizeof(*B.fix));
    }
    memcpy(B.fix + B.n_fix, cf, (size_t)ncf * sizeof(*cf));

    B.member[B.n_member].tcptr = (uint32_t)tcptr;
    B.member[B.n_member].body_len = body_len;
    B.member[B.n_member].fix_end = B.n_fix + ncf;
    B.member[B.n_member].sum =
        w64_sum((const uint8_t *)(uintptr_t)tcptr + W64_BODY_OFF, body_len);
    B.n_fix += ncf;
    B.n_member++;

    /* close on fill, or when a union table is within one TB's worth of
     * new entries (a TB adds at most W64_MAX_* of each) of full */
    if (B.n_member >= (unsigned)mode ||
        B.n_uimp > W64_UMAX_IMPORTS - W64_MAX_IMPORTS - 1 ||
        B.n_utypes > W64_UMAX_TYPES - W64_MAX_TYPES - 1) {
        w64_batch_close();
    }
}

/* First execution of a TB that is still staged in the open batch:
 * assemble + compile the whole batch now (one module for every
 * pending member — the miss TB plus its speculatively translated
 * successors, see cpu-exec.c w64_speculate) instead of a throwaway
 * per-TB module.  Returns false when the TB is not a pending member
 * (batching off, or the batch was skipped for corruption), in which
 * case the caller falls back to the temp module. */
static bool w64_batch_close_pending(uintptr_t tcptr)
{
    static int off = -1;
    unsigned m;

    if (off < 0) {
        off = getenv("W64_NOCLOSEEXEC") != NULL;   /* A/B: temp modules */
    }
    if (B.id == 0 || off) {
        return false;
    }
    for (m = 0; m < B.n_member; m++) {
        if (B.member[m].tcptr == (uint32_t)tcptr) {
            w64_batch_close();
            return ((uint32_t *)tcptr)[W64_DESC_FIDX / 4] != 0;
        }
    }
    return false;
}

/* tb_flush teardown: drop every landed batch thunk and the open batch's
 * temp modules, clear the chain table and recycle the tidx space (the
 * code buffer — descriptors, staged bytes — is freed by the caller). */
void w64_batch_flush(void)
{
    static int debug = -1;
    unsigned landed;
    unsigned m;

    if (debug < 0) {
        debug = getenv("W64_DEBUG") != NULL;
    }
    {
        struct w64_landed *l;
        for (l = w64_live_head; l; l = l->next) {
            w64_remove((int)l->thunk);
            l->thunk = 0;
        }
        landed = w64_live_n;
        w64_landed_free_all();
    }
    for (m = 0; m < B.n_member; m++) {
        uint32_t fidx = ((uint32_t *)(uintptr_t)B.member[m].tcptr)
                        [W64_DESC_FIDX / 4];
        if (fidx) {
            w64_remove((int)fidx);
        }
    }
    if (debug && (landed || B.n_member)) {
        fprintf(stderr, "W64BATCH flush: landed=%u open=%u tidx_next=%u\n",
                landed, B.n_member, w64_next_tidx);
    }
    w64_tab_clear();
    w64_next_tidx = 1;
    B.id = 0;
    B.n_utypes = 0;
    B.n_uimp = 0;
    B.n_member = 0;
    B.n_fix = 0;
}

/* TB-prologue accounting import (see tcg_out_tb_start): runs at EVERY
 * TB entry — dispatcher call or goto_tb chain target — mirroring TCI's
 * INDEX_op_tci_tbhdr so icount2, the diagnostics clock and the lockstep
 * fold stay per-TB-entry exact while TBs are chained. */
void w64_tb_account(unsigned insns)
{
    extern void wasm_tb_account(unsigned insns);
    extern void icount2_advance(uint32_t cycles);

    wasm_tb_account(insns);
    icount2_advance(insns);
    w64_lockstep_account(insns);
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

        /* per-TB accounting (wasm_tb_account / icount2_advance /
         * lockstep fold) now runs in the TB prologue (imported
         * w64_tb_account) so chained entries are counted identically */

        fidx = desc[W64_DESC_FIDX / 4];
        if (fidx == 0) {
            if (desc[W64_DESC_BATCH / 4] & W64_BATCH_TAG) {
                /* evicted batch member: re-assemble its batch from the
                 * staged bodies (desc+4 is the batch tag, not a mod_len) */
                if (!w64_batch_ensure(desc[W64_DESC_BATCH / 4] &
                                      ~W64_BATCH_TAG)) {
                    fprintf(stderr, "w64: evicted batch member (batch %u) "
                            "could not be re-ensured\n",
                            desc[W64_DESC_BATCH / 4] & ~W64_BATCH_TAG);
                    abort();
                }
                fidx = desc[W64_DESC_FIDX / 4];
                tcg_debug_assert(fidx != 0);
            } else if (w64_batch_close_pending(tb)) {
                fidx = desc[W64_DESC_FIDX / 4];
            } else {
                fidx = w64_instantiate(tb);
                tcg_debug_assert(fidx != 0);
                desc[W64_DESC_FIDX / 4] = fidx;
                /* temp module instantiated: desc+4 is no longer needed
                 * as mod_len (the batch landing re-writes it with the
                 * tagged batch id) */
                desc[W64_DESC_BATCH / 4] = 0;
            }
        }

        if (desc[W64_DESC_BATCH / 4] & W64_BATCH_TAG) {
            /* batched member: dispatch through the batch's run thunk,
             * which tail-calls TAB[tidx] with (env, sp, tp) */
            res = ((w64_run_fn *)(uintptr_t)fidx)(
                (uintptr_t)env, (uintptr_t)(w64_frame + 16),
                (uintptr_t)&w64_tb_ptr, desc[W64_DESC_TIDX / 4]);
        } else {
            res = ((w64_tb_fn *)(uintptr_t)fidx)(
                (uintptr_t)env, (uintptr_t)(w64_frame + 16),
                (uintptr_t)&w64_tb_ptr);
        }

        if (LS.stop) {
            /* budget crossed: this insn ran; halt before the next TB.
             * qemu_system_shutdown_request trips an uninitialized
             * main-loop mutex on the wasm build and exit() from the
             * main loop never happens — emscripten exit() from the vCPU
             * is the stop path (flushes stdio, fires onExit). */
            fflush(LS.f);
            exit(0);
        }

        if (res & W64_EXIT_GOTOPTR) {
            uintptr_t next = *(uintptr_t *)w64_frame;
            if (next == 0) {
                return 0;
            }
            tb = next;
            continue;
        }
        return res;
    }
}
