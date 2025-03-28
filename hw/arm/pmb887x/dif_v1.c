/*
 * Display Interface
 * */
#define PMB887X_TRACE_ID		DIF
#define PMB887X_TRACE_PREFIX	"pmb887x-dif"

#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/qdev-properties.h"

#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/dif/lcd_common.h"

#define TYPE_PMB887X_DIF	"pmb887x-dif-v1"
#define PMB887X_DIF(obj)	OBJECT_CHECK(pmb887x_dif_v1_t, (obj), TYPE_PMB887X_DIF)

#define DIFv1_FIFO_SIZE	0xBFFC

typedef struct pmb887x_dif_v1_t pmb887x_dif_v1_t;

struct pmb887x_dif_v1_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;

	qemu_irq irq[4];

	uint32_t br;
	uint32_t con;
	uint32_t prog[6];
	uint32_t fifocfg;
	uint32_t tx_size;

	pmb887x_lcd_t *lcd;

	pmb887x_clc_reg_t clc;
	pmb887x_srb_reg_t srb;
};

static void dif_v1_update_state(pmb887x_dif_v1_t *p) {

}

static int dif_v1_get_index_from_reg(uint32_t reg) {
	switch (reg) {
		case DIFv1_PROG0:		return 0;
		case DIFv1_PROG1:		return 1;
		case DIFv1_PROG2:		return 2;
		case DIFv1_PROG3:		return 3;
		case DIFv1_PROG4:		return 4;
		case DIFv1_PROG5:		return 5;
		default:
			hw_error("pmb887x-dif: unknown reg %d", reg);
	};
}

static uint64_t dif_v1_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_dif_v1_t *p = opaque;

	uint64_t value = 0;

	switch (haddr) {
		case DIFv1_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case DIFv1_ID:
			value = 0xF043C012;
			break;

		case DIFv1_BR:
			value = p->br;
			break;

		case DIFv1_CON:
			value = p->con;
			break;

		case DIFv1_PROG0:
		case DIFv1_PROG1:
		case DIFv1_PROG2:
		case DIFv1_PROG3:
		case DIFv1_PROG4:
		case DIFv1_PROG5:
			value = p->prog[dif_v1_get_index_from_reg(haddr)];
			break;

		case DIFv1_IMSC:
			value = pmb887x_srb_get_imsc(&p->srb);
			break;

		case DIFv1_RIS:
			value = pmb887x_srb_get_ris(&p->srb);
			break;

		case DIFv1_MIS:
			value = pmb887x_srb_get_mis(&p->srb);
			break;

		case DIFv1_ICR:
		case DIFv1_ISR:
		case DIFv1_TB:
		case DIFv1_RB:
			value = 0;
			break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}

	IO_DUMP(haddr + p->mmio.addr, size, value, false);

	return value;
}

static void dif_v1_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_dif_v1_t *p = opaque;

	IO_DUMP(haddr + p->mmio.addr, size, value, true);

	switch (haddr) {
		case DIFv1_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case DIFv1_BR:
			p->br = value;
			break;

		case DIFv1_CON:
			p->con = value;
			break;

		case DIFv1_PROG0:
		case DIFv1_PROG1:
		case DIFv1_PROG2:
		case DIFv1_PROG3:
		case DIFv1_PROG4:
		case DIFv1_PROG5:
			p->prog[dif_v1_get_index_from_reg(haddr)] = value;
			break;

		case DIFv1_IMSC:
			pmb887x_srb_set_imsc(&p->srb, value);
			break;

		case DIFv1_ICR:
			pmb887x_srb_set_icr(&p->srb, value);
			break;

		case DIFv1_ISR:
			pmb887x_srb_set_isr(&p->srb, value);
			break;

		case DIFv1_TB:
		case DIFv1_RB:
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}

	dif_v1_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= dif_v1_io_read,
	.write			= dif_v1_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void dif_v1_init(Object *obj) {
	pmb887x_dif_v1_t *p = PMB887X_DIF(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-dif-v1", DIFv1_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	for (int i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void dif_v1_realize(DeviceState *dev, Error **errp) {
	pmb887x_dif_v1_t *p = PMB887X_DIF(dev);
	pmb887x_clc_init(&p->clc);
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
}

static const Property dif_v1_properties[] = {
	DEFINE_PROP_LINK("lcd", pmb887x_dif_v1_t, lcd, "pmb887x-lcd", pmb887x_lcd_t *),
};

static void dif_v1_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, dif_v1_properties);
	dc->realize = dif_v1_realize;
}

static const TypeInfo dif_v1_info = {
	.name          	= TYPE_PMB887X_DIF,
	.parent        	= TYPE_SYS_BUS_DEVICE,
	.instance_size 	= sizeof(pmb887x_dif_v1_t),
	.instance_init 	= dif_v1_init,
	.class_init    	= dif_v1_class_init,
};

static void dif_v1_register_types(void) {
	type_register_static(&dif_v1_info);
}
type_init(dif_v1_register_types)
