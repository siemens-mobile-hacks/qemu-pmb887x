/*
 * RTC
 * */
#define PMB887X_TRACE_ID		RTC
#define PMB887X_TRACE_PREFIX	"pmb887x-rtc"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"

#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_RTC	"pmb887x-rtc"
#define PMB887X_RTC(obj)	OBJECT_CHECK(pmb887x_rtc_t, (obj), TYPE_PMB887X_RTC)

typedef struct pmb887x_rtc_t pmb887x_rtc_t;

struct pmb887x_rtc_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	pmb887x_clc_reg_t clc;
	pmb887x_src_reg_t src;
	qemu_irq irq;
	
	uint32_t ctrl;
	uint32_t con;
	uint32_t t14;
	uint32_t cnt;
	uint32_t rel;
	uint32_t isnc;
	uint32_t alarm;
	uint32_t unk0;
	
	uint64_t realtime_start;
	uint64_t virtual_start;
};

static uint64_t rtc_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_rtc_t *p = opaque;
	
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
			value = get_clock_realtime() / NANOSECONDS_PER_SECOND;
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
		
		case RTC_UNK0:
			value = p->unk0;
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
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case RTC_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;
		
		case RTC_CTRL:
			p->ctrl = value;
			break;
		
		case RTC_CON:
			p->con = value;
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
			p->isnc = value;
			break;
		
		case RTC_ALARM:
			p->alarm = value;
			break;
		
		case RTC_UNK0:
			p->unk0 = value;
			break;
		
		case RTC_SRC:
			pmb887x_src_set(&p->src, value);
			break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
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

static void rtc_realize(DeviceState *dev, Error **errp) {
	pmb887x_rtc_t *p = PMB887X_RTC(dev);
	
	if (!p->irq)
		hw_error("pmb887x-rtc: irq not set");
	
	p->virtual_start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	p->realtime_start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
	
	pmb887x_clc_init(&p->clc);
	pmb887x_src_init(&p->src, p->irq);
}

static void rtc_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
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
