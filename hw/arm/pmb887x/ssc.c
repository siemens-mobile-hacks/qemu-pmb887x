/*
 * SSC (SPI)
 * */
#define PMB887X_TRACE_ID		SSC
#define PMB887X_TRACE_PREFIX	"pmb887x-ssc"

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

#define TYPE_PMB887X_SSC	"pmb887x-ssc"
OBJECT_DECLARE_SIMPLE_TYPE(pmb887x_ssc_t, PMB887X_SSC);

#define SSC_CON_STATUS		(SSC_CON_EN | SSC_CON_MS)
#define FIFO_SIZE			32

enum SSCIrqType {
	SSC_IRQ_TX,
	SSC_IRQ_RX,
	SSC_IRQ_ERR,
};

enum SSCFifoType {
	SSC_FIFO_RX,
	SSC_FIFO_TX
};

struct pmb887x_ssc_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;

	qemu_irq irq[4];

	uint32_t br;
	uint32_t con;
	uint32_t status;
	uint32_t bmreg[6];
	uint32_t unk[5];
	uint16_t tb;
	uint32_t rxfcon;
	uint32_t txfcon;
	uint32_t pbccon;
	uint32_t bcreg;
	uint32_t bcsel[2];

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

	qemu_irq gpio_sclk;
	qemu_irq gpio_mtsr;
	qemu_irq gpio_rs;
	qemu_irq gpio_cs;
	qemu_irq gpio_reset;

	int dmac_tx_clr;
	int dmac_rx_clr;

	qemu_irq dmac_tx_breq;
	qemu_irq dmac_rx_breq;
};

static void ssc_reset_fifo(pmb887x_ssc_t *p, enum SSCFifoType fifo) {
	if (fifo == SSC_FIFO_RX) {
		pmb887x_fifo_reset(p->rx_fifo);
	} else {
		pmb887x_fifo_reset(p->tx_fifo);
	}
}

static void ssc_set_fifo(pmb887x_ssc_t *p, enum SSCFifoType fifo, bool buffered) {
	if (fifo == SSC_FIFO_RX) {
		p->rx_fifo = buffered ? &p->rx_fifo_buffered : &p->rx_fifo_single;
	} else {
		p->tx_fifo = buffered ? &p->tx_fifo_buffered : &p->tx_fifo_single;
		if (buffered && ((p->txfcon & SSC_TXFCON_TXTMEN)))
			pmb887x_srb_set_isr(&p->srb, SSC_ISR_TX);
	}
	ssc_reset_fifo(p, fifo);
}

static void ssc_trigger_dma(pmb887x_ssc_t *p) {
	uint32_t ris = pmb887x_srb_get_ris_dma(&p->srb);
	if (!p->dmac_tx_clr && (ris & SSC_RIS_TX) != 0)
		qemu_set_irq(p->dmac_tx_breq, 1);
	if (!p->dmac_rx_clr && (ris & SSC_RIS_RX) != 0)
		qemu_set_irq(p->dmac_rx_breq, 1);
}

static bool ssc_is_running(pmb887x_ssc_t *p) {
	return pmb887x_clc_is_enabled(&p->clc) &&
		pmb887x_clc_get_rmc(&p->clc) &&
		(p->con & SSC_CON_EN) != 0 &&
		(p->con & SSC_CON_MS) == SSC_CON_MS_MASTER;
}

static void ssc_transfer(pmb887x_ssc_t *p) {
	if (!ssc_is_running(p) || !pmb887x_fifo_count(p->tx_fifo))
		return;

	p->status &= ~(SSC_CON_TE | SSC_CON_RE);

	if (p->bits != 8 && p->bits != 16)
		hw_error("Invalid data width: %d", p->bits);

	bool is_first_rx = pmb887x_fifo_is_empty(p->rx_fifo);
	while (!pmb887x_fifo_is_empty(p->tx_fifo)) {
		uint16_t data = pmb887x_fifo16_pop(p->tx_fifo);
		uint16_t received = 0;
		if ((p->con & SSC_CON_LB)) {
			received = data;
		} else {
			if ((p->con & SSC_CON_HB_MSB) != 0) {
				for (int shift = p->bits - 8; shift >= 0; shift -= 8)
					received |= (ssi_transfer(p->bus, (data >> shift) & 0xFF) & 0xFF) << shift;
			} else {
				for (int shift = 0; shift < p->bits; shift += 8)
					received |= (ssi_transfer(p->bus, (data >> shift) & 0xFF) & 0xFF) << shift;
			}
		}

		if (pmb887x_fifo_is_full(p->rx_fifo)) {
			if ((p->con & SSC_CON_REN)) {
				DPRINTF("RX FIFO overflow\n");
				p->status |= SSC_CON_RE;
				pmb887x_srb_set_isr(&p->srb, SSC_ISR_ERR);
			}
			pmb887x_fifo16_pop(p->rx_fifo); // overwrite last fifo stage
		}

		pmb887x_fifo16_push(p->rx_fifo, received & p->mask);
	}

	if ((p->rxfcon & SSC_RXFCON_RXTMEN)) {
		if (is_first_rx)
			pmb887x_srb_set_isr(&p->srb, SSC_ISR_RX);
	} else {
		uint32_t rx_level = (p->rxfcon & SSC_RXFCON_RXFEN) ?
			(p->rxfcon & SSC_RXFCON_RXFITL) >> SSC_RXFCON_RXFITL_SHIFT :
			1;
		if (pmb887x_fifo_count(p->rx_fifo) >= rx_level)
			pmb887x_srb_set_isr(&p->srb, SSC_ISR_RX);
	}

	if (((p->txfcon & SSC_TXFCON_TXTMEN))) {
		pmb887x_srb_set_isr(&p->srb, SSC_ISR_TX);
	} else {
		uint32_t tx_level = (p->txfcon & SSC_TXFCON_TXFEN) ?
			(p->txfcon & SSC_TXFCON_TXFITL) >> SSC_TXFCON_TXFITL_SHIFT :
			1;
		if (pmb887x_fifo_count(p->tx_fifo) <= tx_level)
			pmb887x_srb_set_isr(&p->srb, SSC_ISR_TX);
	}
}

