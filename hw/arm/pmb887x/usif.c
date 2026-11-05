#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/hw-error.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "qemu/timer.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/fifo.h"

#define TYPE_PMB887X_USIF	"pmb887x-usif"
#define PMB887X_USIF(obj)	OBJECT_CHECK(pmb887x_usif_t, (obj), TYPE_PMB887X_USIF)

#define USIF_FIFO_STAGES	8
#define USIF_OVERSAMPLING	16
#define USIF_RX_REQUESTS	(USIF_RIS_RXLSREQ | USIF_RIS_RXSREQ | USIF_RIS_RXLBREQ | USIF_RIS_RXBREQ)
#define USIF_TX_REQUESTS	(USIF_RIS_TXLSREQ | USIF_RIS_TXSREQ | USIF_RIS_TXLBREQ | USIF_RIS_TXBREQ)
#define USIF_DMA_REQUESTS	(USIF_RX_REQUESTS | USIF_TX_REQUESTS)
#define USIF_ICR_MASK		(USIF_DMA_REQUESTS | USIF_RIS_AB)
#define USIF_ISR_MASK		(USIF_ICR_MASK | USIF_RIS_ERR | USIF_RIS_PIRQ)
#define USIF_ERRIRQ_MASK	MAKE_64BIT_MASK(0, 10)
#define USIF_PIRQ_MASK		MAKE_64BIT_MASK(0, 8)
#define USIF_MODE_CFG_MASK	( \
	USIF_MODE_CFG_SYNC | USIF_MODE_CFG_MA | USIF_MODE_CFG_RXEN | USIF_MODE_CFG_LB | USIF_MODE_CFG_ECH | \
	USIF_MODE_CFG_TXINV | USIF_MODE_CFG_RXINV | USIF_MODE_CFG_SCFRC | USIF_MODE_CFG_SCPOL | \
	USIF_MODE_CFG_SCPH | USIF_MODE_CFG_TXIDLE | USIF_MODE_CFG_ABEN | USIF_MODE_CFG_EQU \
)
#define USIF_PRTC_CFG_MASK	( \
	USIF_PRTC_CFG_CLEN | USIF_PRTC_CFG_HD | USIF_PRTC_CFG_PAR | USIF_PRTC_CFG_ODD | USIF_PRTC_CFG_ERRFE | \
	USIF_PRTC_CFG_STP2 | USIF_PRTC_CFG_RESWKUP | USIF_PRTC_CFG_FCIEN | USIF_PRTC_CFG_FCOEN | \
	USIF_PRTC_CFG_FCIINV | USIF_PRTC_CFG_FCOTL | USIF_PRTC_CFG_SLIPEN \
)
#define USIF_TXFIFO_CFG_MASK	(USIF_TXFIFO_CFG_TXBS | USIF_TXFIFO_CFG_TXFA | USIF_TXFIFO_CFG_TXFC)
#define USIF_RXFIFO_CFG_MASK	(USIF_RXFIFO_CFG_RXBS | USIF_RXFIFO_CFG_RXFA | USIF_RXFIFO_CFG_RXFC)

enum usif_irq {
	USIF_IRQ_RX_SINGLE,
	USIF_IRQ_RX_BURST,
	USIF_IRQ_TX,
	USIF_IRQ_AB,
	USIF_IRQ_ERR,
	USIF_IRQ_PIRQ,
	USIF_IRQ_COUNT,
};

typedef struct pmb887x_usif_t pmb887x_usif_t;

struct pmb887x_usif_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	DebugChannel dbg;
	uint32_t revision;
	CharFrontend chr;

	qemu_irq irq[USIF_IRQ_COUNT];
	qemu_irq dmac_requests[8];
	uint32_t dma_requests;
	bool dmac_tx_clr;
	bool dmac_rx_clr;

	pmb887x_clc_reg_t clc;
	pmb887x_srb_reg_t srb;
	pmb887x_srb_ext_reg_t srb_err;
	pmb887x_srb_ext_reg_t srb_pirq;

	QEMUTimer *tx_timer;
	QEMUTimer *rx_timeout_timer;
	QEMUTimer *host_rx_timer;

	uint32_t runctrl;
	uint32_t mode_cfg;
	uint32_t prtc_cfg;
	uint32_t bc_cfg;
	uint32_t fdiv_cfg;
	uint32_t ictmo_cfg;
	uint32_t txfifo_cfg;
	uint32_t rxfifo_cfg;
	uint32_t tps_ctrl;
	uint32_t mrps_ctrl;
	uint32_t rps_stat;
	bool fco;

	pmb887x_fifo32_t tx_fifo;
	bool is_tx_busy;
	bool is_tx_requested;
	int64_t tx_char_end_ns;
	uint32_t tx_word_char;
	uint32_t tx_packet_chars;
	uint32_t tx_unwritten_chars;

	pmb887x_fifo32_t rx_fifo;
	bool is_rx_requested;
	bool is_rx_packet_open;
	uint32_t rx_mrps;
	uint32_t rx_packet_chars;
	uint32_t rx_word;
	uint32_t rx_word_chars;
	uint8_t host_rx_char;
};

