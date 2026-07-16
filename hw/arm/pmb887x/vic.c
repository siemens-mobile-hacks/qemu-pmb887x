/*
 * VIC
 * */
#define PMB887X_TRACE_ID		VIC
#define PMB887X_TRACE_PREFIX	"pmb887x-vic"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_VIC	"pmb887x-vic"
#define PMB887X_VIC(obj)	OBJECT_CHECK(pmb887x_vic_t, (obj), TYPE_PMB887X_VIC)
#define IRQS_COUNT			((VIC_CON169 - VIC_CON0) / 4 + 1)
#define VIC_FRAME_DEPTH		15

typedef struct pmb887x_vic_irq_t pmb887x_vic_irq_t;
typedef struct pmb887x_vic_frame_t pmb887x_vic_frame_t;
typedef struct pmb887x_vic_t pmb887x_vic_t;

struct pmb887x_vic_irq_t {
	uint8_t id;
	bool fiq;
	uint8_t priority;
	uint8_t level;
	bool bridge;
};

struct pmb887x_vic_frame_t {
	uint8_t irq;
	uint8_t priority;
};

struct pmb887x_vic_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	pmb887x_vic_irq_t irq_state[IRQS_COUNT];
	
	uint32_t fiq_con;
	uint32_t irq_con;

	qemu_irq parent_irq;
	qemu_irq parent_fiq;
	
	int pending_irq;
	int pending_fiq;

	pmb887x_vic_frame_t irq_frames[VIC_FRAME_DEPTH];
	pmb887x_vic_frame_t fiq_frames[VIC_FRAME_DEPTH];
	uint8_t irq_depth;
	uint8_t fiq_depth;
};

static uint32_t vic_get_nested_priority(pmb887x_vic_irq_t *line) {
	if (!line->level)
		return 0;
	return (line->priority << 16) | (IRQS_COUNT - line->id);
}

static uint32_t vic_get_irq_mask_priority(pmb887x_vic_t *p) {
	if (p->irq_depth)
		return p->irq_frames[p->irq_depth - 1].priority;
	return (p->irq_con & VIC_IRQ_CON_MASK_PRIORITY) >> VIC_IRQ_CON_MASK_PRIORITY_SHIFT;
}

static uint32_t vic_get_fiq_mask_priority(pmb887x_vic_t *p) {
	if (p->fiq_depth)
		return p->fiq_frames[p->fiq_depth - 1].priority;
	return (p->fiq_con & VIC_FIQ_CON_MASK_PRIORITY) >> VIC_FIQ_CON_MASK_PRIORITY_SHIFT;
}

static int vic_pending_irq(pmb887x_vic_t *p) {
	int irq_n = -1;
	uint32_t max_priority = 0;
	uint32_t mask_priority = vic_get_irq_mask_priority(p);

	for (int i = 0; i < IRQS_COUNT; ++i) {
		pmb887x_vic_irq_t *line = &p->irq_state[i];
		
		if (line->fiq || !line->level || line->priority <= mask_priority)
			continue;
		
		uint32_t priority = vic_get_nested_priority(line);
		if (max_priority < priority) {
			irq_n = i;
			max_priority = priority;
		}
	}
	
	return irq_n;
}

static int vic_pending_fiq(pmb887x_vic_t *p) {
	int irq_n = -1;
	uint32_t max_priority = 0;
	uint32_t mask_priority = vic_get_fiq_mask_priority(p);

	for (int i = 0; i < IRQS_COUNT; ++i) {
		pmb887x_vic_irq_t *line = &p->irq_state[i];

		if (!line->fiq || !line->level || line->priority <= mask_priority)
			continue;

		uint32_t priority = vic_get_nested_priority(line);
		if (max_priority < priority) {
			irq_n = i;
			max_priority = priority;
		}
	}
	
	return irq_n;
}

static void vic_update_state(pmb887x_vic_t *p) {
	p->pending_irq = vic_pending_irq(p);
	p->pending_fiq = vic_pending_fiq(p);
	qemu_set_irq(p->parent_irq, p->pending_irq >= 0);
	qemu_set_irq(p->parent_fiq, p->pending_fiq >= 0);
}

static void vic_irq_handler(void *opaque, int irq, int level) {
	pmb887x_vic_t *p = (pmb887x_vic_t *) opaque;
	
	#if PMB887X_IO_BRIDGE
	if (level == 100000) {
		p->irq_state[irq].bridge = true;
		level = 1;
	}
	#endif
	
	p->irq_state[irq].level = level;
	vic_update_state(p);
}

static int vic_current_irq(pmb887x_vic_t *p) {
	int irq = p->pending_irq;

	if (irq < 0 || p->irq_depth >= VIC_FRAME_DEPTH)
		return -1;

	p->irq_frames[p->irq_depth].irq = irq;
	p->irq_frames[p->irq_depth].priority = p->irq_state[irq].priority;
	p->irq_depth++;
	vic_update_state(p);
	return irq;
}

static int vic_current_fiq(pmb887x_vic_t *p) {
	int irq = p->pending_fiq;

	if (irq < 0 || p->fiq_depth >= VIC_FRAME_DEPTH)
		return -1;

	p->fiq_frames[p->fiq_depth].irq = irq;
	p->fiq_frames[p->fiq_depth].priority = p->irq_state[irq].priority;
	p->fiq_depth++;
	vic_update_state(p);
	return irq;
}

