/*
 * USART
 * */
#include <stdint.h>
#define PMB887X_TRACE_ID		USART
#define PMB887X_TRACE_PREFIX	"pmb887x-usart"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/ptimer.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "chardev/char-serial.h"

#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/fifo.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_USART	"pmb887x-usart"
#define PMB887X_USART(obj)	OBJECT_CHECK(struct pmb887x_usart_t, (obj), TYPE_PMB887X_USART)

#define FIFO_SIZE	8
#define USART_LOG_TRX true
#define USART_DUMP_TRX_IO false

enum {
	USART_IRQ_TX,
	USART_IRQ_TBUF,
	USART_IRQ_RX,
	USART_IRQ_ERR,
	USART_IRQ_CTS,
	USART_IRQ_ABDET,
	USART_IRQ_ABSTART,
	USART_IRQ_TMO,
	USART_IRQ_NR
};

struct pmb887x_usart_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	pmb887x_clc_reg_t clc;
	pmb887x_srb_reg_t srb;
	qemu_irq irq[USART_IRQ_NR];
	
	bool apply_workarounds;
	
	guint watch_tag;
	CharBackend chr;
	
	pmb887x_fifo8_t tx_fifo_buffered;
	pmb887x_fifo8_t rx_fifo_buffered;
	
	pmb887x_fifo8_t tx_fifo_single;
	pmb887x_fifo8_t rx_fifo_single;
	
	pmb887x_fifo8_t *rx_fifo;
	pmb887x_fifo8_t *tx_fifo;
	
	bool last_is_icr_tx;
	
	uint32_t con;
	uint32_t bg;
	uint32_t fdv;
	uint32_t pmw;
	uint8_t txb;
	uint32_t abcon;
	uint32_t abstat;
	uint32_t rxfcon;
	uint32_t txfcon;
	uint32_t fstat;
	uint32_t whbcon;
	uint32_t whbabcon;
	uint32_t whbabstat;
	uint32_t fccon;
	uint32_t fcstat;
	uint32_t tmo;
	uint32_t unk;
};

static void usart_transmit_fifo(struct pmb887x_usart_t *p);

static void usart_set_rx_fifo(struct pmb887x_usart_t *p, bool buffered) {
	pmb887x_fifo_reset(&p->rx_fifo_buffered);
	pmb887x_fifo_reset(&p->rx_fifo_single);
	
	if (buffered) {
		p->rx_fifo = &p->rx_fifo_buffered;
	} else {
		p->rx_fifo = &p->rx_fifo_single;
	}
}

static void usart_set_tx_fifo(struct pmb887x_usart_t *p, bool buffered) {
	pmb887x_fifo_reset(&p->tx_fifo_buffered);
	pmb887x_fifo_reset(&p->tx_fifo_single);
	
	if (p->watch_tag) {
		g_source_remove(p->watch_tag);
		p->watch_tag = 0;
	}
	
	if (buffered) {
		p->tx_fifo = &p->tx_fifo_buffered;
	} else {
		p->tx_fifo = &p->tx_fifo_single;
	}
}

static void usart_update_state(struct pmb887x_usart_t *p) {
	if ((p->rxfcon & USART_RXFCON_RXFEN)) {
		if ((p->rxfcon & USART_RXFCON_RXFFLU)) {
			usart_set_rx_fifo(p, false);
			usart_set_rx_fifo(p, true);
			p->rxfcon &= ~USART_RXFCON_RXFFLU;
		}
	}
	
	if ((p->txfcon & USART_TXFCON_TXFEN)) {
		if ((p->txfcon & USART_TXFCON_TXFFLU)) {
			usart_set_tx_fifo(p, false);
			usart_set_tx_fifo(p, true);
			p->txfcon &= ~USART_TXFCON_TXFFLU;
		}
	}
}

static int usart_can_receive(void *opaque) {
	struct pmb887x_usart_t *p = (struct pmb887x_usart_t *) opaque;
	if (!pmb887x_clc_is_enabled(&p->clc))
		return 0;
	return pmb887x_fifo_free_count(p->rx_fifo);
}

