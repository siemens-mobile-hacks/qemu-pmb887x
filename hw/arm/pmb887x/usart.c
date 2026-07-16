/*
 * USART
 * */
#define PMB887X_TRACE_ID		USART
#define PMB887X_TRACE_PREFIX	"pmb887x-usart"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/hw-error.h"
#include "system/memory.h"
#include "cpu.h"
#include "qemu/main-loop.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "chardev/char-serial.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/fifo.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_USART	"pmb887x-usart"
#define PMB887X_USART(obj)	OBJECT_CHECK(pmb887x_usart_t, (obj), TYPE_PMB887X_USART)

#define USART_SEND_FULL_FIFO	1
#define USART_IMMEDIATE_TRANSFER	1
#define USART_FIFO_SIZE			8

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

typedef struct pmb887x_usart_t pmb887x_usart_t;

struct pmb887x_usart_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	pmb887x_clc_reg_t clc;
	pmb887x_srb_reg_t srb;
	qemu_irq irq[USART_IRQ_NR];
	pmb887x_pll_t *pll;

	QEMUTimer *timer;
	QEMUTimer *tmo_timer;
	bool transfer_pending;

	CharFrontend chr;
	guint watch_tag;

	// FIFO enabled
	pmb887x_fifo16_t tx_fifo_buffered;
	pmb887x_fifo16_t rx_fifo_buffered;

	// FIFO disabled
	pmb887x_fifo16_t tx_fifo_single;
	pmb887x_fifo16_t rx_fifo_single;

	// Pending frame to transmit
	pmb887x_fifo16_t tx_buffer;

	// Pointer to current FIFO
	pmb887x_fifo16_t *rx_fifo;
	pmb887x_fifo16_t *tx_fifo;

#if USART_IMMEDIATE_TRANSFER
	int ris_read_count;
#endif

	uint32_t pisel;
	uint32_t con;
	uint32_t bg;
	uint32_t fdv;
	uint32_t pmw;
	uint16_t txb;
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
	uint32_t dma_control;

	qemu_irq gpio_txd;
	qemu_irq gpio_rts;

	int dmac_tx_clr;
	int dmac_rx_clr;

	qemu_irq dmac_tx_breq;
	qemu_irq dmac_rx_breq;
};

static void usart_update_state(pmb887x_usart_t *p);
static void usart_transmit_fifo(pmb887x_usart_t *p);

static uint32_t usart_get_baud_rate(pmb887x_usart_t *p) {
	uint32_t rmc = pmb887x_clc_get_rmc(&p->clc);
	uint64_t frequency = rmc > 0 ? pmb887x_pll_get_fsys(p->pll) / rmc : 0;
	uint64_t reload = (p->bg & 0x1FFF) + 1;
	uint64_t numerator;
	uint64_t denominator;

	if (!pmb887x_clc_is_enabled(&p->clc) || frequency == 0)
		return 0;

	if ((p->con & USART_CON_M) == USART_CON_M_SYNC_8BIT) {
		numerator = frequency;
		denominator = ((p->con & USART_CON_BRS) ? 12 : 8) * reload;
	} else if ((p->con & USART_CON_FDE)) {
		uint64_t divider = (p->fdv & 0x1FF);
		numerator = frequency * (divider ? divider : 512);
		denominator = 512 * 16 * reload;
	} else {
		numerator = frequency;
		denominator = ((p->con & USART_CON_BRS) ? 48 : 32) * reload;
	}

	return numerator / denominator;
}

static int64_t usart_baud_ticks_to_ns(pmb887x_usart_t *p, uint64_t ticks) {
	uint32_t baud_rate = usart_get_baud_rate(p);
	if (baud_rate == 0)
		return 0;

	return MAX(1, (int64_t) muldiv64(ticks, NANOSECONDS_PER_SECOND, baud_rate));
}