static void usif_update_requests(pmb887x_usif_t *p);

static bool usif_is_running(pmb887x_usif_t *p) {
	return (p->runctrl & USIF_RUNCTRL_RUN) != 0;
}

static bool usif_is_loopback(pmb887x_usif_t *p) {
	return (p->mode_cfg & USIF_MODE_CFG_LB) != 0;
}

static bool usif_is_cts_active(pmb887x_usif_t *p) {
	return (p->prtc_cfg & USIF_PRTC_CFG_FCIINV) != 0;
}

static uint32_t usif_mss_stat(pmb887x_usif_t *p) {
	return (p->fco ? USIF_MSS_STAT_FCO : 0) | (usif_is_cts_active(p) ? USIF_MSS_STAT_FCI : 0);
}

static uint32_t usif_chars_per_word(uint32_t alignment) {
	return 4 >> MIN(alignment, 2);
}

static uint32_t usif_tx_chars_per_word(pmb887x_usif_t *p) {
	return usif_chars_per_word((p->txfifo_cfg & USIF_TXFIFO_CFG_TXFA) >> USIF_TXFIFO_CFG_TXFA_SHIFT);
}

static uint32_t usif_rx_chars_per_word(pmb887x_usif_t *p) {
	return usif_chars_per_word((p->rxfifo_cfg & USIF_RXFIFO_CFG_RXFA) >> USIF_RXFIFO_CFG_RXFA_SHIFT);
}

static uint32_t usif_tx_burst_words(pmb887x_usif_t *p) {
	return 1 << ((p->txfifo_cfg & USIF_TXFIFO_CFG_TXBS) >> USIF_TXFIFO_CFG_TXBS_SHIFT);
}

static uint32_t usif_rx_burst_words(pmb887x_usif_t *p) {
	return 1 << ((p->rxfifo_cfg & USIF_RXFIFO_CFG_RXBS) >> USIF_RXFIFO_CFG_RXBS_SHIFT);
}

static uint32_t usif_frame_bits(pmb887x_usif_t *p) {
	uint32_t data_bits = (p->prtc_cfg & USIF_PRTC_CFG_CLEN) >> USIF_PRTC_CFG_CLEN_SHIFT;
	uint32_t parity_bits = (p->prtc_cfg & USIF_PRTC_CFG_PAR) != 0 ? 1 : 0;
	uint32_t stop_bits = (p->prtc_cfg & USIF_PRTC_CFG_STP2) != 0 ? 2 : 1;
	return 1 + (data_bits != 0 ? data_bits : 32) + parity_bits + stop_bits;
}

static int64_t usif_bits_to_ns(pmb887x_usif_t *p, uint64_t bits) {
	uint32_t kernel_hz = pmb887x_clc_get_hz(&p->clc);
	if (kernel_hz == 0 || bits == 0)
		return 0;

	uint64_t bcrv = (p->bc_cfg & USIF_BC_CFG_BCRV) >> USIF_BC_CFG_BCRV_SHIFT;
	uint64_t inc = (p->fdiv_cfg & USIF_FDIV_CFG_INC) >> USIF_FDIV_CFG_INC_SHIFT;
	uint64_t dec = (p->fdiv_cfg & USIF_FDIV_CFG_DEC) >> USIF_FDIV_CFG_DEC_SHIFT;
	uint64_t bit_clocks = bits * USIF_OVERSAMPLING * (bcrv + 1);
	int64_t ns = muldiv64(bit_clocks, NANOSECONDS_PER_SECOND, kernel_hz);
	if (inc != 0)
		ns = muldiv64(ns, inc + dec, inc);
	return MAX(1, ns);
}

