/*
 * wasm diagnostics counters, filled by tcg/tci.c + accel/tcg/cputlb.c +
 * the pmb887x devices and read from JS via the wasm_memstat() export in
 * ui/wasm.c.  See tools/memstat.mjs, tools/diagall.mjs.
 *
 * Most are cold-path (interpreter slow-path entries, TLB fills, TB
 * generation, topology commits) and cost nothing.  The ones that are not
 * are marked with WASM_DIAG_HOT() at their increment sites and compiled
 * out by default - see the block below the enum.
 */
#ifndef QEMU_WASM_DIAG_H
#define QEMU_WASM_DIAG_H

enum {
    WASM_DIAG_LD_HELPER = 0, /* tci_qemu_ld fell to helper_ldXX_mmu */
    WASM_DIAG_ST_HELPER,     /* tci_qemu_st fell to helper_stXX_mmu */
    WASM_DIAG_IO_LD,         /* cputlb MMIO load dispatches */
    WASM_DIAG_IO_ST,         /* cputlb MMIO store dispatches */
    WASM_DIAG_TLB_FILL,      /* tlb_fill_align calls */
    WASM_DIAG_TXN_FAILED,    /* arm_cpu_do_transaction_failed raises */
    WASM_DIAG_TXN_NOEXIT,    /* unused (kept: numeric indices are part
                              * of the wasm_memstat ABI used by the tools) */
    WASM_DIAG_TB_GEN,        /* tb_gen_code calls (translation) */
    WASM_DIAG_TB_FLUSH,      /* tb_flush calls (code buffer full) */
    WASM_DIAG_IO_REWIND,     /* ROM-device io_prepare rewinds (longjmp) */
    WASM_DIAG_ROMD_FLIP,     /* memory_region_rom_device_set_romd transitions */
    WASM_DIAG_TOPO_COMMIT,   /* commits with memory_region_update_pending */
    WASM_DIAG_TOPO_REUSED,   /* stashed-view reuses (saves render+dispatch) */
    WASM_DIAG_LOOKUP,        /* tb_lookup calls (helper_lookup_tb_ptr + loop) */
    WASM_DIAG_LOOKUP_JC,     /* ...served by the jump cache */
    WASM_DIAG_LOOKUP_QHT,    /* ...served by the qht */
    WASM_DIAG_JC_FLUSH,      /* tcg_flush_jmp_cache calls */
    WASM_DIAG_TB_GEN_COUNTED,/* tb_gen_code with a CF_COUNT_MASK-limited TB */
    WASM_DIAG_TLB_FLUSH,     /* tlb_flush_by_mmuidx (full per-mmuidx flush) */
    WASM_DIAG_TLB_FLUSH_RANGE,/* tlb_flush_range_by_mmuidx / page flushes */
    WASM_DIAG_FILL_FETCH,    /* tlb_fill_align for MMU_INST_FETCH */
    WASM_DIAG_FILL_PROBE,    /* tlb_fill_align with probe=true (non-faulting) */
    WASM_DIAG_FILL_SAMEPAGE, /* tlb_set_page_full over an entry for the same page */
    WASM_DIAG_FILL_INVALID,  /* ...page smaller than TARGET_PAGE (refill every access) */
    WASM_DIAG_FILL_LARGE,    /* ...part of a large page */
    WASM_DIAG_FILL_IDX,      /* sum of mmu_idx over fills (avg idx = /fills) */
    WASM_DIAG_FILL_EVICT,    /* fills that evicted a different page (conflict) */
    WASM_DIAG_TLB_SIZE0,     /* current entries of the mmu_idx 0 table (gauge) */
    WASM_DIAG_TLB_USED0,     /* used entries of the mmu_idx 0 table (gauge) */
    WASM_DIAG_HALT,          /* vCPU idle entries (rr_idle_advance: all halted) */
    WASM_DIAG_PHYS_CALL,     /* tlb_flush_phys_ranges calls (topology commits) */
    WASM_DIAG_PHYS_SCAN,     /* ...entries walked by those calls */
    WASM_DIAG_PHYS_DROP,     /* ...entries actually invalidated */
    WASM_DIAG_MOD_BYTES,     /* wasm bytes handed to WebAssembly.Module */
    WASM_DIAG_MOD_COUNT,     /* modules created */
    WASM_DIAG_TB_BYTES,      /* sum of emitted TB body sizes */
    WASM_DIAG_WARP_NS,       /* total ns the idle warp jumped */
    WASM_DIAG_WARP_B0,       /* warp size buckets: <1us,<10us,<100us, */
    WASM_DIAG_WARP_B1,       /*  <1ms,<10ms,<100ms,>=100ms */
    WASM_DIAG_WARP_B2,
    WASM_DIAG_WARP_B3,
    WASM_DIAG_WARP_B4,
    WASM_DIAG_WARP_B5,
    WASM_DIAG_WARP_B6,
    WASM_DIAG_MOD_SRC,       /* assemble source: 1=close 2=compact 3=ensure */
    WASM_DIAG_CLOSE_BYTES,   /* bytes from first batch close */
    WASM_DIAG_COMPACT_BYTES, /* bytes from compaction */
    WASM_DIAG_ENSURE_BYTES,  /* bytes from re-ensure after eviction */
    WASM_DIAG_CLOSE_N,
    WASM_DIAG_COMPACT_N,
    WASM_DIAG_ENSURE_N,
    WASM_DIAG_LC_FILL,       /* helper_lookup_tb_ptr_lc filled a TB's inline cache */
    WASM_DIAG_LC_VHIT,       /* W64_LC_VERIFY: slots the inline test would have taken */
    WASM_DIAG_LC_VBAD,       /* W64_LC_VERIFY: ...that disagreed with the real lookup */
    WASM_DIAG_KEY_GEN,       /* cpu_tb_key_gen_bump calls (every slot retired) */
    WASM_DIAG_LC_CALL,       /* helper_lookup_tb_ptr_lc calls (inline-cache misses) */
    WASM_DIAG_KEY_GEN_FLUSH, /* ...bumps from tcg_flush_jmp_cache */
    WASM_DIAG_KEY_GEN_INVAL, /* ...from tb_phys_invalidate */
    WASM_DIAG_KEY_GEN_PAGE,  /* ...from tb_jmp_cache_clear_page */
    WASM_DIAG_DIF_MUX_REBUILD, /* pmb887x DIF v2 byte-lane mux tables rebuilt */
    WASM_DIAG_DIF_TX_WORD,   /* pmb887x DIF v2 words popped from the TX FIFO */
    WASM_DIAG_DMAC_BURST,    /* pmb887x DMAC dmac_transfer_memory calls */
    WASM_DIAG_DMAC_SCHED_TIMER, /* pmb887x DMAC timer_mod from dmac_schedule */
    WASM_DIAG_DMAC_XLAT_FILL, /* pmb887x DMAC translation-window fills (misses) */
    WASM_DIAG_GPTU_TIMER,    /* pmb887x GPTU QEMU-timer callbacks (T01 + T2) */
    WASM_DIAG_IO_RECOMP,     /* cpu_io_recompile calls (mid-TB MMIO unwinds) */
    WASM_DIAG_IO_BARRIER_EVICT, /* ...that evicted a different barrier PC */
    WASM_DIAG_IO_BARRIER_SPLIT, /* translator TB splits on an io barrier */
    WASM_DIAG_IO_LD_FAST,    /* MMIO loads taken by the fused single-piece path */
    WASM_DIAG_VCLOCK_READ,   /* QEMU_CLOCK_VIRTUAL reads (icount_get) */
    WASM_DIAG_IO_ST_FAST,    /* MMIO stores taken by the fused single-piece path */
    WASM_DIAG_TPU_TIMER,     /* pmb887x TPU QEMU-timer callbacks */
    WASM_DIAG_TPU_REARM,     /* pmb887x TPU timer_mod calls (any caller) */
    WASM_DIAG_ML_WAKE,       /* qemu_main_loop_wake calls (futex wakes issued) */
    WASM_DIAG_ML_WAKE_DUP,   /* qemu_notify_event's own wake after aio_notify */
    WASM_DIAG_TPU_RAM_W,     /* pmb887x TPU RAM writes */
    WASM_DIAG_TPU_RAM_SKIP,  /* ...that skipped the advance (no deadline effect) */
    WASM_DIAG_HFLAGS,        /* AArch32 hflags rebuilds */
    WASM_DIAG_HFLAGS_FAST,   /* ...taken by the pre-v6 short path */
    WASM_DIAG_HFLAGS_BAD,    /* ...where it disagreed with the generic one */
    WASM_DIAG_SPEC_MISS,     /* w64_speculate calls (one per lookup miss) */
    WASM_DIAG_SPEC_NOSUCC,   /* ...where the miss TB recorded no goto_tb successor */
    WASM_DIAG_SPEC_EXISTS,   /* speculation edges whose target was already translated */
    WASM_DIAG_SPEC_NOTRAM,   /* ...rejected by the non-faulting executable-RAM probe */
    WASM_DIAG_SPEC_MADE,     /* ...translated into the open batch */
    WASM_DIAG_HFLAGS_CALLS,  /* arm_rebuild_hflags() entries (unconditional:
                              * the gated WASM_DIAG_HFLAGS counts a different
                              * site, and the question here is the call rate) */
    WASM_DIAG_LOOKUP_CONFL,  /* ...qht lookups whose jump-cache slot held another
                              * TB: a capacity/conflict miss.  qht minus this is
                              * what the flushes and cold pcs cost. */
    WASM_DIAG_MOD_NS,        /* wall ns in w64_batch_instantiate: the
                              * browser's WebAssembly.Module + Instance for
                              * one batch, charged to the vCPU thread that
                              * waits for it.  Always on (~1k/s at boot) */
    WASM_DIAG_FILL_NS,       /* wall ns inside tlb_fill_align (phase build) */
    WASM_DIAG_TB_ICOUNT,     /* sum of tb->icount over translated TBs:
                              * /tbGen is the mean guest insns per TB */
    WASM_DIAG_TB_GEN_NS,     /* wall ns inside tb_gen_code, and only in a
                              * WASM_DIAG_TIME_PHASES build (see
                              * accel/tcg/translate-all.c); the module
                              * compile is the browser's own time, which
                              * tools/modcost.mjs reads from the worker */
    WASM_DIAG_LDST_GEN,      /* guest memory ops emitted (translation) */
    WASM_DIAG_LDST_GEN_NOPROBE, /* ...with no inline TLB probe: bswap, or an
                              * atomicity class the backend will not inline
                              * (MO_ATOM_IFALIGN_PAIR is ldrd/strd).  These
                              * call the *_mmu helper on EVERY execution. */
    WASM_DIAG_LDST_NOPROBE,  /* executions of those, counted in the generated
                              * code itself -- W64_LDSTCOUNT=1 only */
    WASM_DIAG_LDST_MISS,     /* executions where a probe was emitted and
                              * missed (TLB miss or MMIO) -- same knob */
    WASM_DIAG_LDST_EXEC,     /* executions of any guest memory op, the
                              * per-instruction denominator -- W64_LDSTCOUNT=2,
                              * which is a bump on the hottest path there is */
    WASM_DIAG_SLOW_MISS,     /* mmu_lookup1: the entry was for another page */
    WASM_DIAG_SLOW_MMIO,     /* ...entry matched, TLB_MMIO */
    WASM_DIAG_SLOW_NOTDIRTY, /* ...entry matched, TLB_NOTDIRTY (clean page) */
    WASM_DIAG_SLOW_OTHER,    /* ...entry matched, some other flag */
    WASM_DIAG_SLOW_CLEAN,    /* ...entry matched with NO flag set: the inline
                              * probe rejected an access it could have served,
                              * and the helper round trip bought nothing */
    WASM_DIAG_IO_NS,         /* summed wall ns over SAMPLED fused MMIO
                              * LOAD dispatches -- ioNs/ioNsN is ns each.
                              * The browser's clock is quantized to 1 ms, so
                              * a sample is 0 or 1 ms and the mean is a
                              * straddle-probability estimate: unbiased, but
                              * its error is 1/sqrt(nonzero samples). */
    WASM_DIAG_IO_NS_N,       /* ...how many were sampled (1 in W64_IO_SAMPLE) */
    WASM_DIAG_IO_ST_NS,      /* same for fused MMIO STORE dispatches */
    WASM_DIAG_IO_ST_NS_N,
    WASM_DIAG_BQL_NS,        /* ...of which, bql_lock_mmio() alone */
    WASM_DIAG_BQL_NS_N,
    WASM_DIAG_CAL_NS,        /* straddle floor: an EMPTY timed interval,
                              * sampled like the others.  calNs/calNsN is
                              * what two clock reads cost by themselves, and
                              * must be subtracted from ioNs, ioStNs and
                              * bqlNs before any of them is believed. */
    WASM_DIAG_CAL_NS_N,
    WASM_DIAG_DEV_R_NS,      /* the device read callback alone */
    WASM_DIAG_DEV_R_NS_N,
    WASM_DIAG_DEV_W_NS,      /* the device write callback alone: no BQL, no
                              * dispatch, just full->io_write_fn() */
    WASM_DIAG_DEV_W_NS_N,
    WASM_DIAG_TPU_W_NS,      /* the pmb887x TPU write handler alone */
    WASM_DIAG_TPU_W_NS_N,
    WASM_DIAG_SLOWW_ADDR,    /* sum of guest addresses of duration-weighted
                              * MMIO store samples: /slowwN names the device */
    WASM_DIAG_SLOWW_N,
    WASM_DIAG_SLOWW_MIN,     /* ...and the range they span (gauges) */
    WASM_DIAG_SLOWW_MAX,
    WASM_DIAG_SLOWW_B0,      /* duration-weighted MMIO-store samples bucketed
                              * by phys_addr >> 28: B0 = RAM/BROM, BA = the
                              * NOR flash window, BF = the module MMIO block */
    WASM_DIAG_SLOWW_B1,
    WASM_DIAG_SLOWW_B2,
    WASM_DIAG_SLOWW_B3,
    WASM_DIAG_SLOWW_B4,
    WASM_DIAG_SLOWW_B5,
    WASM_DIAG_SLOWW_B6,
    WASM_DIAG_SLOWW_B7,
    WASM_DIAG_SLOWW_B8,
    WASM_DIAG_SLOWW_B9,
    WASM_DIAG_SLOWW_BA,
    WASM_DIAG_SLOWW_BB,
    WASM_DIAG_SLOWW_BC,
    WASM_DIAG_SLOWW_BD,
    WASM_DIAG_SLOWW_BE,
    WASM_DIAG_SLOWW_BF,
    WASM_DIAG_LC2_HIT,       /* W64_LC2 ceiling probe: misses a second cache
                              * way would have caught */
    WASM_DIAG_HFLAGS_NS,     /* sampled time in arm_rebuild_hflags */
    WASM_DIAG_HFLAGS_NS_N,
    WASM_DIAG_LC_NS,         /* sampled time in helper_lookup_tb_ptr_lc */
    WASM_DIAG_LC_NS_N,
    WASM_DIAG_RO_FLIP,       /* memory_region_set_readonly transitions */
    WASM_DIAG_EBU_W,         /* pmb887x EBU register writes */
    WASM_DIAG_EBU_CHANGE,    /* ...chip-selects the sweep actually remapped */
    WASM_DIAG_EBU_CH_SIZE,   /* ...of which by size, base, enable, readonly */
    WASM_DIAG_EBU_CH_ADDR,
    WASM_DIAG_EBU_CH_EN,
    WASM_DIAG_EBU_CH_RO,
    WASM_DIAG_EBU_W_NS,      /* ...and the sampled time in the handler */
    WASM_DIAG_EBU_W_NS_N,
    WASM_DIAG_BQL_CALL,      /* bql_lock_mmio() calls */
    WASM_DIAG_BQL_TOOK,      /* ...that reached the real pthread_mutex_lock:
                              * the rest were served by the deferred hold */
    WASM_DIAG_BQL_REAL_UNLOCK, /* bql_unlock_mmio() calls that really released
                              * (somebody was waiting), ending a deferral */
    /*
     * TOPO_COMMIT counts both commit paths together, which stopped being
     * useful once 0083 gave the cheap one real traffic.  These split it,
     * and TOPO_R* attributes the expensive one: every setter that raises
     * memory_region_update_pending ORs its reason bit into a mask, and a
     * full commit bumps one counter per bit.  A transaction that batches
     * several setters bumps several - the sum over R* is >= TOPO_FULL by
     * design, because the question is "who is in here", not "who won".
     */
    WASM_DIAG_TOPO_FULL,     /* commits that re-rendered every flat view */
    WASM_DIAG_TOPO_VAR,      /* ...that adopted a variant instead (0083) */
    WASM_DIAG_TOPO_R_LOG,    /* memory_region_set_log */
    WASM_DIAG_TOPO_R_NONVOL, /* memory_region_set_nonvolatile */
    WASM_DIAG_TOPO_R_EVFD,   /* memory_region_{add,del}_eventfd */
    WASM_DIAG_TOPO_R_ADDSUB, /* memory_region_add_subregion* */
    WASM_DIAG_TOPO_R_DELSUB, /* memory_region_del_subregion */
    WASM_DIAG_TOPO_R_ENABLE, /* memory_region_set_enabled */
    WASM_DIAG_TOPO_R_SIZE,   /* memory_region_set_size */
    WASM_DIAG_TOPO_R_ALIAS,  /* memory_region_set_alias_offset */
    WASM_DIAG_TOPO_R_UNMERG, /* memory_region_set_unmergeable */
    WASM_DIAG_TOPO_R_DIRTY,  /* global dirty-log start/stop */
    WASM_DIAG_PAD_SINK,      /* W64_LDSTPAD's live-out (never read as a count) */
    /*
     * MOD_NS split four ways.  The JS side has timed these all along into
     * __w64tR/M/I/A, but those globals live in the vCPU worker and the
     * worker runs the guest without yielding, so no evaluate() ever got to
     * read them.  Charged into wasm_diag_stat instead, they reach
     * _wasm_memstat like everything else.  ns, summed.
     */
    WASM_DIAG_MOD_RESOLVE_NS, /* building the import object */
    WASM_DIAG_MOD_COMPILE_NS, /* new WebAssembly.Module */
    WASM_DIAG_MOD_INST_NS,    /* new WebAssembly.Instance */
    WASM_DIAG_MOD_ADDFN_NS,   /* addFunction on the export */
    WASM_DIAG_MOD_UIMP,       /* imports resolved, summed over modules */
    WASM_DIAG_MOD_CLOSE_CNS,  /* ...compile ns, first-close modules only */
    WASM_DIAG_MOD_COMPACT_CNS,/* ...compile ns, compaction modules only */
    WASM_DIAG_MODBENCH_NS,   /* W64_MODBENCH: back-to-back compiles of one
                              * real module, in the vCPU worker's isolate */
    WASM_DIAG_MODBENCH_N,
    WASM_DIAG_MOD_PRE_NS,    /* entry -> import loop: DataView, the byte copy,
                              * the table check.  modNs minus the four phases
                              * left ~14.5 us per module unaccounted */
    WASM_DIAG_MOD_POST_NS,   /* Instance -> addFunction: the GC nudge */
    WASM_DIAG_N
};

