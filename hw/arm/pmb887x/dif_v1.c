/*
 * Display Interface
 * */
#define PMB887X_TRACE_ID		DIF
#define PMB887X_TRACE_PREFIX	"pmb887x-dif"

#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/hw.h"
#include "hw/ssi/ssi.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "qom/object.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/fifo.h"
#include "hw/arm/pmb887x/dmac.h"

#define TYPE_PMB887X_DIF	"pmb887x-dif-v1"
OBJECT_DECLARE_SIMPLE_TYPE(pmb887x_dif_t, PMB887X_DIF);

#define DIF_CON_STATUS		(DIFv1_CON_EN | DIFv1_CON_MS)
#define FIFO_SIZE	4

enum DIFIrqType {
	DIF_IRQ_TX,
	DIF_IRQ_RX,
	DIF_IRQ_ERR,
};

enum DIFFifoType {
	DIF_FIFO_RX,
	DIF_FIFO_TX
};

struct pmb887x_dif_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;

	pmb887x_dmac_t *dmac;
	qemu_irq irq[4];

	uint32_t br;
	uint32_t con;
	uint32_t status;
	uint32_t prog[6];
	uint32_t unk[9];
	uint16_t tb;
	uint32_t rxfcon;
	uint32_t txfcon;
	uint32_t dmacon;

	pmb887x_fifo16_t tx_fifo_buffered;
	pmb887x_fifo16_t rx_fifo_buffered;

	pmb887x_fifo16_t tx_fifo_single;
	pmb887x_fifo16_t rx_fifo_single;

	pmb887x_fifo16_t *rx_fifo;
	pmb887x_fifo16_t *tx_fifo;

	uint32_t mask;
	uint32_t bits;

	pmb887x_clc_reg_t clc;
	pmb887x_srb_reg_t srb;
    SSIBus *bus;

	uint32_t dmac_tx_periph_id;
};

static void dif_reset_fifo(pmb887x_dif_t *p, enum DIFFifoType fifo) {
	if (fifo == DIF_FIFO_RX) {
		pmb887x_fifo_reset(p->rx_fifo);
	} else {
		pmb887x_fifo_reset(p->tx_fifo);
	}
}

static void dif_set_fifo(pmb887x_dif_t *p, enum DIFFifoType fifo, bool buffered) {
	if (fifo == DIF_FIFO_RX) {
		p->rx_fifo = buffered ? &p->rx_fifo_buffered : &p->rx_fifo_single;
	} else {
		p->tx_fifo = buffered ? &p->tx_fifo_buffered : &p->tx_fifo_single;
	}
	dif_reset_fifo(p, fifo);
}

static bool dif_is_running(pmb887x_dif_t *p) {
	return pmb887x_clc_is_enabled(&p->clc) &&
		pmb887x_clc_get_rmc(&p->clc) &&
		(p->con & DIFv1_CON_EN) != 0 &&
		(p->con & DIFv1_CON_MS) == DIFv1_CON_MS_MASTER;
}

static void dif_transfer(pmb887x_dif_t *p) {
	if (!dif_is_running(p))
		return;

	p->status &= ~(DIFv1_CON_TE | DIFv1_CON_RE);

	int size = pmb887x_fifo_count(p->tx_fifo);
	for (uint32_t i = 0; i < size; i++) {
		uint16_t data = pmb887x_fifo16_pop(p->tx_fifo);
		uint16_t received = 0;
		if (!(p->con & DIFv1_CON_LB)) {
			for (int shift = p->bits - 8; shift >= 0; shift -= 8)
				received |= (ssi_transfer(p->bus, (data >> shift) & 0xFF) & 0xFF) << shift;
		}

		if (!pmb887x_fifo_is_full(p->rx_fifo)) {
			pmb887x_fifo16_push(p->rx_fifo, received & p->mask);
		} else {
			pmb887x_fifo16_pop(p->rx_fifo);
			pmb887x_fifo16_push(p->rx_fifo, received & p->mask);

			if ((p->con & DIFv1_CON_REN)) {
				p->status |= DIFv1_CON_RE;
				pmb887x_srb_set_isr(&p->srb, DIFv1_ISR_ERR);
			}
		}
	}

	uint32_t rx_level = (p->rxfcon & DIFv1_RXFCON_RXFEN) ?
		(p->rxfcon & DIFv1_RXFCON_RXFITL) >> DIFv1_RXFCON_RXFITL_SHIFT :
		1;

	uint32_t tx_level = (p->rxfcon & DIFv1_TXFCON_TXFEN) ?
		(p->txfcon & DIFv1_TXFCON_TXFITL) >> DIFv1_TXFCON_TXFITL_SHIFT :
		1;

	if (pmb887x_fifo_count(p->tx_fifo) <= tx_level)
		pmb887x_srb_set_isr(&p->srb, DIFv1_ISR_TX);

	if (pmb887x_fifo_count(p->rx_fifo) >= rx_level)
		pmb887x_srb_set_isr(&p->srb, DIFv1_ISR_RX);
}

