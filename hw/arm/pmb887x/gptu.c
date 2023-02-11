/*
 * GPTU (General Purpose Timer Unit)
 * */
#define PMB887X_TRACE_ID		GPTU
#define PMB887X_TRACE_PREFIX	"pmb887x-gptu"

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

#define TYPE_PMB887X_GPTU	"pmb887x-gptu"
#define PMB887X_GPTU(obj)	OBJECT_CHECK(pmb887x_gptu_t, (obj), TYPE_PMB887X_GPTU)

#define GPTU_OVERFLOW	0x100

const char *TIMER_NAMES[] = {"T0A", "T0B", "T0C", "T0D", "T1A", "T1B", "T1C", "T1D"};

enum {
	INPUT_BYPASS = 0,
	INPUT_CNT0,
	INPUT_CNT1,
	INPUT_CONCAT
};

enum {
	EV_START_A		= 0,
	EV_STOP_A		= 1,
	EV_UPDOWN_A		= 2,
	EV_CLEAR_A		= 3,
	EV_RLCP0_A		= 4,
	EV_RLCP1_A		= 5,
	EV_OUV_T2A		= 6,
	EV_OUV_T2B		= 7,
	EV_START_B		= 8,
	EV_STOP_B		= 9,
	EV_RLCP0_B		= 10,
	EV_RLCP1_B		= 11,
	EV_SR00			= 12,
	EV_SR01			= 13,
	EV_SR10			= 14,
	EV_SR11			= 15,
};

typedef struct {
	int id;
	uint32_t mask;
} pmb887x_gptu_ev_t;

typedef struct {
	int id;
	int prev;
	int next;
	bool enabled;
	bool concat;
	bool ev_ssr[2];
	uint64_t start;
	uint64_t counter;
	uint64_t reload;
} pmb887x_gptu_timer_t;

typedef struct {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	qemu_irq irq[8];
	QEMUTimer *timer;
	
	bool enabled;
	uint32_t freq;
	
	int from;
	int to;
	
	pmb887x_clc_reg_t clc;
	pmb887x_src_reg_t src[8];
	
	pmb887x_gptu_timer_t timers[8];
	pmb887x_gptu_timer_t timers_t2[2];
	pmb887x_gptu_ev_t events[16];
	int events_ssr[2][2];
	
	struct pmb887x_pll_t *pll;
	
	uint64_t next;
	
	uint32_t t01irs;
	uint32_t t01ots;
	uint32_t t2con;
	uint32_t t2rccon;
	uint32_t t2ais;
	uint32_t t2bis;
	uint32_t t2es;
	uint32_t osel;
	uint32_t out;
	uint32_t t2;
	uint32_t t2rc0;
	uint32_t t2rc1;
	uint32_t t012run;
	uint32_t srsel;
} pmb887x_gptu_t;

static void gptu_update_freq(pmb887x_gptu_t *p) {
	uint8_t rmc = pmb887x_clc_get_rmc(&p->clc);
	
	p->freq = rmc > 0 ? pmb887x_pll_get_fsys(p->pll) / rmc : 0;
	p->enabled = pmb887x_clc_is_enabled(&p->clc) && p->freq > 0;
	
	DPRINTF("fgptu=%d %s\n", p->freq, p->enabled ? "[ON]" : "[OFF]");
}

static uint64_t gptu_ticks_to_ns(pmb887x_gptu_t *p, uint64_t ticks) {
    return muldiv64(ticks, NANOSECONDS_PER_SECOND, p->freq);
}

static uint64_t gptu_reload_counter(pmb887x_gptu_timer_t *timer, uint64_t counter) {
	if (!timer->reload)
		return counter % GPTU_OVERFLOW;
	
	while (counter > GPTU_OVERFLOW) {
		counter -= GPTU_OVERFLOW;
		counter += timer->reload;
	}
	return counter;
}

static uint64_t gptu_get_timer_counter(pmb887x_gptu_t *p, pmb887x_gptu_timer_t *timer, uint64_t now, bool real) {
	uint64_t counter = timer->counter;
	
	if (timer->enabled) {
		if (timer->concat) {
			counter += gptu_get_timer_counter(p, &p->timers[timer->prev], now, true) / GPTU_OVERFLOW;
		} else {
			counter += muldiv64(now - timer->start, p->freq, NANOSECONDS_PER_SECOND);
		}
	}
	
	return real ? counter : gptu_reload_counter(timer, counter);
}

static void gptu_trigger_ev_irq(pmb887x_gptu_t *p, int ev_id) {
	pmb887x_gptu_ev_t *ev = &p->events[ev_id];
	for (int i = 0; i < 8; i++) {
		if ((ev->mask) & (1 << i))
			pmb887x_src_set(&p->src[i], MOD_SRC_SETR);
	}
}