static uint16_t ssc_read_fifo(pmb887x_ssc_t *p) {
	if (pmb887x_fifo_is_empty(p->rx_fifo)) {
		if ((p->con & SSC_CON_REN)) {
			DPRINTF("RX FIFO underflow\n");
			p->status |= SSC_CON_RE;
			pmb887x_srb_set_isr(&p->srb, SSC_ISR_ERR);
		}
		return 0;
	}
	uint16_t value = pmb887x_fifo16_pop(p->rx_fifo);
	if ((p->rxfcon & SSC_RXFCON_RXTMEN) && !pmb887x_fifo_is_empty(p->rx_fifo))
		pmb887x_srb_set_isr(&p->srb, SSC_ISR_RX);
	return value;
}

static void ssc_write_fifo(pmb887x_ssc_t *p, uint16_t value) {
	if (pmb887x_fifo_is_full(p->tx_fifo)) {
		if ((p->con & SSC_CON_TEN)) {
			DPRINTF("TX FIFO underflow\n");
			p->status |= SSC_CON_TE;
			pmb887x_srb_set_isr(&p->srb, SSC_ISR_ERR);
		}
		pmb887x_fifo16_pop(p->tx_fifo); // overwrite last fifo stage
	}
	pmb887x_fifo16_push(p->tx_fifo, p->tb & p->mask);
	ssc_transfer(p);
}

static void ssc_update_state(pmb887x_ssc_t *p) {
	uint32_t bits = ((p->con & SSC_CON_BM) >> SSC_CON_BM_SHIFT) + 1;
	p->bits = bits;
	p->mask = (1 << bits) - 1;

	if (ssc_is_running(p) && !pmb887x_fifo_is_empty(p->tx_fifo))
		ssc_transfer(p);
}

static uint64_t ssc_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_ssc_t *p = opaque;

	uint64_t value = 0;

	switch (haddr) {
		case SSC_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case SSC_ID:
			value = 0x00004525;
			break;

		case SSC_CON:
			value = (p->con & SSC_CON_EN) ? p->status | (p->con & SSC_CON_STATUS) : p->con;
			break;

		case SSC_BR:
			value = p->br;
			break;

		case SSC_TB:
			value = p->tb;
			break;

		case SSC_RB:
			value = ssc_read_fifo(p);
			break;

		case SSC_RXFCON:
			value = p->rxfcon;
			break;

		case SSC_TXFCON:
			value = p->txfcon;
			break;

		case SSC_FSTAT:
			if ((p->txfcon & SSC_TXFCON_TXFEN))
				value |= pmb887x_fifo_count(p->tx_fifo) << SSC_FSTAT_TXFFL_SHIFT;
			if ((p->rxfcon & SSC_RXFCON_RXFEN))
				value |= pmb887x_fifo_count(p->rx_fifo) << SSC_FSTAT_RXFFL_SHIFT;
			break;

		case SSC_IMSC:
			value = pmb887x_srb_get_imsc(&p->srb);
			break;

		case SSC_RIS:
			value = pmb887x_srb_get_ris(&p->srb);
			break;

		case SSC_MIS:
			value = pmb887x_srb_get_mis(&p->srb);
			break;

		case SSC_ICR:
		case SSC_ISR:
			value = 0;
			break;

		case SSC_DMAE:
			value = pmb887x_srb_get_dmae(&p->srb);
			break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}

	IO_DUMP(haddr + p->mmio.addr, size, value, false);

	return value;
}