static int64_t usif_char_ns(pmb887x_usif_t *p) {
	return usif_bits_to_ns(p, usif_frame_bits(p));
}

static void usif_update_dma(pmb887x_usif_t *p) {
	uint32_t requests = pmb887x_srb_get_mis(&p->srb) & pmb887x_srb_get_dmae(&p->srb) & USIF_DMA_REQUESTS;
	if (p->dmac_tx_clr)
		requests &= ~USIF_TX_REQUESTS;
	if (p->dmac_rx_clr)
		requests &= ~USIF_RX_REQUESTS;

	uint32_t changed = requests ^ p->dma_requests;
	p->dma_requests = requests;
	for (size_t bit = 0; bit < ARRAY_SIZE(p->dmac_requests); bit++) {
		if ((changed & BIT(bit)) != 0)
			qemu_set_irq(p->dmac_requests[bit], (requests & BIT(bit)) != 0);
	}
}

static void usif_tx_request(pmb887x_usif_t *p) {
	if (p->is_tx_requested || !usif_is_running(p))
		return;

	uint32_t burst_words = usif_tx_burst_words(p);
	uint32_t free_words = pmb887x_fifo_free_count(&p->tx_fifo);
	uint32_t request;

	if ((p->txfifo_cfg & USIF_TXFIFO_CFG_TXFC) == 0) {
		if (free_words < burst_words)
			return;
		request = USIF_RIS_TXBREQ;
	} else {
		if (p->tx_unwritten_chars == 0)
			return;
		uint32_t words = DIV_ROUND_UP(p->tx_unwritten_chars, usif_tx_chars_per_word(p));
		if (words >= burst_words) {
			if (free_words < burst_words)
				return;
			request = words > burst_words ? USIF_RIS_TXBREQ : USIF_RIS_TXLBREQ;
		} else {
			if (free_words == 0)
				return;
			request = words > 1 ? USIF_RIS_TXSREQ : USIF_RIS_TXLSREQ;
		}
	}

	p->is_tx_requested = true;
	pmb887x_srb_set_isr(&p->srb, request);
}

static uint32_t usif_rx_complete_words(pmb887x_usif_t *p) {
	uint32_t words = pmb887x_fifo_count(&p->rx_fifo);
	bool is_word_open = p->is_rx_packet_open && p->rx_word_chars < usif_rx_chars_per_word(p);
	return words - (is_word_open && words > 0 ? 1 : 0);
}

static void usif_rx_request(pmb887x_usif_t *p) {
	if (p->is_rx_requested || !usif_is_running(p))
		return;

	uint32_t burst_words = usif_rx_burst_words(p);
	uint32_t request;

	if ((p->rxfifo_cfg & USIF_RXFIFO_CFG_RXFC) == 0) {
		uint32_t words = usif_rx_complete_words(p);
		if (words == 0)
			return;
		request = words >= burst_words ? USIF_RIS_RXBREQ : USIF_RIS_RXSREQ;
	} else if (p->is_rx_packet_open) {
		if (usif_rx_complete_words(p) < burst_words)
			return;
		request = USIF_RIS_RXBREQ;
	} else {
		uint32_t words = pmb887x_fifo_count(&p->rx_fifo);
		if (words == 0)
			return;
		if (words >= burst_words) {
			request = words > burst_words ? USIF_RIS_RXBREQ : USIF_RIS_RXLBREQ;
		} else {
			request = words > 1 ? USIF_RIS_RXSREQ : USIF_RIS_RXLSREQ;
		}
	}

	p->is_rx_requested = true;
	pmb887x_srb_set_isr(&p->srb, request);
}

static void usif_update_requests(pmb887x_usif_t *p) {
	usif_tx_request(p);
	usif_rx_request(p);
}

static void usif_rx_timeout_restart(pmb887x_usif_t *p) {
	uint32_t timeout_bits = (p->ictmo_cfg & USIF_ICTMO_CFG_TMO) >> USIF_ICTMO_CFG_TMO_SHIFT;
	int64_t timeout_ns = usif_bits_to_ns(p, timeout_bits);
	if (timeout_ns == 0) {
		timer_del(p->rx_timeout_timer);
		return;
	}
	timer_mod_ns(p->rx_timeout_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + timeout_ns);
}