static void dif_update_state(pmb887x_dif_t *p) {
	uint32_t bits = ((p->con & DIFv1_CON_BM) >> DIFv1_CON_BM_SHIFT) + 1;
	p->bits = bits;
	p->mask = (1 << bits) - 1;

	if (dif_is_running(p) && bits != 8 && bits != 16) {
		hw_error("Invalid data width: %d", bits);
	}

	if (dif_is_running(p) && !(p->con & DIFv1_CON_HB_MSB)) {
		hw_error("Only MSB supported.");
	}

	if ((p->rxfcon & DIFv1_RXFCON_RXFLU)) {
		dif_reset_fifo(p, DIF_FIFO_RX);
		p->rxfcon &= ~DIFv1_RXFCON_RXFLU;
	}

	if ((p->txfcon & DIFv1_TXFCON_TXFLU)) {
		dif_reset_fifo(p, DIF_FIFO_TX);
		p->txfcon &= ~DIFv1_TXFCON_TXFLU;
	}

	if (dif_is_running(p) && !pmb887x_fifo_is_empty(p->tx_fifo)) {
		dif_transfer(p);
	}

	if ((p->dmacon & DIFv1_DMACON_TX)) {
		if (!pmb887x_dmac_is_busy(p->dmac))
			pmb887x_dmac_request(p->dmac, p->dmac_tx_periph_id, pmb887x_fifo_free_count(p->tx_fifo));
	}

	if ((p->dmacon & DIFv1_DMACON_RX)) {
		hw_error("TX DMA not supported.");
	}
}

static int dif_get_prog_index_from_reg(uint32_t reg) {
	switch (reg) {
		case DIFv1_PROG0:		return 0;
		case DIFv1_PROG1:		return 1;
		case DIFv1_PROG2:		return 2;
		case DIFv1_PROG3:		return 3;
		case DIFv1_PROG4:		return 4;
		case DIFv1_PROG5:		return 5;
		default:				abort();
	};
}

static int dif_get_unk_index_from_reg(uint32_t reg) {
	switch (reg) {
		case DIFv1_UNK0:		return 0;
		case DIFv1_UNK1:		return 1;
		case DIFv1_UNK2:		return 2;
		case DIFv1_UNK3:		return 3;
		case DIFv1_UNK4:		return 4;
		case DIFv1_UNK5:		return 5;
		case DIFv1_UNK6:		return 6;
		case DIFv1_UNK7:		return 7;
		case DIFv1_UNK8:		return 8;
		default:				abort();
	};
}

static uint64_t dif_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_dif_t *p = opaque;

	uint64_t value = 0;

	switch (haddr) {
		case DIFv1_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case DIFv1_ID:
			value = 0xF043C012;
			break;

		case DIFv1_CON:
			value = (p->con & DIFv1_CON_EN) ? p->status | (p->con & DIF_CON_STATUS) : p->con;
			break;

		case DIFv1_BR:
			value = p->br;
			break;

		case DIFv1_TB:
			value = p->tb;
			break;

		case DIFv1_RB:
			if (dif_is_running(p) && !pmb887x_fifo_is_empty(p->rx_fifo)) {
				value = pmb887x_fifo16_pop(p->rx_fifo);
				if (!pmb887x_fifo_is_empty(p->tx_fifo))
					dif_transfer(p);
			} else {
				EPRINTF("RX fifo is empty!");
				value = 0;
			}
			break;

		case DIFv1_RXFCON:
			value = p->rxfcon;
			break;

		case DIFv1_TXFCON:
			value = p->txfcon;
			break;

		case DIFv1_FSTAT:
			if ((p->txfcon & DIFv1_TXFCON_TXFEN))
				value |= pmb887x_fifo_count(p->tx_fifo) << DIFv1_FSTAT_TXFFL_SHIFT;
			if ((p->txfcon & DIFv1_RXFCON_RXFEN))
				value |= pmb887x_fifo_count(p->rx_fifo) << DIFv1_FSTAT_RXFFL_SHIFT;
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
			value = 0;
			break;

		case DIFv1_DMACON:
			value = p->dmacon;
			break;

		case DIFv1_UNK0:
		case DIFv1_UNK1:
		case DIFv1_UNK2:
		case DIFv1_UNK3:
		case DIFv1_UNK4:
		case DIFv1_UNK5:
		case DIFv1_UNK6:
		case DIFv1_UNK7:
		case DIFv1_UNK8:
			value = p->unk[dif_get_unk_index_from_reg(haddr)];
			break;

		case DIFv1_PROG0:
		case DIFv1_PROG1:
		case DIFv1_PROG2:
		case DIFv1_PROG3:
		case DIFv1_PROG4:
		case DIFv1_PROG5:
			value = p->prog[dif_get_prog_index_from_reg(haddr)];
			break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}

	IO_DUMP(haddr + p->mmio.addr, size, value, false);

	return value;
}

