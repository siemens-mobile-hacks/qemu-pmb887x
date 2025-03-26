/*
 * SCCU
 * */
#define PMB887X_TRACE_ID		SCCU
#define PMB887X_TRACE_PREFIX	"pmb887x-sccu"

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

#include "hw/arm/pmb887x/sccu.h"
#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_RTC	"pmb887x-sccu"
#define PMB887X_RTC(obj)	OBJECT_CHECK(struct pmb887x_sccu_t, (obj), TYPE_PMB887X_RTC)

enum {
	SCCU_IRQ_WAKE = 0,
	SCCU_IRQ_UNK
};

struct pmb887x_sccu_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	qemu_irq irq[2];
	
	pmb887x_clc_reg_t clc;
	pmb887x_src_reg_t src[2];
	
	bool irq_fired;
	uint32_t timer_freq;
	uint64_t start;
	uint64_t next;
	bool enabled;
	
	uint32_t con[4];
	uint32_t cal;
	uint32_t timer_int;
	uint32_t timer_rel;
	uint32_t timer_cnt;
	uint32_t timer_div;
	uint32_t sleep_ctrl;
	uint32_t stat;
	
	QEMUTimer *timer;
	QEMUTimer *cal_timer;
	struct pmb887x_pll_t *pll;
};

static uint64_t sccu_get_counter(struct pmb887x_sccu_t *p, bool real) {
	uint64_t next = p->timer_cnt;
	
	if (p->enabled) {
		uint64_t delta_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - p->start;
		next += muldiv64(delta_ns, p->timer_freq, NANOSECONDS_PER_SECOND);
	}
	
	return real ? next : MIN(next, p->timer_rel);
}

static uint64_t sccu_ticks_to_ns(struct pmb887x_sccu_t *p, uint64_t ticks) {
    return muldiv64(ticks, NANOSECONDS_PER_SECOND, p->timer_freq);
}

static void sccu_cal_timer_reset(void *opaque) {
	// struct pmb887x_sccu_t *p = (struct pmb887x_sccu_t *) opaque;
}

static void sccu_ptimer_reset(void *opaque) {
	struct pmb887x_sccu_t *p = (struct pmb887x_sccu_t *) opaque;
	
	if (!p->enabled)
		return;
	
	uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	uint32_t overflow = p->timer_rel + 1;
	
	if (!p->start) {
		p->stat &= ~SCCU_STAT_TPU;
		p->stat |= SCCU_STAT_TPU_SLEEP;
		
		p->start = now;
		p->timer_cnt = 0;
		
		DPRINTF("sleep timer start %d ms\n", (uint32_t) (sccu_ticks_to_ns(p, p->timer_rel) / 1000000));
		DPRINTF("now %u\n", (uint32_t) (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000));
	}
	
	uint64_t counter = sccu_get_counter(p, true);
	if (counter >= p->timer_int && !p->irq_fired) {
		p->irq_fired = true;
		pmb887x_src_update(&p->src[SCCU_IRQ_WAKE], 0, MOD_SRC_SETR);
	}
	
	if (counter >= p->timer_rel) {
		p->start = 0;
		p->timer_cnt = p->timer_cnt % overflow;
		p->con[1] &= ~SCCU_CON1_TIMER_START;
		
		p->stat &= ~SCCU_STAT_TPU;
		p->stat |= SCCU_STAT_TPU_NORMAL;
		
		DPRINTF("now %u\n", (uint32_t) (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000));
		DPRINTF("sleep timer done\n");
	} else {
		p->next = now + sccu_ticks_to_ns(p, overflow - counter);
		timer_mod(p->timer, p->next);
	}
}

static void sccu_update_timer_timer(struct pmb887x_sccu_t *p) {
	uint32_t sub = (p->con[2] & SCCU_CON2_REL_SUB) >> SCCU_CON2_REL_SUB_SHIFT;
	p->timer_freq = pmb887x_pll_get_frtc(p->pll) / p->timer_div;
	p->enabled = (p->con[1] & SCCU_CON1_TIMER_START) != 0 && pmb887x_clc_is_enabled(&p->clc);
	p->timer_int = p->timer_rel - sub;
	
	// Calibration
	if ((p->con[1] & SCCU_CON1_CAL)) {
		DPRINTF("SCCU_SLEEP_CON0_CAL\n");
		// Unknown magic, similar to hardware value
		uint32_t timer_freq = pmb887x_pll_get_frtc(p->pll) / p->timer_div;
		uint32_t sccu_freq = pmb887x_pll_get_fosc(p->pll) / pmb887x_clc_get_rmc(&p->clc);
		uint32_t ratio = sccu_freq / timer_freq;
		uint32_t cal = (ratio >= 60000 ? 0 : 60000 - ratio) << 4;
		p->cal = (cal << SCCU_CAL_VALUE0_SHIFT) | (0x400 << SCCU_CAL_VALUE1_SHIFT);
		p->con[1] &= ~SCCU_CON1_CAL;
	}
	
	// Reset
	if ((p->con[1] & SCCU_CON1_TIMER_RESET)) {
		DPRINTF("SCCU_SLEEP_CON0_RESET\n");
		p->timer_cnt = 0;
		p->start = 0;
		p->con[1] &= ~SCCU_CON1_TIMER_RESET;
	}
	
	sccu_ptimer_reset(p);
}

