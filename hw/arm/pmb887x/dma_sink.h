#pragma once

#include "qemu/osdep.h"
#include "qom/object.h"
#include "system/memory.h"

#define TYPE_PMB887X_DMA_SINK "pmb887x-dma-sink"

typedef struct pmb887x_dma_sink_t pmb887x_dma_sink_t;
typedef struct pmb887x_dma_sink_class_t pmb887x_dma_sink_class_t;

#define PMB887X_DMA_SINK(obj) INTERFACE_CHECK(pmb887x_dma_sink_t, (obj), TYPE_PMB887X_DMA_SINK)
DECLARE_CLASS_CHECKERS(pmb887x_dma_sink_class_t, PMB887X_DMA_SINK, TYPE_PMB887X_DMA_SINK)

struct pmb887x_dma_sink_class_t {
	InterfaceClass parent_class;
	/*
	 * Accepts count transfers of width bytes only when the result equals count sequential bus writes
	 * at the current time; returns false without side effects otherwise.
	 */
	bool (*write_block)(
		pmb887x_dma_sink_t *sink,
		MemoryRegion *mr,
		hwaddr addr,
		const uint8_t *data,
		uint32_t width,
		uint32_t count,
		bool is_increment
	);
};

bool pmb887x_dma_sink_write_block(
	AddressSpace *as,
	hwaddr addr,
	const uint8_t *data,
	uint32_t width,
	uint32_t count,
	bool is_increment
);
