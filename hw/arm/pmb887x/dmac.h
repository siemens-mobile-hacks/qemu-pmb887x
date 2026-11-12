#pragma once
#include "qemu/osdep.h"

#define TYPE_PMB887X_DMAC	"pmb887x-dmac"
OBJECT_DECLARE_SIMPLE_TYPE(pmb887x_dmac_t, PMB887X_DMAC);

void pmb887x_dmac_set_sel(pmb887x_dmac_t *p, uint32_t value);
uint32_t pmb887x_dmac_get_sel(pmb887x_dmac_t *p);
bool pmb887x_dmac_serve_burst(qemu_irq line);

typedef struct {
	MemoryRegion *sink;
	hwaddr sink_addr;
	uint32_t burst_words;
	bool is_increment;
} pmb887x_dmac_bulk_t;

uint32_t pmb887x_dmac_count_bulk_bursts(qemu_irq line, pmb887x_dmac_bulk_t *bulk);
/*
 * The RAM the next @count bulk bursts read from, as a ram_addr_t range, and
 * in *@available the bytes from @start to the end of that RAM.
 */
bool pmb887x_dmac_get_bulk_source(qemu_irq line, uint32_t count, ram_addr_t *start, ram_addr_t *length,
	ram_addr_t *available);
void pmb887x_dmac_serve_bulk_bursts(qemu_irq line, uint8_t *data, uint32_t count);

/*
 * @fn runs before anything else can read or change the DMAC: a register
 * access, a request line, a selection or clock change.  For a peripheral
 * that serves the DMAC on @line behind the clock.
 */
void pmb887x_dmac_set_catch_up(qemu_irq line, void (*fn)(void *opaque), void *opaque);
