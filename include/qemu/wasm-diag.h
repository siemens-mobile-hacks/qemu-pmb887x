/*
 * wasm diagnostics counters: cold-path memory-subsystem counters filled
 * by tcg/tci.c + accel/tcg/cputlb.c (interpreter slow-path entries, MMIO
 * dispatches, TLB fills - ~40k increments/s on a booting pmb887x, cost is
 * noise) and read from JS via the wasm_memstat() export in ui/wasm.c.
 * See tools/memstat.mjs.  Hot paths (the inline fast paths) deliberately
 * carry no counters.
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
    WASM_DIAG_TXN_NOEXIT,    /* ...converted to no-unwind exits */
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
    WASM_DIAG_N
};

extern uint64_t wasm_diag_stat[WASM_DIAG_N];

#endif
