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
    WASM_DIAG_N
};

extern uint64_t wasm_diag_stat[WASM_DIAG_N];

#endif