static int gptu_get_ssr_ev(int id, int n) {
	if (id == 0 && n == 0)
		return EV_SR00;
	if (id == 0 && n == 1)
		return EV_SR01;
	if (id == 1 && n == 0)
		return EV_SR10;
	if (id == 1 && n == 1)
		return EV_SR11;
	
	hw_error("gptu_get_ssr_ev(%d, %d)", id, n);
	return -1;
}

static void gptu_sync_timer(pmb887x_gptu_t *p) {
	if (p->from == -1)
		return;
	
	uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	
	p->next = now + p->freq;
	
	for (int i = p->from; i < p->to; i++) {
		int timer_id = i % 8;
		pmb887x_gptu_timer_t *timer = &p->timers[timer_id];
		
		if (timer->enabled && !timer->start && !timer->concat)
			timer->start = now;
		
		uint64_t counter = gptu_get_timer_counter(p, timer, now, true);
		if (counter >= GPTU_OVERFLOW) {
			int timer_group = timer_id / 4;
			for (int j = 0; j < 2; j++) {
				if (timer->ev_ssr[j] && p->events_ssr[timer_group][j] == timer_id)
					gptu_trigger_ev_irq(p, gptu_get_ssr_ev(timer_group, j));
			}
			
			pmb887x_gptu_timer_t *next_timer = &p->timers[timer->next];
			if (next_timer->concat)
				next_timer->counter += counter / GPTU_OVERFLOW;
			
			if (timer->concat) {
				timer->counter = gptu_reload_counter(timer, counter);
				counter = timer->counter;
			} else {
				timer->start = timer->enabled ? now : 0;
				timer->counter = gptu_reload_counter(timer, counter);
				counter = timer->counter;
			}
		} else {
			if (!timer->concat && p->enabled) {
				p->next = MIN(p->next, now + gptu_ticks_to_ns(p, GPTU_OVERFLOW - counter));
			}
		}
	}
	
	if (p->enabled)
		timer_mod(p->timer, p->next);
}

static void gptu_update_events(pmb887x_gptu_t *p) {
	for (int i = 0; i < 16; i++) {
		p->events[i].id = i;
		p->events[i].mask = 0;
	}
	
	for (int i = 0; i < 8; i++) {
		int source_id = 8 - i - 1;
		int event_id = (p->srsel >> (4 * i)) & 0xF;
		p->events[event_id].mask |= (1 << source_id);
	}
	
	p->events_ssr[0][0] = (p->t01ots & GPTU_T01OTS_SSR00) >> GPTU_T01OTS_SSR00_SHIFT;
	p->events_ssr[0][1] = (p->t01ots & GPTU_T01OTS_SSR01) >> GPTU_T01OTS_SSR01_SHIFT;
	p->events_ssr[1][0] = ((p->t01ots & GPTU_T01OTS_SSR10) >> GPTU_T01OTS_SSR10_SHIFT) + 4;
	p->events_ssr[1][1] = ((p->t01ots & GPTU_T01OTS_SSR11) >> GPTU_T01OTS_SSR11_SHIFT) + 4;
	
	for (int i = 0; i < 8; i++) {
		for (int j = 0; j < 2; j++) {
			int timer_group = i / 4;
			int ev_id = gptu_get_ssr_ev(timer_group, j);
			p->timers[i].ev_ssr[j] = p->events[ev_id].mask && p->events_ssr[timer_group][j] == i;
		}
	}
	
	gptu_sync_timer(p);
}

static uint32_t gptu_get_counter(pmb887x_gptu_t *p, int id, int size) {
	uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	uint32_t value = 0;
	for (int i = 0; i < size; i++) {
		pmb887x_gptu_timer_t *timer = &p->timers[id * 4 + i];
		value |= gptu_get_timer_counter(p, timer, now, false) << (i * 8);
	}
	return value;
}

static void gptu_set_counter(pmb887x_gptu_t *p, int id, uint32_t value, int size) {
	gptu_sync_timer(p);
	for (int i = 0; i < size; i++) {
		pmb887x_gptu_timer_t *timer = &p->timers[id * 4 + i];
		timer->counter = (value >> (i * 8)) & 0xFF;
		timer->start = 0;
	}
	gptu_sync_timer(p);
}

static uint32_t gptu_get_reload(pmb887x_gptu_t *p, int id, int size) {
	uint32_t value = 0;
	for (int i = 0; i < size; i++) {
		pmb887x_gptu_timer_t *timer = &p->timers[id * 4 + i];
		value |= timer->reload << (i * 8);
	}
	return value;
}

