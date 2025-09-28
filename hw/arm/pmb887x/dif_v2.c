/*
 * Display Interface
 * */
#define PMB887X_TRACE_ID		DIF
#define PMB887X_TRACE_PREFIX	"pmb887x-dif"

#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/qdev-properties.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/dmac.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/ssc/lcd_common.h"

#define TYPE_PMB887X_DIF	"pmb887x-dif-v2"
#define PMB887X_DIF(obj)	OBJECT_CHECK(pmb887x_dif_v2_t, (obj), TYPE_PMB887X_DIF)

#define DIFv2_FIFO_SIZE	0xBFFC

typedef struct pmb887x_dif_v2_t pmb887x_dif_v2_t;

struct pmb887x_dif_v2_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	qemu_irq irq[4];
	
	uint32_t runctrl;
	uint32_t prog[6];
	uint32_t con[15];
	uint32_t fifocfg;
	uint32_t tx_size;
	
	uint32_t dmac_tx_periph_id;
	pmb887x_dmac_t *dmac;
	pmb887x_lcd_t *lcd;
	
	pmb887x_clc_reg_t clc;
	pmb887x_srb_reg_t srb;
};

static int dif_v2_get_prog_index_from_reg(uint32_t reg) {
	switch (reg) {
		case DIFv2_PROG0:		return 0;
		case DIFv2_PROG1:		return 1;
		case DIFv2_PROG2:		return 2;
		case DIFv2_PROG3:		return 3;
		case DIFv2_PROG4:		return 4;
		case DIFv2_PROG5:		return 5;
		default:				abort();
	};
}

static int dif_v2_get_con_index_from_reg(uint32_t reg) {
	switch (reg) {
		case DIFv2_CON0:		return 0;
		case DIFv2_CON1:		return 1;
		case DIFv2_CON3:		return 3;
		case DIFv2_CON4:		return 4;
		case DIFv2_CON5:		return 5;
		case DIFv2_CON6:		return 6;
		case DIFv2_CON7:		return 7;
		case DIFv2_CON8:		return 8;
		case DIFv2_CON9:		return 9;
		case DIFv2_CON10:		return 10;
		case DIFv2_CON11:		return 11;
		case DIFv2_CON12:		return 12;
		case DIFv2_CON13:		return 13;
		case DIFv2_CON14:		return 14;
		default:				abort();
	};
}

