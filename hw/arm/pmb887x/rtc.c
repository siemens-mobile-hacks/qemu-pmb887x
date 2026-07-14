/*
 * RTC
 * */
#define PMB887X_TRACE_ID		RTC
#define PMB887X_TRACE_PREFIX	"pmb887x-rtc"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "system/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_RTC	"pmb887x-rtc"
#define PMB887X_RTC(obj)	OBJECT_CHECK(pmb887x_rtc_t, (obj), TYPE_PMB887X_RTC)
#define RTC_ISNC_ENABLES	(RTC_ISNC_T14IE | RTC_ISNC_RTC0IE | RTC_ISNC_RTC1IE | RTC_ISNC_RTC2IE | RTC_ISNC_RTC3IE | RTC_ISNC_ALARMIE)
#define RTC_ISNC_REQUESTS	(RTC_ISNC_T14IR | RTC_ISNC_RTC0IR | RTC_ISNC_RTC1IR | RTC_ISNC_RTC2IR | RTC_ISNC_RTC3IR | RTC_ISNC_ALARMIR)
#define RTC_ISNRC_REQUESTS	(RTC_ISNRC_T14 | RTC_ISNRC_RTC0 | RTC_ISNRC_RTC1 | RTC_ISNRC_RTC2 | RTC_ISNRC_RTC3 | RTC_ISNRC_ALARM)

typedef struct pmb887x_rtc_t pmb887x_rtc_t;

struct pmb887x_rtc_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	pmb887x_clc_reg_t clc;
	pmb887x_src_reg_t src;
	pmb887x_pll_t *pll;
	qemu_irq irq;
	QEMUTimer *timer;
	
	uint32_t ctrl;
	uint32_t con;
	uint32_t t14;
	uint32_t cnt;
	uint32_t rel;
	uint32_t isnc;
	uint32_t alarm;
	int64_t start;
};

static uint32_t rtc_get_freq(pmb887x_rtc_t *p) {
	uint32_t frtc = pmb887x_pll_get_frtc(p->pll);
	return (p->con & RTC_CON_PRE) ? frtc / 8 : frtc;
}

static int64_t rtc_ticks_to_ns(pmb887x_rtc_t *p, uint64_t ticks) {
	return muldiv64(ticks, NANOSECONDS_PER_SECOND, rtc_get_freq(p));
}

static uint32_t rtc_get_enabled_requests(pmb887x_rtc_t *p) {
	return ((p->isnc & RTC_ISNC_ENABLES) << 1);
}

static void rtc_raise_requests(pmb887x_rtc_t *p, uint32_t requests) {
	uint32_t new_requests = (requests & ~(p->isnc & RTC_ISNC_REQUESTS));
	uint32_t enabled_requests = rtc_get_enabled_requests(p);
	p->isnc |= requests;

	if ((new_requests & enabled_requests)) {
		p->ctrl |= RTC_CTRL_RTCINT;
		pmb887x_src_update(&p->src, 0, MOD_SRC_SETR);
	}
}

static void rtc_increment_counter(pmb887x_rtc_t *p) {
	static const uint8_t shifts[] = { 0, 10, 16, 22 };
	static const uint8_t widths[] = { 10, 6, 6, 10 };
	static const uint32_t requests[] = { RTC_ISNC_RTC0IR, RTC_ISNC_RTC1IR, RTC_ISNC_RTC2IR, RTC_ISNC_RTC3IR };
	bool alarm = p->cnt == p->alarm;
	uint32_t raised = 0;
	bool carry = true;

	for (int i = 0; i < 4 && carry; i++) {
		uint32_t mask = (1U << widths[i]) - 1;
		uint32_t value = (p->cnt >> shifts[i]) & mask;

		if (value == mask) {
			value = (p->rel >> shifts[i]) & mask;
			raised |= requests[i];
		} else {
			value++;
			carry = false;
		}

		p->cnt = (p->cnt & ~(mask << shifts[i])) | (value << shifts[i]);
	}

	if (alarm)
		raised |= RTC_ISNC_ALARMIR;
	if (raised)
		rtc_raise_requests(p, raised);
}

static void rtc_t14_overflow(pmb887x_rtc_t *p) {
	rtc_raise_requests(p, RTC_ISNC_T14IR);
	rtc_increment_counter(p);
}

static void rtc_advance(pmb887x_rtc_t *p, uint64_t ticks) {
	uint32_t count = (p->t14 & RTC_T14_CNT) >> RTC_T14_CNT_SHIFT;
	uint32_t reload = (p->t14 & RTC_T14_REL) >> RTC_T14_REL_SHIFT;

	if (ticks && (p->con & RTC_CON_T14DEC)) {
		count = (count - 1) & 0xFFFF;
		p->con &= ~RTC_CON_T14DEC;
	}
	if (ticks && (p->con & RTC_CON_T14INC)) {
		if (count != 0xFFFF) {
			count++;
			p->con &= ~RTC_CON_T14INC;
		}
	}

	while (ticks) {
		uint64_t distance = 0x10000 - count;
		if (ticks < distance) {
			count += ticks;
			break;
		}

		ticks -= distance;
		count = reload;
		rtc_t14_overflow(p);
	}

	p->t14 = reload | (count << RTC_T14_CNT_SHIFT);
}

static void rtc_sync(pmb887x_rtc_t *p) {
	if (!(p->con & RTC_CON_RUN)) {
		p->start = 0;
		timer_del(p->timer);
		return;
	}

	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
	if (!p->start)
		p->start = now;

	uint64_t elapsed = muldiv64(now - p->start, rtc_get_freq(p), NANOSECONDS_PER_SECOND);
	if (elapsed) {
		rtc_advance(p, elapsed);
		p->start += rtc_ticks_to_ns(p, elapsed);
	}

	uint32_t count = (p->t14 & RTC_T14_CNT) >> RTC_T14_CNT_SHIFT;
	uint64_t next = (p->con & (RTC_CON_T14DEC | RTC_CON_T14INC)) ? 1 : 0x10000 - count;
	timer_mod(p->timer, p->start + rtc_ticks_to_ns(p, next));
}

