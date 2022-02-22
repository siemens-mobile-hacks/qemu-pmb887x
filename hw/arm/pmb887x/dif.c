/*
 * Display Interface
 * */
#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/qdev-properties.h"

#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"

#define DIF_DEBUG

#ifdef DIF_DEBUG
#define DPRINTF(fmt, ...) do { fprintf(stderr, "[pmb887x-dif]: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#define TYPE_PMB887X_DIF	"pmb887x-dif"
#define PMB887X_DIF(obj)	OBJECT_CHECK(struct pmb887x_dif_t, (obj), TYPE_PMB887X_DIF)

struct pmb887x_dif_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
    QemuConsole *console;
	
	qemu_irq irq[4];
	
	uint32_t runctrl;
	uint32_t prog[6];
	uint32_t con[16];
	
	pmb887x_clc_reg_t clc;
	pmb887x_srb_reg_t srb;
};

static void dif_update_state(struct pmb887x_dif_t *p) {
	// TODO
}

static void dif_update_display(void *opaque) {
	struct pmb887x_dif_t *p = (struct pmb887x_dif_t *) opaque;
	// TODO
}

static void dif_invalidate_display(void * opaque) {
	struct pmb887x_dif_t *p = (struct pmb887x_dif_t *) opaque;
	// TODO
}

static int dif_get_index_from_reg(uint32_t reg) {
	switch (reg) {
		case DIF_PROG0:		return 0;
		case DIF_PROG1:		return 1;
		case DIF_PROG2:		return 2;
		case DIF_PROG3:		return 3;
		case DIF_PROG4:		return 4;
		case DIF_PROG5:		return 5;
		
		case DIF_CON0:		return 0;
		case DIF_CON1:		return 1;
		case DIF_CON2:		return 2;
		case DIF_CON3:		return 3;
		case DIF_CON4:		return 4;
		case DIF_CON5:		return 5;
		case DIF_CON6:		return 6;
		case DIF_CON7:		return 7;
		case DIF_CON8:		return 8;
		case DIF_CON9:		return 9;
		case DIF_CON10:		return 10;
		case DIF_CON11:		return 11;
		case DIF_CON12:		return 12;
		case DIF_CON13:		return 13;
		case DIF_CON14:		return 14;
		case DIF_CON15:		return 15;
	};
	error_report("pmb887x-dif: unknown reg %d", reg);
	abort();
	return -1;
}

static uint64_t dif_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb887x_dif_t *p = (struct pmb887x_dif_t *) opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case DIF_CLC:
			value = pmb887x_clc_get(&p->clc);
		break;
		
		case DIF_ID:
			value = 0xF043C012;
		break;
		
		case DIF_RUNCTRL:
			value = p->runctrl;
		break;
		
		case DIF_STAT:
			value = 0;
		break;
		
		case DIF_PROG0:
		case DIF_PROG1:
		case DIF_PROG2:
		case DIF_PROG3:
		case DIF_PROG4:
		case DIF_PROG5:
			value = p->prog[dif_get_index_from_reg(haddr)];
		break;
		
		case DIF_CON0:
		case DIF_CON1:
		case DIF_CON2:
		case DIF_CON3:
		case DIF_CON4:
		case DIF_CON5:
		case DIF_CON6:
		case DIF_CON7:
		case DIF_CON8:
		case DIF_CON9:
		case DIF_CON10:
		case DIF_CON11:
		case DIF_CON12:
		case DIF_CON13:
		case DIF_CON14:
		case DIF_CON15:
			value = p->con[dif_get_index_from_reg(haddr)];
		break;
		
		case DIF_TXD:
			value = 0;
		break;
		
		case DIF_IMSC:
			value = pmb887x_srb_get_imsc(&p->srb);
		break;
		
		case DIF_RIS:
			value = pmb887x_srb_get_ris(&p->srb);
		break;
		
		case DIF_MIS:
			value = pmb887x_srb_get_mis(&p->srb);
		break;
		
		case DIF_ICR:
			value = 0;
		break;
		
		case DIF_ISR:
			value = 0;
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

static void dif_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct pmb887x_dif_t *p = (struct pmb887x_dif_t *) opaque;
	
	pmb887x_dump_io(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case DIF_CLC:
			pmb887x_clc_set(&p->clc, value);
		break;
		
		case DIF_RUNCTRL:
			p->runctrl = value;
		break;
		
		case DIF_PROG0:
		case DIF_PROG1:
		case DIF_PROG2:
		case DIF_PROG3:
		case DIF_PROG4:
		case DIF_PROG5:
			p->prog[dif_get_index_from_reg(haddr)] = value;
		break;
		
		case DIF_CON0:
		case DIF_CON1:
		case DIF_CON2:
		case DIF_CON3:
		case DIF_CON4:
		case DIF_CON5:
		case DIF_CON6:
		case DIF_CON7:
		case DIF_CON8:
		case DIF_CON9:
		case DIF_CON10:
		case DIF_CON11:
		case DIF_CON12:
		case DIF_CON13:
		case DIF_CON14:
		case DIF_CON15:
			p->con[dif_get_index_from_reg(haddr)] = value;
		break;
		
		case DIF_TXD:
			DPRINTF("TX: %08lX\n", value);
		break;
		
		case DIF_IMSC:
			pmb887x_srb_set_imsc(&p->srb, value);
		break;
		
		case DIF_ICR:
			pmb887x_srb_set_icr(&p->srb, value);
		break;
		
		case DIF_ISR:
			pmb887x_srb_set_isr(&p->srb, value);
		break;
		
		default:
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	dif_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= dif_io_read,
	.write			= dif_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static const GraphicHwOps dif_gfx_ops = {
	.invalidate = dif_invalidate_display,
	.gfx_update = dif_update_display,
};

static void dif_init(Object *obj) {
	struct pmb887x_dif_t *p = PMB887X_DIF(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-dif", DIF_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (int i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void dif_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_dif_t *p = PMB887X_DIF(dev);
	
	pmb887x_clc_init(&p->clc);
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
	
	p->console = graphic_console_init(dev, 0, &dif_gfx_ops, p);
	qemu_console_resize(p->console, 240, 320);
	
	dif_update_state(p);
}

static Property dif_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void dif_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, dif_properties);
	dc->realize = dif_realize;
}

static const TypeInfo dif_info = {
    .name          	= TYPE_PMB887X_DIF,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_dif_t),
    .instance_init 	= dif_init,
    .class_init    	= dif_class_init,
};

static void dif_register_types(void) {
	type_register_static(&dif_info);
}
type_init(dif_register_types)
