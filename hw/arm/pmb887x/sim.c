/*
 * PMB887x SIM card interface
 */
#define PMB887X_TRACE_ID SIM
#define PMB887X_TRACE_PREFIX "pmb887x-sim"

#include "qemu/osdep.h"
#include "qemu/fifo8.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/sim.h"
#include "hw/arm/pmb887x/sim/apdu.h"
#include "hw/arm/pmb887x/trace.h"

#define SIM_IRQ_MASK (SIM_IMSC_ERR | SIM_IMSC_IN | SIM_IMSC_OK)
#define SIM_EVENT_ENABLE_MASK ( \
	SIM_IRQEN_ENOKINT | \
	SIM_IRQEN_ENPAR | \
	SIM_IRQEN_ENOVR | \
	SIM_IRQEN_ENT0END | \
	SIM_IRQEN_ENCHTIMER | \
	SIM_IRQEN_ENBWTTIMER | \
	SIM_IRQEN_UNK6 \
)
#define SIM_CON_MASK 0xFFFF
#define SIM_BYTE_TIME_NS SCALE_MS
#define SIM_T0_TX_FIFO_SIZE 16

#define SIM_BRF_RESET 0x5D
#define SIM_RXSPC_RESET 0x28
#define SIM_CHTIMER_RESET 0x2580
#define SIM_BWT_RESET 0x3C0B

#define SIM_CARD_CLOCK_HZ 3250000
#define SIM_BRF_CLOCKS_PER_ETU 4

enum {
	SIM_IRQ_ERR,
	SIM_IRQ_IN,
	SIM_IRQ_OK,
	SIM_IRQ_COUNT,
};

enum sim_t0_state_t {
	SIM_T0_STATE_IDLE,
	SIM_T0_STATE_HEADER,
	SIM_T0_STATE_PROCEDURE,
	SIM_T0_STATE_TX_DATA,
	SIM_T0_STATE_RX_DATA,
	SIM_T0_STATE_SW1,
	SIM_T0_STATE_SW2,
};

struct pmb887x_sim_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	uint32_t revision;
	qemu_irq irq[SIM_IRQ_COUNT];

	pmb887x_clc_reg_t clc;
	pmb887x_srb_reg_t srb;
	CharFrontend chr;
	QEMUTimer *tx_timer;
	QEMUTimer *character_timer;
	Fifo8 t0_tx_fifo;

	uint32_t con;
	uint32_t brf;
	uint32_t stat;
	uint32_t irqen;
	uint32_t rxspc;
	uint32_t txspc;
	uint32_t chtimer;
	uint32_t unk3c;
	uint32_t unk40;
	uint32_t bwt;
	uint32_t txb;
	uint32_t rxb;
	uint32_t ins;
	uint32_t p3;
	uint32_t sw1;
	uint32_t sw2;

	bool tx_pending;
	bool rx_pending;
	bool cc_io_input;
	bool dmac_tx_clr;
	bool dmac_rx_clr;
	bool dma_event_pending;
	enum sim_t0_state_t t0_state;
	uint16_t t0_header_size;
	uint16_t t0_data_size;
	uint16_t t0_data_transferred;
	uint16_t t0_chunk_remaining;

	qemu_irq cc_rst;
	qemu_irq cc_io;
	qemu_irq cc_clk;
	qemu_irq dmac_tx_breq;
	qemu_irq dmac_rx_breq;
};

static bool sim_is_uart_running(pmb887x_sim_t *p) {
	return pmb887x_clc_is_enabled(&p->clc) && pmb887x_clc_get_rmc(&p->clc) != 0 &&
		(p->con & (SIM_CON_SIMEN | SIM_CON_UARTON)) == (SIM_CON_SIMEN | SIM_CON_UARTON);
}

static bool sim_is_t0_running(pmb887x_sim_t *p) {
	return sim_is_uart_running(p) && (p->con & (SIM_CON_SIMT0 | SIM_CON_SIMRST)) == (SIM_CON_SIMT0 | SIM_CON_SIMRST);
}

static void sim_set_dma_request(pmb887x_sim_t *p, bool level) {
	qemu_set_irq(p->dmac_tx_breq, level);
	qemu_set_irq(p->dmac_rx_breq, level);
}