uint32_t pmb887x_sccu_clc_get(struct pmb887x_sccu_t *p) {
	return pmb887x_clc_get(&p->clc);
}

void pmb887x_sccu_clc_set(struct pmb887x_sccu_t *p, uint32_t value) {
	pmb887x_clc_set(&p->clc, value);
	sccu_update_timer_timer(p);
	
}

static int sccu_get_reg_index(hwaddr haddr) {
	switch (haddr) {
		case SCCU_CON0:		return 0;
		case SCCU_CON1:		return 1;
		case SCCU_CON2:		return 2;
		case SCCU_CON3:		return 3;
		
		case SCCU_WAKE_SRC:	return 0;
		case SCCU_UNK_SRC:	return 1;
	}
	abort();
}

static uint64_t sccu_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb887x_sccu_t *p = (struct pmb887x_sccu_t *) opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case SCCU_CAL:
			value = p->cal;
		break;
		
		case SCCU_TIMER_REL:
			value = p->timer_rel;
		break;
		
		case SCCU_TIMER_CNT:
			value = sccu_get_counter(p, false);
		break;
		
		case SCCU_TIMER_DIV:
			value = p->timer_div;
		break;
		
		case SCCU_SLEEP_CTRL:
			value = p->sleep_ctrl;
		break;
		
		case SCCU_CON0:
		case SCCU_CON1:
		case SCCU_CON2:
		case SCCU_CON3:
			value = p->con[sccu_get_reg_index(haddr)];
		break;
		
		case SCCU_STAT:
			value = p->stat;
		break;
		
		case SCCU_WAKE_SRC:
		case SCCU_UNK_SRC:
			value = pmb887x_src_get(&p->src[sccu_get_reg_index(haddr)]);
		break;
		
		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
		break;
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void sccu_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct pmb887x_sccu_t *p = (struct pmb887x_sccu_t *) opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case SCCU_TIMER_REL:
			p->timer_rel = value;
			sccu_update_timer_timer(p);
		break;
		
		case SCCU_TIMER_DIV:
			p->timer_div = value;
			sccu_update_timer_timer(p);
		break;
		
		case SCCU_CON0:
		case SCCU_CON1:
		case SCCU_CON2:
		case SCCU_CON3:
			p->con[sccu_get_reg_index(haddr)] = value;
			sccu_update_timer_timer(p);
		break;
		
		case SCCU_SLEEP_CTRL:
			p->sleep_ctrl = value;
		break;
		
		case SCCU_WAKE_SRC:
		case SCCU_UNK_SRC:
			pmb887x_src_set(&p->src[sccu_get_reg_index(haddr)], value);
		break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
		break;
	}
}

static const MemoryRegionOps io_ops = {
	.read			= sccu_io_read,
	.write			= sccu_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void sccu_init(Object *obj) {
	struct pmb887x_sccu_t *p = PMB887X_RTC(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-sccu", SCCU_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (int i = 0; i < ARRAY_SIZE(p->src); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void sccu_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_sccu_t *p = PMB887X_RTC(dev);
	
	pmb887x_clc_init(&p->clc);
	
	for (int i = 0; i < ARRAY_SIZE(p->src); i++) {
		if (!p->irq[i])
			hw_error("pmb887x-tpu: irq %d not set", i);
		
		pmb887x_src_init(&p->src[i], p->irq[i]);
	}
	
    p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sccu_ptimer_reset, p);
    p->cal_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sccu_cal_timer_reset, p);
	
	p->timer_div = 0x97;
	p->con[2] = 3 << SCCU_CON2_REL_SUB_SHIFT;
	p->stat = SCCU_STAT_CPU_NORMAL | SCCU_STAT_TPU_NORMAL;
	
	sccu_update_timer_timer(p);
}

static const Property sccu_properties[] = {
	DEFINE_PROP_LINK("pll", struct pmb887x_sccu_t, pll, "pmb887x-pll", struct pmb887x_pll_t *),
};

static void sccu_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, sccu_properties);
	dc->realize = sccu_realize;
}

static const TypeInfo sccu_info = {
    .name          	= TYPE_PMB887X_RTC,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_sccu_t),
    .instance_init 	= sccu_init,
    .class_init    	= sccu_class_init,
};

static void sccu_register_types(void) {
	type_register_static(&sccu_info);
}
type_init(sccu_register_types)
