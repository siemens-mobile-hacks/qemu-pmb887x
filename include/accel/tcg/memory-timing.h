/*
 * Memory timing classes for CF_COUNT_CYCLES translation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ACCEL_TCG_MEMORY_TIMING_H
#define ACCEL_TCG_MEMORY_TIMING_H

#include "hw/core/cpu.h"
#include "exec/translation-block.h"

#define TCG_MEMORY_FETCH_KINDS 4

/*
 * Costs of streaming memory traffic past the caches of a CPU, in 1/1000
 * CPU cycles. A line is the CPU cache line of line_bytes. Instruction
 * fetches are indexed by the fetch kind of the code's memory region: a miss
 * stalls until its first word, straight-line code fills whole lines, and
 * literal loads of cold code stall until the first word of their data line.
 */
typedef struct TCGMemoryTiming {
    uint32_t read_first_mcycles;
    uint32_t read_word_mcycles;
    uint32_t write_mcycles;
    uint32_t write_line_mcycles;
    uint32_t turnaround_mcycles;
    uint32_t fetch_first_mcycles[TCG_MEMORY_FETCH_KINDS];
    uint32_t data_first_mcycles[TCG_MEMORY_FETCH_KINDS];
    uint32_t fetch_line_mcycles[TCG_MEMORY_FETCH_KINDS];
    uint32_t fetch_cache_bytes;
    uint32_t fetch_recent_lines;
    uint32_t line_bytes;
} TCGMemoryTiming;

#define TCG_MEMORY_TIMING_MAX 7

/*
 * @timings stays owned by the caller and lives as long as the machine.
 * Selecting index @count or above selects no memory timing. The timing state
 * is global: it models the caches of a machine with one vCPU.
 */
void tcg_memory_timing_init(const TCGMemoryTiming *timings, unsigned count);
void tcg_memory_timing_select(CPUState *cpu, unsigned index);
uint32_t tcg_memory_timing_cflags(void);
const TCGMemoryTiming *tcg_memory_timing_get(uint32_t cflags);

/*
 * The code lines that lookups have passed through the I-cache: a TB counts
 * its lines when other code of more than fetch_recent_lines ran since its
 * last lookup, so tight loops do not count. A TB that finds more than the
 * fetch_cache_bytes of lines counted since its last lookup was evicted and
 * takes its fetch_cycles again.
 */
extern uint32_t tcg_memory_fetch_lines;
extern uint32_t tcg_memory_fetch_recent_lines;
extern uint32_t tcg_memory_fetch_cache_lines;

static inline void tcg_memory_timing_fetch(CPUState *cpu,
                                           TranslationBlock *tb)
{
    uint32_t distance = tcg_memory_fetch_lines - tb->fetch_mark;

    if (unlikely(distance > tcg_memory_fetch_recent_lines)) {
        if (distance > tcg_memory_fetch_cache_lines) {
            cpu->neg.cycle_count += tb->fetch_cycles;
        }
        tcg_memory_fetch_lines += tb->fetch_line_count;
    }
    tb->fetch_mark = tcg_memory_fetch_lines;
}

#endif