static void usif_rx_end_packet(pmb887x_usif_t *p, bool is_timeout) {
	p->rps_stat = p->rx_packet_chars;
	p->is_rx_packet_open = false;
	timer_del(p->rx_timeout_timer);
	if (is_timeout)
		pmb887x_srb_ext_set_isr(&p->srb_pirq, USIF_PIRQSS_RXEND);
}

static void usif_rx_store_char(pmb887x_usif_t *p, uint32_t value) {
	uint32_t chars_per_word = usif_rx_chars_per_word(p);
	bool is_word_full = p->rx_word_chars >= chars_per_word || pmb887x_fifo_is_empty(&p->rx_fifo);

	if (is_word_full) {
		if (pmb887x_fifo_is_full(&p->rx_fifo)) {
			pmb887x_srb_ext_set_isr(&p->srb_err, USIF_ERRIRQSS_RXFOFL);
			return;
		}
		p->rx_word = value;
		p->rx_word_chars = 1;
		pmb887x_fifo32_push(&p->rx_fifo, p->rx_word);
	} else {
		p->rx_word |= value << (p->rx_word_chars * (32 / chars_per_word));
		p->rx_word_chars++;
		pmb887x_fifo32_replace_last(&p->rx_fifo, p->rx_word);
	}
	p->rx_packet_chars++;
}

static void usif_rx_char(pmb887x_usif_t *p, uint32_t value) {
	if (!usif_is_running(p) || (p->mode_cfg & USIF_MODE_CFG_RXEN) == 0)
		return;

	if (!p->is_rx_packet_open) {
		p->is_rx_packet_open = true;
		p->rx_packet_chars = 0;
		p->rx_word_chars = usif_rx_chars_per_word(p);
	}

	usif_rx_store_char(p, value);
	if (p->rx_mrps != 0 && p->rx_packet_chars == p->rx_mrps) {
		usif_rx_end_packet(p, false);
	} else {
		usif_rx_timeout_restart(p);
	}
	usif_update_requests(p);
}

static uint32_t usif_rx_read(pmb887x_usif_t *p) {
	if (!usif_is_running(p))
		return 0;

	if (pmb887x_fifo_is_empty(&p->rx_fifo)) {
		pmb887x_srb_ext_set_isr(&p->srb_err, USIF_ERRIRQSS_RXFUFL);
		return 0;
	}

	uint32_t value = pmb887x_fifo32_pop(&p->rx_fifo);
	usif_update_requests(p);
	return value;
}

static void usif_tx_load_packet(pmb887x_usif_t *p) {
	if (!usif_is_running(p) || p->tx_packet_chars != 0 || p->tps_ctrl == 0)
		return;

	p->tx_packet_chars = p->tps_ctrl;
	p->tx_unwritten_chars = p->tps_ctrl;
	p->tps_ctrl = 0;
}

static bool usif_tx_can_shift(pmb887x_usif_t *p) {
	if (!usif_is_running(p) || pmb887x_fifo_is_empty(&p->tx_fifo))
		return false;
	if ((p->prtc_cfg & USIF_PRTC_CFG_FCIEN) != 0 && !usif_is_cts_active(p))
		return false;
	return usif_char_ns(p) != 0;
}

static void usif_tx_start(pmb887x_usif_t *p) {
	if (p->is_tx_busy || !usif_tx_can_shift(p))
		return;

	p->is_tx_busy = true;
	p->tx_char_end_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + usif_char_ns(p);
	timer_mod_ns(p->tx_timer, p->tx_char_end_ns);
}

static void usif_tx_output_char(pmb887x_usif_t *p, uint32_t value) {
	if (usif_is_loopback(p)) {
		usif_rx_char(p, value);
		return;
	}

	uint8_t byte = value;
	qemu_chr_fe_write(&p->chr, &byte, 1);
}

