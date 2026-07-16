/*
 * DSP
 * */
#define PMB887X_TRACE_ID		DSP
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "hw/core/qdev-properties.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/dsp.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define DSP_RAM_SIZE		(DSP_IO_SIZE - DSP_RAM0)
#define TYPE_PMB887X_DSP	"pmb887x-dsp"
#define PMB887X_DSP(obj)	OBJECT_CHECK(pmb887x_dsp_t, (obj), TYPE_PMB887X_DSP)

typedef struct pmb887x_dsp_t pmb887x_dsp_t;

struct pmb887x_dsp_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	MemoryRegion regs;
	MemoryRegion ram;
	uint32_t revision;
	
	uint32_t com_status;
	uint8_t ram_data[DSP_RAM_SIZE];
	uint32_t ram0_value;
	
	pmb887x_clc_reg_t clc;
};

static void dsp_update_state(pmb887x_dsp_t *p) {
	// TODO
}

static void dsp_reset_internal_state(pmb887x_dsp_t *p) {
	p->com_status = 0;
	memset(p->ram_data, 0, sizeof(p->ram_data));
	p->ram_data[0] = p->ram0_value;
	p->ram_data[1] = p->ram0_value >> 8;
	dsp_update_state(p);
}

static void dsp_reset_input(void *opaque, int id, int level) {
	if (level)
		dsp_reset_internal_state(opaque);
}

static void dsp_interrupt_input(void *opaque, int id, int level) {
	pmb887x_dsp_t *p = opaque;

	if (!level)
		return;

	p->com_status &= ~BIT(id);
	DPRINTF("interrupt %d acknowledged\n", id);
}

static uint64_t dsp_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_dsp_t *p = opaque;
	
	uint64_t value = 0;

	switch (haddr) {
		case DSP_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case DSP_ID:
			value = 0xF022C000 | p->revision;
			break;

		case DSP_COM_STATUS:
			value = p->com_status;
			break;

		case DSP_COM_SET:
		case DSP_COM_CLEAR:
			break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}

	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void dsp_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_dsp_t *p = opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);

	switch (haddr) {
		case DSP_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case DSP_COM_SET:
			p->com_status |= value & DSP_COM_SET_FLAGS;
			break;

		case DSP_COM_CLEAR:
			p->com_status &= ~(value & DSP_COM_CLEAR_FLAGS);
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}

	dsp_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= dsp_io_read,
	.write			= dsp_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static uint64_t dsp_ram_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_dsp_t *p = opaque;
	uint64_t value = 0;

	for (unsigned i = 0; i < size; i++)
		value |= (uint64_t) p->ram_data[haddr + i] << (i * 8);

	IO_DUMP(haddr + p->mmio.addr + DSP_RAM0, size, value, false);
	return value;
}

static void dsp_ram_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_dsp_t *p = opaque;

	IO_DUMP(haddr + p->mmio.addr + DSP_RAM0, size, value, true);
	for (unsigned i = 0; i < size; i++)
		p->ram_data[haddr + i] = value >> (i * 8);
}

static const MemoryRegionOps ram_io_ops = {
	.read			= dsp_ram_read,
	.write			= dsp_ram_write,
	.endianness		= DEVICE_LITTLE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4,
		.unaligned		= true,
	},
	.impl			= {
		.min_access_size	= 1,
		.max_access_size	= 4,
		.unaligned		= true,
	},
};

static void dsp_init(Object *obj) {
	pmb887x_dsp_t *p = PMB887X_DSP(obj);
	memory_region_init(&p->mmio, obj, "pmb887x-dsp", DSP_IO_SIZE);
	memory_region_init_io(&p->regs, obj, &io_ops, p, "pmb887x-dsp-regs", DSP_RAM0);
	memory_region_init_io(&p->ram, obj, &ram_io_ops, p, "pmb887x-dsp-ram", DSP_RAM_SIZE);
	memory_region_add_subregion(&p->mmio, 0, &p->regs);
	memory_region_add_subregion(&p->mmio, DSP_RAM0, &p->ram);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_reset_input, "RESET_IN", 1);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_interrupt_input, "INT_IN", PMB887X_DSP_INT_COUNT);
}

static void dsp_reset(DeviceState *dev) {
	pmb887x_dsp_t *p = PMB887X_DSP(dev);

	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	dsp_reset_internal_state(p);
}

static const Property dsp_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_dsp_t, revision, 0),
	DEFINE_PROP_UINT32("ram0_value", pmb887x_dsp_t, ram0_value, 0x0801),
};

static void dsp_realize(DeviceState *dev, Error **errp) {
	pmb887x_dsp_t *p = PMB887X_DSP(dev);
	
	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	dsp_reset_internal_state(p);
}

static void dsp_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, dsp_properties);
	device_class_set_legacy_reset(dc, dsp_reset);
	dc->realize = dsp_realize;
}

static const TypeInfo dsp_info = {
    .name          	= TYPE_PMB887X_DSP,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_dsp_t),
    .instance_init 	= dsp_init,
    .class_init    	= dsp_class_init,
};

static void dsp_register_types(void) {
	type_register_static(&dsp_info);
}
type_init(dsp_register_types)
