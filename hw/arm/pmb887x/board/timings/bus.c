#include "hw/arm/pmb887x/board/timings/bus.h"

#include "hw/core/clock.h"
#include "hw/core/hw-error.h"
#include "hw/core/qdev-clock.h"

#define BUS_REGIONS_MAX 64

/*
 * Peripheral reads beyond the LDR, measured by bsp/unit cpu-timing: a bridge to an FPI or AHB_PER bus takes CPU
 * cycles plus cycles of its bus clock, an AHB slave a fixed number of CPU cycles. Cycles are x1000.
 */
typedef struct {
	const char *name;
	const char *clock;
	uint32_t cpu_mcycles;
	uint32_t bus_mcycles;
} pmb887x_bus_t;

typedef struct {
	const pmb887x_bus_t *bus;
	MemoryRegion *mr;
} pmb887x_bus_region_t;

static const pmb887x_bus_t BUSES[] = {
	{ "FPI1", "FPI1", 5410, 4620 },
	{ "FPI2", "FPI2", 5410, 4620 },
	{ "FPI3", "FSYS", 5410, 4620 },
	{ "AHB_PER", "AHB_PER", 8670, 7330 },
	{ "ML-AHB", NULL, 6000, 0 },
	{ "AHB", NULL, 5000, 0 },
};

static pmb887x_bus_region_t bus_regions[BUS_REGIONS_MAX];
static size_t bus_regions_count;
static Clock *cpu_clock;
static Clock *bus_clocks[ARRAY_SIZE(BUSES)];

static uint32_t pmb887x_bus_read_cycles(const pmb887x_bus_t *bus) {
	size_t index = bus - BUSES;
	uint64_t bus_mcycles = 0;

	if (bus->clock) {
		uint64_t bus_hz = clock_get_hz(bus_clocks[index]);
		if (bus_hz == 0)
			return 0;
		bus_mcycles = (uint64_t) bus->bus_mcycles * clock_get_hz(cpu_clock) / bus_hz;
	}
	return (bus->cpu_mcycles + bus_mcycles + 500) / 1000;
}

static void pmb887x_bus_update(void *_opaque, ClockEvent _event) {
	for (size_t i = 0; i < bus_regions_count; i++)
		memory_region_set_read_cycles(bus_regions[i].mr, pmb887x_bus_read_cycles(bus_regions[i].bus));
}

void pmb887x_bus_add_region(const char *name, MemoryRegion *mr) {
	const pmb887x_bus_t *bus = NULL;

	for (size_t i = 0; i < ARRAY_SIZE(BUSES); i++) {
		if (strcmp(BUSES[i].name, name) == 0) {
			bus = &BUSES[i];
			break;
		}
	}
	if (!bus)
		hw_error("Unknown bus: %s\n", name);
	g_assert(bus_regions_count < BUS_REGIONS_MAX);

	bus_regions[bus_regions_count++] = (pmb887x_bus_region_t) { bus, mr };
	if (cpu_clock)
		memory_region_set_read_cycles(mr, pmb887x_bus_read_cycles(bus));
}

void pmb887x_bus_init(DeviceState *cgu) {
	cpu_clock = clock_new(OBJECT(cgu), "bus-cpu-clock");
	clock_set_callback(cpu_clock, pmb887x_bus_update, NULL, ClockUpdate);
	clock_set_source(cpu_clock, qdev_get_clock_out(cgu, "CPU"));

	for (size_t i = 0; i < ARRAY_SIZE(BUSES); i++) {
		if (!BUSES[i].clock)
			continue;
		g_autofree char *name = g_strdup_printf("bus-%s-clock", BUSES[i].name);
		bus_clocks[i] = clock_new(OBJECT(cgu), name);
		clock_set_callback(bus_clocks[i], pmb887x_bus_update, NULL, ClockUpdate);
		clock_set_source(bus_clocks[i], qdev_get_clock_out(cgu, BUSES[i].clock));
	}
	pmb887x_bus_update(NULL, ClockUpdate);
}