static void sim_update_dma_request(pmb887x_sim_t *p) {
	bool request;
	if (sim_is_t0_running(p)) {
		bool tx_ready = (
			(p->t0_state == SIM_T0_STATE_HEADER || p->t0_state == SIM_T0_STATE_TX_DATA) &&
			!p->tx_pending && fifo8_is_empty(&p->t0_tx_fifo)
		);
		request = !p->dmac_tx_clr && !p->dmac_rx_clr &&
			(pmb887x_srb_get_dmae(&p->srb) & SIM_DMAE_OK) != 0 && (tx_ready || p->rx_pending);
	} else {
		request = !p->dmac_tx_clr && !p->dmac_rx_clr &&
			p->dma_event_pending && (pmb887x_srb_get_dmae(&p->srb) & SIM_DMAE_OK) != 0;
	}
	sim_set_dma_request(p, request);
}

static void sim_update_outputs(pmb887x_sim_t *p) {
	bool enabled = (p->con & SIM_CON_SIMEN) != 0;
	qemu_set_irq(p->cc_rst, enabled && (p->con & SIM_CON_SIMRST) != 0);
	qemu_set_irq(p->cc_clk, enabled && (p->con & SIM_CON_SIMON) != 0);
	qemu_set_irq(p->cc_io, enabled && (p->con & SIM_CON_SIMIOL) != 0);
}

static void sim_raise_status(pmb887x_sim_t *p, uint32_t status) {
	p->stat |= status;

	if ((status & SIM_STAT_UARTOK) && (p->irqen & SIM_IRQEN_ENOKINT))
		pmb887x_srb_set_isr(&p->srb, SIM_ISR_OK);
	if ((status & SIM_STAT_PARINT) && (p->irqen & SIM_IRQEN_ENPAR))
		pmb887x_srb_set_isr(&p->srb, SIM_ISR_ERR);
	if ((status & SIM_STAT_OVRRUN) && (p->irqen & SIM_IRQEN_ENOVR))
		pmb887x_srb_set_isr(&p->srb, SIM_ISR_ERR);
	if ((status & SIM_STAT_T0END) && (p->irqen & SIM_IRQEN_ENT0END))
		pmb887x_srb_set_isr(&p->srb, SIM_ISR_ERR);
	if ((status & SIM_STAT_CHTIMEOUT) && (p->irqen & SIM_IRQEN_ENCHTIMER))
		pmb887x_srb_set_isr(&p->srb, SIM_ISR_ERR);
}

static void sim_update_character_timer(pmb887x_sim_t *p) {
	bool character_timer_enabled = (
		sim_is_t0_running(p) &&
		(p->t0_state == SIM_T0_STATE_PROCEDURE || p->t0_state == SIM_T0_STATE_RX_DATA ||
			p->t0_state == SIM_T0_STATE_SW1 || p->t0_state == SIM_T0_STATE_SW2) &&
		(p->con & SIM_CON_RPTOFF) == 0 &&
		p->chtimer != 0 &&
		p->brf != 0
	);
	timer_del(p->character_timer);
	if (!character_timer_enabled)
		return;

	uint64_t timeout_ns = muldiv64(
		(uint64_t) p->chtimer * p->brf * SIM_BRF_CLOCKS_PER_ETU,
		NANOSECONDS_PER_SECOND,
		SIM_CARD_CLOCK_HZ
	);
	timer_mod(p->character_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + timeout_ns);
}

static void sim_character_timeout(void *opaque) {
	pmb887x_sim_t *p = opaque;
	sim_raise_status(p, SIM_STAT_CHTIMEOUT);
}

static void sim_t0_reset(pmb887x_sim_t *p, bool start) {
	timer_del(p->tx_timer);
	timer_del(p->character_timer);
	fifo8_reset(&p->t0_tx_fifo);
	p->tx_pending = false;
	p->rx_pending = false;
	p->t0_state = start ? SIM_T0_STATE_HEADER : SIM_T0_STATE_IDLE;
	p->t0_header_size = 0;
	p->t0_data_size = 0;
	p->t0_data_transferred = 0;
	p->t0_chunk_remaining = 0;
	sim_update_dma_request(p);
}

static void sim_t0_finish(pmb887x_sim_t *p) {
	DPRINTF("T=0 complete: SW=%02X%02X\n", p->sw1, p->sw2);
	timer_del(p->character_timer);
	p->t0_state = SIM_T0_STATE_HEADER;
	p->t0_header_size = 0;
	p->t0_data_size = 0;
	p->t0_data_transferred = 0;
	p->t0_chunk_remaining = 0;
	sim_raise_status(p, SIM_STAT_T0END);
	sim_update_dma_request(p);
}