static void usart_receive(void *opaque, const uint8_t *buf, int size) {
	struct pmb887x_usart_t *p = (struct pmb887x_usart_t *) opaque;
	
	if (!pmb887x_clc_is_enabled(&p->clc)) {
		DPRINTF("usart not enabled, drop %d rx chars\n", size);
		pmb887x_fifo_reset(p->rx_fifo);
		return;
	}
	
	g_assert(size <= pmb887x_fifo_free_count(p->rx_fifo));
	pmb887x_fifo8_write(p->rx_fifo, buf, size);
	
	if ((p->rxfcon & USART_RXFCON_RXFEN)) {
		uint32_t rx_level = (p->rxfcon & USART_RXFCON_RXFITL) >> USART_RXFCON_RXFITL_SHIFT;
		rx_level = MAX(1, MIN(FIFO_SIZE, rx_level));
		
		if (pmb887x_fifo_count(p->rx_fifo) >= rx_level)
			pmb887x_srb_set_isr(&p->srb, USART_ISR_RX);
	} else {
		pmb887x_srb_set_isr(&p->srb, USART_ISR_RX);
	}
}

static gboolean usart_transmit_delayed(void *do_not_use, GIOCondition cond, void *opaque) {
	struct pmb887x_usart_t *p = (struct pmb887x_usart_t *) opaque;
	p->watch_tag = 0;
	usart_transmit_fifo(p);
	return false;
}

static void usart_transmit_fifo(struct pmb887x_usart_t *p) {
	if (p->watch_tag)
		return;
	
	if (!pmb887x_clc_is_enabled(&p->clc)) {
		pmb887x_fifo_reset(p->tx_fifo);
		return;
	}
	
	bool is_full = pmb887x_fifo_is_full(p->tx_fifo);
	
	uint8_t buff[FIFO_SIZE];
	uint32_t size = pmb887x_fifo_count(p->tx_fifo);
	pmb887x_fifo8_read(p->tx_fifo, buff, size);
	
	if (USART_LOG_TRX) {
		for (uint32_t i = 0; i < size; i++) {
			DPRINTF("TX=%02X\n", buff[i]);
		}
	}
	
	int ret = qemu_chr_fe_write_all(&p->chr, buff, size);
	if (ret > 0) {
		if (ret < size) {
			// possible?
			pmb887x_fifo8_write(p->tx_fifo, buff + ret, size - ret);
		}
		
		if (is_full)
			pmb887x_srb_set_isr(&p->srb, USART_ISR_TB);
		
		if ((p->txfcon & USART_TXFCON_TXFEN)) {
			uint32_t tx_level = (p->txfcon & USART_TXFCON_TXFITL) >> USART_TXFCON_TXFITL_SHIFT;
			tx_level = MAX(1, MIN(FIFO_SIZE, tx_level));
			
			if (pmb887x_fifo_count(p->tx_fifo) <= tx_level)
				pmb887x_srb_set_isr(&p->srb, USART_ISR_TX);
		} else {
			if (pmb887x_fifo_is_empty(p->tx_fifo))
				pmb887x_srb_set_isr(&p->srb, USART_ISR_TX);
		}
	} else if (size > 0) {
		WPRINTF("qemu_chr_fe_write_all failed! size=%d, ret=%d", size, ret);
		for (uint32_t i = 0; i < size; i++)
			WPRINTF("Lost data: %02X\n", buff[i]);
	}
	
	if (!pmb887x_fifo_is_empty(p->tx_fifo)) {
		p->watch_tag = qemu_chr_fe_add_watch(&p->chr, G_IO_OUT | G_IO_HUP, usart_transmit_delayed, p);
		if (!p->watch_tag) {
			pmb887x_srb_set_isr(&p->srb, USART_ISR_TB | USART_ISR_TX);
			pmb887x_fifo_reset(p->tx_fifo);
		}
	}
}