static uint32_t usart_frame_bits(pmb887x_usart_t *p) {
	uint32_t mode = (p->con & USART_CON_M);
	if (mode == USART_CON_M_SYNC_8BIT)
		return 8;

	uint32_t data_bits = 8;
	bool has_ninth_bit = mode == USART_CON_M_ASYNC_PARITY_8BIT ||
		mode == USART_CON_M_ASYNC_9BIT ||
		mode == USART_CON_M_ASYNC_WAKE_UP_8BIT;
	if (has_ninth_bit)
		data_bits = 9;

	uint32_t stop_bits = (p->con & USART_CON_STP) ? 2 : 1;
	return 1 + data_bits + stop_bits;
}

static bool usart_is_rx_fifo_enabled(pmb887x_usart_t *p) {
	return (p->rxfcon & USART_RXFCON_RXFEN) != 0;
}

static bool usart_is_tx_fifo_enabled(pmb887x_usart_t *p) {
	return (p->txfcon & USART_TXFCON_TXFEN) != 0;
}

static void usart_rx_fifo_config(pmb887x_usart_t *p, uint32_t value) {
	bool old_fifo_enabled = (p->rxfcon & USART_RXFCON_RXFEN) != 0;
	bool new_fifo_enabled = (value & USART_RXFCON_RXFEN) != 0;
	if (old_fifo_enabled != new_fifo_enabled) {
		p->rx_fifo = new_fifo_enabled ? &p->rx_fifo_buffered : &p->rx_fifo_single;
		pmb887x_fifo_reset(p->rx_fifo);
	}
	p->rxfcon = value;
	usart_update_state(p);
}

static void usart_tx_fifo_config(pmb887x_usart_t *p, uint32_t value) {
	bool old_fifo_enabled = (p->txfcon & USART_TXFCON_TXFEN) != 0;
	bool new_fifo_enabled = (value & USART_TXFCON_TXFEN) != 0;
	if (old_fifo_enabled != new_fifo_enabled) {
		p->tx_fifo = new_fifo_enabled ? &p->tx_fifo_buffered : &p->tx_fifo_single;
		pmb887x_fifo_reset(p->tx_fifo);
	}

	bool old_transparent_mode = (p->txfcon & USART_TXFCON_TXTMEN) != 0 && old_fifo_enabled;
	bool new_transparent_mode = (value & USART_TXFCON_TXTMEN) != 0 && new_fifo_enabled;
	if (!old_transparent_mode && new_transparent_mode)
		pmb887x_srb_set_isr(&p->srb, USART_ISR_TB);

	p->txfcon = value;
	usart_update_state(p);
}

static bool usart_tb_irq(pmb887x_usart_t *p) {
	if (usart_is_tx_fifo_enabled(p)) {
		if ((p->txfcon & USART_TXFCON_TXTMEN))
			return !pmb887x_fifo_is_full(p->tx_fifo);
		uint32_t tx_level = (p->txfcon & USART_TXFCON_TXFITL) >> USART_TXFCON_TXFITL_SHIFT;
		tx_level = MAX(1, MIN(USART_FIFO_SIZE, tx_level));
		return pmb887x_fifo_count(p->tx_fifo) <= tx_level;
	}
	return pmb887x_fifo_is_empty(p->tx_fifo);
}

static bool usart_tx_irq(pmb887x_usart_t *p) {
	if (usart_is_tx_fifo_enabled(p)) {
		if ((p->txfcon & USART_TXFCON_TXTMEN))
			return true;
		uint32_t tx_level = (p->txfcon & USART_TXFCON_TXFITL) >> USART_TXFCON_TXFITL_SHIFT;
		tx_level = MAX(1, MIN(USART_FIFO_SIZE, tx_level));
		return pmb887x_fifo_count(p->tx_fifo) <= tx_level;
	}
	return pmb887x_fifo_is_empty(p->tx_fifo);
}

static bool usart_rx_irq(pmb887x_usart_t *p) {
	if (usart_is_rx_fifo_enabled(p)) {
		if ((p->rxfcon & USART_RXFCON_RXTMEN))
			return !pmb887x_fifo_is_empty(p->rx_fifo);
		uint32_t rx_level = (p->rxfcon & USART_RXFCON_RXFITL) >> USART_RXFCON_RXFITL_SHIFT;
		rx_level = MAX(1, MIN(USART_FIFO_SIZE, rx_level));
		return pmb887x_fifo_count(p->rx_fifo) >= rx_level;
	}
	return !pmb887x_fifo_is_empty(p->rx_fifo);
}

