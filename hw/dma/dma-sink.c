/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "hw/dma/dma-sink.h"

static void dma_sink_cache_commit(MemoryListener *listener)
{
    DMASinkCache *cache = container_of(listener, DMASinkCache, listener);

    cache->mr = NULL;
}

void dma_sink_cache_init(DMASinkCache *cache, AddressSpace *as)
{
    *cache = (DMASinkCache) {
        .as = as,
        .listener = {
            .name = "dma-sink",
            .commit = dma_sink_cache_commit,
        },
    };
    memory_listener_register(&cache->listener, as);
}

static DMASink *dma_sink_cache_get_sink(DMASinkCache *cache, MemoryRegion *mr)
{
    if (cache->mr == mr) {
        return cache->sink;
    }

    cache->mr = mr;
    cache->sink = (DMASink *) object_dynamic_cast(memory_region_owner(mr),
                                                  TYPE_DMA_SINK);
    cache->klass = cache->sink ? DMA_SINK_GET_CLASS(cache->sink) : NULL;
    return cache->sink;
}

bool dma_sink_write_block(DMASinkCache *cache, hwaddr addr,
                          const uint8_t *data, uint32_t width, uint32_t count,
                          bool is_increment)
{
    RCU_READ_LOCK_GUARD();
    hwaddr size = is_increment ? (hwaddr) width * count : width;
    hwaddr mapped_size = size;
    hwaddr offset;
    MemoryRegion *mr = address_space_translate(cache->as, addr, &offset,
                                               &mapped_size, true,
                                               MEMTXATTRS_UNSPECIFIED);
    if (mapped_size < size) {
        return false;
    }

    DMASink *sink = dma_sink_cache_get_sink(cache, mr);
    if (!sink) {
        return false;
    }

    return cache->klass->write_block(sink, mr, offset, data, width, count,
                                     is_increment);
}

static const TypeInfo dma_sink_info = {
    .name = TYPE_DMA_SINK,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(DMASinkClass),
};

static void dma_sink_register_types(void)
{
    type_register_static(&dma_sink_info);
}

type_init(dma_sink_register_types)
