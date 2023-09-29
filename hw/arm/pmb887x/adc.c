/*
 * ADC
 * */
#define PMB887X_TRACE_ID		ADC
#define PMB887X_TRACE_PREFIX	"pmb887x-adc"

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

#include "hw/arm/pmb887x/adc.h"
#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_ADC	"pmb887x-adc"
#define PMB887X_ADC(obj)	OBJECT_CHECK(struct pmb887x_adc_t, (obj), TYPE_PMB887X_ADC)
#define ADC_REF_VOLTAGE		1800

struct pmb887x_adc_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	qemu_irq irq[2];
	
	pmb887x_clc_reg_t clc;
	pmb887x_src_reg_t src[2];
	
	uint32_t pllcon;
	uint32_t con0;
	uint32_t con1;
	
	pmb887x_adc_input_t inputs[PMB887X_ADC_MAX_INPUTS];
};

void pmb887x_adc_set_input(DeviceState *dev, uint32_t n, const pmb887x_adc_input_t *input) {
	struct pmb887x_adc_t *p = PMB887X_ADC(dev);
	memcpy(&p->inputs[n], input, sizeof(pmb887x_adc_input_t));
}

static void adc_update_state(struct pmb887x_adc_t *p) {
	// TODO
}

// 0xD00 // 1421mV // 3552mV before rdiv
// 0xD40 // 1498mV // 3744mV before rdiv
// 0xDB9 // 1642mV // 4106mV before rdiv
// 0xCAA // 1318mV // 3294mV before rdiv

static uint32_t adc_get_stub_value(struct pmb887x_adc_t *p) {
	switch ((p->con1 & 0xFFFF)) {
		case 0x0002:	return 0xCAA;
		case 0x0042:	return 0xFFF - 3017;
		
		case 0x4018:	return 0x990;
		case 0x4058:	return 0x66D;
		
		case 0x4008:	return 0x7FE;
		case 0x4048:	return 0x7FE;
		case 0x2008:	return 0x7FF;
		case 0x2048:	return 0x7FD;
	}
	return 0;
}

static uint64_t adc_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb887x_adc_t *p = (struct pmb887x_adc_t *) opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case ADC_CLC:
			value = pmb887x_clc_get(&p->clc);
		break;
		
		case ADC_ID:
			value = 0x0000C011;
		break;
		
		case ADC_STAT:
			value = ADC_STAT_READY;
		break;
		
		case ADC_FIFO0 ... ADC_FIFO7:
			value = adc_get_stub_value(p);
		break;
		
		case ADC_PLLCON:
			value = p->pllcon;
		break;
		
		case ADC_CON0:
			value = p->con0;
		break;
		
		case ADC_CON1:
			value = p->con1;
		break;
		
		case ADC_SRC0:
			value = pmb887x_src_get(&p->src[0]);
		break;
		
		case ADC_SRC1:
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

static void adc_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct pmb887x_adc_t *p = (struct pmb887x_adc_t *) opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case ADC_CLC:
			pmb887x_clc_set(&p->clc, value);
		break;
		
		case ADC_PLLCON:
			p->pllcon = value;
		break;
		
		case ADC_CON0:
			p->con0 = value;
		break;
		
		case ADC_CON1:
			p->con1 = value;
		break;
		
		case ADC_SRC0:
			pmb887x_src_set(&p->src[0], value);
		break;
		
		case ADC_SRC1:
			pmb887x_src_set(&p->src[1], value);
		break;
		
		default:
			EPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	adc_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= adc_io_read,
	.write			= adc_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void adc_init(Object *obj) {
	struct pmb887x_adc_t *p = PMB887X_ADC(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-adc", ADC_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (size_t i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void adc_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_adc_t *p = PMB887X_ADC(dev);
	
	pmb887x_clc_init(&p->clc);
	
	for (size_t i = 0; i < ARRAY_SIZE(p->irq); i++)
		pmb887x_src_init(&p->src[i], p->irq[i]);
	
	adc_update_state(p);
}

static Property adc_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void adc_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, adc_properties);
	dc->realize = adc_realize;
}

static const TypeInfo adc_info = {
    .name          	= TYPE_PMB887X_ADC,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_adc_t),
    .instance_init 	= adc_init,
    .class_init    	= adc_class_init,
};

static void adc_register_types(void) {
	type_register_static(&adc_info);
}
type_init(adc_register_types)