static void sim_t0_receive(pmb887x_sim_t *p, uint8_t value) {
	uint8_t instruction = p->ins & SIM_INS_INS;
	timer_del(p->character_timer);

	switch (p->t0_state) {
		case SIM_T0_STATE_PROCEDURE:
			if (value == PMB887X_T0_NULL_BYTE) {
				sim_update_character_timer(p);
				return;
			}
			if (value == instruction || value == (uint8_t) (~instruction)) {
				uint16_t remaining = p->t0_data_size - p->t0_data_transferred;
				p->t0_chunk_remaining = value == instruction ? remaining : MIN(remaining, 1);
				p->t0_state = (p->ins & SIM_INS_INSDIR) ? SIM_T0_STATE_RX_DATA : SIM_T0_STATE_TX_DATA;
				DPRINTF("T=0 procedure: %02X, %s %u byte(s)\n", value,
					(p->ins & SIM_INS_INSDIR) ? "receive" : "transmit", p->t0_chunk_remaining);
				sim_update_dma_request(p);
				sim_update_character_timer(p);
				return;
			}
			p->sw1 = value;
			p->t0_state = SIM_T0_STATE_SW2;
			sim_update_character_timer(p);
			return;
		case SIM_T0_STATE_RX_DATA:
			p->rxb = value;
			p->rx_pending = true;
			p->t0_data_transferred++;
			p->t0_chunk_remaining--;
			if (p->t0_data_transferred == p->t0_data_size)
				p->t0_state = SIM_T0_STATE_SW1;
			else if (p->t0_chunk_remaining == 0)
				p->t0_state = SIM_T0_STATE_PROCEDURE;
			sim_update_dma_request(p);
			sim_update_character_timer(p);
			return;
		case SIM_T0_STATE_SW1:
			if (value == PMB887X_T0_NULL_BYTE) {
				sim_update_character_timer(p);
				return;
			}
			p->sw1 = value;
			p->t0_state = SIM_T0_STATE_SW2;
			sim_update_character_timer(p);
			return;
		case SIM_T0_STATE_SW2:
			p->sw2 = value;
			sim_t0_finish(p);
			return;
		default:
			sim_raise_status(p, SIM_STAT_OVRRUN);
			return;
	}
}

static int sim_can_receive(void *opaque) {
	pmb887x_sim_t *p = opaque;
	return sim_is_uart_running(p) && !p->rx_pending ? 1 : 0;
}

static void sim_receive(void *opaque, const uint8_t *buffer, int size) {
	pmb887x_sim_t *p = opaque;

	if (!sim_is_uart_running(p) || size == 0)
		return;
	if (sim_is_t0_running(p)) {
		sim_t0_receive(p, buffer[0]);
		return;
	}

	if (p->rx_pending) {
		sim_raise_status(p, SIM_STAT_OVRRUN);
		return;
	}

	p->rxb = buffer[0];
	p->rx_pending = true;
	sim_raise_status(p, SIM_STAT_UARTOK);
}

void pmb887x_sim_set_chardev(pmb887x_sim_t *p, Chardev *chardev, Error **errp) {
	if (p->chr.chr) {
		error_setg(errp, "pmb887x-sim already has a chardev");
		return;
	}
	if (!qemu_chr_fe_init(&p->chr, chardev, errp))
		return;
	qemu_chr_fe_set_handlers(&p->chr, sim_can_receive, sim_receive, NULL, NULL, p, NULL, true);
}

static void sim_tx_complete(void *opaque) {
	pmb887x_sim_t *p = opaque;

	if (!fifo8_is_empty(&p->t0_tx_fifo)) {
		uint8_t value = fifo8_pop(&p->t0_tx_fifo);
		qemu_chr_fe_write_all(&p->chr, &value, 1);
		if (!fifo8_is_empty(&p->t0_tx_fifo)) {
			timer_mod(p->tx_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SIM_BYTE_TIME_NS);
		} else {
			p->tx_pending = false;
			sim_update_dma_request(p);
			sim_update_character_timer(p);
		}
		return;
	}

	p->tx_pending = false;
	uint8_t value = p->txb;
	qemu_chr_fe_write_all(&p->chr, &value, 1);
	sim_raise_status(p, SIM_STAT_UARTOK);
}

