/*
 * Display Interface (modified SSC)
 * */
#define PMB887X_TRACE_ID		DIF
#define PMB887X_TRACE_PREFIX	"pmb887x-dif"

#define PMB887X_DIF_DUMP_BIT_MUX	0 // print bit mux config

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
#define FIFO_SIZE			32

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
		if (buffered && ((p->txfcon & DIFv1_TXFCON_TXTMEN)))
			pmb887x_srb_set_isr(&p->srb, DIFv1_ISR_TX);
	}
	dif_reset_fifo(p, fifo);
}

static void dif_trigger_dma(pmb887x_dif_t *p) {
	uint32_t ris = pmb887x_srb_get_ris_dma(&p->srb);
	if (!p->dmac_tx_clr && (ris & DIFv1_RIS_TX) != 0)
		qemu_set_irq(p->dmac_tx_breq, 1);
	if (!p->dmac_rx_clr && (ris & DIFv1_RIS_RX) != 0)
		qemu_set_irq(p->dmac_rx_breq, 1);
}

static bool dif_is_running(pmb887x_dif_t *p) {
	return pmb887x_clc_is_enabled(&p->clc) &&
		pmb887x_clc_get_rmc(&p->clc) &&
		(p->con & DIFv1_CON_EN) != 0 &&
		(p->con & DIFv1_CON_MS) == DIFv1_CON_MS_MASTER;
}

static void dif_transfer(pmb887x_dif_t *p) {
	if (!dif_is_running(p) || !pmb887x_fifo_count(p->tx_fifo))
		return;

	p->status &= ~(DIFv1_CON_TE | DIFv1_CON_RE);

	if (p->bits != 8 && p->bits != 16)
		hw_error("Invalid data width: %d", p->bits);

	bool is_first_rx = pmb887x_fifo_is_empty(p->rx_fifo);
	while (!pmb887x_fifo_is_empty(p->tx_fifo)) {
		uint16_t data = pmb887x_fifo16_pop(p->tx_fifo);
		uint16_t received = 0;
		if ((p->con & DIFv1_CON_LB)) {
			received = data;
		} else {
			if ((p->con & DIFv1_CON_HB_MSB) != 0) {
				for (int shift = p->bits - 8; shift >= 0; shift -= 8)
					received |= (ssi_transfer(p->bus, (data >> shift) & 0xFF) & 0xFF) << shift;
			} else {
				for (int shift = 0; shift < p->bits; shift += 8)
					received |= (ssi_transfer(p->bus, (data >> shift) & 0xFF) & 0xFF) << shift;
			}
		}

		if (pmb887x_fifo_is_full(p->rx_fifo)) {
			if ((p->con & DIFv1_CON_REN)) {
				DPRINTF("RX FIFO overflow\n");
				p->status |= DIFv1_CON_RE;
				pmb887x_srb_set_isr(&p->srb, DIFv1_ISR_ERR);
			}
			pmb887x_fifo16_pop(p->rx_fifo); // overwrite last fifo stage
		}

		pmb887x_fifo16_push(p->rx_fifo, received & p->mask);
	}

	if ((p->rxfcon & DIFv1_RXFCON_RXTMEN)) {
		if (is_first_rx)
			pmb887x_srb_set_isr(&p->srb, DIFv1_ISR_RX);
	} else {
		uint32_t rx_level = (p->rxfcon & DIFv1_RXFCON_RXFEN) ?
			(p->rxfcon & DIFv1_RXFCON_RXFITL) >> DIFv1_RXFCON_RXFITL_SHIFT :
			1;
		if (pmb887x_fifo_count(p->rx_fifo) >= rx_level)
			pmb887x_srb_set_isr(&p->srb, DIFv1_ISR_RX);
	}

	if (((p->txfcon & DIFv1_TXFCON_TXTMEN))) {
		pmb887x_srb_set_isr(&p->srb, DIFv1_ISR_TX);
	} else {
		uint32_t tx_level = (p->txfcon & DIFv1_TXFCON_TXFEN) ?
			(p->txfcon & DIFv1_TXFCON_TXFITL) >> DIFv1_TXFCON_TXFITL_SHIFT :
			1;
		if (pmb887x_fifo_count(p->tx_fifo) <= tx_level)
			pmb887x_srb_set_isr(&p->srb, DIFv1_ISR_TX);
	}
}