static void rtc_ptimer_reset(void *opaque) {
	rtc_sync(opaque);
}

static uint64_t rtc_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_rtc_t *p = opaque;
	rtc_sync(p);
	
	uint64_t value = 0;
	
	switch (haddr) {
		case RTC_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;
		
		case RTC_ID:
			value = 0xF049C011;
			break;
		
		case RTC_CTRL:
			value = p->ctrl;
			break;
		
		case RTC_CON:
			value = p->con | RTC_CON_ACCPOS;
			break;
		
		case RTC_T14:
			value = p->t14;
			break;
		
		case RTC_CNT:
			value = p->cnt;
			break;
		
		case RTC_REL:
			value = p->rel;
			break;
		
		case RTC_ISNC:
			value = p->isnc;
			break;
		
		case RTC_ALARM:
			value = p->alarm;
			break;
		
		case RTC_ISNRC:
			value = 0;
			break;
		
		case RTC_SRC:
			value = pmb887x_src_get(&p->src);
			break;
		
		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void rtc_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_rtc_t *p = opaque;
	rtc_sync(p);
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case RTC_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;
		
		case RTC_CTRL:
			p->ctrl = (p->ctrl & (RTC_CTRL_RTCINT | RTC_CTRL_RTCBAD)) |
				(value & (RTC_CTRL_RTCOUTEN | RTC_CTRL_CLK32KEN | RTC_CTRL_PU32K | RTC_CTRL_CLK_SEL));
			if (value & RTC_CTRL_CLR_RTCINT)
				p->ctrl &= ~RTC_CTRL_RTCINT;
			if (value & RTC_CTRL_CLR_RTCBAD)
				p->ctrl &= ~RTC_CTRL_RTCBAD;
			break;
		
		case RTC_CON:
			p->con = value & (RTC_CON_RUN | RTC_CON_PRE | RTC_CON_T14DEC | RTC_CON_T14INC);
			break;
		
		case RTC_T14:
			p->t14 = value;
			break;
		
		case RTC_CNT:
			p->cnt = value;
			break;
		
		case RTC_REL:
			p->rel = value;
			break;
		
		case RTC_ISNC:
			p->isnc = (value & RTC_ISNC_ENABLES) | (p->isnc & value & RTC_ISNC_REQUESTS);
			break;
		
		case RTC_ALARM:
			p->alarm = value;
			break;

		case RTC_ISNRC:
			p->isnc &= ~((value & RTC_ISNRC_REQUESTS) << 1);
			break;
		
		case RTC_SRC:
			pmb887x_src_set(&p->src, value);
			break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}

	rtc_sync(p);
}

static const MemoryRegionOps io_ops = {
	.read			= rtc_io_read,
	.write			= rtc_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void rtc_init(Object *obj) {
	pmb887x_rtc_t *p = PMB887X_RTC(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-rtc", RTC_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq);
}

static void rtc_reset(DeviceState *dev) {
	pmb887x_rtc_t *p = PMB887X_RTC(dev);

	timer_del(p->timer);

	pmb887x_clc_init(&p->clc);
	pmb887x_src_reset(&p->src);

	p->ctrl = 0;
	p->con = RTC_CON_RUN | RTC_CON_PRE;

	uint32_t t14_start = (UINT16_MAX + 1) - rtc_get_freq(p);
	p->t14 = ((t14_start << RTC_T14_CNT_SHIFT) | (t14_start << RTC_T14_REL_SHIFT));
	p->cnt = qemu_clock_get_ns(QEMU_CLOCK_HOST) / NANOSECONDS_PER_SECOND;
	p->rel = 0;
	p->isnc = 0;
	p->alarm = 0xFFFFFFFF;

	rtc_sync(p);
}

static void rtc_realize(DeviceState *dev, Error **errp) {
	pmb887x_rtc_t *p = PMB887X_RTC(dev);
	
	if (!p->pll)
		hw_error("PLL not found...");
	if (!p->irq)
		hw_error("pmb887x-rtc: irq not set");
	
	pmb887x_clc_init(&p->clc);
	pmb887x_src_init(&p->src, p->irq);
	p->timer = timer_new_ns(QEMU_CLOCK_REALTIME, rtc_ptimer_reset, p);
	p->con = RTC_CON_RUN | RTC_CON_PRE;
	uint32_t t14_start = (UINT16_MAX + 1) - rtc_get_freq(p);
	p->t14 = ((t14_start << RTC_T14_CNT_SHIFT) | (t14_start << RTC_T14_REL_SHIFT));
	p->cnt = qemu_clock_get_ns(QEMU_CLOCK_HOST) / NANOSECONDS_PER_SECOND;
	p->alarm = 0xFFFFFFFF;
	rtc_sync(p);
}

static const Property rtc_properties[] = {
	DEFINE_PROP_LINK("pll", pmb887x_rtc_t, pll, "pmb887x-pll", pmb887x_pll_t *),
};

static void rtc_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, rtc_properties);
	device_class_set_legacy_reset(dc, rtc_reset);
	dc->realize = rtc_realize;
}

static const TypeInfo rtc_info = {
    .name          	= TYPE_PMB887X_RTC,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_rtc_t),
    .instance_init 	= rtc_init,
    .class_init    	= rtc_class_init,
};

static void rtc_register_types(void) {
	type_register_static(&rtc_info);
}
type_init(rtc_register_types)