static void sim_t0_write_txb(pmb887x_sim_t *p, uint8_t value) {
	if (fifo8_is_full(&p->t0_tx_fifo)) {
		sim_raise_status(p, SIM_STAT_OVRRUN);
		return;
	}
	if (p->t0_state != SIM_T0_STATE_HEADER && p->t0_state != SIM_T0_STATE_TX_DATA) {
		sim_raise_status(p, SIM_STAT_OVRRUN);
		return;
	}

	p->txb = value;
	fifo8_push(&p->t0_tx_fifo, value);
	if (p->t0_state == SIM_T0_STATE_HEADER) {
		p->t0_header_size++;
		if (p->t0_header_size == PMB887X_APDU_HEADER_SIZE) {
			p->t0_state = SIM_T0_STATE_PROCEDURE;
			p->t0_data_size = p->p3 ? p->p3 : PMB887X_APDU_MAX_DATA_SIZE;
			DPRINTF("T=0 header queued: INS=%02X, P3=%u\n", p->ins & SIM_INS_INS, p->t0_data_size);
		}
	} else {
		p->t0_data_transferred++;
		p->t0_chunk_remaining--;
		if (p->t0_data_transferred == p->t0_data_size)
			p->t0_state = SIM_T0_STATE_SW1;
		else if (p->t0_chunk_remaining == 0)
			p->t0_state = SIM_T0_STATE_PROCEDURE;
	}

	if (!p->tx_pending) {
		p->tx_pending = true;
		timer_mod(p->tx_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SIM_BYTE_TIME_NS);
	}
}

static void sim_write_txb(pmb887x_sim_t *p, uint8_t value) {
	if (!sim_is_uart_running(p))
		return;
	if (sim_is_t0_running(p)) {
		sim_t0_write_txb(p, value);
		return;
	}

	if (p->tx_pending) {
		sim_raise_status(p, SIM_STAT_OVRRUN);
		return;
	}

	p->txb = value;
	p->tx_pending = true;
	timer_mod(p->tx_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SIM_BYTE_TIME_NS);
}

static uint64_t sim_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_sim_t *p = opaque;
	uint64_t value = 0;

	switch (haddr) {
		case SIM_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;
		case SIM_ID:
			value = 0xF000C000 | p->revision;
			break;
		case SIM_CON:
			value = p->con;
			break;
		case SIM_BRF:
			value = p->brf;
			break;
		case SIM_STAT:
			value = p->stat;
			break;
		case SIM_IRQEN:
			value = p->irqen;
			break;
		case SIM_RXSPC:
			value = p->rxspc;
			break;
		case SIM_TXSPC:
			value = p->txspc;
			break;
		case SIM_CHTIMER:
			value = p->chtimer;
			break;
		case SIM_UNK3C:
			value = p->unk3c;
			break;
		case SIM_UNK40:
			value = p->unk40;
			break;
		case SIM_BWT:
			value = p->bwt;
			break;
		case SIM_TXB:
			value = p->txb;
			break;
		case SIM_RXB:
			value = p->rxb;
			p->rx_pending = false;
			qemu_chr_fe_accept_input(&p->chr);
			sim_update_dma_request(p);
			break;
		case SIM_INS:
			value = p->ins;
			break;
		case SIM_P3:
			value = p->p3;
			break;
		case SIM_SW1:
			value = p->sw1;
			break;
		case SIM_SW2:
			value = p->sw2;
			break;
		case SIM_IMSC:
			value = pmb887x_srb_get_imsc(&p->srb);
			break;
		case SIM_RIS:
			value = pmb887x_srb_get_ris(&p->srb);
			break;
		case SIM_MIS:
			value = pmb887x_srb_get_mis(&p->srb);
			break;
		case SIM_ICR:
		case SIM_ISR:
			break;
		case SIM_DMAE:
			value = pmb887x_srb_get_dmae(&p->srb);
			break;
		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			return 0;
	}

	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	return value;
}