static uint64_t dif_v2_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_dif_v2_t *p = opaque;
	
	uint64_t value = 0;
	switch (haddr) {
		case DIFv2_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case DIFv2_ID:
			value = 0xF043C012;
			break;

		case DIFv2_RUNCTRL:
			value = p->runctrl;
			break;

		case DIFv2_STAT:
			value = 0;
			break;

		case DIFv2_PROG0:
		case DIFv2_PROG1:
		case DIFv2_PROG2:
		case DIFv2_PROG3:
		case DIFv2_PROG4:
		case DIFv2_PROG5:
			value = p->prog[dif_v2_get_prog_index_from_reg(haddr)];
			break;

		case DIFv2_FIFOCFG:
			value = p->fifocfg;
			break;

		case DIFv2_CON0:
		case DIFv2_CON1:
		case DIFv2_CON3:
		case DIFv2_CON4:
		case DIFv2_CON5:
		case DIFv2_CON6:
		case DIFv2_CON7:
		case DIFv2_CON8:
		case DIFv2_CON9:
		case DIFv2_CON10:
		case DIFv2_CON11:
		case DIFv2_CON12:
		case DIFv2_CON13:
		case DIFv2_CON14:
			value = p->con[dif_v2_get_con_index_from_reg(haddr)];
			break;

		case DIFv2_TX_SIZE:
			value = p->tx_size;
			break;

		case DIFv2_FIFO ... (DIFv2_FIFO + DIFv2_FIFO_SIZE):
			value = 0;
			break;

		case DIFv2_IMSC:
			value = pmb887x_srb_get_imsc(&p->srb);
			break;

		case DIFv2_RIS:
			value = pmb887x_srb_get_ris(&p->srb);
			break;

		case DIFv2_MIS:
			value = pmb887x_srb_get_mis(&p->srb);
			break;

		case DIFv2_ICR:
		case DIFv2_ISR:
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

static void dif_v2_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_dif_v2_t *p = opaque;
	
	bool supress = (haddr >= DIFv2_FIFO && haddr < DIFv2_FIFO + DIFv2_FIFO_SIZE);
	
	if (!supress)
		IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case DIFv2_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case DIFv2_RUNCTRL:
			p->runctrl = value;
			break;

		case DIFv2_FIFO ... (DIFv2_FIFO + DIFv2_FIFO_SIZE - 1):
			pmb887x_lcd_write(p->lcd, value, ((p->fifocfg & DIFv2_FIFOCFG_BS) >> DIFv2_FIFOCFG_BS_SHIFT) + 1);
			break;

		case DIFv2_PROG0:
		case DIFv2_PROG1:
		case DIFv2_PROG2:
		case DIFv2_PROG3:
		case DIFv2_PROG4:
		case DIFv2_PROG5:
			p->prog[dif_v2_get_prog_index_from_reg(haddr)] = value;
			break;

		case DIFv2_FIFOCFG:
			p->fifocfg = value;
			pmb887x_lcd_set_cd(p->lcd, (p->fifocfg & DIFv2_FIFOCFG_MODE) == DIFv2_FIFOCFG_MODE_CMD);
			break;

		case DIFv2_CON0:
		case DIFv2_CON1:
		case DIFv2_CON3:
		case DIFv2_CON4:
		case DIFv2_CON5:
		case DIFv2_CON6:
		case DIFv2_CON7:
		case DIFv2_CON8:
		case DIFv2_CON9:
		case DIFv2_CON10:
		case DIFv2_CON11:
		case DIFv2_CON12:
		case DIFv2_CON13:
		case DIFv2_CON14:
			p->con[dif_v2_get_con_index_from_reg(haddr)] = value;
			break;

		case DIFv2_TX_SIZE:
			p->tx_size = value;

			if (p->dmac)
				pmb887x_dmac_request(p->dmac, p->dmac_tx_periph_id, p->tx_size);
			break;

		case DIFv2_IMSC:
			pmb887x_srb_set_imsc(&p->srb, value);
			break;

		case DIFv2_ICR:
			pmb887x_srb_set_icr(&p->srb, value);
			break;

		case DIFv2_ISR:
			pmb887x_srb_set_isr(&p->srb, value);
			break;

		case 0x80:
			// ???
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
}

static const MemoryRegionOps io_ops = {
	.read			= dif_v2_io_read,
	.write			= dif_v2_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void dif_v2_init(Object *obj) {
	pmb887x_dif_v2_t *p = PMB887X_DIF(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-dif-v2", DIFv2_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (int i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void dif_v2_realize(DeviceState *dev, Error **errp) {
	pmb887x_dif_v2_t *p = PMB887X_DIF(dev);
	pmb887x_clc_init(&p->clc);
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
}

static const Property dif_v2_properties[] = {
	DEFINE_PROP_LINK("dmac", pmb887x_dif_v2_t, dmac, TYPE_PMB887X_DMAC, pmb887x_dmac_t *),
	DEFINE_PROP_LINK("lcd", pmb887x_dif_v2_t, lcd, "pmb887x-lcd", pmb887x_lcd_t *),
	DEFINE_PROP_UINT32("dmac-tx-periph-id", pmb887x_dif_v2_t, dmac_tx_periph_id, 4),
};

static void dif_v2_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, dif_v2_properties);
	dc->realize = dif_v2_realize;
}

static const TypeInfo dif_v2_info = {
    .name          	= TYPE_PMB887X_DIF,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_dif_v2_t),
    .instance_init 	= dif_v2_init,
    .class_init    	= dif_v2_class_init,
};

static void dif_v2_register_types(void) {
	type_register_static(&dif_v2_info);
}
type_init(dif_v2_register_types)