static void usif_tx_shift_char(pmb887x_usif_t *p) {
	uint32_t chars_per_word = usif_tx_chars_per_word(p);
	uint32_t char_bits = 32 / chars_per_word;
	uint32_t word = pmb887x_fifo32_peek(&p->tx_fifo);
	uint32_t value = (word >> (p->tx_word_char * char_bits)) & (uint32_t) MAKE_64BIT_MASK(0, char_bits);
	bool is_packet_char = p->tx_packet_chars != 0;

	p->tx_word_char++;
	if (is_packet_char)
		p->tx_packet_chars--;

	bool is_packet_end = is_packet_char && p->tx_packet_chars == 0;
	if (p->tx_word_char >= chars_per_word || is_packet_end) {
		pmb887x_fifo32_pop(&p->tx_fifo);
		p->tx_word_char = 0;
	}

	usif_tx_output_char(p, value);
	if (is_packet_end) {
		pmb887x_srb_ext_set_isr(&p->srb_pirq, USIF_PIRQSS_TXEND);
		usif_tx_load_packet(p);
	}
}

static void usif_tx_timer(void *opaque) {
	pmb887x_usif_t *p = opaque;
	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

	while (p->is_tx_busy && p->tx_char_end_ns <= now) {
		usif_tx_shift_char(p);
		if (usif_tx_can_shift(p)) {
			p->tx_char_end_ns += usif_char_ns(p);
		} else {
			p->is_tx_busy = false;
		}
	}

	if (p->is_tx_busy)
		timer_mod_ns(p->tx_timer, p->tx_char_end_ns);
	usif_update_requests(p);
}

static void usif_tx_write(pmb887x_usif_t *p, uint32_t value) {
	if (!usif_is_running(p))
		return;

	if (pmb887x_fifo_is_full(&p->tx_fifo)) {
		DEBUG_LOG(&p->dbg, "TX FIFO overflow\n");
		return;
	}

	pmb887x_fifo32_push(&p->tx_fifo, value);
	p->tx_unwritten_chars -= MIN(p->tx_unwritten_chars, usif_tx_chars_per_word(p));
	usif_tx_start(p);
	usif_update_requests(p);
}

static void usif_rx_timeout(void *opaque) {
	pmb887x_usif_t *p = opaque;
	if (!p->is_rx_packet_open)
		return;
	usif_rx_end_packet(p, true);
	usif_update_requests(p);
}

static void usif_stop(pmb887x_usif_t *p) {
	timer_del(p->tx_timer);
	timer_del(p->rx_timeout_timer);
	pmb887x_fifo_reset(&p->tx_fifo);
	pmb887x_fifo_reset(&p->rx_fifo);
	p->is_tx_busy = false;
	p->is_tx_requested = false;
	p->tx_word_char = 0;
	p->tx_packet_chars = 0;
	p->tx_unwritten_chars = 0;
	p->is_rx_requested = false;
	p->is_rx_packet_open = false;
	p->rx_mrps = 0;
	p->rx_word_chars = 0;
}

static void usif_runctrl_write(pmb887x_usif_t *p, uint32_t value) {
	bool was_running = usif_is_running(p);
	p->runctrl = value & USIF_RUNCTRL_RUN;

	if (was_running && !usif_is_running(p)) {
		usif_stop(p);
	} else if (!was_running && usif_is_running(p)) {
		usif_tx_load_packet(p);
		usif_update_requests(p);
	}
}

static void usif_tps_write(pmb887x_usif_t *p, uint32_t value) {
	p->tps_ctrl = value & USIF_TPS_CTRL_TPS;
	usif_tx_load_packet(p);
	usif_update_requests(p);
}

static void usif_mrps_write(pmb887x_usif_t *p, uint32_t value) {
	p->mrps_ctrl = value & USIF_MRPS_CTRL_MRPS;
	if (usif_is_running(p))
		p->rx_mrps = p->mrps_ctrl;
}

static void usif_config_write(pmb887x_usif_t *p, uint32_t *reg, uint32_t value, uint32_t mask) {
	if (usif_is_running(p)) {
		DEBUG_GUEST_ERROR(&p->dbg, "configuration write while running\n");
		return;
	}
	*reg = value & mask;
}

static int usif_irq_router(void *opaque, int event_id) {
	switch (BIT(event_id)) {
		case USIF_RIS_RXLSREQ:
		case USIF_RIS_RXSREQ:
		case USIF_RIS_RXLBREQ:
			return USIF_IRQ_RX_SINGLE;
		case USIF_RIS_RXBREQ:
			return USIF_IRQ_RX_BURST;
		case USIF_RIS_TXLSREQ:
		case USIF_RIS_TXSREQ:
		case USIF_RIS_TXLBREQ:
		case USIF_RIS_TXBREQ:
			return USIF_IRQ_TX;
		case USIF_RIS_AB:
			return USIF_IRQ_AB;
		case USIF_RIS_ERR:
			return USIF_IRQ_ERR;
		case USIF_RIS_PIRQ:
			return USIF_IRQ_PIRQ;
		default:
			hw_error("[pmb887x-usif] unknown event: %d\n", event_id);
	}
}

