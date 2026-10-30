#include "qemu/osdep.h"
#include "qemu/rcu.h"
#include "hw/arm/pmb887x/dma_sink.h"

bool pmb887x_dma_sink_write_block(
	AddressSpace *as,
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
	MemoryRegion *mr = address_space_translate(as, addr, &offset, &mapped_size, true, MEMTXATTRS_UNSPECIFIED);
	if (mapped_size < size)
		return false;

	Object *owner = memory_region_owner(mr);
	pmb887x_dma_sink_t *sink = (pmb887x_dma_sink_t *) object_dynamic_cast(owner, TYPE_PMB887X_DMA_SINK);
	if (!sink)
		return false;

	return PMB887X_DMA_SINK_GET_CLASS(sink)->write_block(sink, mr, offset, data, width, count, is_increment);
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