static uint16_t dif_read_fifo(pmb887x_dif_t *p) {
	if (pmb887x_fifo_is_empty(p->rx_fifo)) {
		if ((p->con & DIFv1_CON_REN)) {
			DPRINTF("RX FIFO underflow\n");
			p->status |= DIFv1_CON_RE;
			pmb887x_srb_set_isr(&p->srb, DIFv1_ISR_ERR);
		}
		return 0;
	}
	uint16_t value = pmb887x_fifo16_pop(p->rx_fifo);
	if ((p->rxfcon & DIFv1_RXFCON_RXTMEN) && !pmb887x_fifo_is_empty(p->rx_fifo))
		pmb887x_srb_set_isr(&p->srb, DIFv1_ISR_RX);
	return value;
}

static void dif_write_fifo(pmb887x_dif_t *p, uint16_t value) {
	if (pmb887x_fifo_is_full(p->tx_fifo)) {
		if ((p->con & DIFv1_CON_TEN)) {
			DPRINTF("TX FIFO underflow\n");
			p->status |= DIFv1_CON_TE;
			pmb887x_srb_set_isr(&p->srb, DIFv1_ISR_ERR);
		}
		pmb887x_fifo16_pop(p->tx_fifo); // overwrite last fifo stage
	}
	pmb887x_fifo16_push(p->tx_fifo, p->tb & p->mask);
	dif_transfer(p);
}

static void dif_update_state(pmb887x_dif_t *p) {
	uint32_t bits = ((p->con & DIFv1_CON_BM) >> DIFv1_CON_BM_SHIFT) + 1;
	p->bits = bits;
	p->mask = (1 << bits) - 1;

	if (dif_is_running(p) && !pmb887x_fifo_is_empty(p->tx_fifo))
		dif_transfer(p);
}

static int dif_get_unk_index_from_reg(uint32_t reg) {
	switch (reg) {
		case DIFv1_UNK0:		return 0;
		case DIFv1_UNK1:		return 1;
		case DIFv1_UNK2:		return 2;
		case DIFv1_UNK3:		return 3;
		case DIFv1_UNK4:		return 4;
		default:				abort();
	};
}

static void dif_dump_bit_mux(pmb887x_dif_t *p) {
	#if PMB887X_DIF_DUMP_BIT_MUX
	g_autoptr(GString) mux_str = g_string_new("");
	g_autoptr(GString) bcsel_str = g_string_new("");
	g_autoptr(GString) bc_str = g_string_new("");

	for (uint32_t i = 0; i < 32; i++) {
		// DIF_BMREGx
		uint32_t bm_reg_index = i / 6;
		uint32_t bm_shift = i * 5 - (bm_reg_index * (5 * 6));
		if (bm_shift >= 15)
			bm_shift++;
		uint32_t mux = (p->bmreg[bm_reg_index] >> bm_shift) & 0x1F;
		g_string_append_printf(mux_str, " %3d", mux);

		// DIF_BCSELx
		uint32_t bcsel_reg_index = i / 16;
		uint32_t bcsel_shift = i * 2 - (bcsel_reg_index * 32);
		uint32_t bcsel = (p->bcsel[bcsel_reg_index] >> bcsel_shift) & 3;
		g_string_append_printf(bcsel_str, " %3d", bcsel);

		// DIF_BCREG
		g_string_append_printf(bc_str, " %3d", p->bcreg & (1 << i) ? 1 : 0);
	}

	DPRINTF(
		"BMREGx = { %08X, %08X, %08X, %08X, %08X, %08X }\n",
		 p->bmreg[0], p->bmreg[1], p->bmreg[2],
		 p->bmreg[3],p->bmreg[4], p->bmreg[5]
	);
	DPRINTF(
		"BCSELx = { %08X, %08X }, BCREG = %08X\n",
		 p->bcsel[0], p->bcsel[1], p->bcreg
	);
	DPRINTF("   MUX: %s\n", mux_str->str);
	DPRINTF(" BCSEL: %s\n", bcsel_str->str);
	DPRINTF("    BC: %s\n", bc_str->str);
	#endif
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
			value = dif_read_fifo(p);
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
			if ((p->rxfcon & DIFv1_RXFCON_RXFEN))
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

		case DIFv1_DMAE:
			value = pmb887x_srb_get_dmae(&p->srb);
			break;

		case DIFv1_UNK0:
		case DIFv1_UNK1:
		case DIFv1_UNK2:
		case DIFv1_UNK3:
		case DIFv1_UNK4:
			value = p->unk[dif_get_unk_index_from_reg(haddr)];
			break;

		case DIFv1_PBCCON:
			value = p->pbccon;
			break;

		case DIFv1_BCSEL0:
		case DIFv1_BCSEL1:
			value = p->bcsel[(haddr - DIFv1_BCSEL0) / 4];
			break;

		case DIFv1_BMREG0:
		case DIFv1_BMREG1:
		case DIFv1_BMREG2:
		case DIFv1_BMREG3:
		case DIFv1_BMREG4:
		case DIFv1_BMREG5:
			value = p->bmreg[(haddr - DIFv1_BMREG0) / 4];
			break;

		case DIFv1_BCREG:
			value = p->bcreg;
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
			dif_write_fifo(p, value);
			break;

		case DIFv1_RXFCON:
			if ((value & DIFv1_RXFCON_RXFEN) != (p->rxfcon & DIFv1_RXFCON_RXFEN))
				dif_set_fifo(p, DIF_FIFO_RX, (value & DIFv1_RXFCON_RXFEN) != 0);

			if ((value & DIFv1_RXFCON_RXFLU)) {
				dif_reset_fifo(p, DIF_FIFO_RX);
				value &= ~DIFv1_RXFCON_RXFLU;
			}

			p->rxfcon = value;
			break;

		case DIFv1_TXFCON:
			if ((value & DIFv1_TXFCON_TXFEN) != (p->txfcon & DIFv1_TXFCON_TXFEN))
				dif_set_fifo(p, DIF_FIFO_TX, (value & DIFv1_TXFCON_TXFEN) != 0);

			if ((value & DIFv1_TXFCON_TXFLU)) {
				dif_reset_fifo(p, DIF_FIFO_TX);
				value &= ~DIFv1_TXFCON_TXFLU;

				if (((p->txfcon & DIFv1_TXFCON_TXTMEN)))
					pmb887x_srb_set_isr(&p->srb, DIFv1_ISR_TX);
			}

			p->txfcon = value;
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

		case DIFv1_DMAE:
			pmb887x_srb_set_dmae(&p->srb, value);
			break;

		case DIFv1_UNK0:
		case DIFv1_UNK1:
		case DIFv1_UNK2:
		case DIFv1_UNK3:
		case DIFv1_UNK4:
			p->unk[dif_get_unk_index_from_reg(haddr)] = value;
			break;

		case DIFv1_PBCCON:
			p->pbccon = value;
			break;

		case DIFv1_BCSEL0:
		case DIFv1_BCSEL1:
			p->bcsel[(haddr - DIFv1_BCSEL0) / 4] = value;
			dif_dump_bit_mux(p);
			break;

		case DIFv1_BMREG0:
		case DIFv1_BMREG1:
		case DIFv1_BMREG2:
		case DIFv1_BMREG3:
		case DIFv1_BMREG4:
		case DIFv1_BMREG5:
			p->bmreg[(haddr - DIFv1_BMREG0) / 4] = value;
			dif_dump_bit_mux(p);
			break;

		case DIFv1_BCREG:
			p->bcreg = value;
			dif_dump_bit_mux(p);
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
}

