#include "hw/arm/pmb887x/board/timings/memory.h"
#include "hw/arm/pmb887x/board/memory.h"

#include "accel/tcg/memory-timing.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"

#define ICACHE_BYTES (16 * 1024)
#define LINE_WORDS 8
#define SRAM_LINE_MCYCLES 8000
#define FETCH_RECENT_LINES 8

/*
 * A cost of the path through EBU in 1/1000 CPU cycles: CPU cycles, EBU cycles, and CPU cycles of the synchronizer
 * that SCU_EBUCLC2.FLAG1 enables. Fitted by bsp/unit cpu-timing over CPU, AHB and EBU clock combinations.
 */
typedef struct {
	uint32_t cpu;
	uint32_t ebu;
	uint32_t sync;
} pmb887x_ebu_cost_t;

typedef struct {
	CPUState *cpu;
	Clock *cpu_clock;
	Clock *ebu_clock;
	bool ebu_sync;
	TCGMemoryTiming timings[TCG_MEMORY_TIMING_MAX];
	unsigned timings_count;
} pmb887x_memory_timing_t;

static const pmb887x_ebu_cost_t SDRAM_READ_FIRST = { 6960, 17200, 2240 };
static const pmb887x_ebu_cost_t SDRAM_LINE = { 5040, 24610, 1690 };
static const pmb887x_ebu_cost_t SDRAM_WRITE = { 1750, 8600, 1650 };
static const pmb887x_ebu_cost_t SDRAM_WRITE_LINE = { 7200, 10750, 3550 };
static const pmb887x_ebu_cost_t SDRAM_COPY = { 20100, 58880, 2440 };
static const pmb887x_ebu_cost_t FLASH_CODE_FIRST = { 10080, 27700, 2040 };
static const pmb887x_ebu_cost_t FLASH_READ_FIRST = { 6160, 26880, 2240 };
static const pmb887x_ebu_cost_t FLASH_LINE = { 4290, 33250, 2200 };

static uint32_t pmb887x_ebu_cost(const pmb887x_ebu_cost_t *cost, uint64_t cpu_hz, uint64_t ebu_hz, bool sync) {
	return cost->cpu + cost->ebu * cpu_hz / ebu_hz + (sync ? cost->sync : 0);
}

/* After the first word of a line, the rest arrives a word per EBU cycle while the CPU takes a word per cycle. */
static uint32_t pmb887x_line_rest(uint64_t cpu_hz, uint64_t ebu_hz) {
	uint64_t rest = (LINE_WORDS - 1) * 1000 * cpu_hz / ebu_hz;
	return rest > (LINE_WORDS - 1) * 1000 ? rest - (LINE_WORDS - 1) * 1000 : 0;
}

static TCGMemoryTiming pmb887x_memory_timing_from_clocks(uint64_t cpu_hz, uint64_t ebu_hz, bool sync) {
	uint32_t rest = pmb887x_line_rest(cpu_hz, ebu_hz);
	uint32_t read_first = pmb887x_ebu_cost(&SDRAM_READ_FIRST, cpu_hz, ebu_hz, sync);
	uint32_t line = pmb887x_ebu_cost(&SDRAM_LINE, cpu_hz, ebu_hz, sync) + rest;
	uint32_t write_line = pmb887x_ebu_cost(&SDRAM_WRITE_LINE, cpu_hz, ebu_hz, sync);
	uint32_t copy = pmb887x_ebu_cost(&SDRAM_COPY, cpu_hz, ebu_hz, sync);

	return (TCGMemoryTiming) {
		.read_first_mcycles = read_first,
		.read_word_mcycles = line > read_first ? (line - read_first) / (LINE_WORDS - 1) : 0,
		.write_mcycles = pmb887x_ebu_cost(&SDRAM_WRITE, cpu_hz, ebu_hz, sync),
		.write_line_mcycles = write_line,
		.turnaround_mcycles = copy > line + write_line ? copy - line - write_line : 0,
		.fetch_first_mcycles = {
			[PMB887X_FETCH_SDRAM] = read_first,
			[PMB887X_FETCH_FLASH] = pmb887x_ebu_cost(&FLASH_CODE_FIRST, cpu_hz, ebu_hz, sync),
			[PMB887X_FETCH_SRAM] = SRAM_LINE_MCYCLES,
		},
		.data_first_mcycles = {
			[PMB887X_FETCH_SDRAM] = read_first,
			[PMB887X_FETCH_FLASH] = pmb887x_ebu_cost(&FLASH_READ_FIRST, cpu_hz, ebu_hz, sync),
			[PMB887X_FETCH_SRAM] = SRAM_LINE_MCYCLES,
		},
		.fetch_line_mcycles = {
			[PMB887X_FETCH_SDRAM] = line,
			[PMB887X_FETCH_FLASH] = pmb887x_ebu_cost(&FLASH_LINE, cpu_hz, ebu_hz, sync) + rest,
			[PMB887X_FETCH_SRAM] = SRAM_LINE_MCYCLES,
		},
		.fetch_cache_bytes = ICACHE_BYTES,
		.fetch_recent_lines = FETCH_RECENT_LINES,
		.line_bytes = LINE_WORDS * 4,
	};
}

