/*
 * System Timer (56 bit)
 * */
#define PMB887X_TRACE_ID		STM
#define PMB887X_TRACE_PREFIX	"pmb887x-stm"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_STM	"pmb887x-stm"
#define PMB887X_STM(obj)	OBJECT_CHECK(pmb887x_stm_t, (obj), TYPE_PMB887X_STM)
#define STM_CLC_RESET_VALUE	((1U << MOD_CLC_RMC_SHIFT) | (1U << STM_CLC_RMC2_SHIFT))

typedef struct pmb887x_stm_t pmb887x_stm_t;

struct pmb887x_stm_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	uint32_t revision;
	
	pmb887x_clc_reg_t clc;
	bool rmc2_enabled;
	
	bool enabled;
	uint32_t freq;
	int64_t start;
	int64_t capture;
	int64_t counter;
};

static int64_t stm_get_time(pmb887x_stm_t *p) {
	if (p->enabled) {
		uint64_t delta_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - p->start;
		return p->counter + muldiv64(delta_ns, p->freq, NANOSECONDS_PER_SECOND);
	}
	return p->counter;
}

static uint32_t stm_get_frequency_hz(pmb887x_stm_t *p) {
	if (!pmb887x_clc_is_enabled(&p->clc))
		return 0;

	uint32_t divider = pmb887x_clc_get_rmc(&p->clc);
	if (divider == 0)
		return 0;

	if (p->rmc2_enabled)
		divider += (pmb887x_clc_get(&p->clc) & STM_CLC_RMC2) >> STM_CLC_RMC2_SHIFT;

	return clock_get_hz(p->clc.clock) / divider;
}

static void stm_update_state(pmb887x_stm_t *p) {
	uint32_t new_freq = stm_get_frequency_hz(p);
	bool new_enabled = new_freq > 0;
	
	if (new_enabled != p->enabled || new_freq != p->freq) {
		p->counter = stm_get_time(p);
		p->freq = new_freq;
		p->enabled = new_enabled;
		
		if (p->enabled) {
			p->start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
		} else {
			p->start = 0;
		}
		
		DPRINTF("clk=%u, fSTM=%u [%s]\n", clock_get_hz(p->clc.clock), p->freq, p->enabled ? "ON" : "OFF");
	}
}

static void stm_clock_update(void *opaque) {
	stm_update_state(opaque);
}

static void stm_handle_rmc2_enable(void *opaque, int line, int level) {
	pmb887x_stm_t *p = opaque;
	(void) line;

	p->rmc2_enabled = level;
	stm_update_state(p);
}

static uint64_t stm_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_stm_t *p = opaque;
	
	uint64_t value = 0;
	switch (haddr) {
		case STM_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;
		
		case STM_ID:
			value = 0x0000C000 | p->revision;
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
			IO_DUMP_READ(haddr + p->mmio.addr, size, 0xFFFFFFFF);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	IO_DUMP_READ(haddr + p->mmio.addr, size, value);
	
	return value;
}

static void stm_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_stm_t *p = opaque;
	
	IO_DUMP_WRITE(haddr + p->mmio.addr, size, value);
	
	switch (haddr) {
		case STM_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
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
	pmb887x_clc_init(&p->clc, DEVICE(obj));
	pmb887x_clc_set_callback(&p->clc, stm_clock_update, p);
	qdev_init_gpio_in_named(DEVICE(obj), stm_handle_rmc2_enable, "RMC2_ENABLE_IN", 1);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-stm", STM_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
}

static void stm_reset(DeviceState *dev) {
	pmb887x_stm_t *p = PMB887X_STM(dev);

	pmb887x_clc_set(&p->clc, STM_CLC_RESET_VALUE);
	p->enabled = false;
	p->start = 0;
	p->capture = 0;
	p->counter = 0;
	stm_update_state(p);
}

static void stm_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_stm_t *p = PMB887X_STM(dev);
	
	pmb887x_clc_set(&p->clc, STM_CLC_RESET_VALUE);
	
	stm_update_state(p);
}

static const Property stm_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_stm_t, revision, 0),
};

static void stm_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, stm_properties);
	device_class_set_legacy_reset(dc, stm_reset);
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
