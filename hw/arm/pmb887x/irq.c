/*
 * PMB8876 Interrupt Controller
 * */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/ptimer.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"

#define TYPE_PMB8876_INTC	"pmb8876-intc"

#define PMB8876_INTC_ERR(s, ...) fprintf(stderr, "[pmb8876-intc] " s "\n", ##__VA_ARGS__);
#define PMB8876_INTC_DBG(s, ...) fprintf(stderr, "[pmb8876-intc] " s "\n", ##__VA_ARGS__);

#define PMB8876_INTC(obj) OBJECT_CHECK(struct pmb8876_intc, (obj), TYPE_PMB8876_INTC)

struct pmb8876_intc_regs {
	PMB8876_IRQ_ID_TypeDef				ID;
	PMB8876_FIQ_ACK_TypeDef				FIQ_ACK;
	PMB8876_IRQ_ACK_TypeDef				IRQ_ACK;
	PMB8876_FIQ_CURRENT_NUM_TypeDef		FIQ_CURRENT_NUM;
	PMB8876_IRQ_CURRENT_NUM_TypeDef		IRQ_CURRENT_NUM;
	PMB8876_IRQx_TypeDef				IRQS[PMB8876_IRQ__NR];
};

struct pmb8876_intc {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	struct pmb8876_intc_regs regs;
	
	qemu_irq parent_irq;
	qemu_irq parent_fiq;
	
	bool irq_state[PMB8876_IRQ__NR];
	
	bool lock_irq;
	bool lock_fiq;
	
	int32_t current_irq;
	int32_t current_fiq;
};

static int32_t pmb8876_intc_current_irq(struct pmb8876_intc *p) {
	int irq_n = -1;
	int max_priority = -1;
	
	for (int i = 0; i < PMB8876_IRQ__NR; ++i) {
		if (p->regs.IRQS[i].b.PRIORITY > 0 && !p->regs.IRQS[i].b.FIQ && p->irq_state[i]) {
			if (max_priority == -1 || max_priority >= p->regs.IRQS[i].b.PRIORITY) {
				irq_n = i;
				max_priority = p->regs.IRQS[i].b.PRIORITY;
			}
		}
	}
	
	return irq_n;
}

static int32_t pmb8876_intc_current_fiq(struct pmb8876_intc *p) {
	int irq_n = -1;
	int max_priority = -1;
	
	for (int i = 0; i < PMB8876_IRQ__NR; ++i) {
		if (p->regs.IRQS[i].b.PRIORITY > 0 && p->regs.IRQS[i].b.FIQ && p->irq_state[i]) {
			if (max_priority == -1 || max_priority >= p->regs.IRQS[i].b.PRIORITY) {
				irq_n = i;
				max_priority = p->regs.IRQS[i].b.PRIORITY;
			}
		}
	}
	
	return irq_n;
}

static void pmb8876_intc_update(struct pmb8876_intc *p) {
	if (!p->lock_irq) {
		p->current_irq = pmb8876_intc_current_irq(p);
		p->regs.IRQ_CURRENT_NUM.b.NUM = p->current_irq < 0 ? 0 : p->current_irq;
		qemu_set_irq(p->parent_irq, p->current_irq != -1);
		p->lock_irq = p->current_irq != -1;
	}
	
	if (!p->lock_fiq) {
		p->current_fiq = pmb8876_intc_current_fiq(p);
		p->regs.FIQ_CURRENT_NUM.b.NUM = p->current_fiq < 0 ? 0 : p->current_fiq;
		qemu_set_irq(p->parent_fiq, p->current_fiq != -1);
		p->lock_fiq = p->current_fiq != -1;
	}
}

static void pmb8876_intc_handler(void *opaque, int irq, int level) {
	struct pmb8876_intc *p = (struct pmb8876_intc *) opaque;
	
	p->irq_state[irq] = level != 0;
	
	#ifdef PMB887X_IO_BRIDGE
	int has_irq = 0;
	for (int i = 0; i < PMB8876_IRQ__NR; ++i) {
		if (p->irq_state[i]) {
			has_irq++;
		}
	}
	PMB8876_INTC_DBG("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! has_irq=%d [irq=%d, level=%d]", has_irq, irq, level);
	qemu_set_irq(p->parent_irq, has_irq > 0);
	#else
	pmb8876_intc_update(p);
	#endif
}

