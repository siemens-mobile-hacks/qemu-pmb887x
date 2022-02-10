/*
 * NVIC
 * */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/ptimer.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"

#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"

#define NVIC_DEBUG

#ifdef NVIC_DEBUG
#define DPRINTF(fmt, ...) do { fprintf(stderr, "[pmb887x-nvic]: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#define TYPE_PMB887X_NVIC	"pmb887x-nvic"
#define PMB887X_NVIC(obj)	OBJECT_CHECK(struct pmb887x_nvic_t, (obj), TYPE_PMB887X_NVIC)
#define IRQS_COUNT			((NVIC_CON169 - NVIC_CON0) / 4 + 1)

struct pmb887x_nvic_irq_t {
	bool fiq;
	uint8_t priority;
	uint8_t level;
};

struct pmb887x_nvic_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	struct pmb887x_nvic_irq_t irq_state[IRQS_COUNT];
	
	qemu_irq parent_irq;
	qemu_irq parent_fiq;
	
	int current_irq;
	int current_fiq;
	
	bool lock_irq;
	bool lock_fiq;
};

static int32_t nvic_current_irq(struct pmb887x_nvic_t *p, bool fiq) {
	int irq_n = -1;
	uint32_t max_priority = 0;
	
	for (int i = 0; i < IRQS_COUNT; ++i) {
		struct pmb887x_nvic_irq_t *line = &p->irq_state[i];
		
		if (fiq != line->fiq)
			continue;
		
		uint32_t level = line->level ? (line->priority << 8) | line->level : 0;
		if (level && (irq_n == -1 || max_priority >= level)) {
			irq_n = i;
			max_priority = level;
		}
	}
	
	return irq_n;
}

static void nvic_update_state(struct pmb887x_nvic_t *p) {
	if (!p->lock_irq) {
		p->current_irq = nvic_current_irq(p, false);
		qemu_set_irq(p->parent_irq, p->current_irq != -1);
		p->lock_irq = p->current_irq != -1;
	}
	
	if (!p->lock_fiq) {
		p->current_fiq = nvic_current_irq(p, true);
		qemu_set_irq(p->parent_fiq, p->current_fiq != -1);
		p->lock_fiq = p->current_fiq != -1;
	}
}

static void nvic_irq_handler(void *opaque, int irq, int level) {
	struct pmb887x_nvic_t *p = (struct pmb887x_nvic_t *) opaque;
	p->irq_state[irq].level = level;
	nvic_update_state(p);
}

static uint64_t nvic_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb887x_nvic_t *p = (struct pmb887x_nvic_t *) opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case NVIC_ID:
			value = 0x0031C011;
		break;
		
		case NVIC_CURRENT_IRQ:
			value = p->current_irq != -1 ? p->current_irq : 0;
		break;
		
		case NVIC_CURRENT_FIQ:
			value = p->current_fiq != -1 ? p->current_fiq : 0;
		break;
		
		case NVIC_CON0 ... NVIC_CON169:
		{
			int irq_n = (haddr - NVIC_CON0) / 4;
			
			value = p->irq_state[irq_n].priority << NVIC_CON_PRIORITY_SHIFT;
			
			if (p->irq_state[irq_n].fiq)
				value |= NVIC_CON_FIQ;
		}
		break;
		
		default:
			pmb887x_dump_io(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	pmb887x_dump_io(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void nvic_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct pmb887x_nvic_t *p = (struct pmb887x_nvic_t *) opaque;
	
	pmb887x_dump_io(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		default:
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
		
		case NVIC_IRQ_ACK:
			if (value)
				p->lock_irq = false;
		break;
		
		case NVIC_FIQ_ACK:
			if (value)
				p->lock_fiq = false;
		break;
		
		case NVIC_CON0 ... NVIC_CON169:
		{
			int irq_n = (haddr - NVIC_CON0) / 4;
			p->irq_state[irq_n].fiq = (value & NVIC_CON_FIQ) != 0;
			p->irq_state[irq_n].priority = (value & NVIC_CON_PRIORITY) >> NVIC_CON_PRIORITY_SHIFT;
		}
		break;
	}
	
	nvic_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= nvic_io_read,
	.write			= nvic_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 4,
		.max_access_size	= 4
	}
};

static void nvic_init(Object *obj) {
	struct pmb887x_nvic_t *p = PMB887X_NVIC(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-nvic", NVIC_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	p->current_irq = -1;
	p->current_fiq = -1;
	
	DPRINTF("irq count: %d\n", IRQS_COUNT);
	
	qdev_init_gpio_in(DEVICE(obj), nvic_irq_handler, IRQS_COUNT);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->parent_irq);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->parent_fiq);
}

static void nvic_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_nvic_t *p = PMB887X_NVIC(dev);
	
	nvic_update_state(p);
}

static Property nvic_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void nvic_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, nvic_properties);
	dc->realize = nvic_realize;
}

static const TypeInfo nvic_info = {
    .name          	= TYPE_PMB887X_NVIC,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_nvic_t),
    .instance_init 	= nvic_init,
    .class_init    	= nvic_class_init,
};

static void nvic_register_types(void) {
	type_register_static(&nvic_info);
}
type_init(nvic_register_types)
