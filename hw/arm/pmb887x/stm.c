/*
 * System Timer (56 bit)
 * */
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

#define STM_DEBUG

#ifdef STM_DEBUG
#define DPRINTF(fmt, ...) do { fprintf(stderr, "[pmb887x-stm]: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#define TYPE_PMB887X_STM	"pmb887x-stm"
#define PMB887X_STM(obj)	OBJECT_CHECK(struct pmb887x_stm_t, (obj), TYPE_PMB887X_STM)

struct pmb887x_stm_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	struct pmb887x_clc_reg_t clc;
	
	bool enabled;
	uint32_t freq;
    uint64_t counter;
    uint64_t last_time;
    uint64_t capture;
	struct pmb887x_pll_t *pll;
};

static uint64_t stm_capture_time(struct pmb887x_stm_t *p) {
	if (p->last_time) {
		uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
		uint64_t ticks = now - p->last_time;
		p->last_time = now;
		p->counter += muldiv64(ticks, p->freq, NANOSECONDS_PER_SECOND);
	}
	return p->counter;
}

static uint64_t stm_get_time(struct pmb887x_stm_t *p) {
	if (p->last_time) {
		uint64_t ticks = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - p->last_time;
		return p->counter + muldiv64(ticks, p->freq, NANOSECONDS_PER_SECOND);
	} else {
		return p->counter;
	}
}

static void stm_update_state(struct pmb887x_stm_t *p) {
	uint32_t div = pmb887x_clc_get_rmc(&p->clc);
	uint32_t new_freq = div > 0 ? pmb887x_pll_get_fsys(p->pll) / div : 0;
	bool new_enabled = new_freq > 0 && pmb887x_clc_is_enabled(&p->clc);
	
	if (new_enabled != p->enabled || new_freq != p->freq) {
		// Update counter before state change
		stm_capture_time(p);
		
		p->freq = new_freq;
		p->enabled = new_enabled;
		
		if (!p->enabled)
			p->last_time = 0;
		
		if (p->enabled && !p->last_time)
			p->last_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
		
		DPRINTF("fsys=%d, fstm=%d [%s]\n", pmb887x_pll_get_fsys(p->pll), p->freq, p->enabled ? "ON" : "OFF");
	}
}

static void stm_update_state_callback(void *opaque) {
	stm_update_state((struct pmb887x_stm_t *) opaque);
}

static uint64_t stm_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb887x_stm_t *p = (struct pmb887x_stm_t *) opaque;
	
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
			pmb887x_dump_io(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	// pmb887x_dump_io(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void stm_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct pmb887x_stm_t *p = (struct pmb887x_stm_t *) opaque;
	
	pmb887x_dump_io(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case STM_CLC:
			pmb887x_clc_set(&p->clc, value);
		break;
		
		default:
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	stm_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= stm_io_read,
	.write			= stm_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 4,
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

static Property stm_properties[] = {
	DEFINE_PROP_LINK("pll", struct pmb887x_stm_t, pll, "pmb887x-pll", struct pmb887x_pll_t *),
    DEFINE_PROP_END_OF_LIST(),
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
