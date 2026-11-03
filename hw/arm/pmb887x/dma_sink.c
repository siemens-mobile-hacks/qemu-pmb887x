#include "qemu/osdep.h"
#include "qemu/rcu.h"
#include "hw/arm/pmb887x/dma_sink.h"

static void dma_sink_cache_commit(MemoryListener *listener) {
	pmb887x_dma_sink_cache_t *cache = container_of(listener, pmb887x_dma_sink_cache_t, listener);

	cache->mr = NULL;
}

void pmb887x_dma_sink_cache_init(pmb887x_dma_sink_cache_t *cache, AddressSpace *as) {
	*cache = (pmb887x_dma_sink_cache_t) {
		.as = as,
		.listener = {
			.name = "pmb887x-dma-sink",
			.commit = dma_sink_cache_commit,
		},
	};
	memory_listener_register(&cache->listener, as);
}

static pmb887x_dma_sink_t *dma_sink_cache_get_sink(pmb887x_dma_sink_cache_t *cache, MemoryRegion *mr) {
	if (cache->mr == mr)
		return cache->sink;

	cache->mr = mr;
	cache->sink = (pmb887x_dma_sink_t *) object_dynamic_cast(memory_region_owner(mr), TYPE_PMB887X_DMA_SINK);
	cache->klass = cache->sink ? PMB887X_DMA_SINK_GET_CLASS(cache->sink) : NULL;
	return cache->sink;
}

bool pmb887x_dma_sink_write_block(
	pmb887x_dma_sink_cache_t *cache,
	hwaddr addr,
	const uint8_t *data,
	uint32_t width,
	uint32_t count,
	bool is_increment
) {
	RCU_READ_LOCK_GUARD();
	hwaddr size = is_increment ? (hwaddr) width * count : width;
	hwaddr mapped_size = size;
	hwaddr offset;
	MemoryRegion *mr = address_space_translate(cache->as, addr, &offset, &mapped_size, true, MEMTXATTRS_UNSPECIFIED);
	if (mapped_size < size)
		return false;

	pmb887x_dma_sink_t *sink = dma_sink_cache_get_sink(cache, mr);
	if (!sink)
		return false;

	return cache->klass->write_block(sink, mr, offset, data, width, count, is_increment);
}

static const TypeInfo dma_sink_info = {
	.name = TYPE_PMB887X_DMA_SINK,
	.parent = TYPE_INTERFACE,
	.class_size = sizeof(pmb887x_dma_sink_class_t),
};

static void dma_sink_register_types(void) {
	type_register_static(&dma_sink_info);
}
type_init(dma_sink_register_types)