static void usart_handle_dma(pmb887x_usart_t *p) {
	bool tx_request = !p->dmac_tx_clr && (p->dma_control & USART_DMAE_TX) && pmb887x_fifo_is_empty(p->tx_fifo);
	bool rx_request = !p->dmac_rx_clr && (p->dma_control & USART_DMAE_RX) && usart_rx_irq(p);

	qemu_set_irq(p->dmac_tx_breq, tx_request);
	qemu_set_irq(p->dmac_rx_breq, rx_request);
}

static void usart_tmo_timer_reset(void *opaque) {
	pmb887x_usart_t *p = opaque;
	pmb887x_srb_set_isr(&p->srb, USART_ISR_TMO);
}

static void usart_timer_reset(void *opaque) {
	pmb887x_usart_t *p = opaque;
	usart_transmit_fifo(p);
}

#if USART_IMMEDIATE_TRANSFER
static void usart_immediate_transfer(pmb887x_usart_t *p) {
	if (p->watch_tag || !p->transfer_pending)
		return;
	timer_del(p->timer);
	usart_transmit_fifo(p);
}
#endif

static void usart_update_state(pmb887x_usart_t *p) {
	if (usart_is_rx_fifo_enabled(p)) {
		if ((p->rxfcon & USART_RXFCON_RXFFLU)) {
			pmb887x_fifo_reset(p->rx_fifo);
			p->rxfcon &= ~USART_RXFCON_RXFFLU;
		}
	}

	if (usart_is_tx_fifo_enabled(p)) {
		if ((p->txfcon & USART_TXFCON_TXFFLU)) {
			pmb887x_fifo_reset(p->tx_fifo);
			p->txfcon &= ~USART_TXFCON_TXFFLU;
			pmb887x_srb_set_isr(&p->srb, USART_ISR_TB);
		}
	}

	if (!pmb887x_clc_is_enabled(&p->clc)) {
		pmb887x_fifo_reset(&p->tx_buffer);
		pmb887x_fifo_reset(p->tx_fifo);
		pmb887x_fifo_reset(p->rx_fifo);
		timer_del(p->tmo_timer);

		if (p->transfer_pending) {
			timer_del(p->timer);
			p->transfer_pending = false;
		}

		if (p->watch_tag) {
			g_source_remove(p->watch_tag);
			p->watch_tag = 0;
		}
	}

	if (p->whbcon) {
		if (p->whbcon & USART_WHBCON_CLRREN)
			p->con &= ~USART_CON_REN;
		if (p->whbcon & USART_WHBCON_SETREN)
			p->con |= USART_CON_REN;

		if (p->whbcon & USART_WHBCON_CLRPE)
			p->con &= ~USART_CON_PE;
		if (p->whbcon & USART_WHBCON_SETPE)
			p->con |= USART_CON_PE;

		if (p->whbcon & USART_WHBCON_CLRFE)
			p->con &= ~USART_CON_FE;
		if (p->whbcon & USART_WHBCON_SETFE)
			p->con |= USART_CON_FE;

		if (p->whbcon & USART_WHBCON_CLROE)
			p->con &= ~USART_CON_OE;
		if (p->whbcon & USART_WHBCON_SETOE)
			p->con |= USART_CON_OE;

		p->whbcon = 0;
	}

	if (!p->tmo) {
		timer_del(p->tmo_timer);
	}
}

static int usart_can_receive(void *opaque) {
	pmb887x_usart_t *p = opaque;
	if (!pmb887x_clc_is_enabled(&p->clc))
		return 0;
	return pmb887x_fifo_free_count(p->rx_fifo);
}

