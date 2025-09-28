/*
 * System Timer (56 bit)
 * */
#define PMB887X_TRACE_ID		STM
#define PMB887X_TRACE_PREFIX	"pmb887x-stm"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"

#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_STM	"pmb887x-stm"
#define PMB887X_STM(obj)	OBJECT_CHECK(pmb887x_stm_t, (obj), TYPE_PMB887X_STM)

typedef struct pmb887x_stm_t pmb887x_stm_t;

struct pmb887x_stm_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	pmb887x_clc_reg_t clc;
	
	bool enabled;
	uint32_t freq;
	int64_t start;
	int64_t capture;
	int64_t counter;

	pmb887x_pll_t *pll;
};

static int64_t stm_get_time(pmb887x_stm_t *p) {
	if (p->enabled) {
		uint64_t delta_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - p->start;
		return p->counter + muldiv64(delta_ns, p->freq, NANOSECONDS_PER_SECOND);
	}
	return p->counter;
}

static void stm_update_state(pmb887x_stm_t *p) {
	uint32_t div = pmb887x_clc_get_rmc(&p->clc);
	uint32_t new_freq = div > 0 ? pmb887x_pll_get_fstm(p->pll) / div : 0;
	bool new_enabled = new_freq > 0 && pmb887x_clc_is_enabled(&p->clc);
	
	if (new_enabled != p->enabled || new_freq != p->freq) {
		p->freq = new_freq;
		p->enabled = new_enabled;
		
		if (p->start)
			p->counter = stm_get_time(p);
		
		if (p->enabled) {
			p->start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
		} else {
			p->start = 0;
		}
		
		DPRINTF("fstm=%d, fstm / RMC=%d [%s]\n", pmb887x_pll_get_fstm(p->pll), p->freq, p->enabled ? "ON" : "OFF");
	}
}

static void stm_update_state_callback(void *opaque) {
	stm_update_state(opaque);
}

static uint64_t stm_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_stm_t *p = opaque;
	
	uint64_t value = 0;
	switch (haddr) {
		case STM_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;
		
		case STM_ID:
			value = 0x0000C011;
			break;
		
		case STM_TIM0:
			p->capture = stm_get_time(p);
			value = (p->capture >> 0) & 0xFFFFFFFF;
			break;
		
		case STM_TIM1:
			p->capture = stm_get_time(p);
			value = (p->capture >> 4) & 0xFFFFFFFF;
			break;
		
		case STM_TIM2:
			p->capture = stm_get_time(p);
			value = (p->capture >> 8) & 0xFFFFFFFF;
			break;
		
		case STM_TIM3:
			p->capture = stm_get_time(p);
			value = (p->capture >> 12) & 0xFFFFFFFF;
			break;
		
		case STM_TIM4:
			p->capture = stm_get_time(p);
			value = (p->capture >> 16) & 0xFFFFFFFF;
			break;
		
		case STM_TIM5:
			p->capture = stm_get_time(p);
			value = (p->capture >> 20) & 0xFFFFFFFF;
			break;
		
		case STM_TIM6:
			p->capture = stm_get_time(p);
			value = (p->capture >> 32) & 0x00FFFFFF;
			break;
		
		case STM_CAP:
			value = (p->capture >> 32) & 0x00FFFFFF;
			break;
		
		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void stm_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_stm_t *p = opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case STM_CLC:
			pmb887x_clc_set(&p->clc, value);
		break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	stm_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= stm_io_read,
	.write			= stm_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void stm_init(Object *obj) {
	struct pmb887x_stm_t *p = PMB887X_STM(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-stm", STM_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
}

static void stm_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_stm_t *p = PMB887X_STM(dev);
	
	pmb887x_clc_init(&p->clc);
	
	stm_update_state(p);
	pmb887x_pll_add_freq_update_callback(p->pll, stm_update_state_callback, p);
}

static const Property stm_properties[] = {
	DEFINE_PROP_LINK("pll", struct pmb887x_stm_t, pll, "pmb887x-pll", struct pmb887x_pll_t *),
};

static void stm_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, stm_properties);
	dc->realize = stm_realize;
}

static const TypeInfo stm_info = {
    .name          	= TYPE_PMB887X_STM,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_stm_t),
    .instance_init 	= stm_init,
    .class_init    	= stm_class_init,
};

static void stm_register_types(void) {
	type_register_static(&stm_info);
}
type_init(stm_register_types)