static void gptu_set_reload(pmb887x_gptu_t *p, int id, uint32_t value, int size) {
	gptu_sync_timer(p);
	for (int i = 0; i < size; i++) {
		pmb887x_gptu_timer_t *timer = &p->timers[id * 4 + i];
		timer->reload = (value >> (i * 8)) & 0xFF;
	}
}

static void gptu_rebuild_timers(pmb887x_gptu_t *p) {
	int last_head = -1;
	bool processed[8] = {false};
	int prev_timer_id = 7;
	
	p->from = -1;
	p->to = -1;
		
	for (int i = 0; i < 16; i++) {
		int timer_id = i % 8;
		
		if (processed[timer_id])
			continue;
		
		uint32_t shift = timer_id * 2;
		uint32_t input = (p->t01irs >> shift) & 3;
		
		p->timers[timer_id].id = timer_id;
		p->timers[timer_id].enabled = p->enabled && (p->t012run & (1 << timer_id)) != 0;
		
		if (input == INPUT_BYPASS) {
			last_head = timer_id;
			
			if (p->from == -1)
				p->from = i;
			
			p->to = i;
			
			p->timers[timer_id].concat = false;
			p->timers[timer_id].next = (i + 1) % 8;
			p->timers[timer_id].prev = prev_timer_id;
			processed[timer_id] = true;
		} else if (input == INPUT_CONCAT) {
			if (last_head != -1) {
				p->timers[timer_id].concat = true;
				p->timers[timer_id].next = (i + 1) % 8;
				p->timers[timer_id].prev = prev_timer_id;
				processed[timer_id] = true;
				p->to = i;
			}
		} else {
			hw_error("Unsupported gptu input source: %08X", input);
		}
		
		prev_timer_id = timer_id;
	}
	
	if (last_head == -1)
		hw_error("Main timer not found!");
	
	gptu_sync_timer(p);
}

static void gptu_ptimer_reset(void *opaque) {
	pmb887x_gptu_t *p = (pmb887x_gptu_t *) opaque;
	gptu_sync_timer(p);
}

static int get_src_index_by_addr(hwaddr haddr) {
	switch (haddr) {
		case GPTU_SRC0:		return 0;
		case GPTU_SRC1:		return 1;
		case GPTU_SRC2:		return 2;
		case GPTU_SRC3:		return 3;
		case GPTU_SRC4:		return 4;
		case GPTU_SRC5:		return 5;
		case GPTU_SRC6:		return 6;
		case GPTU_SRC7:		return 7;
	}
	return -1;
}