static void sim_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_sim_t *p = opaque;
	IO_DUMP(haddr + p->mmio.addr, size, value, true);

	switch (haddr) {
		case SIM_CLC:
			pmb887x_clc_set(&p->clc, value);
			if (!pmb887x_clc_is_enabled(&p->clc))
				sim_t0_reset(p, false);
			sim_update_dma_request(p);
			break;
		case SIM_CON: {
			bool was_t0_running = sim_is_t0_running(p);
			p->con = value & SIM_CON_MASK;
			if (!sim_is_uart_running(p)) {
				sim_t0_reset(p, false);
			} else {
				bool is_t0_running = sim_is_t0_running(p);
				if (!was_t0_running && is_t0_running) {
					sim_t0_reset(p, true);
				} else if (was_t0_running && !is_t0_running) {
					sim_t0_reset(p, false);
				}
			}
			sim_update_outputs(p);
			sim_update_dma_request(p);
			break;
		}
		case SIM_BRF:
			p->brf = value & SIM_BRF_BRF;
			sim_update_character_timer(p);
			break;
		case SIM_IRQEN:
			p->irqen = value & SIM_EVENT_ENABLE_MASK;
			break;
		case SIM_RXSPC:
			p->rxspc = value & SIM_RXSPC_RXSPC;
			break;
		case SIM_TXSPC:
			p->txspc = value & SIM_TXSPC_TXSPC;
			break;
		case SIM_CHTIMER:
			p->chtimer = value & SIM_CHTIMER_CHTIMER;
			sim_update_character_timer(p);
			break;
		case SIM_UNK3C:
			p->unk3c = value & SIM_UNK3C_VALUE;
			break;
		case SIM_UNK40:
			p->unk40 = value & SIM_UNK40_VALUE;
			break;
		case SIM_BWT:
			p->bwt = value & SIM_BWT_BWT;
			break;
		case SIM_TXB:
			sim_write_txb(p, value);
			break;
		case SIM_INS:
			p->ins = value & (SIM_INS_INS | SIM_INS_INSDIR);
			break;
		case SIM_P3:
			p->p3 = value & SIM_P3_P3;
			break;
		case SIM_SW1:
			p->sw1 = value & SIM_SW1_SW1;
			break;
		case SIM_SW2:
			p->sw2 = value & SIM_SW2_SW2;
			break;
		case SIM_IMSC:
			pmb887x_srb_set_imsc(&p->srb, value & SIM_IRQ_MASK);
			break;
		case SIM_ICR:
			pmb887x_srb_set_icr(&p->srb, value & SIM_IRQ_MASK);
			if ((value & SIM_ICR_OK))
				p->stat &= ~SIM_STAT_UARTOK;
			if ((value & SIM_ICR_ERR))
				p->stat &= ~(SIM_STAT_PARINT | SIM_STAT_OVRRUN | SIM_STAT_T0END | SIM_STAT_CHTIMEOUT);
			break;
		case SIM_ISR:
			pmb887x_srb_set_isr(&p->srb, value & SIM_IRQ_MASK);
			break;
		case SIM_DMAE: {
			pmb887x_srb_set_dmae(&p->srb, value & SIM_DMAE_OK);
			if ((value & SIM_DMAE_OK) == 0)
				p->dma_event_pending = false;
			sim_update_dma_request(p);
			break;
		}
		case SIM_ID:
		case SIM_STAT:
		case SIM_RXB:
		case SIM_RIS:
		case SIM_MIS:
			break;
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}
}

static const MemoryRegionOps sim_io_ops = {
	.read = sim_io_read,
	.write = sim_io_write,
	.endianness = DEVICE_NATIVE_ENDIAN,
	.valid = {
		.min_access_size = 1,
		.max_access_size = 4,
	},
};

static void sim_handle_cc_io(void *opaque, int id, int level) {
	pmb887x_sim_t *p = opaque;
	p->cc_io_input = level != 0;
}

static void sim_handle_dmac_tx_clr(void *opaque, int id, int level) {
	pmb887x_sim_t *p = opaque;
	p->dmac_tx_clr = level != 0;
	if (level)
		pmb887x_srb_set_icr(&p->srb, SIM_ICR_OK);
	sim_update_dma_request(p);
}

static void sim_handle_dmac_rx_clr(void *opaque, int id, int level) {
	pmb887x_sim_t *p = opaque;
	p->dmac_rx_clr = level != 0;
	if (level)
		pmb887x_srb_set_icr(&p->srb, SIM_ICR_OK);
	sim_update_dma_request(p);
}

static void sim_event_handler(void *opaque, int event_id, int level) {
	pmb887x_sim_t *p = opaque;
	if (event_id != SIM_RIS_OK_SHIFT)
		return;
	if (!level) {
		p->dma_event_pending = false;
	} else if ((pmb887x_srb_get_dmae(&p->srb) & SIM_DMAE_OK) != 0) {
		p->dma_event_pending = true;
	}
	sim_update_dma_request(p);
}