static void usif_event_handler(void *opaque, int event_id, int level) {
	pmb887x_usif_t *p = opaque;
	uint32_t event = BIT(event_id);
	bool is_request_cleared = level == 0 && (event & USIF_DMA_REQUESTS) != 0;

	if (is_request_cleared) {
		if ((event & USIF_TX_REQUESTS) != 0)
			p->is_tx_requested = false;
		if ((event & USIF_RX_REQUESTS) != 0)
			p->is_rx_requested = false;
	}
	usif_update_dma(p);
	if (is_request_cleared)
		usif_update_requests(p);
}

static uint64_t usif_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_usif_t *p = opaque;
	uint64_t value = 0;

	switch (haddr) {
		case USIF_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;
		case USIF_ID:
			value = 0xF051C000 | p->revision;
			break;
		case USIF_RUNCTRL:
			value = p->runctrl;
			break;
		case USIF_MODE_CFG:
			value = p->mode_cfg;
			break;
		case USIF_PRTC_CFG:
			value = p->prtc_cfg;
			break;
		case USIF_BC_CFG:
			value = p->bc_cfg;
			break;
		case USIF_FDIV_CFG:
			value = p->fdiv_cfg;
			break;
		case USIF_ICTMO_CFG:
			value = p->ictmo_cfg;
			break;
		case USIF_MSS_STAT:
			value = usif_mss_stat(p);
			break;
		case USIF_TXFIFO_CFG:
			value = p->txfifo_cfg;
			break;
		case USIF_TPS_CTRL:
			value = p->tps_ctrl;
			break;
		case USIF_TXFIFO_STAT:
			value = pmb887x_fifo_count(&p->tx_fifo) | (p->is_tx_busy ? USIF_TXFIFO_STAT_TXBUSY : 0);
			break;
		case USIF_RXFIFO_CFG:
			value = p->rxfifo_cfg;
			break;
		case USIF_MRPS_CTRL:
			value = p->mrps_ctrl;
			break;
		case USIF_RPS_STAT:
			value = p->rps_stat;
			break;
		case USIF_RXFIFO_STAT:
			value = pmb887x_fifo_count(&p->rx_fifo);
			break;
		case USIF_ERRIRQSM:
			value = pmb887x_srb_ext_get_imsc(&p->srb_err);
			break;
		case USIF_ERRIRQSS:
			value = pmb887x_srb_ext_get_ris(&p->srb_err);
			break;
		case USIF_PIRQSM:
			value = pmb887x_srb_ext_get_imsc(&p->srb_pirq);
			break;
		case USIF_PIRQSS:
			value = pmb887x_srb_ext_get_ris(&p->srb_pirq);
			break;
		case USIF_RIS:
			value = pmb887x_srb_get_ris(&p->srb);
			break;
		case USIF_IMSC:
			value = pmb887x_srb_get_imsc(&p->srb);
			break;
		case USIF_MIS:
			value = pmb887x_srb_get_mis(&p->srb);
			break;
		case USIF_DMAE:
			value = pmb887x_srb_get_dmae(&p->srb);
			break;
		case USIF_RXD:
			value = usif_rx_read(p);
			break;
		case USIF_MSS_SET:
		case USIF_MSS_CLR:
		case USIF_ERRIRQSC:
		case USIF_PIRQSC:
		case USIF_ICR:
		case USIF_ISR:
		case USIF_TXD:
			break;
		default:
			DEBUG_GUEST_ERROR(&p->dbg, "unknown reg read: %02"PRIX64"\n", haddr);
			break;
	}

	return value;
}