static void usart_receive_word(pmb887x_usart_t *p, uint16_t value) {
	if (pmb887x_fifo_is_full(p->rx_fifo)) {
		pmb887x_fifo16_replace_last(p->rx_fifo, value);
		p->con |= USART_CON_OE;
		pmb887x_srb_set_isr(&p->srb, USART_ISR_RX | USART_ISR_ERR);
	} else {
		pmb887x_fifo16_push(p->rx_fifo, value);
	}
}

static void usart_receive_complete(pmb887x_usart_t *p) {
	if (p->tmo > 0) {
		int64_t timeout_ns = usart_baud_ticks_to_ns(p, p->tmo);
		if (timeout_ns > 0) {
			int64_t virtual = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
			timer_mod_ns(p->tmo_timer, virtual + timeout_ns);
		}
	}

	if (usart_rx_irq(p))
		pmb887x_srb_set_isr(&p->srb, USART_ISR_RX);

	usart_handle_dma(p);
}

static void usart_receive(void *opaque, const uint8_t *buf, int size) {
	pmb887x_usart_t *p = opaque;

	for (int i = 0; i < size; i++)
		usart_receive_word(p, buf[i]);

	if (size > 0)
		usart_receive_complete(p);
}

static void usart_schedule_transmit(pmb887x_usart_t *p) {
	uint16_t word = pmb887x_fifo16_pop(p->tx_fifo);
	pmb887x_fifo16_push(&p->tx_buffer, word);
#if USART_IMMEDIATE_TRANSFER
	p->ris_read_count = 0;
#endif
	p->transfer_pending = true;
	int64_t frame_time_ns = usart_baud_ticks_to_ns(p, usart_frame_bits(p));
	int64_t virtual = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	timer_mod(p->timer, virtual + frame_time_ns);
	if (usart_tb_irq(p))
		pmb887x_srb_set_isr(&p->srb, USART_ISR_TB);
	usart_handle_dma(p);
}

static gboolean usart_transmit_fifo_delayed(void *do_not_use, GIOCondition cond, void *opaque) {
	pmb887x_usart_t *p = opaque;
	p->watch_tag = 0;
	usart_transmit_fifo(p);
	return false;
}

static uint8_t usart_get_parity_bit(pmb887x_usart_t *p, uint8_t value) {
	bool is_odd_data = (ctpop8(value) % 2) != 0;
	bool is_odd_parity = (p->con & USART_CON_ODD) != 0;
	return is_odd_data != is_odd_parity ? 1 : 0;
}

static uint16_t usart_loopback_frame(pmb887x_usart_t *p, uint16_t value) {
	uint32_t mode = (p->con & USART_CON_M);
	if (mode == USART_CON_M_ASYNC_PARITY_7BIT) {
		value &= 0x7F;
		value |= (usart_get_parity_bit(p, value) << 7);
	} else if (mode == USART_CON_M_ASYNC_PARITY_8BIT) {
		value &= 0xFF;
		value |= (usart_get_parity_bit(p, value) << 8);
	} else if (mode == USART_CON_M_ASYNC_9BIT || mode == USART_CON_M_ASYNC_WAKE_UP_8BIT) {
		value &= 0x1FF;
	} else {
		value &= 0xFF;
	}

	return value;
}

