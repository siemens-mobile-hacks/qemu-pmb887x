/*
 * PLL
 * */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/pll.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "hw/qdev-properties.h"
#include "cpu.h"

#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"

#define PLL_DEBUG

#ifdef PLL_DEBUG
#define DPRINTF(fmt, ...) do { fprintf(stderr, "[pmb887x-pll]: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#define TYPE_PMB887X_PLL	"pmb887x-pll"
#define PMB887X_PLL(obj)	OBJECT_CHECK(struct pmb887x_pll_t, (obj), TYPE_PMB887X_PLL)

struct pmb887x_pll_callback_t {
	void *opaque;
	void (*callback)(void *);
};

struct pmb887x_pll_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	struct pmb887x_pll_callback_t *callbacks;
	int callbacks_count;
	
	struct pmb887x_src_reg_t src;
	qemu_irq irq;
	
	uint32_t xtal;
	uint32_t fsys;
	
	uint32_t osc;
	uint32_t con0;
	uint32_t con1;
	uint32_t con2;
	uint32_t stat;
	uint32_t con3;
};

static void pll_update_state(struct pmb887x_pll_t *p) {
	uint32_t new_fsys = p->xtal;
	uint32_t div = (p->con1 & PLL_CON1_FSYS_DIV) >> PLL_CON1_FSYS_DIV_SHIFT;
	
	if ((p->con1 & PLL_CON1_FSYS_DIV_EN))
		new_fsys = p->xtal / 4 / (div + 1);
	
	if (p->fsys != new_fsys) {
		DPRINTF("fSYS=%d\n", p->fsys);
		
		p->fsys = new_fsys;
		
		for (int i = 0; i < p->callbacks_count; ++i)
			p->callbacks[i].callback(p->callbacks[i].opaque);
	}
}

static uint64_t pll_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb887x_pll_t *p = (struct pmb887x_pll_t *) opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case PLL_OSC:
			value = p->osc;
		break;
		
		case PLL_CON0:
			value = p->con0;
		break;
		
		case PLL_CON1:
			value = p->con1;
		break;
		
		case PLL_CON2:
			value = p->con2;
		break;
		
		case PLL_STAT:
			value = p->stat;
		break;
		
		case PLL_CON3:
			value = p->con3;
		break;
		
		case PLL_SRC:
			value = pmb887x_src_get(&p->src);
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

static void pll_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct pmb887x_pll_t *p = (struct pmb887x_pll_t *) opaque;
	
	pmb887x_dump_io(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case PLL_OSC:
			p->osc = value;
		break;
		
		case PLL_CON0:
			p->con0 = value;
		break;
		
		case PLL_CON1:
			p->con1 = value;
		break;
		
		case PLL_CON2:
			p->con2 = value;
		break;
		
		case PLL_STAT:
			p->stat = value;
		break;
		
		case PLL_CON3:
			p->con3 = value;
		break;
		
		case PLL_SRC:
			pmb887x_src_set(&p->src, value);
		break;
		
		default:
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	pll_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= pll_io_read,
	.write			= pll_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 4,
		.max_access_size	= 4
	}
};

uint32_t pmb887x_pll_get_fsys(struct pmb887x_pll_t *p) {
	return p->fsys;
}

void pmb887x_pll_add_freq_update_callback(struct pmb887x_pll_t *p, void (*callback)(void *), void *opaque) {
	p->callbacks = g_realloc(p->callbacks, (p->callbacks_count + 1) * sizeof(struct pmb887x_pll_callback_t));
	p->callbacks[p->callbacks_count].opaque = opaque;
	p->callbacks[p->callbacks_count].callback = callback;
	p->callbacks_count++;
}

static void pll_init(Object *obj) {
	struct pmb887x_pll_t *p = PMB887X_PLL(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-pll", PLL_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq);
}

static void pll_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_pll_t *p = PMB887X_PLL(dev);
	
	if (!p->irq) {
		error_report("pmb887x-pll: irq not set");
		abort();
	}
	
	pmb887x_src_init(&p->src, p->irq);
	pmb887x_src_set(&p->src, MOD_SRC_SRE);
	
	p->fsys = p->xtal;
	
	p->callbacks = NULL;
	p->callbacks_count = 0;
	
	// Initial values
	p->osc	= 0x01070001;
	p->con0	= 0x22000012;
	p->con1	= 0x00000000;
	p->con2	= 0x00000000;
	p->stat	= 0x00002000;
	p->con3	= 0x00000000;
}

static Property pll_properties[] = {
	DEFINE_PROP_UINT32("xtal", struct pmb887x_pll_t, xtal, 26000000),
    DEFINE_PROP_END_OF_LIST(),
};

static void pll_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, pll_properties);
	dc->realize = pll_realize;
}

static const TypeInfo pll_info = {
    .name          	= TYPE_PMB887X_PLL,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_pll_t),
    .instance_init 	= pll_init,
    .class_init    	= pll_class_init,
};

static void pll_register_types(void) {
	type_register_static(&pll_info);
}
type_init(pll_register_types)