static void dif_event_handler(void *opaque, int event_id, int level) {
	pmb887x_dif_t *p = opaque;
	if (level != 0)
		dif_trigger_dma(p);
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

static void dif_handle_gpio_input(void *opaque, int id, int level) {
	// nothing
}

static void dif_handle_dmac_tx_clr(void *opaque, int id, int level) {
	pmb887x_dif_t *p = opaque;
	p->dmac_tx_clr = level;
	if (level == 1) {
		qemu_set_irq(p->dmac_tx_breq, 0);
	} else {
		dif_trigger_dma(p);
	}
}

static void dif_handle_dmac_rx_clr(void *opaque, int id, int level) {
	pmb887x_dif_t *p = opaque;
	p->dmac_rx_clr = level;
	if (level == 1) {
		qemu_set_irq(p->dmac_rx_breq, 0);
	} else {
		dif_trigger_dma(p);
	}
}

static void dif_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_dif_t *p = PMB887X_DIF(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, TYPE_PMB887X_DIF, DIFv1_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	p->bus = ssi_create_bus(DEVICE(obj), TYPE_PMB887X_DIF);

	for (int i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);

	// DMAC
	qdev_init_gpio_in_named(dev, dif_handle_dmac_tx_clr, "DMAC_TX_CLR", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_tx_breq, "DMAC_TX_BREQ", 1);

	qdev_init_gpio_in_named(dev, dif_handle_dmac_rx_clr, "DMAC_RX_CLR", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_rx_breq, "DMAC_RX_BREQ", 1);

	qdev_init_gpio_in_named(dev, dif_handle_gpio_input, "MRST_IN", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_sclk, "SCLK_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_mtsr, "MTSR_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_rs, "RS_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_cs, "CS_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_reset, "RESET_OUT", 1);
}

static void dif_realize(DeviceState *dev, Error **errp) {
	pmb887x_dif_t *p = PMB887X_DIF(dev);
	pmb887x_clc_init(&p->clc);
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
	pmb887x_srb_set_event_handler(&p->srb, dev, dif_event_handler);

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
};

static void dif_class_init(ObjectClass *klass, const void *data) {
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
