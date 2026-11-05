/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_DMA_DMA_SINK_H
#define HW_DMA_DMA_SINK_H

#include "qom/object.h"
#include "system/memory.h"

/*
 * A device that takes a run of DMA writes in one call, so the DMA controller
 * does not dispatch every transfer through the memory API.
 */
#define TYPE_DMA_SINK "dma-sink"
typedef struct DMASink DMASink;
typedef struct DMASinkClass DMASinkClass;

DECLARE_CLASS_CHECKERS(DMASinkClass, DMA_SINK, TYPE_DMA_SINK)
#define DMA_SINK(obj) INTERFACE_CHECK(DMASink, (obj), TYPE_DMA_SINK)

struct DMASinkClass {
    InterfaceClass parent_class;
    /*
     * Accepts @count transfers of @width bytes only when the result equals
     * @count sequential bus writes at the current time; returns false without
     * side effects otherwise.
     */
    bool (*write_block)(DMASink *sink, MemoryRegion *mr, hwaddr addr,
                        const uint8_t *data, uint32_t width, uint32_t count,
                        bool is_increment);
};

typedef struct DMASinkCache {
    AddressSpace *as;
    MemoryListener listener;
    MemoryRegion *mr;
    DMASink *sink;
    DMASinkClass *klass;
} DMASinkCache;

void dma_sink_cache_init(DMASinkCache *cache, AddressSpace *as);

/* Returns false when nothing was written and the caller must use the bus. */
bool dma_sink_write_block(DMASinkCache *cache, hwaddr addr,
                          const uint8_t *data, uint32_t width, uint32_t count,
                          bool is_increment);

#endif
