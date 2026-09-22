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
    WASM_DIAG_DIF_TX_WORD,   /* pmb887x DIF words popped from the TX FIFO */
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
    WASM_DIAG_LC2_HIT,       /* retired with the W64_LC2 probe; the slot
                              * stays because the ABI is positional */
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
    WASM_DIAG_PAD_SINK,      /* retired with the calibration pads (see
                              * LC2_HIT on why the slot stays) */
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
    /*
     * Miss classification (accel/tcg/cpu-exec.c w64_speculate).  Module
     * count is miss count, so the only question that matters about a miss
     * is whether anything could have predicted it.  Two static edge
     * classes have been tried and failed (call returns: -2.6 %; the
     * address after an unconditional transfer: 0 %), so the next question
     * is coarser: is the missed pc in a guest page that has been
     * translated from before, or a page nothing has ever run in?
     *
     * misses-per-touched-page is what decides whether translating a whole
     * page on first entry is worth building: at ~12 us a translation
     * against ~96 us a module, it pays if a page costs fewer than ~8
     * wasted translations per miss it removes.
     */
    WASM_DIAG_MISS_NEWPAGE,  /* ...the first miss ever in its guest page */
    /*
     * The C dispatcher (tcg/wasm64/wasm64.c tcg_qemu_tb_exec).  After
     * 0091 a chained TB never comes back here -- goto_tb and goto_ptr
     * both tail-call through the shared table -- so DISP_ITER minus
     * DISP_CALL is how often a chain really did unwind, and DISP_CALL
     * is how often cpu_exec_loop re-entered.  Both are needed to turn a
     * profile share for this function into ns per call.
     */
    WASM_DIAG_DISP_CALL,     /* tcg_qemu_tb_exec entries */
    WASM_DIAG_DISP_ITER,     /* ...its loop iterations */
    /*
     * Exit mix, counted in the generated code itself (W64_XCOUNT=1, a
     * measurement build like W64_LDSTCOUNT).  A TB entry costs a
     * return_call_indirect through the shared table whatever ends it,
     * and the only lever on that cost is executing fewer of them, so
     * the question is which exits could ever be merged away: X_GOTOTB
     * is a direct branch the translator already resolved, X_SELF the
     * subset that chains back to the same TB (a guest loop), X_GOTOPTR
     * an indirect one taken through the inline cache.
     */
    WASM_DIAG_X_GOTOTB,      /* which = 0: the branch the frontend took */
    WASM_DIAG_X_GOTOTB1,     /* which = 1: the fall-through */
    WASM_DIAG_X_SELF,
    WASM_DIAG_X_GOTOPTR,
    /*
     * Retired with the inline-TLB-probe ceiling family, whose findings are
     * in the playbook (the probe is 5.1 % of EL71 wall; a two-load check
     * saves 2.3 %; hoisting collects 66 % of that).  The slots stay
     * because the ABI is positional.
     */
    WASM_DIAG_TLBC_HIT,
    WASM_DIAG_TLBC_MISS,
    /*
     * What an interpreter tier would have to interpret (W64_TBHIST=1,
     * and only meaningful with W64_NOCLOSEEXEC=1).
     *
     * The tier's saving is easy to price -- fewer batch closes at ~86 us
     * each -- but its cost is not: a TB stays interpreted from its
     * translation until its batch closes, and how many times it runs in
     * that window depends on an interleaving of translation and
     * execution order that only the real thing produces.  W64_NOCLOSEEXEC
     * *is* that real thing with the close deferred: batches then fill to
     * W64_BATCH_N.  So at close time, sum what the members have already
     * executed (w64_tbhist, exact per TB) and the interpreted-entry
     * count is measured rather than assumed.
     */
    WASM_DIAG_CLOSE_PRE_ENT, /* member entries already run at batch close */
    WASM_DIAG_CLOSE_PRE_TB,  /* ...members that had run at least once */

    WASM_DIAG_IREC_N,        /* TBs the interpreter tier recorded */
    WASM_DIAG_IREC_BYTES,    /* bytes recorded, cumulative: the harness
                                differences samples, so a live gauge here
                                reads as a negative rate whenever a record
                                is dropped.  Live = IREC_BYTES - IREC_FREED */
    WASM_DIAG_INTERP_ENT,    /* TB entries served by the interpreter */
    WASM_DIAG_TB_JOIN,       /* deferred taken paths (0108) whose target the
                              * same TB went on to translate, so the branch
                              * became a br and cost no exit at all */
    WASM_DIAG_TB_ABSORB,     /* unconditional direct branches whose target the
                              * TB swallowed by translating on from there,
                              * costing no exit at all */
    WASM_DIAG_X_SAMEMOD,     /* goto_ptr exits whose target TB lives in the
                              * same batch module as the TB exiting (W64_COLOC
                              * with W64_LC_VERIFY: the share of indirect exits
                              * an intra-module branch could ever replace) */
    WASM_DIAG_X_DIFFMOD,     /* ...a different one */
    WASM_DIAG_X_NOMOD,       /* ...either side not yet landed in a batch */
    /*
     * Why a direct branch still ends its TB, i.e. what an absorb rule that
     * was not the one in w64_absorb would be worth.  Counted where the
     * absorb is refused, so the five are disjoint and sum with TB_ABSORB
     * to every direct branch the backend sees.
     */
    WASM_DIAG_AB_BACKIN,     /* backward, into this TB's own range: a loop
                              * whose head is not the TB's first insn */
    WASM_DIAG_AB_BACKOUT,    /* backward, before this TB */
    WASM_DIAG_AB_FAR,        /* forward, past W64_ABSORB bytes */
    WASM_DIAG_AB_PAGE,       /* forward and near, but on the next page */
    WASM_DIAG_AB_STATE,      /* refused by the IT-block / single-step guards */
    WASM_DIAG_AB_COND,       /* ...of which: a conditional branch's taken path,
                              * which w64_defer_taken handles instead */
    WASM_DIAG_AB_JMP,        /* ...the TB was already ending for another reason */
    WASM_DIAG_AB_IT,         /* ...inside an IT block or an ECI resume */
    WASM_DIAG_AB_ISET,       /* ...the target runs in the other instruction set */
    /*
     * ldm/stm move n registers as n separate guest memory ops, each with its
     * own inline TLB probe and address add.  W64_LSMCOUNT=1 counts the
     * executed ones against WASM_DIAG_LDST_EXEC (W64_LDSTCOUNT=2), which is
     * the share one address translation per instruction could serve.
     */
    WASM_DIAG_LSM_N,         /* ldm/stm instructions executed */
    WASM_DIAG_LSM_EXEC,      /* registers they moved */
    /*
     * Register-allocator spills, counted at translation time: the backend
     * has 28 allocatable registers against target/arm's 22 globals, and a
     * spill is a store to env memory the optimizing tier cannot remove.
     * Read per tbGen; raising TCG_TARGET_NB_REGS only helps if this is
     * more than a fraction of one per TB.
     */
    WASM_DIAG_TCG_SPILL,
    /*
     * W64_XWHY=1: which guest instruction asked for a goto_ptr exit.
     * Indirect exits are two thirds of all TB boundaries and a boundary
     * is ~34 ns, so the split decides which one is worth a mechanism.
     * Emitted into the generated code, so this is a measurement build
     * like W64_XCOUNT -- per-Mi rates exact, wall not comparable.
     */
    WASM_DIAG_XW_OTHER,
    WASM_DIAG_XW_PCST,       /* a store to r15: pop {pc}, ldr pc, mov pc */
    WASM_DIAG_XW_BX,         /* bx / blx register, i.e. most returns */
    WASM_DIAG_XW_PSR,        /* msr cpsr -- gen_set_psr */
    WASM_DIAG_XW_RFE,        /* rfe, and ldm with an SPSR restore */
    WASM_DIAG_XW_DEFER,      /* a deferred taken path with no slot left */
    WASM_DIAG_XW_NOCHAIN,    /* DISAS_UPDATE_NOCHAIN */
    WASM_DIAG_XW_SVC,        /* svc taken in the TB (arm_take_svc_aarch32) */
    WASM_DIAG_XW_BL,         /* a direct bl that w64_inline_call refused */
    WASM_DIAG_RAM_1P,       /* accesses do_ram_1p served inline -- what
                              * SLOW_CLEAN counted before it existed */
    /*
     * The rates the dispatcher's profile share cannot be turned into ns
     * without: how often cpu_exec_loop goes round (a chain unwind --
     * since 0091 a chained TB never comes back), how often cpu_exec is
     * entered at all, and how often the CPU takes an ARM exception.
     * The first two together say what fraction of unwinds the exception
     * accounts for: 77 %, on an S75 boot.
     */
    WASM_DIAG_EXEC_ITER,     /* cpu_exec_loop inner-loop iterations */
    WASM_DIAG_EXEC_SJMP,     /* cpu_exec_setjmp calls (not longjmps) */
    /*
     * cpu_loop_exit longjmps actually taken.  A longjmp here is the
     * emscripten JS-exception unwind (the artifact carries setThrew and
     * _emscripten_throw_longjmp), priced at ~15 us by the HELPER(wfi)
     * comment that removed the last hot one -- so this reading near
     * zero is what makes the exception path affordable, and a build
     * that puts one back on a hot path will say so here.
     */
    WASM_DIAG_EXEC_LJMP,
    WASM_DIAG_ARM_IRQ,       /* arm_cpu_do_interrupt calls */
    /*
     * ARM_IRQ by kind.  93k exceptions a second on an S75 boot is two
     * orders above any device's natural rate, so which kind it is
     * decides whether a device model is firing too often or the guest
     * is making that many syscalls.  It is the latter: 90 % are SWI.
     * Slot = exception_index for
     * EXCP_UDEF..EXCP_FIQ (1..6), 0 for anything else, so these must
     * stay in the ARM constants' order and contiguous.
     */
    WASM_DIAG_EXC_OTHER,
    WASM_DIAG_EXC_UDEF,
    WASM_DIAG_EXC_SWI,
    WASM_DIAG_EXC_PABT,
    WASM_DIAG_EXC_DABT,
    WASM_DIAG_EXC_IRQ,
    WASM_DIAG_EXC_FIQ,
    /*
     * The display stream, taken as a burst instead of a word at a time.
     * dmacRun is bursts the DMAC handed to a device's write_run, difRun
     * the subset the DIF took in its own run loop (the rest it unrolled
     * back into per-word register writes), and difTxWord is words either
     * way -- so difTxWord/dmacBurst is the burst length and
     * difRun/dmacBurst says whether the fast path is actually the path.
     */
    WASM_DIAG_DMAC_RUN,
    WASM_DIAG_DIF_RUN,
    /*
     * One step lower: the bytes of a burst handed to the SSI bus in one
     * call instead of one at a time.  ssiRun/difRun says the byte run is
     * taken on every burst the DIF runs, and ssiByte/difTxWord is bytes
     * per word -- 2 for a 16-bit panel.
     */
    WASM_DIAG_SSI_RUN,
    WASM_DIAG_SSI_BYTE,

    /*
     * The global next-TB cache in helper_lookup_tb_ptr{,_lc}.  pccHit is
     * the share of lookup helper calls answered without
     * arm_get_tb_cpu_state or a jump-cache probe; pccFill counts the
     * misses that could be cached at all -- a fill is refused for an
     * uncompiled target, a non-standard cflags or an A64 / M-profile /
     * single-stepping CPU.
     *
     * pccHit is NOT the cache's hit rate, and reading it as one has now
     * cost two sessions.  The cache is probed twice: once by code the
     * backend emits inline, and again here in the helper.  Only the
     * second bumps this counter.  A hit on the emitted probe returns
     * without ever calling the helper, so it is invisible here -- its
     * successes are lookup calls that never happen, and absent events
     * cannot be counted where they would have occurred.  pccHit/(pccHit
     * + lookup) therefore measures the *leftover* C-side cache on the
     * misses of the emitted one, and reads like a ~0.5 % failure when
     * the cache is in fact carrying most of the traffic.
     *
     * To size it, A/B it: W64_NOPCC disables both halves and lookup/Mi
     * rises 8.9x (972.7 -> 8688.4) for +3.9 % wall.  That is the number.
     * W64_NOPCCIN disables only the emitted half, which is how the two
     * are told apart.  See "A counter on the slow path is not a hit
     * rate" in doc/lessons.md.
     */
    WASM_DIAG_PCC_HIT,
    WASM_DIAG_PCC_FILL,
    WASM_DIAG_PCC_BAD,       /* W64_PCC_VERIFY: hits the full lookup
                              * disagreed with.  Must be 0. */

    /*
     * The real-time cap's idle wait: the vCPU sitting still on purpose,
     * because warping would put the virtual clock ahead of wall time.
     * It is the one way the vCPU can be quiet for seconds while nothing
     * is wrong, and the halt counter cannot see it -- the halt is
     * counted on entry to the idle advance, and the wait happens inside
     * it -- so a long one is indistinguishable from a dead guest unless
     * it is counted here.  rtcapWaitMax is the longest single wait.
     */
    WASM_DIAG_RTCAP_WAIT,
    WASM_DIAG_RTCAP_WAIT_NS,
    WASM_DIAG_RTCAP_WAIT_MAX,
    /* the running guest's own cap, capped at 20 ms a time but able to
     * stack: many in a row is a crawl, not a freeze, and tells them apart */
    WASM_DIAG_RTCAP_THROT,
    WASM_DIAG_RTCAP_THROT_NS,

    /*
     * Display writes that took the row run rather than the per-pixel
     * loop.  lcdPx over lcdRow is the run length: a blit that arrives as
     * long runs is being decoded a row at a time, and a ratio near 1
     * means the fast path is being entered for nothing.
     */
    WASM_DIAG_LCD_ROW,
    WASM_DIAG_LCD_PX,

    /* stores into a protected page that no TB covered, so the page
     * collection was never built.  Against slowNotdirty this says how
     * much of the guest's writing to its own code pages is real SMC. */
    WASM_DIAG_SMC_MISS,

    /*
     * Of those, the ones the page's code-granule mask rejected without
     * touching the TB list, and the number of TBs the list walk visited
     * when the mask did not reject.  smcWalk / (smcMiss - smcMask) is the
     * chain length a store used to pay unconditionally.
     */
    WASM_DIAG_SMC_MASK,
    WASM_DIAG_SMC_WALK,

    /*
     * Wall ns inside the DMA display stream -- the whole DMA -> DIF -> SSI
     * -> LCD chain for one burst, which is the only place those 1608
     * bursts per Mi are paid for.  DISP_CAL is an empty interval taken the
     * same number of times right beside it: two clock reads back to back
     * measure nothing, so subtracting it removes the instrument's own
     * floor instead of leaving it inside the answer.
     */
    WASM_DIAG_DISP_NS,
    WASM_DIAG_DISP_CAL,
    WASM_DIAG_DISP_BURST,

    /*
     * W64_EXCNS=1: what a guest exception costs outside the guest's own
     * instructions.  A J2ME game takes ~700 of them per Mi -- 98 % SWI --
     * against 756 dispatcher iterations, so on this workload the exception
     * path *is* the C dispatcher, and every one of them crosses the
     * setjmp/longjmp that the boot and idle benchmarks almost never take.
     *
     * Three spans, because they are three different things to fix: the
     * unwind (EXC_LJ_NS, cpu_loop_exit's siglongjmp to the sigsetjmp's
     * return), the BQL round trip around do_interrupt (EXC_BQL_NS), and
     * arm_cpu_do_interrupt itself (EXC_DO_NS).  EXC_CAL is an empty
     * interval taken once per exception: each measured span carries one
     * clock read's own floor, so the floor has to be measured beside them
     * rather than assumed.
     */
    WASM_DIAG_EXC_LJ_NS,
    WASM_DIAG_EXC_LJ_N,
    WASM_DIAG_EXC_BQL_NS,
    WASM_DIAG_EXC_DO_NS,
    WASM_DIAG_EXC_CAL,
    WASM_DIAG_EXC_N,

    /*
     * DMAC_COAL counts the transfers that carried more than one burst
     * because the destination had taken the previous burst through its
     * run-write path.  DMAC_BURST / DMAC_COAL is how many bursts a
     * coalesced transfer averaged; with the display stream it should
     * approach the W64_DMACOAL cap divided by the channel's burst size.
     */
    WASM_DIAG_DMAC_COAL,

    /*
     * DIF_RXSKIP counts the words of a DIF burst whose received value the
     * RX FIFO could not still be holding when the burst ends, so the
     * reassembly and the push were skipped.  DIF_RXSKIP / DIF_TX_WORD is
     * the share of the burst's RX bookkeeping that was unobservable.
     */
    WASM_DIAG_DIF_RXSKIP,

    /*
     * DIF_TXFAST counts the words a burst packed with the byte-swap path
     * instead of the generic mux-and-shift loop.  DIF_TXFAST / DIF_TX_WORD
     * is the share of transmitted words that took it; on the display
     * stream it should be ~1.
     */
    WASM_DIAG_DIF_TXFAST,

    /*
     * CHAIN_DRV counts entries into the W64_CHAINLOOP chain driver, i.e.
     * dispatcher iterations whose first TB ended by handing its successor
     * back instead of tail-calling it.  With the knob on it should track
     * execIter; with it off it is zero, which is what says the mechanism
     * is engaged before any clock is read.
     */
    WASM_DIAG_CHAIN_DRV,

    /*
     * Which branch scheme a TB was emitted with: TB_NESTED is one wasm
     * block per label and a plain br, TB_LOOPMODE is the $bp dispatch
     * loop whose taken branches walk a chain of `if (bp <= k)`.  Both are
     * translation-time, so they count TBs and not executions -- the cost
     * of the scheme itself is what W64_NONESTED prices.
     */
    WASM_DIAG_TB_NESTED,
    WASM_DIAG_TB_LOOPMODE,

    /*
     * W64_MERGE: MERGE_MOD counts batches assembled as one merged
     * function and MERGE_MEMB the member bodies folded into them, so
     * MERGE_MEMB / MERGE_MOD is the fan-in that tier-up amortises over
     * -- the whole point of the mechanism, and the number to check
     * before reading any clock.  MERGE_SKIP counts batches that asked
     * to merge and could not because their members' locals declarations
     * disagreed; it should be 0, and a nonzero value silently halves the
     * fan-in, so it is a counter and not an assertion.  All three are per module (~1k/s), hence uncounted by
     * WASM_DIAG_HOT.
     */
    WASM_DIAG_MERGE_MOD,
    WASM_DIAG_MERGE_MEMB,
    WASM_DIAG_MERGE_SKIP,

    /*
     * Straddle floors for the two WASM_DIAG_TIME_PHASES samplers that
     * never had one.  ioNs has had calNs beside it since it was built,
     * and for the same reason: the browser's clock is quantized, so a
     * sample of a 50 ns span is 0 or one whole tick and the mean is a
     * straddle probability, not a duration.  That estimate is unbiased
     * for span-plus-two-clock-reads, so the two reads have to be
     * measured beside it rather than assumed small -- at these spans
     * they are most of it.  Sampled 1-in-8 with their partners, so the
     * denominator is the existing LC_NS_N / HFLAGS_NS_N.
     */
    WASM_DIAG_LC_CAL,
    WASM_DIAG_HFLAGS_CAL,

    /*
     * Guest-register traffic across a TB boundary, counted at translation
     * time.  TCG_SPILL is ~0 on this backend (2 in 3920 Mi) because the 16
     * TCG registers are wasm locals and locals are unlimited, so there is
     * no allocator pressure -- but that is not the same as no traffic.
     * TCG's globals live in env memory, and liveness writes every dirty one
     * back at the end of the basic block and loads it again in the next TB.
     * That round trip is amortised over 8.4 guest instructions and is the
     * one component of the 27.9 ns boundary no mechanism A/B could move:
     * neither dispatchbench nor the CHAINLOOP driver has guest state.
     *
     * GLD/GST are per generated TB, so divide by TB_GEN, not by tbIcount.
     */
    WASM_DIAG_TCG_GLD,       /* a global loaded from env (temp_load) */
    WASM_DIAG_TCG_GST,       /* a global written back to env (temp_sync) */

    /*
     * Why each of those write-backs exists.  The comment above calls the
     * round trip a boundary cost, which was measured on a board averaging
     * 8.4 guest instructions per TB; it is an assumption, not a finding,
     * and these five counters test it.  Their sum should track TCG_GST:
     * a residue means stores are being emitted for a reason other than
     * liveness demanding one (allocator pressure is the candidate), and
     * that residue is itself worth reading.
     *
     * Liveness runs backwards, so the site that first flips a global to
     * TS_MEM is the nearest *following* one that needs it in memory, and
     * the store its defining op later emits belongs to that site.  Blaming
     * per global at the flip and charging where SYNC_ARG is set is
     * therefore exact, not a share-out.
     *
     * The split is the difference between a cost that could be removed and
     * one that could not.  SE is an op that can fault -- a guest memory
     * access -- and CALL a helper that reads env: both must leave env
     * coherent, and no code shape changes that.  CBR (a brcond) and BBEND
     * (a label or br) are shape: on this guest they are mostly ARM
     * predication and folded branches, and predication has a branchless
     * form.
     *
     * Do not read "predication" as CBR alone.  A predicated A32 instruction
     * emits brcond-over-itself *and* a label after itself
     * (arm_post_translate_insn), and the two charge different halves: the
     * brcond claims globals dirtied before it (CBR), while the label claims
     * the instruction's own outputs (BBEND), because temp_sync clears TS_MEM
     * at every write (tcg.c, "Output args are dead") and so restarts the
     * blame span.  Sizing the movcond lever off CBR alone therefore counts
     * only the half that if-conversion does *not* remove.
     */
    WASM_DIAG_GSYNC_CBR,     /* a conditional branch (la_bb_sync) */
    WASM_DIAG_GSYNC_BBEND,   /* a label or br (la_bb_end); goto_tb is BB_EXIT,
                                checked first, so it charges GSYNC_EXIT */
    WASM_DIAG_GSYNC_SE,      /* an op that can fault (qemu_ld/st) */
    WASM_DIAG_GSYNC_CALL,    /* a helper that reads or writes env */
    WASM_DIAG_GSYNC_EXIT,    /* the end of the TB (la_func_end) */

    /*
     * A32 predication: every instruction with a condition other than AL
     * emits a brcond over itself, and TCG treats both that and the label
     * closing it as basic-block boundaries -- so it charges GSYNC_CBR and
     * GSYNC_BBEND, per the note above, not GSYNC_CBR alone.
     *
     * PRED_SEL is the share of them a movcond could replace instead --
     * data-processing, immediate or immediate-shifted register, S clear
     * and Rd not PC, so the value can be computed unconditionally and
     * selected into the destination with nothing to fault and no flags to
     * restore.  Everything else (a predicated load, a predicated branch, a
     * write to PC) has to keep its branch.  PRED_SEL/PRED_A32 is therefore
     * the ceiling on what a branchless form could remove, and
     * (GSYNC_CBR + the predication share of GSYNC_BBEND) * that share is
     * the ceiling in stores.  GSYNC_BBCOND measures that share: it is the
     * part of GSYNC_BBEND charged at a label arm_gen_condlabel made, so
     * BBEND - BBCOND is every other label and br.
     */
    WASM_DIAG_PRED_A32,      /* an A32 instruction with cond != AL */
    WASM_DIAG_PRED_SEL,      /* ... of which this many could be a movcond */

    /*
     * Appended instead of filed with the other GSYNC_* entries because
     * site/app.js hardcodes WASM_DIAG_HALT's index, and a mid-enum insert
     * moves every counter after it.
     */
    WASM_DIAG_GSYNC_BBCOND,  /* of GSYNC_BBEND: at an A32 predication skip */
    WASM_DIAG_IREC_FREED,    /* bytes of interpreter records released */

    /* PRED_A32 by encoding class; exclusive, and they sum to PRED_A32 */
    WASM_DIAG_PRED_DP_NOS,   /* data processing, S clear (PRED_SEL plus the
                                shapes its narrower filter excludes) */
    WASM_DIAG_PRED_DP_S,     /* data processing, S set: selectable only if
                                the four flag globals are selected too */
    WASM_DIAG_PRED_LDST,     /* single load/store: cannot be selected */
    WASM_DIAG_PRED_LSM,      /* load/store multiple */
    WASM_DIAG_PRED_BR,       /* B/BL */
    WASM_DIAG_PRED_OTHER,    /* reg-shifted DP, multiplies, extra ldst, cp */

    /*
     * Of LDST_GEN: memops that follow another memop with no call, label or
     * branch in between.  A TLB mask/table cache held in TB locals has to
     * be dropped at every one of those, so LDST_RUN/LDST_GEN is the share
     * of memops such a cache could actually serve -- the multiplier on the
     * +2.6 % hoist ceiling round thirty-eight measured per memop.
     */
    WASM_DIAG_LDST_RUN,

    /*
     * cpu_handle_interrupt() iterations whose whole pending set was
     * CPU_INTERRUPT_EXITTB, taken without the BQL.  Every guest exception
     * leaves one behind (arm_cpu_do_interrupt sets it), so on a workload
     * that syscalls as hard as the SL65's video player this is the rate
     * of a BQL round trip the loop used to make for no other reason.
     */
    WASM_DIAG_EXITTB_FAST,

    /*
     * Call inlining (target/arm w64_inline_call): a direct bl whose
     * callee is translated in place, the bx lr that ends it compared
     * against the return address.  INL_CALL / INL_RET are translation
     * counts (calls inlined, return checks emitted); INL_REFUSE counts
     * calls a page rule turned down; INL_UNLINK the chains into inlined
     * TBs dropped by a tb_key_gen bump (cpu_tb_key_gen_bump).
     */
    WASM_DIAG_INL_CALL,
    WASM_DIAG_INL_RET,
    WASM_DIAG_INL_REFUSE,
    WASM_DIAG_INL_UNLINK,
    WASM_DIAG_INL_WALK,      /* tb_unlink_inlined walks (one per TLB flush) */
    /*
     * Why a TB ended while still inside an inlined callee (translation
     * counts, arm_tr_tb_stop): the callee raised an svc, left through an
     * indirect or far branch, ended at a conditional instruction, ran out
     * of instructions or page, or something else.  And why a direct bl
     * was not inlined: it was conditional, the depth limit, or a page
     * refusal (INL_REFUSE above).
     */
    WASM_DIAG_INL_END_SVC,
    WASM_DIAG_INL_END_JUMP,
    WASM_DIAG_INL_END_COND,
    WASM_DIAG_INL_END_MANY,
    WASM_DIAG_INL_END_OTHER,
    WASM_DIAG_INL_NO_COND,
    WASM_DIAG_INL_NO_DEPTH,
    /* w64_absorb refusals while inside an inlined callee, by reason */
    WASM_DIAG_INL_AB_COND,
    WASM_DIAG_INL_AB_BACK,
    WASM_DIAG_INL_AB_FAR,
    WASM_DIAG_INL_AB_PAGE,
    WASM_DIAG_INL_AB_OTHER,
    WASM_DIAG_INL_END_PSR,   /* ...ended at a CPSR write (gen_set_psr) */
    WASM_DIAG_INL_SMC_RESUME, /* a store patched another stream of its own
                               * inlined TB: resumed after it on fresh code */
    /*
     * gen_set_psr continuing the TB past an `msr cpsr` / `msr spsr`
     * instead of ending it (target/arm/tcg/translate.c).  PSR_CONT is the
     * guarded CPSR case, PSR_SPSR the SPSR one, which needs no guard;
     * PSR_NO_* are the refusals.  Translation-time counts: the runtime
     * effect is xwPsr under W64_XWHY.
     */
    WASM_DIAG_PSR_CONT,
    WASM_DIAG_PSR_SPSR,
    WASM_DIAG_PSR_NO_COND,
    WASM_DIAG_PSR_NO_ROOM,
    /*
     * xwBl split by why w64_inline_call refused, counted in the generated
     * code like the rest of the W64_XWHY family: which refusal is worth a
     * mechanism depends on its *runtime* weight, and the inl* counters
     * above are translation-time.
     */
    WASM_DIAG_XW_BL_PAGE,    /* callee on a third page, or a return off it */
    WASM_DIAG_XW_BL_DEPTH,   /* out of nesting levels, misses or records */
    WASM_DIAG_XW_BL_COND,    /* a conditional call, or one inside an IT */
    WASM_DIAG_XW_BL_RET,     /* everything else (cflags, eci, M-profile) */
    WASM_DIAG_AB_CROSS,      /* w64_absorb took a branch to the TB's other
                              * tracked page instead of ending the TB */
    /*
     * xwBlPage, and the page share of xwOther, split by which rule in
     * w64_inl_pick_page refused the stream its page.  A call and an
     * absorbed branch share these because they would share the fix.
     * Only xwPgThird is what a third tracked page collects; the other
     * three are not more slots away, so this split is what says whether
     * that mechanism is worth building.
     */
    WASM_DIAG_XW_PG_THIRD,   /* page 1 is taken by a different page */
    WASM_DIAG_XW_PG_LIN,     /* page 1 belongs to a linear crossing */
    WASM_DIAG_XW_PG_PROBE,   /* the target page is not mapped for fetch */
    WASM_DIAG_XW_PG_RET,     /* a bl returning off the stream's page */
    WASM_DIAG_XW_PG_MORE,    /* ...and would still be refused by a third:
                              * the TB had already asked for four pages */

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