static void dif_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_dif_t *p = opaque;

	IO_DUMP(haddr + p->mmio.addr, size, value, true);

	switch (haddr) {
		case DIFv1_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case DIFv1_CON:
			p->con = (p->con & DIFv1_CON_EN) ? ((p->con & ~DIF_CON_STATUS) | (value & DIF_CON_STATUS)) : value;
			dif_update_state(p);
			break;

		case DIFv1_BR:
			p->br = value;
			break;

		case DIFv1_TB:
			p->tb = value & p->mask;
			if (!pmb887x_fifo_is_full(p->tx_fifo)) {
				pmb887x_fifo16_push(p->tx_fifo, p->tb & p->mask);
			} else {
				pmb887x_fifo16_pop(p->rx_fifo);
				pmb887x_fifo16_push(p->rx_fifo, p->tb & p->mask);
				if ((p->con & DIFv1_CON_TEN)) {
					p->status |= DIFv1_CON_TE;
					pmb887x_srb_set_isr(&p->srb, DIFv1_ISR_ERR);
				}
			}
			dif_transfer(p);
			break;

		case DIFv1_RXFCON:
			if ((value & DIFv1_RXFCON_RXFEN) != (p->rxfcon & DIFv1_RXFCON_RXFEN))
				dif_set_fifo(p, DIF_FIFO_RX, (value & DIFv1_RXFCON_RXFEN) != 0);
			p->rxfcon = value;
			dif_update_state(p);
			break;

		case DIFv1_TXFCON:
			if ((value & DIFv1_TXFCON_TXFEN) != (p->rxfcon & DIFv1_TXFCON_TXFEN))
				dif_set_fifo(p, DIF_FIFO_TX, (value & DIFv1_TXFCON_TXFEN) != 0);
			p->txfcon = value;
			dif_update_state(p);
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

		case DIFv1_DMACON:
			p->dmacon = value;
			dif_update_state(p);
			break;

		case DIFv1_UNK0:
		case DIFv1_UNK1:
		case DIFv1_UNK2:
		case DIFv1_UNK3:
		case DIFv1_UNK4:
		case DIFv1_UNK5:
		case DIFv1_UNK6:
		case DIFv1_UNK7:
		case DIFv1_UNK8:
			p->unk[dif_get_unk_index_from_reg(haddr)] = value;
			break;

		case DIFv1_PROG0:
		case DIFv1_PROG1:
		case DIFv1_PROG2:
		case DIFv1_PROG3:
		case DIFv1_PROG4:
		case DIFv1_PROG5:
			p->prog[dif_get_prog_index_from_reg(haddr)] = value;
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
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

static void dif_init(Object *obj) {
	pmb887x_dif_t *p = PMB887X_DIF(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, TYPE_PMB887X_DIF, DIFv1_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	p->bus = ssi_create_bus(DEVICE(obj), TYPE_PMB887X_DIF);

	for (int i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void dif_realize(DeviceState *dev, Error **errp) {
	pmb887x_dif_t *p = PMB887X_DIF(dev);
	pmb887x_clc_init(&p->clc);
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));

	pmb887x_fifo16_init(&p->tx_fifo_buffered, FIFO_SIZE);
	pmb887x_fifo16_init(&p->rx_fifo_buffered, FIFO_SIZE);

	pmb887x_fifo16_init(&p->tx_fifo_single, 1);
	pmb887x_fifo16_init(&p->rx_fifo_single, 1);

	dif_set_fifo(p, DIF_FIFO_RX, false);
	dif_set_fifo(p, DIF_FIFO_TX, false);

	dif_update_state(p);
}

static const Property dif_properties[] = {
	DEFINE_PROP_LINK("bus", pmb887x_dif_t, bus, "SSI", SSIBus *),
	DEFINE_PROP_LINK("dmac", pmb887x_dif_t, dmac, TYPE_PMB887X_DMAC, pmb887x_dmac_t *),
	DEFINE_PROP_UINT32("dmac-tx-periph-id", pmb887x_dif_t, dmac_tx_periph_id, 4),
};

static void dif_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, dif_properties);
	dc->realize = dif_realize;
}

static const TypeInfo dif_info = {
	.name          	= TYPE_PMB887X_DIF,
	.parent        	= TYPE_SYS_BUS_DEVICE,
	.instance_size 	= sizeof(pmb887x_dif_t),
	.instance_init 	= dif_init,
	.class_init    	= dif_class_init,
};

static void dif_register_types(void) {
	type_register_static(&dif_info);
}
type_init(dif_register_types)