static void vic_ack_irq(pmb887x_vic_t *p) {
	if (p->irq_depth)
		p->irq_depth--;
	if (!p->irq_depth)
		p->irq_con &= ~VIC_IRQ_CON_MASK_PRIORITY;
	vic_update_state(p);
}

static void vic_ack_fiq(pmb887x_vic_t *p) {
	if (p->fiq_depth)
		p->fiq_depth--;
	if (!p->fiq_depth)
		p->fiq_con &= ~VIC_FIQ_CON_MASK_PRIORITY;
	vic_update_state(p);
}

static uint64_t vic_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_vic_t *p = opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case VIC_ID:
			value = 0x0031C011;
			break;
		
		case VIC_FIQ_CON:
			value = vic_get_fiq_mask_priority(p) << VIC_FIQ_CON_MASK_PRIORITY_SHIFT;
			if (p->pending_fiq >= 0) {
				value |= p->irq_state[p->pending_fiq].priority << VIC_FIQ_CON_PRIORITY_SHIFT;
				value |= p->pending_fiq;
			}
			break;
		
		case VIC_IRQ_CON:
			value = vic_get_irq_mask_priority(p) << VIC_IRQ_CON_MASK_PRIORITY_SHIFT;
			if (p->pending_irq >= 0) {
				value |= p->irq_state[p->pending_irq].priority << VIC_IRQ_CON_PRIORITY_SHIFT;
				value |= p->pending_irq;
			}
			break;
		
		case VIC_IRQ_CURRENT: {
			int irq = vic_current_irq(p);
			value = irq >= 0 ? irq : 0;
			break;
		}
		
		case VIC_FIQ_CURRENT: {
			int irq = vic_current_fiq(p);
			value = irq >= 0 ? irq : 0;
			break;
		}
		
		case VIC_CON0 ... VIC_CON169:
		{
			uint32_t irq_n = (haddr - VIC_CON0) / 4;
			value = p->irq_state[irq_n].priority << VIC_CON_PRIORITY_SHIFT;
			if (p->irq_state[irq_n].fiq)
				value |= VIC_CON_FIQ;
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

static void vic_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_vic_t *p = opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case VIC_IRQ_ACK:
			#if PMB887X_IO_BRIDGE
			if (p->irq_depth && p->irq_state[p->irq_frames[p->irq_depth - 1].irq].bridge) {
				p->irq_state[p->irq_frames[p->irq_depth - 1].irq].level = 0;
				pmb8876_io_bridge_write(haddr + p->mmio.addr, size, value);
			}
			#endif
			
			vic_ack_irq(p);
		break;
		
		case VIC_FIQ_ACK:
			vic_ack_fiq(p);
		break;
		
		case VIC_CON0 ... VIC_CON169:
		{
			uint32_t irq_n = (haddr - VIC_CON0) / 4;
			p->irq_state[irq_n].fiq = (value & VIC_CON_FIQ) != 0;
			p->irq_state[irq_n].priority = (value & VIC_CON_PRIORITY) >> VIC_CON_PRIORITY_SHIFT;
			
			#if PMB887X_IO_BRIDGE
			if (irq_n != 22 && irq_n != 23 && irq_n != 24) {
				pmb8876_io_bridge_write(haddr + p->mmio.addr, size, value);
			}
			#endif
		}
		break;
		
		case VIC_IRQ_CON:
			p->irq_con = value & VIC_IRQ_CON_MASK_PRIORITY;
			break;

		case VIC_FIQ_CON:
			p->fiq_con = value & VIC_FIQ_CON_MASK_PRIORITY;
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	vic_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= vic_io_read,
	.write			= vic_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void vic_init(Object *obj) {
	pmb887x_vic_t *p = PMB887X_VIC(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-vic", VIC_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	p->pending_fiq = -1;
	p->pending_irq = -1;

	for (int i = 0; i < ARRAY_SIZE(p->irq_state); i++)
		p->irq_state[i].id = i;
	
	DPRINTF("irq count: %d\n", IRQS_COUNT);
	
	qdev_init_gpio_in(DEVICE(obj), vic_irq_handler, IRQS_COUNT);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->parent_irq);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->parent_fiq);
}

static void vic_reset(DeviceState *dev) {
	pmb887x_vic_t *p = PMB887X_VIC(dev);

	for (int i = 0; i < IRQS_COUNT; i++) {
		p->irq_state[i].id = i;
		p->irq_state[i].fiq = false;
		p->irq_state[i].priority = 0;
		p->irq_state[i].level = 0;
		p->irq_state[i].bridge = false;
	}

	p->fiq_con = 0;
	p->irq_con = 0;

	p->pending_irq = -1;
	p->pending_fiq = -1;
	p->irq_depth = 0;
	p->fiq_depth = 0;
	memset(p->irq_frames, 0, sizeof(p->irq_frames));
	memset(p->fiq_frames, 0, sizeof(p->fiq_frames));

	vic_update_state(p);
}

static void vic_realize(DeviceState *dev, Error **errp) {
	pmb887x_vic_t *p = PMB887X_VIC(dev);
	vic_update_state(p);
}

static void vic_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_legacy_reset(dc, vic_reset);
	dc->realize = vic_realize;
}

static const TypeInfo vic_info = {
    .name          	= TYPE_PMB887X_VIC,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_vic_t),
    .instance_init 	= vic_init,
    .class_init    	= vic_class_init,
};

static void vic_register_types(void) {
	type_register_static(&vic_info);
}
type_init(vic_register_types);
