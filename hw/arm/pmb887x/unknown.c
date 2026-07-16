/*
 * Unknown memory
 * */
#define PMB887X_TRACE_ID		UNKNOWN
#define PMB887X_TRACE_PREFIX	"pmb887x-unknown"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"

#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_UNKNOWN	"pmb887x-unknown"
#define PMB887X_UNKNOWN(obj)	OBJECT_CHECK(pmb887x_unknown_t, (obj), TYPE_PMB887X_UNKNOWN)

typedef struct pmb887x_unknown_t pmb887x_unknown_t;

struct pmb887x_unknown_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
};

static uint64_t unknown_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_unknown_t *p = opaque;

	uint64_t value = 0;

	#if PMB887X_IO_BRIDGE
	value = pmb8876_io_bridge_read(haddr, size);
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	return value;
	#endif

	if (haddr == 0xF4C0001C)
		value = 0;

	if (haddr == 0xF4C00000)
		value = 0xFFFFFFFF;

	IO_DUMP(haddr + p->mmio.addr, size, value, false);

	return value;
}

static void unknown_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_unknown_t *p = opaque;

#if PMB887X_IO_BRIDGE
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	pmb8876_io_bridge_write(haddr, size, value);
	return;
#endif

	IO_DUMP(haddr + p->mmio.addr, size, value, true);

}

static const MemoryRegionOps io_ops = {
	.read			= unknown_io_read,
	.write			= unknown_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void unknown_init(Object *obj) {
	pmb887x_unknown_t *p = PMB887X_UNKNOWN(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-unknown", 0x100000000);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
}

static const TypeInfo unknown_info = {
	.name          	= TYPE_PMB887X_UNKNOWN,
	.parent        	= TYPE_SYS_BUS_DEVICE,
	.instance_size 	= sizeof(struct pmb887x_unknown_t),
	.instance_init 	= unknown_init,
};

static void unknown_register_types(void) {
	type_register_static(&unknown_info);
}
type_init(unknown_register_types)
