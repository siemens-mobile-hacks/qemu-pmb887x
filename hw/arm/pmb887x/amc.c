/*
 * AMC (Analog Measurement Controller?) / ADC
 * */
#define PMB887X_TRACE_ID		AMC
#define PMB887X_TRACE_PREFIX	"pmb887x-amc"

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

#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_AMC	"pmb887x-amc"
#define PMB887X_AMC(obj)	OBJECT_CHECK(struct pmb887x_amc_t, (obj), TYPE_PMB887X_AMC)

struct pmb887x_amc_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	qemu_irq irq[2];
	
	pmb887x_clc_reg_t clc;
	pmb887x_src_reg_t src[2];
	
	uint32_t con0;
	uint32_t con1;
	uint32_t con2;
};

static void amc_update_state(struct pmb887x_amc_t *p) {
	// TODO
}

static uint32_t amc_get_stub_value(struct pmb887x_amc_t *p) {
	switch ((p->con1 & 0xFFFF)) {
		case 0x0002:	return 0xD72;
		case 0x0042:	return 0x28C;
		case 0x4018:	return 0x990;
		case 0x4058:	return 0x66D;
		case 0x4008:	return 0x7FE;
		case 0x4048:	return 0x7FE;
		case 0x2008:	return 0x7FF;
		case 0x2048:	return 0x7FD;
	}
	return 0;
}

static uint64_t amc_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb887x_amc_t *p = (struct pmb887x_amc_t *) opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case AMC_CLC:
			value = pmb887x_clc_get(&p->clc);
		break;
		
		case AMC_ID:
			value = 0x0000C011;
		break;
		
		case AMC_STAT:
			value = AMC_STAT_READY;
		break;
		
		case AMC_FIFO0 ... AMC_FIFO7:
			value = amc_get_stub_value(p);
		break;
		
		case AMC_CON0:
			value = p->con0;
		break;
		
		case AMC_CON1:
			value = p->con1;
		break;
		
		case AMC_CON2:
			value = p->con2;
		break;
		
		case AMC_SRC0:
			value = pmb887x_src_get(&p->src[0]);
		break;
		
		case AMC_SRC1:
			value = pmb887x_src_get(&p->src[1]);
		break;
		
		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void amc_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct pmb887x_amc_t *p = (struct pmb887x_amc_t *) opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case AMC_CLC:
			pmb887x_clc_set(&p->clc, value);
		break;
		
		case AMC_CON0:
			p->con0 = value;
		break;
		
		case AMC_CON1:
			p->con1 = value;
		break;
		
		case AMC_CON2:
			p->con2 = value;
		break;
		
		case AMC_SRC0:
			pmb887x_src_set(&p->src[0], value);
		break;
		
		case AMC_SRC1:
			pmb887x_src_set(&p->src[1], value);
		break;
		
		default:
			EPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	amc_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= amc_io_read,
	.write			= amc_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void amc_init(Object *obj) {
	struct pmb887x_amc_t *p = PMB887X_AMC(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-amc", AMC_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (size_t i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void amc_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_amc_t *p = PMB887X_AMC(dev);
	
	pmb887x_clc_init(&p->clc);
	
	for (size_t i = 0; i < ARRAY_SIZE(p->irq); i++)
		pmb887x_src_init(&p->src[i], p->irq[i]);
	
	amc_update_state(p);
}

static Property amc_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void amc_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, amc_properties);
	dc->realize = amc_realize;
}

static const TypeInfo amc_info = {
    .name          	= TYPE_PMB887X_AMC,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_amc_t),
    .instance_init 	= amc_init,
    .class_init    	= amc_class_init,
};

static void amc_register_types(void) {
	type_register_static(&amc_info);
}
type_init(amc_register_types)