static void usart_transmit_fifo(pmb887x_usart_t *p) {
	if (!p->transfer_pending)
		return;

	p->transfer_pending = false;
#if USART_IMMEDIATE_TRANSFER
	p->ris_read_count = 0;
#endif

	uint16_t buffer[USART_FIFO_SIZE * 2] = { };
	int buffer_size = 0;

	// Next frame to transmit
	if (!pmb887x_fifo_is_empty(&p->tx_buffer)) {
		buffer[0] = pmb887x_fifo16_pop(&p->tx_buffer);
		buffer_size++;
	}

#ifdef USART_SEND_FULL_FIFO
	// Also, we can send more data from FIFO
	if ((p->con & USART_CON_LB) == 0 && !pmb887x_fifo_is_empty(p->tx_fifo)) {
		int size = pmb887x_fifo_count(p->tx_fifo);
		pmb887x_fifo16_read(p->tx_fifo, buffer + buffer_size, size);
		buffer_size += size;
	}
#endif

	uint8_t bytes[ARRAY_SIZE(buffer)] = { };
	for (int i = 0; i < buffer_size; i++)
		bytes[i] = buffer[i];
	int ret = qemu_chr_fe_write(&p->chr, bytes, buffer_size);

	if ((p->con & USART_CON_LB) != 0) {
		bool is_received = false;
		if ((p->con & USART_CON_REN) != 0) {
			for (int i = 0; i < buffer_size; i++) {
				uint16_t frame = usart_loopback_frame(p, buffer[i]);
				if ((p->con & USART_CON_M) == USART_CON_M_ASYNC_WAKE_UP_8BIT && (frame & BIT(8)) == 0)
					continue;
				usart_receive_word(p, frame);
				is_received = true;
			}
		}
		if (is_received)
			usart_receive_complete(p);
		ret = buffer_size;
	}

	// Transmission incomplete, schedule next transmission when char backend available again
	int transmitted = MAX(0, ret);
	if (transmitted < buffer_size) {
		p->watch_tag = qemu_chr_fe_add_watch(&p->chr, G_IO_OUT | G_IO_HUP, usart_transmit_fifo_delayed, p);
		if (p->watch_tag) {
			pmb887x_fifo16_push(&p->tx_buffer, buffer[transmitted]);
			if (transmitted + 1 < buffer_size)
				pmb887x_fifo16_write(p->tx_fifo, buffer + transmitted + 1, buffer_size - transmitted - 1);
			p->transfer_pending = true;
#if USART_IMMEDIATE_TRANSFER
			p->ris_read_count = 0;
#endif
		} else {
			// QEMU char backend is not connected, data is lost
			pmb887x_fifo_reset(p->tx_fifo);
			transmitted = buffer_size;
		}
	}

	// Trigger IRQ/DMA if some data is transmitted
	if (transmitted > 0) {
		if (usart_tb_irq(p))
			pmb887x_srb_set_isr(&p->srb, USART_ISR_TB);
		if (usart_tx_irq(p))
			pmb887x_srb_set_isr(&p->srb, USART_ISR_TX);
		usart_handle_dma(p);
	}

	// If some data is still in the buffer, schedule transmission
	if (!p->transfer_pending && !pmb887x_fifo_is_empty(p->tx_fifo))
		usart_schedule_transmit(p);
}

static void usart_write_txb(pmb887x_usart_t *p, uint16_t value) {
	p->txb = (value & 0x1FF);

	if (pmb887x_fifo_is_full(p->tx_fifo)) {
		pmb887x_fifo16_replace_last(p->tx_fifo, p->txb);
		if (usart_is_tx_fifo_enabled(p)) {
			p->con |= USART_CON_OE;
			pmb887x_srb_set_isr(&p->srb, USART_ISR_ERR);
		}
		return;
	}

	pmb887x_fifo16_push(p->tx_fifo, p->txb);

	if (p->transfer_pending) {
		if (usart_tb_irq(p))
			pmb887x_srb_set_isr(&p->srb, USART_ISR_TB);
		usart_handle_dma(p);
	} else {
		usart_schedule_transmit(p);
	}
}