static uint64_t usart_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb887x_usart_t *p = (struct pmb887x_usart_t *) opaque;

	uint64_t value = 0;

	// Workaround for broken firmwares
	if (haddr != USART_RIS)
		p->last_is_icr_tx = false;

	bool no_dump = true;

	switch (haddr) {
		case USART_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case USART_ID:
			value = 0x000044F1;
			break;

		case USART_CON:
			value = p->con;
			break;

		case USART_BG:
			value = p->bg;
			break;

		case USART_FDV:
			value = p->fdv;
			break;

		case USART_PMW:
			value = p->pmw;
			break;

		case USART_TXB:
			no_dump = !USART_DUMP_TRX_IO;
			value = p->txb;
			break;

		case USART_RXB:
			no_dump = !USART_DUMP_TRX_IO;

			if (!pmb887x_fifo_is_empty(p->rx_fifo)) {
				bool is_full = pmb887x_fifo_is_full(p->rx_fifo);
				value = pmb887x_fifo8_pop(p->rx_fifo);
				if (USART_LOG_TRX) {
					if (isprint(value)) {
						DPRINTF("RX=%02X '%c'\n", (uint8_t) value, (uint8_t) value);
					} else {
						DPRINTF("RX=%02X\n", (uint8_t) value);
					}
				}
				if (is_full)
					qemu_chr_fe_accept_input(&p->chr);
			}
			break;

		case USART_ABCON:
			value = p->abcon;
			break;

		case USART_ABSTAT:
			value = p->abstat;
			break;

		case USART_RXFCON:
			value = p->rxfcon;
			break;

		case USART_TXFCON:
			value = p->txfcon;
			break;

		case USART_FSTAT:
			if ((p->txfcon & USART_TXFCON_TXFEN))
				value |= pmb887x_fifo_count(p->tx_fifo) << USART_FSTAT_TXFFL_SHIFT;

			if ((p->txfcon & USART_RXFCON_RXFEN))
				value |= pmb887x_fifo_count(p->rx_fifo) << USART_FSTAT_RXFFL_SHIFT;
			break;

		case USART_WHBCON:
			value = p->whbcon;
			break;

		case USART_WHBABCON:
			value = p->whbabcon;
			break;

		case USART_WHBABSTAT:
			value = p->whbabstat;
			break;

		case USART_FCCON:
			value = p->fccon;
			break;

		case USART_FCSTAT:
			value = p->fcstat;
			break;

		case USART_IMSC:
			value = pmb887x_srb_get_imsc(&p->srb);
			break;

		case USART_RIS:
			no_dump = true;
			value = pmb887x_srb_get_ris(&p->srb);

			// workaround for broken firmwares
			if (p->apply_workarounds) {
				if (p->last_is_icr_tx) {
					DPRINTF("apply USART_RIS_TX workaround\n");
					value |= USART_RIS_TX;
				}
			}
			break;

		case USART_MIS:
			no_dump = true;
			value = pmb887x_srb_get_mis(&p->srb);
			break;

		case USART_ICR:
			no_dump = true;
			value = 0;
			break;

		case USART_ISR:
			value = 0;
			break;

		case USART_UNK:
			value = p->unk;
			break;

		case USART_TMO:
			value = p->tmo;
			break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}

	if (!no_dump)
		IO_DUMP(haddr + p->mmio.addr, size, value, false);

	return value;
}

