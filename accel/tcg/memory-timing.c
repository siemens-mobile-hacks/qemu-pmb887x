/*
 * Memory timing classes for CF_COUNT_CYCLES translation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "exec/translation-block.h"
#include "accel/tcg/memory-timing.h"

static const TCGMemoryTiming *memory_timings;
static unsigned memory_timing_count;
static uint32_t memory_timing_cflags;
uint32_t tcg_memory_fetch_lines;
uint32_t tcg_memory_fetch_recent_lines;
uint32_t tcg_memory_fetch_cache_lines;
static bool memory_fetch_off;

/*
 * PMB887X-WEB FEATURE (deterministic clock): lookups charge the I-cache,
 * and a TB is looked up after every exit - a kick, a timer - and wherever
 * the backend did not chain to it, so no lookup costs a cycle: a fetch
 * distance never exceeds UINT32_MAX.
 */
void tcg_memory_timing_fetch_off(void)
{
    memory_fetch_off = true;
    tcg_memory_fetch_recent_lines = UINT32_MAX;
}

void tcg_memory_timing_init(const TCGMemoryTiming *timings, unsigned count)
{
    assert(count <= TCG_MEMORY_TIMING_MAX);
    for (unsigned i = 0; i < count; i++) {
        assert(is_power_of_2(timings[i].line_bytes));
        assert(is_power_of_2(timings[i].fetch_cache_bytes));
        assert(timings[i].fetch_cache_bytes == timings[0].fetch_cache_bytes);
        assert(timings[i].fetch_recent_lines == timings[0].fetch_recent_lines);
    }
    if (count) {
        tcg_memory_fetch_cache_lines = timings[0].fetch_cache_bytes /
                                       timings[0].line_bytes;
        tcg_memory_fetch_recent_lines = memory_fetch_off ? UINT32_MAX :
                                        timings[0].fetch_recent_lines;
    }
    memory_timings = timings;
    memory_timing_count = count;
}

/*
 * The class is part of the TB cflags, so a TB keeps the costs it was
 * translated with and a class change selects other TBs instead of a flush.
 */
void tcg_memory_timing_select(CPUState *cpu, unsigned index)
{
    uint32_t class = index < memory_timing_count ? index + 1 : 0;

    memory_timing_cflags = class << CF_MEMORY_TIMING_SHIFT;
    if (tcg_cflags_has(cpu, CF_COUNT_CYCLES) &&
        (cpu->tcg_cflags & CF_MEMORY_TIMING) != memory_timing_cflags) {
        cpu->tcg_cflags = (cpu->tcg_cflags & ~CF_MEMORY_TIMING) |
                          memory_timing_cflags;
        cpu_tb_key_gen_bump(cpu);   /* WASM PORT: next-TB caches */
    }
}

uint32_t tcg_memory_timing_cflags(void)
{
    return memory_timing_cflags;
}

const TCGMemoryTiming *tcg_memory_timing_get(uint32_t cflags)
{
    uint32_t class = (cflags & CF_MEMORY_TIMING) >> CF_MEMORY_TIMING_SHIFT;

    return class ? &memory_timings[class - 1] : NULL;
}
