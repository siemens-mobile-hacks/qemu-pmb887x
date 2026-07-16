/*
 * Automatic Frequency Correction
 * */
#define PMB887X_TRACE_ID		AFC
#define PMB887X_TRACE_PREFIX	"pmb887x-afc"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "hw/core/qdev-properties.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_AFC	"pmb887x-afc"
#define PMB887X_AFC(obj)	OBJECT_CHECK(pmb887x_afc_t, (obj), TYPE_PMB887X_AFC)

typedef struct pmb887x_afc_t pmb887x_afc_t;

struct pmb887x_afc_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	pmb887x_clc_reg_t clc;
	uint32_t revision;
	uint32_t afcval;
};

static uint64_t afc_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_afc_t *p = opaque;
	uint64_t value = 0;

	switch (haddr) {
		case AFC_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case AFC_ID:
			value = 0xF004C000 | p->revision;
			break;

		case AFC_AFCVAL:
			value = p->afcval;
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}

	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	return value;
}

static void afc_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_afc_t *p = opaque;

	IO_DUMP(haddr + p->mmio.addr, size, value, true);

	switch (haddr) {
		case AFC_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case AFC_AFCVAL:
			p->afcval = value & (AFC_AFCVAL_AFC | AFC_AFCVAL_ENAFC);
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}
}

static const MemoryRegionOps afc_io_ops = {
	.read = afc_io_read,
	.write = afc_io_write,
	.endianness = DEVICE_NATIVE_ENDIAN,
	.valid = {
		.min_access_size = 1,
		.max_access_size = 4,
	},
};

static void afc_init(Object *obj) {
	pmb887x_afc_t *p = PMB887X_AFC(obj);

	memory_region_init_io(&p->mmio, obj, &afc_io_ops, p, TYPE_PMB887X_AFC, AFC_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
}

static void afc_reset(DeviceState *dev) {
	pmb887x_afc_t *p = PMB887X_AFC(dev);

	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	p->afcval = 0;
}

static const Property afc_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_afc_t, revision, 0),
};

static void afc_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);

	device_class_set_props(dc, afc_properties);
	device_class_set_legacy_reset(dc, afc_reset);
}

static const TypeInfo afc_info = {
	.name = TYPE_PMB887X_AFC,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(pmb887x_afc_t),
	.instance_init = afc_init,
	.class_init = afc_class_init,
};

static void afc_register_types(void) {
	type_register_static(&afc_info);
}
type_init(afc_register_types)