static void sim_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_sim_t *p = PMB887X_SIM(obj);

	memory_region_init_io(&p->mmio, obj, &sim_io_ops, p, TYPE_PMB887X_SIM, SIM_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	for (size_t i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);

	qdev_init_gpio_in_named(dev, sim_handle_cc_io, "CC_IO_IN", 1);
	qdev_init_gpio_out_named(dev, &p->cc_io, "CC_IO_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->cc_clk, "CC_CLK_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->cc_rst, "CC_RST_OUT", 1);

	qdev_init_gpio_in_named(dev, sim_handle_dmac_tx_clr, "DMAC_TX_CLR", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_tx_breq, "DMAC_TX_BREQ", 1);
	qdev_init_gpio_in_named(dev, sim_handle_dmac_rx_clr, "DMAC_RX_CLR", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_rx_breq, "DMAC_RX_BREQ", 1);
}

static void sim_realize(DeviceState *dev, Error **errp) {
	pmb887x_sim_t *p = PMB887X_SIM(dev);
	pmb887x_clc_init(&p->clc);
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
	pmb887x_srb_set_event_handler(&p->srb, p, sim_event_handler);
	if (p->chr.chr)
		qemu_chr_fe_set_handlers(&p->chr, sim_can_receive, sim_receive, NULL, NULL, p, NULL, true);
	p->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sim_tx_complete, p);
	p->character_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sim_character_timeout, p);
	fifo8_create(&p->t0_tx_fifo, SIM_T0_TX_FIFO_SIZE);
}

static void sim_reset(DeviceState *dev) {
	pmb887x_sim_t *p = PMB887X_SIM(dev);

	timer_del(p->tx_timer);
	timer_del(p->character_timer);
	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	pmb887x_srb_reset(&p->srb);

	p->con = 0;
	p->brf = SIM_BRF_RESET;
	p->stat = 0;
	p->irqen = 0;
	p->rxspc = SIM_RXSPC_RESET;
	p->txspc = 0;
	p->chtimer = SIM_CHTIMER_RESET;
	p->unk3c = 0;
	p->unk40 = 0;
	p->bwt = SIM_BWT_RESET;
	p->txb = 0;
	p->rxb = 0;
	p->ins = 0;
	p->p3 = 0;
	p->sw1 = 0;
	p->sw2 = 0;
	p->tx_pending = false;
	p->rx_pending = false;
	p->cc_io_input = false;
	p->dmac_tx_clr = false;
	p->dmac_rx_clr = false;
	p->dma_event_pending = false;
	p->t0_state = SIM_T0_STATE_IDLE;
	p->t0_header_size = 0;
	p->t0_data_size = 0;
	p->t0_data_transferred = 0;
	p->t0_chunk_remaining = 0;
	fifo8_reset(&p->t0_tx_fifo);

	sim_set_dma_request(p, false);
	sim_update_outputs(p);
}

static void sim_unrealize(DeviceState *dev) {
	pmb887x_sim_t *p = PMB887X_SIM(dev);
	if (p->chr.chr)
		qemu_chr_fe_deinit(&p->chr, false);
}

static void sim_finalize(Object *obj) {
	pmb887x_sim_t *p = PMB887X_SIM(obj);
	if (p->tx_timer)
		timer_free(p->tx_timer);
	if (p->character_timer)
		timer_free(p->character_timer);
	if (p->t0_tx_fifo.data)
		fifo8_destroy(&p->t0_tx_fifo);
}

static const Property sim_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_sim_t, revision, 0),
	DEFINE_PROP_CHR("chardev", pmb887x_sim_t, chr),
};

static void sim_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *device_class = DEVICE_CLASS(klass);
	device_class_set_props(device_class, sim_properties);
	device_class_set_legacy_reset(device_class, sim_reset);
	device_class->realize = sim_realize;
	device_class->unrealize = sim_unrealize;
}

static const TypeInfo sim_info = {
	.name = TYPE_PMB887X_SIM,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(pmb887x_sim_t),
	.instance_init = sim_init,
	.instance_finalize = sim_finalize,
	.class_init = sim_class_init,
};

static void sim_register_types(void) {
	type_register_static(&sim_info);
}
type_init(sim_register_types)