extern uint64_t wasm_diag_stat[WASM_DIAG_N];

/*
 * The counters above started out on cold paths, where the cost is noise.
 * Several now sit on the hottest paths there are - every MMIO dispatch,
 * every virtual-clock read, every hflags rebuild - which on an idle S75
 * is ~14M read-modify-writes a second across four cache lines.
 *
 * WASM_DIAG_HOT() marks those.  To get them back - which is what you
 * want whenever a rate, not a wall-clock number, is the thing in
 * question - add
 *
 *     #define WASM_DIAG_HOT_COUNTERS 1
 *
 * above this block and rebuild; uibench and diagall then report real
 * rates for ioLd, ioSt, vclock, hflags and tpuRamW instead of zero.
 * Never measure wall-clock A/B against such a build.  Cold counters are
 * unconditional, so the tb/flush/fill/warp diagnostics keep working in
 * the shipping build.
 */
/*
 * WASM_DIAG_TIME_PHASES: wall-clock timers around whole phases, read as
 * a share of wall time by tools/modcost.mjs.  Separate from the hot
 * counters because the cost is different in kind: emscripten's
 * gettimeofday is a call out to JS, so a timer is only affordable on a
 * phase entered a few thousand times a second, and the sites it guards
 * (tb_gen_code ~4k/s, tlb_fill_align ~54k/s) are above that.  Like the
 * hot counters: define it here, rebuild, measure -- and never A/B wall
 * clock against such a build.
 *
 *     #define WASM_DIAG_TIME_PHASES 1
 *
 * WASM_DIAG_MOD_NS is NOT behind this: module compiles are ~1k/s, so
 * that one is affordable always and ships on.
 */
#ifdef WASM_DIAG_HOT_COUNTERS
#define WASM_DIAG_HOT(idx) (wasm_diag_stat[idx]++)
#else
#define WASM_DIAG_HOT(idx) ((void) 0)
#endif

#endif
