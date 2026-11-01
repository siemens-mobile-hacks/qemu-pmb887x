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
void pmb887x_dmac_serve_bulk_bursts(qemu_irq line, uint8_t *data, uint32_t count);