static void ssc_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_ssc_t *p = opaque;

	IO_DUMP(haddr + p->mmio.addr, size, value, true);

	switch (haddr) {
		case SSC_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case SSC_CON:
			p->con = (p->con & SSC_CON_EN) ? ((p->con & ~SSC_CON_STATUS) | (value & SSC_CON_STATUS)) : value;
			ssc_update_state(p);
			break;

		case SSC_BR:
			p->br = value;
			break;

		case SSC_TB:
			p->tb = value & p->mask;
			ssc_write_fifo(p, value);
			break;

		case SSC_RXFCON:
			if ((value & SSC_RXFCON_RXFEN) != (p->rxfcon & SSC_RXFCON_RXFEN))
				ssc_set_fifo(p, SSC_FIFO_RX, (value & SSC_RXFCON_RXFEN) != 0);

			if ((value & SSC_RXFCON_RXFLU)) {
				ssc_reset_fifo(p, SSC_FIFO_RX);
				value &= ~SSC_RXFCON_RXFLU;
			}

			p->rxfcon = value;
			break;

		case SSC_TXFCON:
			if ((value & SSC_TXFCON_TXFEN) != (p->txfcon & SSC_TXFCON_TXFEN))
				ssc_set_fifo(p, SSC_FIFO_TX, (value & SSC_TXFCON_TXFEN) != 0);

			if ((value & SSC_TXFCON_TXFLU)) {
				ssc_reset_fifo(p, SSC_FIFO_TX);
				value &= ~SSC_TXFCON_TXFLU;

				if (((p->txfcon & SSC_TXFCON_TXTMEN)))
					pmb887x_srb_set_isr(&p->srb, SSC_ISR_TX);
			}

			p->txfcon = value;
			break;

		case SSC_IMSC:
			pmb887x_srb_set_imsc(&p->srb, value);
			break;

		case SSC_ICR:
			pmb887x_srb_set_icr(&p->srb, value);
			break;

		case SSC_ISR:
			pmb887x_srb_set_isr(&p->srb, value);
			break;

		case SSC_DMAE:
			pmb887x_srb_set_dmae(&p->srb, value);
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
}

static void ssc_event_handler(void *opaque, int event_id, int level) {
	pmb887x_ssc_t *p = opaque;
	if (level != 0)
		ssc_trigger_dma(p);
}

static const MemoryRegionOps io_ops = {
	.read			= ssc_io_read,
	.write			= ssc_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void ssc_handle_gpio_input(void *opaque, int id, int level) {
	// nothing
}

static void ssc_handle_dmac_tx_clr(void *opaque, int id, int level) {
	pmb887x_ssc_t *p = opaque;
	p->dmac_tx_clr = level;
	if (level == 1) {
		qemu_set_irq(p->dmac_tx_breq, 0);
	} else {
		ssc_trigger_dma(p);
	}
}

static void ssc_handle_dmac_rx_clr(void *opaque, int id, int level) {
	pmb887x_ssc_t *p = opaque;
	p->dmac_rx_clr = level;
	if (level == 1) {
		qemu_set_irq(p->dmac_rx_breq, 0);
	} else {
		ssc_trigger_dma(p);
	}
}

static void ssc_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_ssc_t *p = PMB887X_SSC(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, TYPE_PMB887X_SSC, SSC_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	p->bus = ssi_create_bus(DEVICE(obj), TYPE_PMB887X_SSC);

	for (int i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);

	// DMAC
	qdev_init_gpio_in_named(dev, ssc_handle_dmac_tx_clr, "DMAC_TX_CLR", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_tx_breq, "DMAC_TX_BREQ", 1);

	qdev_init_gpio_in_named(dev, ssc_handle_dmac_rx_clr, "DMAC_RX_CLR", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_rx_breq, "DMAC_RX_BREQ", 1);

	qdev_init_gpio_in_named(dev, ssc_handle_gpio_input, "MRST_IN", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_sclk, "SCLK_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_mtsr, "MTSR_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_rs, "RS_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_cs, "CS_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_reset, "RESET_OUT", 1);
}

static void ssc_realize(DeviceState *dev, Error **errp) {
	pmb887x_ssc_t *p = PMB887X_SSC(dev);
	pmb887x_clc_init(&p->clc);
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
	pmb887x_srb_set_event_handler(&p->srb, dev, ssc_event_handler);

	pmb887x_fifo16_init(&p->tx_fifo_buffered, FIFO_SIZE);
	pmb887x_fifo16_init(&p->rx_fifo_buffered, FIFO_SIZE);

	pmb887x_fifo16_init(&p->tx_fifo_single, 1);
	pmb887x_fifo16_init(&p->rx_fifo_single, 1);

	ssc_set_fifo(p, SSC_FIFO_RX, false);
	ssc_set_fifo(p, SSC_FIFO_TX, false);

	ssc_update_state(p);
}

static const Property ssc_properties[] = {
	DEFINE_PROP_LINK("bus", pmb887x_ssc_t, bus, "SSI", SSIBus *),
};

static void ssc_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, ssc_properties);
	dc->realize = ssc_realize;
}

static const TypeInfo ssc_info = {
	.name          	= TYPE_PMB887X_SSC,
	.parent        	= TYPE_SYS_BUS_DEVICE,
	.instance_size 	= sizeof(pmb887x_ssc_t),
	.instance_init 	= ssc_init,
	.class_init    	= ssc_class_init,
};

static void ssc_register_types(void) {
	type_register_static(&ssc_info);
}
type_init(ssc_register_types)