static uint64_t usart_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_usart_t *p = opaque;
	uint64_t value = 0;

	if (haddr != USART_ID && haddr != USART_CLC) {
		if (!pmb887x_clc_is_enabled(&p->clc))
			EPRINTF("usart clock not enabled\n");
	}

	switch (haddr) {
		case USART_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case USART_PISEL:
			value = p->pisel;
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
			value = p->txb;
			break;

		case USART_RXB:
			if (!pmb887x_fifo_is_empty(p->rx_fifo)) {
				bool is_full = pmb887x_fifo_is_full(p->rx_fifo);
				value = pmb887x_fifo16_pop(p->rx_fifo);
				if (is_full)
					qemu_chr_fe_accept_input(&p->chr);
				if ((p->rxfcon & USART_RXFCON_RXTMEN) && !pmb887x_fifo_is_empty(p->rx_fifo))
					pmb887x_srb_set_isr(&p->srb, USART_ISR_RX);
			} else {
				pmb887x_srb_set_isr(&p->srb, USART_ISR_ERR);
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
			if (usart_is_tx_fifo_enabled(p))
				value |= pmb887x_fifo_count(p->tx_fifo) << USART_FSTAT_TXFFL_SHIFT;

			if (usart_is_rx_fifo_enabled(p))
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
			value = pmb887x_srb_get_ris(&p->srb);

#if USART_IMMEDIATE_TRANSFER
			// Hack for speed-up emulation
			if (p->transfer_pending && p->ris_read_count++ > 10) {
				DPRINTF("immediate transfer RIS\n");
				usart_immediate_transfer(p);
			}
#endif
			break;

		case USART_MIS:
			value = pmb887x_srb_get_mis(&p->srb);

#if USART_IMMEDIATE_TRANSFER
			// Hack for speed-up emulation
			if (p->transfer_pending && p->ris_read_count++ > 10) {
				DPRINTF("immediate transfer MIS\n");
				usart_immediate_transfer(p);
			}
#endif
			break;

		case USART_ICR:
			value = 0;
			break;

		case USART_ISR:
			value = 0;
			break;

		case USART_DMAE:
			value = p->dma_control;
			break;

		case USART_TMO:
			value = p->tmo;
			break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}

	IO_DUMP(haddr + p->mmio.addr, size, value, false);

	return value;
}

static void usart_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_usart_t *p = opaque;

	if (haddr != USART_ID && haddr != USART_CLC) {
		if (!pmb887x_clc_is_enabled(&p->clc))
			EPRINTF("usart clock not enabled\n");
	}

	switch (haddr) {
		case USART_CLC:
			pmb887x_clc_set(&p->clc, value);
			usart_update_state(p);
			break;

		case USART_PISEL:
			p->pisel = value & 1;
			break;

		case USART_ID:
			value = 0x000044F1;
			break;

		case USART_CON: {
			bool baudrate_generator_start = (p->con & USART_CON_CON_R) == 0 && (value & USART_CON_CON_R) != 0;
			p->con = value;
			if (baudrate_generator_start)
				DPRINTF("baudrate=%u\n", usart_get_baud_rate(p));
			break;
		}

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
			usart_write_txb(p, value);
			break;

		case USART_ABCON:
			p->abcon = value;
			break;

		case USART_RXFCON:
			usart_rx_fifo_config(p, value);
			break;

		case USART_TXFCON:
			usart_tx_fifo_config(p, value);
			break;

		case USART_WHBCON:
			p->whbcon = value;
			usart_update_state(p);
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
			pmb887x_srb_set_icr(&p->srb, value);
			break;

		case USART_ISR:
			pmb887x_srb_set_isr(&p->srb, value);
			break;

		case USART_DMAE:
			p->dma_control = value;
			usart_update_state(p);
			usart_handle_dma(p);
			break;

		case USART_TMO:
			p->tmo = value;
			usart_update_state(p);
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}

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

static void usart_handle_gpio_input(void *opaque, int id, int level) {
	// nothing
}

static void usart_handle_dmac_tx_clr(void *opaque, int id, int level) {
	pmb887x_usart_t *p = opaque;
	p->dmac_tx_clr = level;
	usart_handle_dma(p);
}

static void usart_handle_dmac_rx_clr(void *opaque, int id, int level) {
	pmb887x_usart_t *p = opaque;
	p->dmac_rx_clr = level;
	usart_handle_dma(p);
}

static void usart_event_handler(void *opaque, int event_id, int level) {
#if USART_IMMEDIATE_TRANSFER
	pmb887x_usart_t *p = opaque;
	if (event_id == USART_RIS_TX && !level)
		usart_immediate_transfer(p);
#endif
}

