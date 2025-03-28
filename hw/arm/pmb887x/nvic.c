/*
 * NVIC
 * */
#define PMB887X_TRACE_ID		NVIC
#define PMB887X_TRACE_PREFIX	"pmb887x-nvic"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"

#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_NVIC	"pmb887x-nvic"
#define PMB887X_NVIC(obj)	OBJECT_CHECK(pmb887x_nvic_t, (obj), TYPE_PMB887X_NVIC)
#define IRQS_COUNT			((NVIC_CON169 - NVIC_CON0) / 4 + 1)

typedef struct pmb887x_nvic_irq_t pmb887x_nvic_irq_t;
typedef struct pmb887x_nvic_t pmb887x_nvic_t;

struct pmb887x_nvic_irq_t {
	uint8_t id;
	bool fiq;
	uint8_t priority;
	uint8_t level;
	bool bridge;
};

struct pmb887x_nvic_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	pmb887x_nvic_irq_t irq_state[IRQS_COUNT];
	
	qemu_irq parent_irq;
	qemu_irq parent_fiq;
	
	int current_irq;
	int current_fiq;
	
	bool irq_lock;
	bool fiq_lock;
};

static uint32_t nvic_get_priority(pmb887x_nvic_irq_t *line) {
	if (!line->level)
		return 0;
	return (line->priority << 16) | (line->level << 8) | ((IRQS_COUNT - line->id));
}

static int nvic_current_irq(pmb887x_nvic_t *p, bool fiq) {
	int irq_n = 0;
	uint32_t max_priority = 0;
	
	for (int i = 0; i < IRQS_COUNT; ++i) {
		pmb887x_nvic_irq_t *line = &p->irq_state[i];
		
		if (fiq != line->fiq || !line->level)
			continue;
		
		uint32_t priority = nvic_get_priority(line);
		if (max_priority < priority) {
			irq_n = i;
			max_priority = priority;
		}
	}
	
	return irq_n;
}

static void nvic_update_state(pmb887x_nvic_t *p) {
	if (!p->irq_lock) {
		p->current_irq = nvic_current_irq(p, false);
		qemu_set_irq(p->parent_irq, p->current_irq > 0);
	}
	
	if (!p->fiq_lock) {
		p->current_fiq = nvic_current_irq(p, true);
		qemu_set_irq(p->parent_fiq, p->current_fiq > 0);
	}
}

static void nvic_irq_handler(void *opaque, int irq, int level) {
	pmb887x_nvic_t *p = (pmb887x_nvic_t *) opaque;
	
	#if PMB887X_IO_BRIDGE
	if (level == 100000) {
		p->irq_state[irq].bridge = true;
		level = 1;
	}
	#endif
	
	p->irq_state[irq].level = level;
	nvic_update_state(p);
}

static uint64_t nvic_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_nvic_t *p = opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case NVIC_ID:
			value = 0x0031C011;
			break;
		
		case NVIC_FIQ_STAT:
			if (p->current_fiq > 0) {
				if (!p->fiq_lock) {
					value |= NVIC_FIQ_STAT_UNREAD;
					value |= p->current_fiq;
				} else {
					value |= NVIC_FIQ_STAT_NOT_ACK;
				}
			} else {
				value = 0;
			}
			break;
		
		case NVIC_IRQ_STAT:
			if (p->current_irq > 0) {
				if (!p->irq_lock) {
					value |= NVIC_IRQ_STAT_UNREAD;
					value |= p->current_irq;
				} else {
					value |= NVIC_IRQ_STAT_NOT_ACK;
				}
			} else {
				value = 0;
			}
			break;
		
		case NVIC_CURRENT_IRQ:
			if (p->current_irq > 0 && !p->irq_lock) {
				value = p->current_irq;
				p->irq_lock = true;
				qemu_set_irq(p->parent_irq, 0);
			} else {
				value = 0;
			}
			break;
		
		case NVIC_CURRENT_FIQ:
			if (p->current_fiq > 0 && !p->fiq_lock) {
				value = p->current_fiq;
				p->fiq_lock = true;
				qemu_set_irq(p->parent_fiq, 0);
			} else {
				value = 0;
			}
			break;
		
		case NVIC_CON0 ... NVIC_CON169:
		{
			uint32_t irq_n = (haddr - NVIC_CON0) / 4;
			value = p->irq_state[irq_n].priority << NVIC_CON_PRIORITY_SHIFT;
			if (p->irq_state[irq_n].fiq)
				value |= NVIC_CON_FIQ;
			break;
		}
		
		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void nvic_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_nvic_t *p = opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
		
		case NVIC_IRQ_ACK:
			#if PMB887X_IO_BRIDGE
			if (p->current_irq > 0 && p->irq_state[p->current_irq].bridge) {
				p->irq_state[p->current_irq].level = 0;
				pmb8876_io_bridge_write(haddr + p->mmio.addr, size, value);
			}
			#endif
			
			if (value)
				p->irq_lock = false;
		break;
		
		case NVIC_FIQ_ACK:
			if (value)
				p->fiq_lock = false;
		break;
		
		case NVIC_CON0 ... NVIC_CON169:
		{
			uint32_t irq_n = (haddr - NVIC_CON0) / 4;
			p->irq_state[irq_n].fiq = (value & NVIC_CON_FIQ) != 0;
			p->irq_state[irq_n].priority = (value & NVIC_CON_PRIORITY) >> NVIC_CON_PRIORITY_SHIFT;
			
			#if PMB887X_IO_BRIDGE
			if (irq_n != 22 && irq_n != 23 && irq_n != 24) {
				pmb8876_io_bridge_write(haddr + p->mmio.addr, size, value);
			}
			#endif
		}
		break;
		
		case NVIC_IRQ_STAT:
			// Ignore write...
			break;
	}
	
	nvic_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= nvic_io_read,
	.write			= nvic_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void nvic_init(Object *obj) {
	pmb887x_nvic_t *p = PMB887X_NVIC(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-nvic", NVIC_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (int i = 0; i < ARRAY_SIZE(p->irq_state); i++)
		p->irq_state[i].id = i;
	
	DPRINTF("irq count: %d\n", IRQS_COUNT);
	
	qdev_init_gpio_in(DEVICE(obj), nvic_irq_handler, IRQS_COUNT);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->parent_irq);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->parent_fiq);
}

static void nvic_realize(DeviceState *dev, Error **errp) {
	pmb887x_nvic_t *p = PMB887X_NVIC(dev);
	nvic_update_state(p);
}

static void nvic_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	dc->realize = nvic_realize;
}

static const TypeInfo nvic_info = {
    .name          	= TYPE_PMB887X_NVIC,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_nvic_t),
    .instance_init 	= nvic_init,
    .class_init    	= nvic_class_init,
};

static void nvic_register_types(void) {
	type_register_static(&nvic_info);
}
type_init(nvic_register_types)
