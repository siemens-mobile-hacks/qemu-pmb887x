/*
 * RTC
 * */
#define PMB887X_TRACE_ID		UNKNOWN
#define PMB887X_TRACE_PREFIX	"pmb887x-unknown"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/ptimer.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"

#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_RTC	"pmb887x-unknown"
#define PMB887X_RTC(obj)	OBJECT_CHECK(struct pmb887x_unknown_t, (obj), TYPE_PMB887X_RTC)

struct pmb887x_unknown_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	uint32_t unk_reg_F4600040;
};

static uint64_t unknown_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb887x_unknown_t *p = (struct pmb887x_unknown_t *) opaque;

	uint64_t value = 0;

#ifdef PMB887X_TRACE_UNHANDLED_IO
	bool dump_io = true;
#else
	bool dump_io = haddr < 0xF0000000;
#endif

	if (haddr == 0xF4C0001C)
		value = 0;

	if (haddr == 0xF4C00000)
		value = 0xFFFFFFFF;

	if (haddr == 0xF4600024)
		value = 0x800000 | 0x11;

	if (haddr == 0xF4600040)
		value = p->unk_reg_F4600040;

#ifdef PMB887X_IO_BRIDGE
	value = pmb8876_io_bridge_read(addr, size);
	pmb887x_dump_io(addr, size, value, false);
	return value;
#endif

	if (dump_io) {
		IO_DUMP(haddr + p->mmio.addr, size, value, false);
	}

	return value;
}

static void unknown_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct pmb887x_unknown_t *p = (struct pmb887x_unknown_t *) opaque;

#ifdef PMB887X_TRACE_UNHANDLED_IO
	bool dump_io = true;
	#else
	bool dump_io = haddr < 0xF0000000;
#endif

	if (dump_io) {
		IO_DUMP(haddr + p->mmio.addr, size, value, true);
	}

#ifdef PMB887X_IO_BRIDGE
	pmb887x_dump_io(addr, size, value, true);
	pmb8876_io_bridge_write(addr, size, value);
	return;
#endif

	if (haddr == 0xf460001c) {
		if (value == 0x8) {
			p->unk_reg_F4600040 = 2;
		} else {
			p->unk_reg_F4600040 = 1;
		}
	}

	if (haddr == 0xF460002C) {
		if (value == 2) {
			p->unk_reg_F4600040 = 1;
		} else {
			p->unk_reg_F4600040 = 0;
		}
	}
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
	struct pmb887x_unknown_t *p = PMB887X_RTC(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-unknown", 0x100000000);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
}

static void unknown_realize(DeviceState *dev, Error **errp) {
	// Nothing
}

static void unknown_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	dc->realize = unknown_realize;
}

static const TypeInfo unknown_info = {
	.name          	= TYPE_PMB887X_RTC,
	.parent        	= TYPE_SYS_BUS_DEVICE,
	.instance_size 	= sizeof(struct pmb887x_unknown_t),
	.instance_init 	= unknown_init,
	.class_init    	= unknown_class_init,
};

static void unknown_register_types(void) {
	type_register_static(&unknown_info);
}
type_init(unknown_register_types)