static void usart_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct pmb887x_usart_t *p = (struct pmb887x_usart_t *) opaque;

	// Workaround for broken firmwares
	p->last_is_icr_tx = (haddr == USART_ICR && (value & USART_ICR_TX));

	bool no_dump = false;

	switch (haddr) {
		case USART_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case USART_ID:
			value = 0x000044F1;
			break;

		case USART_CON:
			p->con = value;
			break;

		case USART_BG:
			p->bg = value;
			break;

		case USART_FDV:
			p->fdv = value;
			break;

		case USART_PMW:
			p->pmw = value;
			break;

		case USART_TXB:
			p->txb = value & 0xFF;

			no_dump = !USART_DUMP_TRX_IO;

			if (!pmb887x_fifo_is_full(p->tx_fifo)) {
				pmb887x_fifo8_push(p->tx_fifo, p->txb);

				if (!pmb887x_fifo_is_full(p->tx_fifo))
					pmb887x_srb_set_isr(&p->srb, USART_ISR_TB);

				usart_transmit_fifo(p);
			} else {
				EPRINTF("TX FIFO is FULL :(\n");
				abort();
			}
			break;

		case USART_ABCON:
			p->abcon = value;
			break;

		case USART_RXFCON:
			if ((value & USART_RXFCON_RXFEN) != (p->rxfcon & USART_RXFCON_RXFEN))
				usart_set_rx_fifo(p, (value & USART_RXFCON_RXFEN) != 0);
			p->rxfcon = value;
			usart_update_state(p);
			break;

		case USART_TXFCON:
			if ((value & USART_TXFCON_TXFEN) != (p->txfcon & USART_TXFCON_TXFEN))
				usart_set_tx_fifo(p, (value & USART_TXFCON_TXFEN) != 0);
			p->txfcon = value;
			usart_update_state(p);
			break;

		case USART_WHBCON:
			p->whbcon = value;
			break;

		case USART_WHBABCON:
			p->whbabcon = value;
			break;

		case USART_WHBABSTAT:
			p->whbabstat = value;
			break;

		case USART_FCCON:
			p->fccon = value;
			break;

		case USART_IMSC:
			pmb887x_srb_set_imsc(&p->srb, value);
			break;

		case USART_ICR:
			no_dump = true;

			pmb887x_srb_set_icr(&p->srb, value);
			break;

		case USART_ISR:
			pmb887x_srb_set_isr(&p->srb, value);
			break;

		case USART_UNK:
			p->unk = value;
			break;

		case USART_TMO:
			p->tmo = value;
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}

	if (!no_dump)
		IO_DUMP(haddr + p->mmio.addr, size, value, true);
}

static const MemoryRegionOps io_ops = {
	.read			= usart_io_read,
	.write			= usart_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void usart_init(Object *obj) {
	struct pmb887x_usart_t *p = PMB887X_USART(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-usart", USART_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (int i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void usart_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_usart_t *p = PMB887X_USART(dev);
	
	pmb887x_clc_init(&p->clc);
	
	for (int i = 0; i < ARRAY_SIZE(p->irq); i++) {
		if (!p->irq[i])
			hw_error("pmb887x-usart: irq %d not set", i);
	}
	
    pmb887x_fifo8_init(&p->tx_fifo_buffered, FIFO_SIZE);
    pmb887x_fifo8_init(&p->rx_fifo_buffered, FIFO_SIZE);
	
    pmb887x_fifo8_init(&p->tx_fifo_single, 2);
    pmb887x_fifo8_init(&p->rx_fifo_single, 1);
	
	usart_set_rx_fifo(p, false);
	usart_set_tx_fifo(p, false);
	
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
	
	qemu_chr_fe_set_handlers(&p->chr, usart_can_receive, usart_receive, NULL, NULL, p, NULL, true);
	
	usart_update_state(p);
}

static const Property usart_properties[] = {
    DEFINE_PROP_CHR("chardev", struct pmb887x_usart_t, chr),
    DEFINE_PROP_BOOL("apply-workarounds", struct pmb887x_usart_t, apply_workarounds, true),
};

static void usart_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, usart_properties);
	dc->realize = usart_realize;
}

static const TypeInfo usart_info = {
    .name          	= TYPE_PMB887X_USART,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_usart_t),
    .instance_init 	= usart_init,
    .class_init    	= usart_class_init,
};

static void usart_register_types(void) {
	type_register_static(&usart_info);
}
type_init(usart_register_types)