static void usif_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_usif_t *p = opaque;

	switch (haddr) {
		case USIF_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;
		case USIF_RUNCTRL:
			usif_runctrl_write(p, value);
			break;
		case USIF_MSS_SET:
			if ((value & USIF_MSS_SET_FCO) != 0)
				p->fco = true;
			break;
		case USIF_MSS_CLR:
			if ((value & USIF_MSS_CLR_FCO) != 0)
				p->fco = false;
			break;
		case USIF_MODE_CFG:
			usif_config_write(p, &p->mode_cfg, value, USIF_MODE_CFG_MASK);
			break;
		case USIF_PRTC_CFG:
			usif_config_write(p, &p->prtc_cfg, value, USIF_PRTC_CFG_MASK);
			break;
		case USIF_BC_CFG:
			usif_config_write(p, &p->bc_cfg, value, USIF_BC_CFG_BCRV);
			break;
		case USIF_FDIV_CFG:
			usif_config_write(p, &p->fdiv_cfg, value, USIF_FDIV_CFG_INC | USIF_FDIV_CFG_DEC);
			break;
		case USIF_ICTMO_CFG:
			usif_config_write(p, &p->ictmo_cfg, value, USIF_ICTMO_CFG_TMO);
			break;
		case USIF_TXFIFO_CFG:
			usif_config_write(p, &p->txfifo_cfg, value, USIF_TXFIFO_CFG_MASK);
			break;
		case USIF_RXFIFO_CFG:
			usif_config_write(p, &p->rxfifo_cfg, value, USIF_RXFIFO_CFG_MASK);
			break;
		case USIF_TPS_CTRL:
			usif_tps_write(p, value);
			break;
		case USIF_MRPS_CTRL:
			usif_mrps_write(p, value);
			break;
		case USIF_ERRIRQSM:
			pmb887x_srb_ext_set_imsc(&p->srb_err, value & USIF_ERRIRQ_MASK);
			break;
		case USIF_ERRIRQSC:
			pmb887x_srb_ext_set_icr(&p->srb_err, value & USIF_ERRIRQ_MASK);
			break;
		case USIF_PIRQSM:
			pmb887x_srb_ext_set_imsc(&p->srb_pirq, value & USIF_PIRQ_MASK);
			break;
		case USIF_PIRQSC:
			pmb887x_srb_ext_set_icr(&p->srb_pirq, value & USIF_PIRQ_MASK);
			break;
		case USIF_IMSC:
			pmb887x_srb_set_imsc(&p->srb, value & USIF_ISR_MASK);
			usif_update_dma(p);
			break;
		case USIF_ICR:
			pmb887x_srb_set_icr(&p->srb, value & USIF_ICR_MASK);
			break;
		case USIF_ISR:
			pmb887x_srb_set_isr(&p->srb, value & USIF_ISR_MASK);
			break;
		case USIF_DMAE:
			pmb887x_srb_set_dmae(&p->srb, value & USIF_DMA_REQUESTS);
			usif_update_dma(p);
			break;
		case USIF_TXD:
			usif_tx_write(p, value);
			break;
		default:
			DEBUG_GUEST_ERROR(&p->dbg, "unknown reg write: %02"PRIX64" = %08"PRIX64"\n", haddr, value);
			break;
	}
}

static const MemoryRegionOps io_ops = {
	.read			= usif_io_read,
	.write			= usif_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static int usif_can_receive(void *opaque) {
	pmb887x_usif_t *p = opaque;
	if (timer_pending(p->host_rx_timer) || usif_char_ns(p) == 0)
		return 0;
	return 1;
}

static void usif_receive(void *opaque, const uint8_t *buf, int size) {
	pmb887x_usif_t *p = opaque;
	p->host_rx_char = buf[0];
	timer_mod_ns(p->host_rx_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + usif_char_ns(p));
}

static void usif_host_rx_timer(void *opaque) {
	pmb887x_usif_t *p = opaque;
	if (!usif_is_loopback(p))
		usif_rx_char(p, p->host_rx_char);
	qemu_chr_fe_accept_input(&p->chr);
}

static void usif_handle_dmac_tx_clr(void *opaque, int id, int level) {
	pmb887x_usif_t *p = opaque;
	p->dmac_tx_clr = level != 0;
	if (p->dmac_tx_clr)
		pmb887x_srb_set_icr(&p->srb, USIF_TX_REQUESTS);
	usif_update_dma(p);
}

static void usif_handle_dmac_rx_clr(void *opaque, int id, int level) {
	pmb887x_usif_t *p = opaque;
	p->dmac_rx_clr = level != 0;
	if (p->dmac_rx_clr)
		pmb887x_srb_set_icr(&p->srb, USIF_RX_REQUESTS);
	usif_update_dma(p);
}

static void usif_clock_changed(void *opaque) {
	pmb887x_usif_t *p = opaque;
	usif_tx_start(p);
	qemu_chr_fe_accept_input(&p->chr);
}

static void usif_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_usif_t *p = PMB887X_USIF(obj);
	pmb887x_clc_init(&p->clc, dev);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-usif", USIF_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	for (size_t i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);

	static const char *const DMA_REQUEST_NAMES[] = {
		"DMAC_RX_LSREQ", "DMAC_RX_SREQ", "DMAC_RX_LBREQ", "DMAC_RX_BREQ",
		"DMAC_TX_LSREQ", "DMAC_TX_SREQ", "DMAC_TX_LBREQ", "DMAC_TX_BREQ",
	};
	for (size_t i = 0; i < ARRAY_SIZE(p->dmac_requests); i++)
		qdev_init_gpio_out_named(dev, &p->dmac_requests[i], DMA_REQUEST_NAMES[i], 1);
	qdev_init_gpio_in_named(dev, usif_handle_dmac_tx_clr, "DMAC_TX_CLR", 1);
	qdev_init_gpio_in_named(dev, usif_handle_dmac_rx_clr, "DMAC_RX_CLR", 1);
}