static uint64_t gptu_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_gptu_t *p = (pmb887x_gptu_t *) opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case GPTU_CLC:
			value = pmb887x_clc_get(&p->clc);
		break;
		
		case GPTU_ID:
			value = 0x0001C011;
		break;
		
		case GPTU_T01IRS:
			value = p->t01irs;
		break;
		
		case GPTU_T01OTS:
			value = p->t01ots;
		break;
		
		case GPTU_T2CON:
			value = p->t2con;
		break;
		
		case GPTU_T2RCCON:
			value = p->t2rccon;
		break;
		
		case GPTU_T2AIS:
			value = p->t2ais;
		break;
		
		case GPTU_T2BIS:
			value = p->t2bis;
		break;
		
		case GPTU_T2ES:
			value = p->t2es;
		break;
		
		case GPTU_OSEL:
			value = p->osel;
		break;
		
		case GPTU_OUT:
			value = p->out;
		break;
		
		case GPTU_T0DCBA:
			value = gptu_get_counter(p, 0, 4);
		break;
		
		case GPTU_T0CBA:
			value = gptu_get_counter(p, 0, 3);
		break;
		
		case GPTU_T0RDCBA:
			value = gptu_get_reload(p, 0, 4);
		break;
		
		case GPTU_T0RCBA:
			value = gptu_get_reload(p, 0, 3);
		break;
		
		case GPTU_T1DCBA:
			value = gptu_get_counter(p, 1, 4);
		break;
		
		case GPTU_T1CBA:
			value = gptu_get_counter(p, 1, 3);
		break;
		
		case GPTU_T1RDCBA:
			value = gptu_get_reload(p, 1, 4);
		break;
		
		case GPTU_T1RCBA:
			value = gptu_get_reload(p, 1, 3);
		break;
		
		case GPTU_T2:
			value = p->t2;
		break;
		
		case GPTU_T2RC0:
			value = p->t2rc0;
		break;
		
		case GPTU_T2RC1:
			value = p->t2rc1;
		break;
		
		case GPTU_T012RUN:
			value = p->t012run;
		break;
		
		case GPTU_SRSEL:
			value = p->srsel;
		break;
		
		case GPTU_SRC0:
		case GPTU_SRC1:
		case GPTU_SRC2:
		case GPTU_SRC3:
		case GPTU_SRC4:
		case GPTU_SRC5:
		case GPTU_SRC6:
		case GPTU_SRC7:
			value = pmb887x_src_get(&p->src[get_src_index_by_addr(haddr)]);
		break;
		
		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void gptu_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_gptu_t *p = (pmb887x_gptu_t *) opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case GPTU_CLC:
			pmb887x_clc_set(&p->clc, value);
			gptu_sync_timer(p);
			gptu_update_freq(p);
			gptu_rebuild_timers(p);
		break;
		
		case GPTU_ID:
			value = 0x0001C011;
		break;
		
		case GPTU_T01IRS:
			p->t01irs = value;
			gptu_sync_timer(p);
			gptu_rebuild_timers(p);
		break;
		
		case GPTU_T01OTS:
			p->t01ots = value;
			gptu_update_events(p);
		break;
		
		case GPTU_T2CON:
			p->t2con = value;
		break;
		
		case GPTU_T2RCCON:
			p->t2rccon = value;
		break;
		
		case GPTU_T2AIS:
			p->t2ais = value;
		break;
		
		case GPTU_T2BIS:
			p->t2bis = value;
		break;
		
		case GPTU_T2ES:
			p->t2es = value;
		break;
		
		case GPTU_OSEL:
			p->osel = value;
		break;
		
		case GPTU_OUT:
			p->out = value;
		break;
		
		case GPTU_T0DCBA:
			gptu_set_counter(p, 0, value, 4);
		break;
		
		case GPTU_T0CBA:
			gptu_set_counter(p, 0, value, 3);
		break;
		
		case GPTU_T0RDCBA:
			gptu_set_reload(p, 0, value, 4);
		break;
		
		case GPTU_T0RCBA:
			gptu_set_reload(p, 0, value, 3);
		break;
		
		case GPTU_T1DCBA:
			gptu_set_counter(p, 1, value, 4);
		break;
		
		case GPTU_T1CBA:
			gptu_set_counter(p, 1, value, 3);
		break;
		
		case GPTU_T1RDCBA:
			gptu_set_reload(p, 1, value, 4);
		break;
		
		case GPTU_T1RCBA:
			gptu_set_reload(p, 1, value, 3);
		break;
		
		case GPTU_T2:
			p->t2 = value;
		break;
		
		case GPTU_T2RC0:
			p->t2rc0 = value;
		break;
		
		case GPTU_T2RC1:
			p->t2rc1 = value;
		break;
		
		case GPTU_T012RUN:
			p->t012run = value;
			gptu_sync_timer(p);
			gptu_rebuild_timers(p);
		break;
		
		case GPTU_SRSEL:
			p->srsel = value;
			gptu_update_events(p);
		break;
		
		case GPTU_SRC0:
		case GPTU_SRC1:
		case GPTU_SRC2:
		case GPTU_SRC3:
		case GPTU_SRC4:
		case GPTU_SRC5:
		case GPTU_SRC6:
		case GPTU_SRC7:
			pmb887x_src_set(&p->src[get_src_index_by_addr(haddr)], value);
		break;
		
		default:
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
}

static const MemoryRegionOps io_ops = {
	.read			= gptu_io_read,
	.write			= gptu_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 4,
		.max_access_size	= 4
	}
};

static void gptu_init(Object *obj) {
	pmb887x_gptu_t *p = PMB887X_GPTU(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-gptu", GPTU_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (int i = 0; i < ARRAY_SIZE(p->src); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void gptu_realize(DeviceState *dev, Error **errp) {
	pmb887x_gptu_t *p = PMB887X_GPTU(dev);
	
	if (!p->pll)
		hw_error("PLL not found...");
	
	pmb887x_clc_init(&p->clc);
	
	for (int i = 0; i < ARRAY_SIZE(p->src); i++) {
		if (!p->irq[i])
			hw_error("pmb887x-gptu: irq %d not set", i);
		pmb887x_src_init(&p->src[i], p->irq[i]);
	}
	
    p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gptu_ptimer_reset, p);
    
	gptu_update_freq(p);
	gptu_update_events(p);
	gptu_rebuild_timers(p);
	gptu_sync_timer(p);
}

static Property gptu_properties[] = {
	DEFINE_PROP_LINK("pll", pmb887x_gptu_t, pll, "pmb887x-pll", struct pmb887x_pll_t *),
    DEFINE_PROP_END_OF_LIST(),
};

static void gptu_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, gptu_properties);
	dc->realize = gptu_realize;
}

static const TypeInfo gptu_info = {
    .name          	= TYPE_PMB887X_GPTU,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_gptu_t),
    .instance_init 	= gptu_init,
    .class_init    	= gptu_class_init,
};

static void gptu_register_types(void) {
	type_register_static(&gptu_info);
}
type_init(gptu_register_types)