/* Every clock combination seen gets a timing class of its own, so TBs keep the costs they were translated with. */
static unsigned pmb887x_memory_timing_index(pmb887x_memory_timing_t *p, const TCGMemoryTiming *timing) {
	for (unsigned i = 0; i < p->timings_count; i++) {
		if (memcmp(&p->timings[i], timing, sizeof(*timing)) == 0)
			return i;
	}
	if (p->timings_count == TCG_MEMORY_TIMING_MAX)
		return TCG_MEMORY_TIMING_MAX;

	p->timings[p->timings_count] = *timing;
	tcg_memory_timing_init(p->timings, ++p->timings_count);
	return p->timings_count - 1;
}

static void pmb887x_memory_timing_update(pmb887x_memory_timing_t *p) {
	uint64_t cpu_hz = clock_get_hz(p->cpu_clock);
	uint64_t ebu_hz = clock_get_hz(p->ebu_clock);

	if (cpu_hz == 0 || ebu_hz == 0) {
		tcg_memory_timing_select(p->cpu, TCG_MEMORY_TIMING_MAX);
		return;
	}

	TCGMemoryTiming timing = pmb887x_memory_timing_from_clocks(cpu_hz, ebu_hz, p->ebu_sync);
	tcg_memory_timing_select(p->cpu, pmb887x_memory_timing_index(p, &timing));
}

static void pmb887x_memory_timing_clock_update(void *opaque, ClockEvent _event) {
	pmb887x_memory_timing_update(opaque);
}

static void pmb887x_memory_timing_sync_input(void *opaque, int _n, int level) {
	pmb887x_memory_timing_t *p = opaque;

	p->ebu_sync = level != 0;
	pmb887x_memory_timing_update(p);
}

void pmb887x_memory_timing_init(CPUState *cpu, DeviceState *cgu, DeviceState *scu) {
	pmb887x_memory_timing_t *p = g_new0(pmb887x_memory_timing_t, 1);

	p->cpu = cpu;
	p->cpu_clock = clock_new(OBJECT(cgu), "memory-timing-cpu-clock");
	p->ebu_clock = clock_new(OBJECT(cgu), "memory-timing-ebu-clock");
	clock_set_callback(p->cpu_clock, pmb887x_memory_timing_clock_update, p, ClockUpdate);
	clock_set_callback(p->ebu_clock, pmb887x_memory_timing_clock_update, p, ClockUpdate);
	clock_set_source(p->cpu_clock, qdev_get_clock_out(cgu, "CPU"));
	clock_set_source(p->ebu_clock, qdev_get_clock_out(cgu, "EBU"));
	qdev_connect_gpio_out_named(scu, "EBU_SYNC_OUT", 0, qemu_allocate_irq(pmb887x_memory_timing_sync_input, p, 0));
	pmb887x_memory_timing_update(p);
}