static uint64_t io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb8876_intc *p = (struct pmb8876_intc *) opaque;
	
	uint64_t ret = 0;
	
	#ifdef PMB887X_IO_BRIDGE
	ret = pmb8876_io_bridge_read(haddr + PMB8876_IRQ__BASE, size, ARM_CPU(qemu_get_cpu(0))->env.regs[15]);
	PMB8876_INTC_DBG("READ %08lX %08lX [%08X]", haddr + PMB8876_IRQ__BASE, ret, ARM_CPU(qemu_get_cpu(0))->env.regs[15]);
	return ret;
	#endif
	
	switch (haddr) {
		case PMB8876_IRQ_ID:
			ret = p->regs.ID.v;
		break;
		
		case PMB8876_IRQ_ACK:
			p->regs.IRQ_ACK.b.ACTIVE = p->lock_irq ? 1 : 0;
			p->regs.IRQ_ACK.b.RESET = p->lock_irq ? 0 : 1;
			ret = p->regs.IRQ_ACK.v;
		break;
		
		case PMB8876_FIQ_ACK:
			p->regs.FIQ_ACK.b.ACTIVE = p->lock_fiq ? 1 : 0;
			p->regs.FIQ_ACK.b.RESET = p->lock_fiq ? 0 : 1;
			ret = p->regs.FIQ_ACK.v;
		break;
		
		case PMB8876_FIQ_CURRENT_NUM:
			ret = p->regs.FIQ_CURRENT_NUM.v;
		break;
		
		case PMB8876_IRQ_CURRENT_NUM:
			ret = p->regs.IRQ_CURRENT_NUM.v;
		break;
		
		case PMB8876_IRQ_TABLE ... (PMB8876_IRQ_TABLE + PMB8876_IRQ__NR * 4):
			ret = p->regs.IRQS[(haddr - PMB8876_IRQ_TABLE) / 4].v;
		break;
		
		default:
			PMB8876_INTC_ERR("read unknown register: %08lX", haddr);
		break;
	}
	
	PMB8876_INTC_DBG("READ %08lX %08lX [%08X]", haddr + PMB8876_IRQ__BASE, ret, ARM_CPU(qemu_get_cpu(0))->env.regs[15]);
	
	return ret;
}

static void io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct pmb8876_intc *p = (struct pmb8876_intc *) opaque;
	
	#ifdef PMB887X_IO_BRIDGE
	PMB8876_INTC_DBG("WRITE %08lX %08lX [%08X]", haddr + PMB8876_IRQ__BASE, value, ARM_CPU(qemu_get_cpu(0))->env.regs[15]);
	pmb8876_io_bridge_write(haddr + PMB8876_IRQ__BASE, size, value, ARM_CPU(qemu_get_cpu(0))->env.regs[15]);
	return;
	#endif
	
	PMB8876_INTC_DBG("WRITE %08lX %08lX [%08X]", haddr + PMB8876_IRQ__BASE, value, ARM_CPU(qemu_get_cpu(0))->env.regs[15]);
	
	switch (haddr) {
		case PMB8876_FIQ_ACK:
			p->lock_fiq = false;
		break;
		
		case PMB8876_IRQ_ACK:
			p->lock_irq = false;
		break;
		
		case PMB8876_IRQ_TABLE ... (PMB8876_IRQ_TABLE + PMB8876_IRQ__NR * 4):
			p->regs.IRQS[(haddr - PMB8876_IRQ_TABLE) / 4].v = value;
		break;
		
		default:
			PMB8876_INTC_ERR("write unknown register: %08lX = %08lX", haddr, value);
		break;
	}
	
	#ifdef PMB887X_IO_BRIDGE
	pmb8876_io_bridge_write(haddr + PMB8876_IRQ__BASE, size, value, ARM_CPU(qemu_get_cpu(0))->env.regs[15]);
	#endif
	
	pmb8876_intc_update(p);
}

static const MemoryRegionOps io_ops = {
	.read			= io_read,
	.write			= io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 4,
		.max_access_size	= 4
	}
};

static void pmb8876_intc_init(Object *obj) {
	struct pmb8876_intc *p = PMB8876_INTC(obj);
	
	p->current_irq = -1;
	p->current_fiq = -1;
	
	qdev_init_gpio_in(DEVICE(obj), pmb8876_intc_handler, PMB8876_IRQ__NR);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->parent_irq);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->parent_fiq);
	
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb8876-intc", PMB8876_IRQ__SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
}

static void pmb8876_intc_realize(DeviceState *dev, Error **errp) {
	struct pmb8876_intc *p = PMB8876_INTC(dev);
	
	// Default values
	p->regs.ID.v = 0x0031C011;
	
	pmb8876_intc_update(p);
}

static Property pmb8876_intc_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void pmb8876_intc_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, pmb8876_intc_properties);
	dc->realize = pmb8876_intc_realize;
}

static const TypeInfo pmb8876_intc_info = {
    .name          	= TYPE_PMB8876_INTC,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb8876_intc),
    .instance_init 	= pmb8876_intc_init,
    .class_init    	= pmb8876_intc_class_init,
};

static void pmb8876_intc_register_types(void) {
	type_register_static(&pmb8876_intc_info);
}
type_init(pmb8876_intc_register_types)