static void usart_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_usart_t *p = PMB887X_USART(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-usart", USART_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (int i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);

	// DMAC
	qdev_init_gpio_in_named(dev, usart_handle_dmac_tx_clr, "DMAC_TX_CLR", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_tx_breq, "DMAC_TX_BREQ", 1);

	qdev_init_gpio_in_named(dev, usart_handle_dmac_rx_clr, "DMAC_RX_CLR", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_rx_breq, "DMAC_RX_BREQ", 1);

	qdev_init_gpio_in_named(dev, usart_handle_gpio_input, "RXD_IN", 1);
	qdev_init_gpio_in_named(dev, usart_handle_gpio_input, "CTS_IN", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_txd, "TXD_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_rts, "RTS_OUT", 1);
}

static void usart_realize(DeviceState *dev, Error **errp) {
	pmb887x_usart_t *p = PMB887X_USART(dev);
	
	pmb887x_clc_init(&p->clc);
	
	for (int i = 0; i < ARRAY_SIZE(p->irq); i++) {
		if (!p->irq[i])
			hw_error("pmb887x-usart: irq %d not set", i);
	}
	
	pmb887x_fifo16_init(&p->tx_fifo_buffered, USART_FIFO_SIZE);
	pmb887x_fifo16_init(&p->rx_fifo_buffered, USART_FIFO_SIZE);
	
	pmb887x_fifo16_init(&p->tx_fifo_single, 1);
	pmb887x_fifo16_init(&p->rx_fifo_single, 1);

	pmb887x_fifo16_init(&p->tx_buffer, 1);

	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
	pmb887x_srb_set_event_handler(&p->srb, dev, usart_event_handler);

	p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, usart_timer_reset, p);
	p->tmo_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, usart_tmo_timer_reset, p);
	p->tx_fifo = &p->tx_fifo_single;
	p->rx_fifo = &p->rx_fifo_single;
	usart_tx_fifo_config(p, 0);
	usart_rx_fifo_config(p, 0);
	usart_update_state(p);

	qemu_chr_fe_set_handlers(&p->chr, usart_can_receive, usart_receive, NULL, NULL, p, NULL, true);
}

static void usart_reset(DeviceState *dev) {
	pmb887x_usart_t *p = PMB887X_USART(dev);

	timer_del(p->timer);
	timer_del(p->tmo_timer);
	if (p->watch_tag) {
		g_source_remove(p->watch_tag);
		p->watch_tag = 0;
	}

	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	pmb887x_srb_reset(&p->srb);

	pmb887x_fifo_reset(&p->tx_fifo_buffered);
	pmb887x_fifo_reset(&p->rx_fifo_buffered);
	pmb887x_fifo_reset(&p->tx_fifo_single);
	pmb887x_fifo_reset(&p->rx_fifo_single);
	pmb887x_fifo_reset(&p->tx_buffer);

	p->transfer_pending = false;
#if USART_IMMEDIATE_TRANSFER
	p->ris_read_count = 0;
#endif

	p->pisel = 0;
	p->con = 0;
	p->bg = 0;
	p->fdv = 0;
	p->pmw = 0;
	p->txb = 0;
	p->abcon = 0;
	p->abstat = 0;
	p->rxfcon = 1 << USART_RXFCON_RXFITL_SHIFT;
	p->txfcon = 1 << USART_TXFCON_TXFITL_SHIFT;
	p->fstat = 0;
	p->whbcon = 0;
	p->whbabcon = 0;
	p->whbabstat = 0;
	p->fccon = 0;
	p->fcstat = 0;
	p->tmo = 0;
	p->dma_control = 0;
	p->dmac_tx_clr = 0;
	p->dmac_rx_clr = 0;

	p->tx_fifo = &p->tx_fifo_single;
	p->rx_fifo = &p->rx_fifo_single;

	usart_update_state(p);
	usart_handle_dma(p);
}

static const Property usart_properties[] = {
	DEFINE_PROP_LINK("pll", pmb887x_usart_t, pll, "pmb887x-pll", pmb887x_pll_t *),
    DEFINE_PROP_CHR("chardev", struct pmb887x_usart_t, chr),
};

static void usart_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, usart_properties);
	device_class_set_legacy_reset(dc, usart_reset);
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