static void usif_realize(DeviceState *dev, Error **errp) {
	pmb887x_usif_t *p = PMB887X_USIF(dev);
	pmb887x_mod_debug_init(&p->dbg, dev, &p->mmio, &pmb887x_usif_regs);

	pmb887x_clc_set(&p->clc, 1U << MOD_CLC_RMC_SHIFT);
	pmb887x_clc_set_callback(&p->clc, usif_clock_changed, p);
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
	pmb887x_srb_set_irq_router(&p->srb, p, usif_irq_router);
	pmb887x_srb_set_event_handler(&p->srb, p, usif_event_handler);
	pmb887x_srb_ext_init(&p->srb_err, &p->srb, USIF_RIS_ERR);
	pmb887x_srb_ext_init(&p->srb_pirq, &p->srb, USIF_RIS_PIRQ);
	pmb887x_fifo32_init(&p->tx_fifo, USIF_FIFO_STAGES);
	pmb887x_fifo32_init(&p->rx_fifo, USIF_FIFO_STAGES);

	p->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, usif_tx_timer, p);
	p->rx_timeout_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, usif_rx_timeout, p);
	p->host_rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, usif_host_rx_timer, p);
	qemu_chr_fe_set_handlers(&p->chr, usif_can_receive, usif_receive, NULL, NULL, p, NULL, true);
}

static void usif_reset(DeviceState *dev) {
	pmb887x_usif_t *p = PMB887X_USIF(dev);

	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	usif_stop(p);
	timer_del(p->host_rx_timer);
	pmb887x_srb_reset(&p->srb);
	pmb887x_srb_ext_reset(&p->srb_err);
	pmb887x_srb_ext_reset(&p->srb_pirq);
	pmb887x_srb_ext_set_imsc(&p->srb_err, 0);
	pmb887x_srb_ext_set_imsc(&p->srb_pirq, 0);

	p->runctrl = 0;
	p->mode_cfg = 0;
	p->prtc_cfg = 0;
	p->bc_cfg = 0;
	p->fdiv_cfg = 0;
	p->ictmo_cfg = 0;
	p->txfifo_cfg = 0;
	p->rxfifo_cfg = 0;
	p->tps_ctrl = 0;
	p->mrps_ctrl = 0;
	p->rps_stat = 0;
	p->fco = false;
	p->dmac_tx_clr = false;
	p->dmac_rx_clr = false;
	usif_update_dma(p);
}

static const Property usif_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_usif_t, revision, 0),
	DEFINE_PROP_CHR("chardev", pmb887x_usif_t, chr),
};

static void usif_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, usif_properties);
	device_class_set_legacy_reset(dc, usif_reset);
	dc->realize = usif_realize;
}

static const TypeInfo usif_info = {
	.name          	= TYPE_PMB887X_USIF,
	.parent        	= TYPE_SYS_BUS_DEVICE,
	.instance_size 	= sizeof(struct pmb887x_usif_t),
	.instance_init 	= usif_init,
	.class_init    	= usif_class_init,
};

static void usif_register_types(void) {
	type_register_static(&usif_info);
}
type_init(usif_register_types)
