/*
 * Display Interface
 * */
#include "hw/arm/pmb887x/fifo.h"
#define PMB887X_TRACE_ID		DIF
#define PMB887X_TRACE_PREFIX	"pmb887x-dif"

#define PMB887X_DIF_DUMP_BIT_MUX	0 // print bit mux config

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/hw-error.h"
#include "hw/ssi/ssi.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "hw/core/qdev-properties.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/dmac.h"
#include "hw/arm/pmb887x/dma_sink.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_DIF	"pmb887x-dif-v2"
#define PMB887X_DIF(obj)	OBJECT_CHECK(pmb887x_dif_t, (obj), TYPE_PMB887X_DIF)

#define FIFO_IO_SIZE	0x3FFF
#define FIFO_SIZE		16
#define PS_PER_S		1000000000000ULL
#define DIF_PINS_POLARITY_MASK	( \
	DIFv2_PERREG_CS1POL | DIFv2_PERREG_CS2POL | DIFv2_PERREG_CS3POL | \
	DIFv2_PERREG_CDPOL | DIFv2_PERREG_RDPOL | DIFv2_PERREG_WRPOL \
)
#define DIF_TX_DMA_REQUESTS	( \
	DIFv2_RIS_TXSREQ | DIFv2_RIS_TXBREQ | DIFv2_RIS_TXLSREQ | DIFv2_RIS_TXLBREQ \
)
#define DIF_RX_DMA_REQUESTS	( \
	DIFv2_RIS_RXSREQ | DIFv2_RIS_RXBREQ | DIFv2_RIS_RXLSREQ | DIFv2_RIS_RXLBREQ \
)
#define FIFO_ICR_MASK	( \
	DIFv2_ICR_RXLSREQ | DIFv2_ICR_RXSREQ | \
	DIFv2_ICR_RXLBREQ | DIFv2_ICR_RXBREQ | \
	DIFv2_ICR_TXLSREQ | DIFv2_ICR_TXSREQ | \
	DIFv2_ICR_TXLBREQ | DIFv2_ICR_TXBREQ \
)

typedef struct pmb887x_dif_t pmb887x_dif_t;

static void dif_start_rx(pmb887x_dif_t *p);
static void dif_start_tx(pmb887x_dif_t *p);
static void dif_kernel_reset(pmb887x_dif_t *p, uint32_t new_state);
static bool dif_is_running(pmb887x_dif_t *p);
static void dif_tx_from_fifo(pmb887x_dif_t *p);
static void dif_work(pmb887x_dif_t *p);
static uint16_t dif_bus_transfer(pmb887x_dif_t *p, uint16_t value);

enum DIFIrqType {
	DIF_RX_SINGLE_IRQ = 0,
	DIF_RX_BURST_IRQ,
	DIF_TX_IRQ,
	DIF_ERROR_IRQ,
};

enum DIFWorkState {
	DIF_STATE_NONE,
	DIF_STATE_TX,
	DIF_STATE_RX,
};

struct pmb887x_dif_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	uint32_t revision;
	SSIBus *bus;

	qemu_irq irq[4];

	qemu_irq gpio_data[8];
	qemu_irq gpio_cs[3];
	qemu_irq gpio_cd;
	qemu_irq gpio_wr;
	qemu_irq gpio_rd;
	uint32_t pin_levels;
	bool are_pins_driven;

	QEMUBH *rx_refill_bh;
	bool in_schedule;
	bool transfer_pending;
	bool in_dma_handshake;
	bool in_tx_bus_wake;

	bool tx_fifo_req;
	bool rx_fifo_req;
	bool is_tx_started;
	uint32_t tx_words_remaining;
	uint32_t tx_words_preloaded;
	uint32_t rx_words_remaining;

	uint32_t rx_words_in_fifo;
	uint16_t pbc_word;
	bool is_pbc_word_valid;
	bool is_pbc_pair_completed;

	uint8_t bit_mux[32];
	uint8_t bit_invert[32];
	uint8_t bit_bcreg[32];
	uint8_t bit_bcsel[32];
	uint32_t mux_select_mask;
	uint32_t mux_constant_bits;
	uint32_t mux_byte_bits[4][256];
	bool is_mux_identity;

	uint32_t con;
	uint32_t perreg;
	uint32_t csreg;
	uint32_t lcdtim[2];
	uint32_t runctrl;
	uint32_t startlcdrd;
	uint32_t coeff[4];
	uint32_t pbccon;
	uint32_t bmreg[6];
	uint32_t bcsel[2];
	uint32_t bcreg;
	uint32_t invert_bit;
	uint32_t sync_config;
	uint32_t sync_count;
	uint32_t br;
	uint32_t fdiv;
	uint32_t debug;
	uint32_t rxfifo_cfg;
	uint32_t mrps_ctrl;
	uint32_t txfifo_cfg;
	uint32_t tps_ctrl;

	pmb887x_clc_reg_t clc;
	pmb887x_srb_reg_t srb;
	pmb887x_srb_ext_reg_t srb_err;

	pmb887x_fifo32_t tx_fifo;
	pmb887x_fifo32_t tx_csreg_fifo;
	/* Stages already sent to the device but still on the parallel bus, oldest first. */
	int64_t tx_bus_end_ps[FIFO_SIZE];
	uint32_t tx_bus_first;
	uint32_t tx_bus_count;
	uint32_t tx_bus_words;
	int64_t tx_bus_free_ps;
	QEMUTimer *tx_bus_timer;
	uint32_t tx_isr_deferred;
	pmb887x_fifo32_t rx_fifo;
	uint32_t tx_csreg;
	bool is_tx_csreg_active;

	uint32_t rx_packet_words;
	uint32_t rx_buffer;
	uint32_t rx_buffer_word_count;

	enum DIFWorkState state;

	int dmac_tx_clr;
	int dmac_rx_clr;

	qemu_irq dmac_tx_breq;
	qemu_irq dmac_tx_sreq;
	qemu_irq dmac_tx_lbreq;
	qemu_irq dmac_tx_lsreq;

	qemu_irq dmac_rx_breq;
	qemu_irq dmac_rx_sreq;
	qemu_irq dmac_rx_lbreq;
	qemu_irq dmac_rx_lsreq;
	uint32_t dma_requests;
	bool are_dma_requests_driven;
};

static inline uint32_t dif_get_transfer_csreg(pmb887x_dif_t *p) {
	return p->is_tx_csreg_active ? p->tx_csreg : p->csreg;
}

static inline uint32_t dif_get_rx_align(pmb887x_dif_t *p) {
	return MIN(1 << ((p->rxfifo_cfg & DIFv2_RXFIFO_CFG_RXFA) >> DIFv2_RXFIFO_CFG_RXFA_SHIFT), 4);
}

static inline uint32_t dif_get_tx_align(pmb887x_dif_t *p) {
	return MIN(1 << ((p->txfifo_cfg & DIFv2_TXFIFO_CFG_TXFA) >> DIFv2_TXFIFO_CFG_TXFA_SHIFT), 4);
}

static inline uint32_t dif_get_rx_burst_size(pmb887x_dif_t *p) {
	uint32_t bs = 1 << ((p->rxfifo_cfg & DIFv2_RXFIFO_CFG_RXBS) >> DIFv2_RXFIFO_CFG_RXBS_SHIFT);
	return bs * (4 / dif_get_rx_align(p));
}

static inline uint32_t dif_get_tx_burst_size(pmb887x_dif_t *p) {
	uint32_t bs = 1 << ((p->txfifo_cfg & DIFv2_TXFIFO_CFG_TXBS) >> DIFv2_TXFIFO_CFG_TXBS_SHIFT);
	return bs * (4 / dif_get_tx_align(p));
}

static inline uint32_t dif_get_bsconf_word_count(pmb887x_dif_t *p) {
	uint32_t csreg = dif_get_transfer_csreg(p);

	switch ((csreg & DIFv2_CSREG_BSCONF)) {
		case DIFv2_CSREG_BSCONF_OFF:
			return 0;
		case DIFv2_CSREG_BSCONF_1x8BIT:
		case DIFv2_CSREG_BSCONF_1x9BIT:
			return 1;
		case DIFv2_CSREG_BSCONF_2x8BIT:
		case DIFv2_CSREG_BSCONF_2x9BIT:
			return 2;
		case DIFv2_CSREG_BSCONF_3x8BIT:
		case DIFv2_CSREG_BSCONF_3x9BIT:
			return 3;
		case DIFv2_CSREG_BSCONF_4x8BIT:
			return 4;
	}
	hw_error("Invalid bsconf value: %08X", (csreg & DIFv2_CSREG_BSCONF) >> DIFv2_CSREG_BSCONF_SHIFT);
	return 0;
}

static inline bool dif_is_bsconf_9bit(pmb887x_dif_t *p) {
	uint32_t bsconf = dif_get_transfer_csreg(p) & DIFv2_CSREG_BSCONF;
	return bsconf == DIFv2_CSREG_BSCONF_1x9BIT || bsconf == DIFv2_CSREG_BSCONF_2x9BIT || bsconf == DIFv2_CSREG_BSCONF_3x9BIT;
}

static inline uint32_t dif_get_word_bits(pmb887x_dif_t *p) {
	return ((p->con & DIFv2_CON_BM) >> DIFv2_CON_BM_SHIFT) + 1;
}

static inline bool dif_is_serial(pmb887x_dif_t *p) {
	return (p->perreg & DIFv2_PERREG_DIFPERMODE) == DIFv2_PERREG_DIFPERMODE_SERIAL;
}

static inline uint32_t dif_get_tx_word_bits(pmb887x_dif_t *p) {
	if (!dif_is_serial(p))
		return 8;

	return MIN(dif_get_word_bits(p), dif_get_tx_align(p) * 8);
}

/* dif_get_word_bits() reaches 32 (CON.BM is 5 bits), where "1 << bits" would be UB. */
static inline uint32_t dif_get_word_mask(uint32_t word_bits) {
	return word_bits >= 32 ? 0xFFFFFFFFU : (1U << word_bits) - 1;
}

static inline bool dif_is_pbc_enabled(pmb887x_dif_t *p) {
	return (p->pbccon & DIFv2_PBCCON_PBBCONV_MODE) != 0;
}

static int64_t dif_get_now_ps(void) {
	return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) * 1000;
}

/* bsp dif-v2-timings: a parallel bus word takes ((ACCESSCYCLE - 1) mod 32) + 2 module clock cycles. */
static uint64_t dif_get_bus_word_ps(pmb887x_dif_t *p) {
	uint32_t hz = pmb887x_clc_get_hz(&p->clc);
	if (dif_is_serial(p) || hz == 0)
		return 0;

	uint32_t access_cycle = (p->lcdtim[0] & DIFv2_LCDTIM1_ACCESSCYCLE) >> DIFv2_LCDTIM1_ACCESSCYCLE_SHIFT;
	uint32_t cycles = ((access_cycle - 1) & 0x1F) + 2;
	return cycles * PS_PER_S / hz;
}

static void dif_retire_tx_bus(pmb887x_dif_t *p) {
	int64_t now_ps = dif_get_now_ps();
	while (p->tx_bus_count != 0 && p->tx_bus_end_ps[p->tx_bus_first] <= now_ps) {
		p->tx_bus_first = (p->tx_bus_first + 1) % FIFO_SIZE;
		p->tx_bus_count--;
	}
}

static void dif_reset_tx_bus(pmb887x_dif_t *p) {
	timer_del(p->tx_bus_timer);
	p->tx_bus_first = 0;
	p->tx_bus_count = 0;
	p->tx_bus_words = 0;
	p->tx_bus_free_ps = 0;
	p->tx_isr_deferred = 0;
}

static uint32_t dif_get_tx_fifo_level(pmb887x_dif_t *p) {
	return pmb887x_fifo_count(&p->tx_fifo) + p->tx_bus_count;
}

static bool dif_has_tx_fifo_free(pmb887x_dif_t *p, uint32_t stages) {
	if (FIFO_SIZE - dif_get_tx_fifo_level(p) >= stages)
		return true;

	dif_retire_tx_bus(p);
	return FIFO_SIZE - dif_get_tx_fifo_level(p) >= stages;
}

static void dif_occupy_tx_bus(pmb887x_dif_t *p, uint64_t word_ps) {
	uint32_t words = p->tx_bus_words;
	p->tx_bus_words = 0;
	if (word_ps == 0 || words == 0)
		return;

	g_assert(p->tx_bus_count < FIFO_SIZE);
	int64_t end_ps = MAX(p->tx_bus_free_ps, dif_get_now_ps()) + words * word_ps;
	p->tx_bus_end_ps[(p->tx_bus_first + p->tx_bus_count) % FIFO_SIZE] = end_ps;
	p->tx_bus_count++;
	p->tx_bus_free_ps = end_ps;
}

static void dif_wake_on_tx_fifo_free(pmb887x_dif_t *p, uint32_t stages) {
	uint32_t retiring_stages = stages - (FIFO_SIZE - dif_get_tx_fifo_level(p));
	if (p->tx_bus_count < retiring_stages)
		return;

	int64_t end_ps = p->tx_bus_end_ps[(p->tx_bus_first + retiring_stages - 1) % FIFO_SIZE];
	int64_t wake_ns = DIV_ROUND_UP(end_ps, 1000);
	if ((int64_t) timer_expire_time_ns(p->tx_bus_timer) != wake_ns)
		timer_mod(p->tx_bus_timer, wake_ns);
}

/*
 * Firmware may set a TX DMA request through ISR while earlier bursts are still
 * on the parallel bus. The DMAC serves a request at once, so apply it only when
 * the FIFO has room for the transfer instead of overflowing the FIFO.
 */
static uint32_t dif_defer_tx_isr(pmb887x_dif_t *p, uint32_t value) {
	uint32_t requests = value & DIF_TX_DMA_REQUESTS;
	if (requests == 0)
		return value;

	uint32_t stages = (requests & (DIFv2_ISR_TXBREQ | DIFv2_ISR_TXLBREQ)) != 0 ?
		dif_get_tx_burst_size(p) / (4 / dif_get_tx_align(p)) : 1;
	if (dif_has_tx_fifo_free(p, stages))
		return value;

	p->tx_isr_deferred |= requests;
	dif_wake_on_tx_fifo_free(p, stages);
	return value & ~requests;
}

static uint32_t dif_get_pin_levels(pmb887x_dif_t *p) {
	uint32_t csreg = dif_get_transfer_csreg(p);
	uint32_t active_pins = (
		((csreg & DIFv2_CSREG_CS1) ? DIFv2_PERREG_CS1POL : 0) |
		((csreg & DIFv2_CSREG_CS2) ? DIFv2_PERREG_CS2POL : 0) |
		((csreg & DIFv2_CSREG_CS3) ? DIFv2_PERREG_CS3POL : 0) |
		((csreg & DIFv2_CSREG_CD) ? DIFv2_PERREG_CDPOL : 0) |
		(p->state == DIF_STATE_RX ? DIFv2_PERREG_RDPOL : DIFv2_PERREG_WRPOL)
	);
	return ~(active_pins ^ p->perreg) & DIF_PINS_POLARITY_MASK;
}

static void dif_update_gpio_state(pmb887x_dif_t *p) {
	uint32_t pin_levels = dif_get_pin_levels(p);
	uint32_t changed_pins = p->are_pins_driven ? pin_levels ^ p->pin_levels : DIF_PINS_POLARITY_MASK;
	if (changed_pins == 0)
		return;

	struct {
		uint32_t polarity;
		qemu_irq pin;
		const char *name;
	} pins[] = {
		{ DIFv2_PERREG_CS1POL, p->gpio_cs[0], "CS1" },
		{ DIFv2_PERREG_CS2POL, p->gpio_cs[1], "CS2" },
		{ DIFv2_PERREG_CS3POL, p->gpio_cs[2], "CS3" },
		{ DIFv2_PERREG_CDPOL, p->gpio_cd, "CD" },
		{ DIFv2_PERREG_RDPOL, p->gpio_rd, "RD" },
		{ DIFv2_PERREG_WRPOL, p->gpio_wr, "WR" },
	};
	p->pin_levels = pin_levels;
	p->are_pins_driven = true;
	for (int i = 0; i < ARRAY_SIZE(pins); i++) {
		if ((changed_pins & pins[i].polarity) == 0)
			continue;

		bool level = (pin_levels & pins[i].polarity) != 0;
		DPRINTF("%s set %s\n", pins[i].name, level ? "HIGH" : "LOW");
		qemu_set_irq(pins[i].pin, level);
	}
}

static void dif_schedule(pmb887x_dif_t *p) {
	p->transfer_pending = true;
	if (p->in_schedule)
		return;

	p->in_schedule = true;
	while (p->transfer_pending) {
		p->transfer_pending = false;
		dif_work(p);
		if (pmb887x_srb_get_ris(&p->srb) != 0)
			break;
	}
	p->in_schedule = false;
}

static void dif_trigger_dma(pmb887x_dif_t *p) {
	uint32_t requests = pmb887x_srb_get_ris_dma(&p->srb) & (DIF_TX_DMA_REQUESTS | DIF_RX_DMA_REQUESTS);
	if (p->dmac_tx_clr)
		requests &= ~DIF_TX_DMA_REQUESTS;
	if (p->dmac_rx_clr)
		requests &= ~DIF_RX_DMA_REQUESTS;

	uint32_t changed_requests = p->are_dma_requests_driven ? requests ^ p->dma_requests :
		DIF_TX_DMA_REQUESTS | DIF_RX_DMA_REQUESTS;
	if (changed_requests == 0)
		return;

	struct {
		uint32_t request;
		qemu_irq line;
	} lines[] = {
		{ DIFv2_RIS_TXSREQ, p->dmac_tx_sreq },
		{ DIFv2_RIS_TXBREQ, p->dmac_tx_breq },
		{ DIFv2_RIS_TXLSREQ, p->dmac_tx_lsreq },
		{ DIFv2_RIS_TXLBREQ, p->dmac_tx_lbreq },
		{ DIFv2_RIS_RXSREQ, p->dmac_rx_sreq },
		{ DIFv2_RIS_RXBREQ, p->dmac_rx_breq },
		{ DIFv2_RIS_RXLSREQ, p->dmac_rx_lsreq },
		{ DIFv2_RIS_RXLBREQ, p->dmac_rx_lbreq },
	};
	p->dma_requests = requests;
	p->are_dma_requests_driven = true;
	for (int i = 0; i < ARRAY_SIZE(lines); i++) {
		if ((changed_requests & lines[i].request) != 0)
			qemu_set_irq(lines[i].line, (requests & lines[i].request) != 0);
	}
}

/*
 * A bus wake raising the TX burst request runs no guest code before the DMAC
 * pass serves it, and the DMAC alone sees the request line, so the DMAC can
 * serve it at once: the request still raises and clears the TX interrupt as it
 * would, only the line and the pass are skipped.
 */
static bool dif_serve_tx_burst(pmb887x_dif_t *p) {
	if (!p->in_tx_bus_wake || p->dmac_tx_clr || p->tx_isr_deferred != 0)
		return false;
	if ((pmb887x_srb_get_dmae(&p->srb) & DIFv2_ISR_TXBREQ) == 0 || pmb887x_srb_get_ris(&p->srb) != 0)
		return false;
	if (!p->are_dma_requests_driven || (p->dma_requests & DIF_TX_DMA_REQUESTS) != 0)
		return false;

	p->in_dma_handshake = true;
	pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_TXBREQ);
	p->tx_fifo_req = true;
	bool is_served = pmb887x_dmac_serve_burst(p->dmac_tx_breq);
	if (is_served)
		pmb887x_srb_set_icr(&p->srb, DIFv2_ICR_TXBREQ);
	p->in_dma_handshake = false;

	if (is_served) {
		p->tx_fifo_req = false;
		dif_schedule(p);
	} else {
		dif_trigger_dma(p);
	}
	return true;
}

static void dif_raise_tx_burst_request(pmb887x_dif_t *p) {
	if (dif_serve_tx_burst(p))
		return;
	pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_TXBREQ);
	p->tx_fifo_req = true;
}

static void dif_tx_fifo_req(pmb887x_dif_t *p) {
	if (p->tx_fifo_req)
		return;

	uint32_t burst_req_size = dif_get_tx_burst_size(p);
	uint32_t single_req_size = 4 / dif_get_tx_align(p);
	uint32_t burst_req_count = burst_req_size / single_req_size;

	if (p->state == DIF_STATE_NONE) {
		if (!dif_is_running(p) || dif_is_serial(p))
			return;
		if ((p->txfifo_cfg & DIFv2_TXFIFO_CFG_TXFC) != 0)
			return;
		if (!dif_has_tx_fifo_free(p, burst_req_count)) {
			dif_wake_on_tx_fifo_free(p, burst_req_count);
			return;
		}
		pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_TXBREQ);
		p->tx_fifo_req = true;
		return;
	}

	if (p->state != DIF_STATE_TX)
		return;

	uint32_t tx_words_remaining = p->tx_words_remaining - MIN(p->tx_words_remaining,
		pmb887x_fifo_count(&p->tx_fifo) * single_req_size);
	uint32_t pending_req_count = DIV_ROUND_UP(tx_words_remaining, single_req_size);

	if (tx_words_remaining == 0)
		return;

	bool is_flow_controlled = (p->txfifo_cfg & DIFv2_TXFIFO_CFG_TXFC) != 0;
	uint32_t required_free = (!is_flow_controlled || tx_words_remaining >= burst_req_size) ? burst_req_count : 1;
	if (!dif_has_tx_fifo_free(p, required_free)) {
		dif_wake_on_tx_fifo_free(p, required_free);
		return;
	}

	if (is_flow_controlled) {
		if (tx_words_remaining > burst_req_size) {
			dif_raise_tx_burst_request(p);
			return;
		} else if (pending_req_count == burst_req_count) {
			pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_TXLBREQ);
		} else if (tx_words_remaining > single_req_size) {
			pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_TXSREQ);
		} else {
			pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_TXLSREQ);
		}
	} else {
		dif_raise_tx_burst_request(p);
		return;
	}
	p->tx_fifo_req = true;
}

static void dif_rx_fifo_req(pmb887x_dif_t *p) {
	if (p->state != DIF_STATE_TX && p->state != DIF_STATE_RX)
		return;
	if (p->rx_fifo_req)
		return;
	if (p->rx_words_in_fifo == 0)
		return;

	uint32_t burst_req_size = dif_get_rx_burst_size(p);
	uint32_t single_req_size = 4 / dif_get_rx_align(p);
	uint32_t pending_req_count = DIV_ROUND_UP(p->rx_words_in_fifo, single_req_size);
	uint32_t burst_req_count = burst_req_size / single_req_size;
	bool is_packet_complete = (
		(p->state == DIF_STATE_RX && p->rx_words_remaining == 0) ||
		(p->state == DIF_STATE_TX && p->tx_words_remaining == 0)
	);

	if ((p->rxfifo_cfg & DIFv2_RXFIFO_CFG_RXFC) != 0) {
		if (!is_packet_complete) {
			if (p->rx_words_in_fifo < burst_req_size)
				return;
			pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_RXBREQ);
		} else if (pending_req_count == burst_req_count) {
			pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_RXLBREQ);
		} else if (p->rx_words_in_fifo > single_req_size) {
			pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_RXSREQ);
		} else {
			pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_RXLSREQ);
		}
	} else if (p->rx_words_in_fifo >= burst_req_size) {
		pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_RXBREQ);
	} else {
		pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_RXSREQ);
	}
	p->rx_fifo_req = true;
}

static void dif_fifo_req(pmb887x_dif_t *p) {
	dif_tx_fifo_req(p);
	dif_rx_fifo_req(p);
}

static void dif_fifo_clr_req(pmb887x_dif_t *p, uint32_t mask) {
	if ((mask & (DIFv2_ICR_TXLSREQ | DIFv2_ICR_TXSREQ | DIFv2_ICR_TXLBREQ | DIFv2_ICR_TXBREQ)) != 0)
		p->tx_fifo_req = false;
	if ((mask & (DIFv2_ICR_RXLSREQ | DIFv2_ICR_RXSREQ | DIFv2_ICR_RXLBREQ | DIFv2_ICR_RXBREQ)) != 0)
		p->rx_fifo_req = false;
}

static void dif_kernel_reset_state(pmb887x_dif_t *p, uint32_t new_state) {
	p->state = new_state;
	p->tx_words_remaining = 0;
	p->rx_words_remaining = 0;
	p->tx_fifo_req = false;
	p->rx_fifo_req = false;
	p->is_tx_started = false;
	dif_update_gpio_state(p);
}

static void dif_kernel_reset(pmb887x_dif_t *p, uint32_t new_state) {
	dif_kernel_reset_state(p, new_state);
	dif_schedule(p);
}

static void dif_reset_rx_fifo(pmb887x_dif_t *p) {
	pmb887x_fifo_reset(&p->rx_fifo);
	p->rx_words_in_fifo = 0;
	p->rx_buffer = 0;
	p->rx_buffer_word_count = 0;
}

static inline uint32_t dif_mux(pmb887x_dif_t *p, uint32_t value) {
	bool is_mux_bypassed = p->is_mux_identity || (dif_get_transfer_csreg(p) & DIFv2_CSREG_CD) != 0;
	uint32_t selected = value;
	if (!is_mux_bypassed) {
		selected = (
			p->mux_byte_bits[0][value & 0xFF] |
			p->mux_byte_bits[1][(value >> 8) & 0xFF] |
			p->mux_byte_bits[2][(value >> 16) & 0xFF] |
			p->mux_byte_bits[3][value >> 24]
		);
	}
	return ((selected & p->mux_select_mask) | p->mux_constant_bits) ^ p->invert_bit;
}

static uint32_t dif_convert_color(pmb887x_dif_t *p, uint32_t value) {
	int32_t input[3] = {
		value & 0xFF,
		(value >> 8) & 0xFF,
		(value >> 16) & 0xFF,
	};
	uint32_t output = 0;

	for (uint32_t row = 0; row < 3; row++) {
		int32_t result = 0;

		for (uint32_t column = 0; column < 3; column++) {
			int32_t coefficient = sextract32(p->coeff[row], column * 10, 10);
			int32_t offset = sextract32(p->coeff[3], column * 10, 10);

			result += coefficient * (input[column] - offset);
		}
		result >>= 7;
		if (result < 0) {
			result = 0;
		} else if (result > 0xFF) {
			result = 0xFF;
		}
		output |= result << (row * 8);
	}

	return output;
}

static void dif_rx_from_bus(pmb887x_dif_t *p);

static void dif_tx_fifo_push(pmb887x_dif_t *p, uint32_t value) {
	pmb887x_fifo32_push(&p->tx_fifo, value);
	pmb887x_fifo32_push(&p->tx_csreg_fifo, p->csreg);
}

static void dif_fifo_write(pmb887x_dif_t *p, uint32_t value) {
	if (!dif_has_tx_fifo_free(p, 1)) {
		DPRINTF("TX FIFO overflow!\n");
		pmb887x_srb_ext_set_isr(&p->srb_err, DIFv2_ERRIRQSS_TXFOFL);
		dif_kernel_reset(p, DIF_STATE_NONE);
		return;
	}

	DPRINTF("TXD %08X csreg=%02X run=%d state=%d fifo=%d\n", value, p->csreg, dif_is_running(p),
		p->state, pmb887x_fifo_count(&p->tx_fifo));

	dif_tx_fifo_push(p, value);
	dif_schedule(p);
}

static bool dif_can_write_tx_block(pmb887x_dif_t *p, uint32_t count) {
	if (dif_is_serial(p))
		return false;
	if (!p->tx_fifo_req)
		return false;
	if (p->rx_words_in_fifo != 0)
		return false;
	if (!dif_has_tx_fifo_free(p, count))
		return false;
	if (p->state == DIF_STATE_NONE)
		return true;
	if (p->state != DIF_STATE_TX)
		return false;

	uint32_t words_per_entry = 4 / dif_get_tx_align(p);
	uint32_t queued_words = pmb887x_fifo_count(&p->tx_fifo) * words_per_entry;
	return p->tx_words_remaining > queued_words + (count - 1) * words_per_entry;
}

static bool dif_dma_write_block(
	pmb887x_dma_sink_t *sink,
	MemoryRegion *mr,
	hwaddr addr,
	const uint8_t *data,
	uint32_t width,
	uint32_t count,
	bool is_increment
) {
	pmb887x_dif_t *p = PMB887X_DIF(sink);
	if (mr != &p->mmio)
		return false;
	if (width != sizeof(uint32_t))
		return false;

	hwaddr last_addr = is_increment ? addr + (count - 1) * width : addr;
	if (addr < DIFv2_TXD || last_addr > DIFv2_TXD + FIFO_IO_SIZE)
		return false;
	if (!dif_can_write_tx_block(p, count))
		return false;

	for (uint32_t i = 0; i < count; i++) {
		hwaddr word_addr = is_increment ? addr + i * width : addr;
		uint32_t value = ldl_le_p(data + i * width);
		IO_DUMP_WRITE(word_addr + p->mmio.addr, width, value);
		dif_tx_fifo_push(p, value);
	}
	dif_schedule(p);
	return true;
}

static void dif_fifo_read(pmb887x_dif_t *p, uint64_t *value) {
	if (pmb887x_fifo_is_empty(&p->rx_fifo)) {
		DPRINTF("RX FIFO underflow!\n");
		pmb887x_srb_ext_set_isr(&p->srb_err, DIFv2_ERRIRQSS_RXFUFL);
		dif_kernel_reset(p, DIF_STATE_NONE);
		return;
	}

	*value = pmb887x_fifo32_pop(&p->rx_fifo);
	p->rx_words_in_fifo -= MIN(p->rx_words_in_fifo, 4 / dif_get_rx_align(p));

	if (p->rx_words_remaining != 0 || p->rx_words_in_fifo == 0)
		qemu_bh_schedule(p->rx_refill_bh);
}

static bool dif_is_running(pmb887x_dif_t *p) {
	return pmb887x_clc_is_enabled(&p->clc) && pmb887x_clc_get_rmc(&p->clc) && p->runctrl;
}

static void dif_start_tx(pmb887x_dif_t *p) {
	if (!dif_is_running(p) || p->state != DIF_STATE_NONE)
		return;

	uint32_t tx_word_count = p->tps_ctrl;
	p->tps_ctrl = 0;
	uint32_t preloaded_words = MIN(tx_word_count, p->tx_words_preloaded);
	p->tx_words_preloaded -= preloaded_words;
	tx_word_count -= preloaded_words;

	if (!tx_word_count)
		return;

	DPRINTF("new transfer: tx=%d\n", tx_word_count);
	dif_kernel_reset_state(p, DIF_STATE_TX);
	p->tx_words_remaining = tx_word_count;
	p->is_tx_started = preloaded_words != 0;
	p->rx_packet_words = 0;
	p->rx_buffer = 0;
	p->rx_buffer_word_count = 0;
	p->is_pbc_pair_completed = false;
	dif_fifo_req(p);
	dif_schedule(p);
}

static void dif_start_rx(pmb887x_dif_t *p) {
	if (!dif_is_running(p) || p->state != DIF_STATE_NONE)
		return;

	uint32_t rx_word_count = 0;
	if ((p->startlcdrd & DIFv2_STARTLCDRD_STARTREAD) != 0) {
		rx_word_count = ((p->startlcdrd & DIFv2_STARTLCDRD_READBYTES) >> DIFv2_STARTLCDRD_READBYTES_SHIFT) + 1;
	} else if (p->mrps_ctrl > 0) {
		rx_word_count = p->mrps_ctrl;
		p->mrps_ctrl = 0;
	}

	if (!rx_word_count)
		return;

	DPRINTF("new transfer: rx=%d\n", rx_word_count);
	dif_kernel_reset_state(p, DIF_STATE_RX);
	p->rx_words_remaining = rx_word_count;
	p->rx_packet_words = rx_word_count;
	p->rx_buffer = 0;
	p->rx_buffer_word_count = 0;
	dif_rx_from_bus(p);
	dif_fifo_req(p);
}

static bool dif_push_rx_word(pmb887x_dif_t *p, uint16_t value) {
	uint32_t align = dif_get_rx_align(p);
	bool is_packed_9bit = dif_is_serial(p) && dif_is_bsconf_9bit(p);
	uint32_t words_per_stage = is_packed_9bit ? dif_get_bsconf_word_count(p) : 4 / align;
	uint32_t word_shift = is_packed_9bit ? 9 : align * 8;

	p->rx_buffer |= ((uint32_t) value << (p->rx_buffer_word_count * word_shift));
	p->rx_buffer_word_count++;

	if (p->rx_buffer_word_count == words_per_stage) {
		uint32_t received_words = is_packed_9bit ? 1 : p->rx_buffer_word_count;

		if (pmb887x_fifo_is_full(&p->rx_fifo)) {
			DPRINTF("RX FIFO overflow!\n");
			pmb887x_srb_ext_set_isr(&p->srb_err, DIFv2_ERRIRQSS_RXFOFL);
			dif_kernel_reset(p, DIF_STATE_NONE);
			return false;
		}

		if (p->state == DIF_STATE_RX) {
			p->rx_words_remaining -= received_words;
		} else {
			p->rx_packet_words += received_words;
		}
		p->rx_words_in_fifo += received_words;
		pmb887x_fifo32_push(&p->rx_fifo, p->rx_buffer);
		p->rx_buffer = 0;
		p->rx_buffer_word_count = 0;
	}

	return true;
}

static bool dif_flush_rx_buffer(pmb887x_dif_t *p) {
	if (p->rx_buffer_word_count != 0) {
		uint32_t received_words = dif_is_bsconf_9bit(p) ? 1 : p->rx_buffer_word_count;

		if (pmb887x_fifo_is_full(&p->rx_fifo)) {
			DPRINTF("RX FIFO overflow!\n");
			pmb887x_srb_ext_set_isr(&p->srb_err, DIFv2_ERRIRQSS_RXFOFL);
			dif_kernel_reset(p, DIF_STATE_NONE);
			return false;
		}

		if (p->state == DIF_STATE_RX) {
			p->rx_words_remaining -= received_words;
		} else {
			p->rx_packet_words += received_words;
		}
		p->rx_words_in_fifo += received_words;
		pmb887x_fifo32_push(&p->rx_fifo, p->rx_buffer);
		p->rx_buffer = 0;
		p->rx_buffer_word_count = 0;
	}

	return true;
}

static void dif_rx_from_bus(pmb887x_dif_t *p) {
	if (p->state != DIF_STATE_RX || (p->startlcdrd & DIFv2_STARTLCDRD_STARTREAD) == 0)
		return;

	uint32_t words_per_stage = 4 / dif_get_rx_align(p);
	uint32_t words_to_receive = MIN(p->rx_words_remaining, dif_get_rx_burst_size(p));
	while (words_to_receive != 0 && !pmb887x_fifo_is_full(&p->rx_fifo)) {
		uint32_t word_count = MIN(words_per_stage, p->rx_words_remaining);
		word_count = MIN(word_count, words_to_receive);

		for (uint32_t i = 0; i < word_count; i++) {
			if (!dif_push_rx_word(p, dif_bus_transfer(p, 0)))
				return;
		}
		if (word_count != words_per_stage)
			dif_flush_rx_buffer(p);
		dif_rx_fifo_req(p);
		if (p->rx_fifo_req)
			return;
		words_to_receive -= word_count;
	}
}

static uint16_t dif_bus_transfer(pmb887x_dif_t *p, uint16_t value) {
	if (dif_is_serial(p) && (p->con & DIFv2_CON_LB) != 0)
		return value;

	return ssi_transfer(p->bus, value);
}

static bool dif_send_word(pmb887x_dif_t *p, uint16_t value) {
	DPRINTF("BUS %03X cd=%d\n", value, (dif_get_transfer_csreg(p) & DIFv2_CSREG_CD) != 0);

	uint16_t received = dif_bus_transfer(p, value);
	p->tx_bus_words++;
	if (dif_is_serial(p))
		return dif_push_rx_word(p, received);

	return true;
}

static void dif_send_parallel_words(pmb887x_dif_t *p, const uint32_t *words, uint32_t count) {
	DPRINTF("BUS %u words cd=%d\n", count, (dif_get_transfer_csreg(p) & DIFv2_CSREG_CD) != 0);

	ssi_transfer_block(p->bus, words, NULL, count);
}

static bool dif_convert_word(pmb887x_dif_t *p, uint16_t value, uint32_t *output) {
	if (!dif_is_pbc_enabled(p)) {
		uint32_t word_mask = dif_get_word_mask(dif_get_tx_word_bits(p));

		*output = dif_mux(p, value & word_mask);
		return true;
	}

	if (!p->is_pbc_word_valid) {
		p->pbc_word = value;
		p->is_pbc_word_valid = true;
		return false;
	}

	uint32_t input = (p->pbc_word | ((uint32_t) value << 16));

	*output = dif_mux(p, input);
	p->is_pbc_word_valid = false;
	p->is_pbc_pair_completed = true;
	return true;
}

static bool dif_is_pbc_selection_valid(pmb887x_dif_t *p) {
	uint32_t word_bits = dif_get_tx_word_bits(p);

	for (uint32_t output_bit = 0; output_bit < word_bits; output_bit++) {
		if (p->bit_bcsel[output_bit] > 1)
			return false;
	}

	return true;
}

static void dif_tx_from_fifo(pmb887x_dif_t *p) {
	if (p->state != DIF_STATE_TX && p->state != DIF_STATE_NONE)
		return;

	uint32_t align = dif_get_tx_align(p);
	uint32_t word_bits = dif_get_tx_word_bits(p);

	uint64_t bus_word_ps = dif_get_bus_word_ps(p);
	uint32_t parallel_words[FIFO_SIZE * 4 * 4];
	uint32_t parallel_word_count = 0;
	while (pmb887x_fifo_count(&p->tx_fifo) > 0) {
		uint32_t value = pmb887x_fifo32_pop(&p->tx_fifo);
		uint32_t tx_csreg = pmb887x_fifo32_pop(&p->tx_csreg_fifo);
		if (parallel_word_count != 0 && tx_csreg != p->tx_csreg) {
			dif_send_parallel_words(p, parallel_words, parallel_word_count);
			parallel_word_count = 0;
		}
		p->tx_csreg = tx_csreg;
		p->is_tx_csreg_active = true;
		dif_update_gpio_state(p);
		uint32_t bsconf_word_count = dif_get_bsconf_word_count(p);
		uint32_t bsconf_word_bits = dif_is_serial(p) && dif_is_bsconf_9bit(p) ? 9 : 8;
		if (!dif_is_serial(p) && dif_is_bsconf_9bit(p))
			bsconf_word_count = 1;
		uint32_t words_in_fifo_reg = MIN(4 / align, p->tx_words_remaining);
		bool is_direct_write = p->state == DIF_STATE_NONE;

		if (p->state == DIF_STATE_TX)
			p->is_tx_started = true;
		p->tx_words_remaining -= words_in_fifo_reg;
		if (words_in_fifo_reg == 0 && (bsconf_word_count == 0 || is_direct_write))
			words_in_fifo_reg = 4 / align; // Direct TXD writes do not require TPS_CTRL.
		if (p->state == DIF_STATE_NONE)
			p->tx_words_preloaded += bsconf_word_count != 0 ? 1 : words_in_fifo_reg;

		if (bsconf_word_count != 0) {
			uint32_t converted;
			bool is_color_configured = (p->coeff[0] | p->coeff[1] | p->coeff[2] | p->coeff[3]) != 0;

			if (!dif_is_pbc_enabled(p) && (!dif_is_serial(p) || !dif_is_bsconf_9bit(p) || is_color_configured)) {
				value = dif_mux(p, value);
			} else {
				if (!dif_convert_word(p, value, &converted))
					continue;
				value = converted;
			}
			if ((dif_get_transfer_csreg(p) & DIFv2_CSREG_GRACMD) != 0)
				value = dif_convert_color(p, value);

			uint32_t lanes = 4 / align;
			for (uint32_t lane = 0; lane < lanes; lane++) {
				for (uint32_t i = 0; i < bsconf_word_count; i++) {
					uint16_t word = (value >> (bsconf_word_bits * (lane * align + i))) & ((1 << bsconf_word_bits) - 1);

					if (!dif_is_serial(p)) {
						parallel_words[parallel_word_count++] = word;
						p->tx_bus_words++;
					} else if (!dif_send_word(p, word)) {
						goto done;
					}
				}
			}
		} else {
			if (parallel_word_count != 0) {
				dif_send_parallel_words(p, parallel_words, parallel_word_count);
				parallel_word_count = 0;
			}
			for (uint32_t i = 0; i < words_in_fifo_reg; i++) {
				uint32_t converted;
				uint16_t word = (value >> (8 * i * align)) & dif_get_word_mask(word_bits);

				if (dif_convert_word(p, word, &converted) && !dif_send_word(p, converted))
					goto done;
			}
		}
		dif_occupy_tx_bus(p, bus_word_ps);
		if (p->tx_words_remaining != 0) {
			dif_rx_fifo_req(p);
			if (p->rx_fifo_req)
				goto done;
		}
	}

	if (p->tx_words_remaining == 0 && dif_is_serial(p))
		dif_flush_rx_buffer(p);

done:
	if (parallel_word_count != 0)
		dif_send_parallel_words(p, parallel_words, parallel_word_count);
	p->is_tx_csreg_active = false;
	dif_update_gpio_state(p);
}

static void dif_work(pmb887x_dif_t *p) {
	if (p->state == DIF_STATE_NONE) {
		if (p->tps_ctrl > 0) {
			dif_start_tx(p);
		} else if (p->mrps_ctrl > 0) {
			dif_start_rx(p);
		} else if (dif_is_running(p) && !dif_is_serial(p)) {
			if (!pmb887x_fifo_is_empty(&p->tx_fifo))
				dif_tx_from_fifo(p);
			dif_tx_fifo_req(p);
		}
	} else if (p->state == DIF_STATE_TX) {
		dif_tx_from_fifo(p);

		if (p->tx_words_remaining == 0) {
			DPRINTF("transfer done\n");
			if (dif_is_serial(p)) {
				p->state = DIF_STATE_RX;
				bool is_phase_error = (
					!dif_is_pbc_enabled(p) ||
					(p->is_pbc_pair_completed && dif_is_pbc_selection_valid(p))
				);
				if (is_phase_error)
					pmb887x_srb_ext_set_isr(&p->srb_err, DIFv2_ERRIRQSS_PHASE);
				dif_fifo_req(p);
				if (p->rx_words_remaining == 0 && p->rx_words_in_fifo == 0) {
					p->state = DIF_STATE_NONE;
					dif_update_gpio_state(p);
				}
			} else {
				dif_kernel_reset(p, DIF_STATE_NONE);
			}
		} else {
			dif_fifo_req(p);
		}
	} else if (p->state == DIF_STATE_RX) {
		dif_rx_from_bus(p);
		dif_fifo_req(p);
		if (p->rx_words_remaining == 0 && p->rx_words_in_fifo == 0) {
			DPRINTF("transfer done\n");
			dif_kernel_reset(p, DIF_STATE_NONE);
		}
	}
}

static void dif_rx_refill(void *opaque) {
	dif_schedule(opaque);
}

static void dif_tx_bus_wake(void *opaque) {
	pmb887x_dif_t *p = opaque;
	uint32_t deferred = p->tx_isr_deferred;

	p->in_tx_bus_wake = true;
	dif_schedule(p);
	p->in_tx_bus_wake = false;
	if (deferred != 0) {
		p->tx_isr_deferred &= ~deferred;
		pmb887x_srb_set_isr(&p->srb, dif_defer_tx_isr(p, deferred));
	}
}

static void dif_update_mux(pmb887x_dif_t *p) {
	p->mux_select_mask = 0;
	p->mux_constant_bits = 0;
	p->is_mux_identity = true;

	for (uint32_t i = 0; i < 32; i++) {
		// DIF_BMREGx
		uint32_t bm_reg_index = i / 6;
		uint32_t bm_shift = i * 5 - (bm_reg_index * (5 * 6));
		if (bm_shift >= 15)
			bm_shift++;
		p->bit_mux[i] = (p->bmreg[bm_reg_index] >> bm_shift) & 0x1F;

		// DIF_BCSELx
		uint32_t bcsel_reg_index = i / 16;
		uint32_t bcsel_shift = i * 2 - (bcsel_reg_index * 32);
		p->bit_bcsel[i] = (p->bcsel[bcsel_reg_index] >> bcsel_shift) & 3;

		// DIF_BCREG
		p->bit_bcreg[i] = p->bcreg & (1 << i) ? 1 : 0;

		// DIF_INVERT_BIT
		p->bit_invert[i] = p->invert_bit & (1 << i) ? 1 : 0;

		if (p->bit_bcsel[i] == 0) {
			p->mux_select_mask |= 1U << i;
			if (p->bit_mux[i] != i)
				p->is_mux_identity = false;
		} else if (p->bit_bcsel[i] == 1) {
			p->mux_constant_bits |= (uint32_t) p->bit_bcreg[i] << i;
		}
	}

	uint32_t input_bit_outputs[32] = { 0 };
	for (uint32_t output_bit = 0; output_bit < 32; output_bit++)
		input_bit_outputs[p->bit_mux[output_bit]] |= 1U << output_bit;

	for (uint32_t lane = 0; lane < 4; lane++) {
		uint32_t *outputs = p->mux_byte_bits[lane];
		outputs[0] = 0;
		for (uint32_t byte = 1; byte < 256; byte++) {
			uint32_t lowest_bit = ctz32(byte);
			outputs[byte] = outputs[byte & (byte - 1)] | input_bit_outputs[lane * 8 + lowest_bit];
		}
	}

#if PMB887X_DIF_DUMP_BIT_MUX
	g_autoptr(GString) mux_str = g_string_new("");
	g_autoptr(GString) bcsel_str = g_string_new("");
	g_autoptr(GString) bc_str = g_string_new("");
	g_autoptr(GString) invert_str = g_string_new("");

	for (uint32_t i = 0; i < 32; i++) {
		g_string_append_printf(mux_str, " %3d", p->bit_mux[i]);
		g_string_append_printf(bcsel_str, " %3d", p->bit_bcsel[i]);
		g_string_append_printf(bc_str, " %3d", p->bit_bcreg[i]);
		g_string_append_printf(invert_str, " %3d", p->bit_invert[i]);
	}

	DPRINTF(
		"BMREGx = { %08X, %08X, %08X, %08X, %08X, %08X }\n",
		p->bmreg[0], p->bmreg[1], p->bmreg[2],
		p->bmreg[3],p->bmreg[4], p->bmreg[5]
	);
	DPRINTF(
		"BCSELx = { %08X, %08X }, BCREG = %08X, INVERT_BIT = %08X\n",
		 p->bcsel[0], p->bcsel[1], p->bcreg, p->invert_bit
	);
	DPRINTF("   MUX: %s\n", mux_str->str);
	DPRINTF(" BCSEL: %s\n", bcsel_str->str);
	DPRINTF("    BC: %s\n", bc_str->str);
	DPRINTF("INVERT: %s\n", invert_str->str);
#endif
}

static uint64_t dif_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_dif_t *p = opaque;

	uint64_t value = 0;
	switch (haddr) {
		case DIFv2_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case DIFv2_ID:
			value = 0xF043C000 | p->revision;
			break;

		case DIFv2_RUNCTRL:
			value = p->runctrl;
			break;

		case DIFv2_CON:
			value = p->con;
			break;

		case DIFv2_PERREG:
			value = p->perreg;
			break;

		case DIFv2_CSREG:
			value = p->csreg;
			break;

		case DIFv2_LCDTIM1:
		case DIFv2_LCDTIM2:
			value = p->lcdtim[(haddr - DIFv2_LCDTIM1) / 4];
			break;

		case DIFv2_STARTLCDRD:
			value = p->startlcdrd;
			break;

		case DIFv2_STAT:
			dif_retire_tx_bus(p);
			if (dif_is_running(p)) {
				bool is_busy = (
					(p->state == DIF_STATE_TX && p->is_tx_started) ||
					(p->state == DIF_STATE_RX && (p->startlcdrd & DIFv2_STARTLCDRD_STARTREAD) != 0) ||
					dif_get_tx_fifo_level(p) > 0
				);
				value |= is_busy ? DIFv2_STAT_BSY : 0;
			}
			break;

		case DIFv2_COEFF_REG1:
		case DIFv2_COEFF_REG2:
		case DIFv2_COEFF_REG3:
		case DIFv2_OFFSET:
			value = p->coeff[(haddr - DIFv2_COEFF_REG1) / 4];
			break;

		case DIFv2_BMREG0:
		case DIFv2_BMREG1:
		case DIFv2_BMREG2:
		case DIFv2_BMREG3:
		case DIFv2_BMREG4:
		case DIFv2_BMREG5:
			value = p->bmreg[(haddr - DIFv2_BMREG0) / 4];
			break;

		case DIFv2_PBCCON:
			value = p->pbccon;
			break;

		case DIFv2_BCSEL0:
		case DIFv2_BCSEL1:
			value = p->bcsel[(haddr - DIFv2_BCSEL0) / 4];
			break;

		case DIFv2_BCREG:
			value = p->bcreg;
			break;

		case DIFv2_INVERT_BIT:
			value = p->invert_bit;
			break;

		case DIFv2_SYNC_CONFIG:
			value = p->sync_config;
			break;

		case DIFv2_SYNC_COUNT:
			value = p->sync_count;
			break;

		case DIFv2_BR:
			value = p->br;
			break;

		case DIFv2_FDIV:
			value = p->fdiv;
			break;

		case DIFv2_DEBUG:
			value = p->debug;
			break;

		case DIFv2_RXFIFO_CFG:
			value = p->rxfifo_cfg;
			break;

		case DIFv2_MRPS_CTRL:
			value = p->mrps_ctrl;
			hw_error("DIFv2_MRPS_CTRL not supported!");
			break;

		case DIFv2_RPS_STAT:
			value = p->rx_packet_words;
			break;

		case DIFv2_RXFFS_STAT:
			value = pmb887x_fifo_count(&p->rx_fifo);
			break;

		case DIFv2_TXFIFO_CFG:
			value = p->txfifo_cfg;
			break;

		case DIFv2_TPS_CTRL:
			value = p->tps_ctrl;
			break;

		case DIFv2_TXFFS_STAT:
			dif_retire_tx_bus(p);
			value = dif_get_tx_fifo_level(p);
			break;

		case DIFv2_ERRIRQSM:
			value = pmb887x_srb_ext_get_imsc(&p->srb_err);
			break;

		case DIFv2_ERRIRQSS:
			value = pmb887x_srb_ext_get_mis(&p->srb_err);
			break;

		case DIFv2_ERRIRQSC:
			value = 0;
			break;

		case DIFv2_RIS:
			value = pmb887x_srb_get_ris(&p->srb);
			break;

		case DIFv2_IMSC:
			value = pmb887x_srb_get_imsc(&p->srb);
			break;

		case DIFv2_MIS:
			value = pmb887x_srb_get_mis(&p->srb);
			break;

		case DIFv2_ICR:
		case DIFv2_ISR:
			value = 0;
			break;

		case DIFv2_DMAE:
			value = pmb887x_srb_get_dmae(&p->srb);
			break;

		case DIFv2_TXD ... (DIFv2_TXD + FIFO_IO_SIZE):
			value = 0;
			break;

		case DIFv2_RXD ... (DIFv2_RXD + FIFO_IO_SIZE):
			dif_fifo_read(p, &value);
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}

	IO_DUMP_READ(haddr + p->mmio.addr, size, value);

	return value;
}

static void dif_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_dif_t *p = opaque;

	IO_DUMP_WRITE(haddr + p->mmio.addr, size, value);

	switch (haddr) {
		case DIFv2_CLC:
			pmb887x_clc_set(&p->clc, value);
			/* Enabling the kernel has to drain whatever was written to TXD while it
			 * was stopped, the same way dif_v1 reschedules from its CLC handler. */
			if (dif_is_running(p))
				dif_schedule(p);
			break;

		case DIFv2_RUNCTRL:
			p->runctrl = value;
			if (!p->runctrl) {
				p->is_pbc_word_valid = false;
				p->tx_words_preloaded = 0;
			}
			if (!p->runctrl && (p->state == DIF_STATE_RX || p->state == DIF_STATE_TX)) {
				dif_kernel_reset(p, DIF_STATE_NONE);
				pmb887x_fifo_reset(&p->tx_fifo);
				pmb887x_fifo_reset(&p->tx_csreg_fifo);
				dif_reset_tx_bus(p);
				dif_reset_rx_fifo(p);
			}
			/* Without this a byte queued while stopped stays in the FIFO until the next
			 * TXD write, and a RUNCTRL=0 during an RX transaction discards it above. */
			if (dif_is_running(p))
				dif_schedule(p);
			break;

		case DIFv2_CON:
			p->con = value;
			break;

		case DIFv2_PERREG:
			p->perreg = value;
			dif_update_gpio_state(p);
			break;

		case DIFv2_CSREG:
			p->csreg = value;
			dif_update_gpio_state(p);
			break;

		case DIFv2_LCDTIM1:
		case DIFv2_LCDTIM2:
			p->lcdtim[(haddr - DIFv2_LCDTIM1) / 4] = value;
			break;

		case DIFv2_STARTLCDRD: {
			bool is_read_stopped = (
				(p->startlcdrd & DIFv2_STARTLCDRD_STARTREAD) != 0 &&
				(value & DIFv2_STARTLCDRD_STARTREAD) == 0
			);
			p->startlcdrd = dif_is_running(p) ? value : (value & ~DIFv2_STARTLCDRD_STARTREAD);
			if (is_read_stopped && p->state == DIF_STATE_RX) {
				dif_kernel_reset(p, DIF_STATE_NONE);
				dif_reset_rx_fifo(p);
			} else if ((p->startlcdrd & DIFv2_STARTLCDRD_STARTREAD) != 0) {
				dif_start_rx(p);
			}
			break;
		}

		case DIFv2_COEFF_REG1:
		case DIFv2_COEFF_REG2:
		case DIFv2_COEFF_REG3:
		case DIFv2_OFFSET:
			p->coeff[(haddr - DIFv2_COEFF_REG1) / 4] = value;
			break;

		case DIFv2_BMREG0:
		case DIFv2_BMREG1:
		case DIFv2_BMREG2:
		case DIFv2_BMREG3:
		case DIFv2_BMREG4:
		case DIFv2_BMREG5:
			p->bmreg[(haddr - DIFv2_BMREG0) / 4] = value;
			dif_update_mux(p);
			break;

		case DIFv2_PBCCON:
			p->pbccon = value;
			break;

		case DIFv2_BCSEL0:
		case DIFv2_BCSEL1:
			p->bcsel[(haddr - DIFv2_BCSEL0) / 4] = value;
			dif_update_mux(p);
			break;

		case DIFv2_BCREG:
			p->bcreg = value;
			dif_update_mux(p);
			break;

		case DIFv2_INVERT_BIT:
			p->invert_bit = value;
			dif_update_mux(p);
			break;

		case DIFv2_SYNC_CONFIG:
			p->sync_config = value;
			break;

		case DIFv2_SYNC_COUNT:
			p->sync_count = value;
			break;

		case DIFv2_BR:
			p->br = value;
			break;

		case DIFv2_FDIV:
			p->fdiv = value;
			break;

		case DIFv2_DEBUG:
			p->debug = value;
			break;

		case DIFv2_RXFIFO_CFG:
			p->rxfifo_cfg = value;
			break;

		case DIFv2_MRPS_CTRL:
			p->mrps_ctrl = value;
			dif_start_rx(p);
			break;

		case DIFv2_TXFIFO_CFG:
			p->txfifo_cfg = value;
			break;

		case DIFv2_TPS_CTRL:
			p->tps_ctrl = value;
			dif_start_tx(p);
			break;

		case DIFv2_ERRIRQSM:
			pmb887x_srb_ext_set_imsc(&p->srb_err, value);
			break;

		case DIFv2_ERRIRQSC:
			pmb887x_srb_ext_set_icr(&p->srb_err, value);
			break;

		case DIFv2_IMSC:
			pmb887x_srb_set_imsc(&p->srb, value);
			break;

		case DIFv2_ICR:
			pmb887x_srb_set_icr(&p->srb, value);
			break;

		case DIFv2_ISR:
			pmb887x_srb_set_isr(&p->srb, dif_defer_tx_isr(p, value));
			break;

		case DIFv2_DMAE:
			pmb887x_srb_set_dmae(&p->srb, value);
			dif_trigger_dma(p);
			break;

		case DIFv2_TXD ... (DIFv2_TXD + FIFO_IO_SIZE):
			dif_fifo_write(p, value);
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}
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

static void dif_handle_gpio_data_input(void *opaque, int id, int level) {
	// nothing
}

/*
 * A DMAC clear withdraws all four requests of a direction. Only the DMAC sees
 * the request lines, so react once to the cleared set instead of per request:
 * per request, a request not yet cleared re-raised the line for a moment and
 * cost the DMAC an empty pass, once per transferred word.
 */
static void dif_clear_dma_requests(pmb887x_dif_t *p, uint32_t requests) {
	p->in_dma_handshake = true;
	pmb887x_srb_set_icr(&p->srb, requests);
	p->in_dma_handshake = false;
	dif_fifo_clr_req(p, requests);
	dif_schedule(p);
}

static void dif_handle_dmac_tx_clr(void *opaque, int id, int level) {
	pmb887x_dif_t *p = opaque;
	p->dmac_tx_clr = level;
	if (level == 1)
		dif_clear_dma_requests(p, DIFv2_ICR_TXSREQ | DIFv2_ICR_TXBREQ | DIFv2_ICR_TXLSREQ | DIFv2_ICR_TXLBREQ);
	dif_trigger_dma(p);
}

static void dif_handle_dmac_rx_clr(void *opaque, int id, int level) {
	pmb887x_dif_t *p = opaque;
	p->dmac_rx_clr = level;
	if (level == 1)
		dif_clear_dma_requests(p, DIFv2_ICR_RXSREQ | DIFv2_ICR_RXBREQ | DIFv2_ICR_RXLSREQ | DIFv2_ICR_RXLBREQ);
	dif_trigger_dma(p);
}

static int dif_irq_router(void *opaque, int event_id) {
	switch ((1 << event_id)) {
		case DIFv2_ISR_RXLSREQ:
		case DIFv2_ISR_RXSREQ:
		case DIFv2_ISR_RXLBREQ:
			return DIF_RX_SINGLE_IRQ;
		case DIFv2_ISR_RXBREQ:
			return DIF_RX_BURST_IRQ;
		case DIFv2_ISR_TXLSREQ:
		case DIFv2_ISR_TXSREQ:
		case DIFv2_ISR_TXLBREQ:
		case DIFv2_ISR_TXBREQ:
			return DIF_TX_IRQ;
		case DIFv2_ISR_ERR:
			return DIF_ERROR_IRQ;
		default:
			hw_error("Unknown event id: %d\n", event_id);
	}
	return 0;
}

static void dif_event_handler(void *opaque, int event_id, int level) {
	pmb887x_dif_t *p = opaque;
	uint32_t mask = 1 << event_id;
	if (p->in_dma_handshake)
		return;
	if (level == 0 && (mask & FIFO_ICR_MASK) != 0) {
		dif_fifo_clr_req(p, mask);
		dif_schedule(p);
	}
	if ((mask & pmb887x_srb_get_dmae(&p->srb)) != 0)
		dif_trigger_dma(p);
}

static void dif_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_dif_t *p = PMB887X_DIF(obj);
	pmb887x_clc_init(&p->clc, dev);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-dif-v2", DIFv2_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	p->bus = ssi_create_bus(DEVICE(obj), TYPE_PMB887X_DIF);

	for (int i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);

	// DMAC
	qdev_init_gpio_in_named(dev, dif_handle_dmac_tx_clr, "DMAC_TX_CLR", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_tx_sreq, "DMAC_TX_SREQ", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_tx_breq, "DMAC_TX_BREQ", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_tx_lsreq, "DMAC_TX_LSREQ", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_tx_lbreq, "DMAC_TX_LBREQ", 1);

	qdev_init_gpio_in_named(dev, dif_handle_dmac_rx_clr, "DMAC_RX_CLR", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_rx_sreq, "DMAC_RX_SREQ", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_rx_breq, "DMAC_RX_BREQ", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_rx_lsreq, "DMAC_RX_LSREQ", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_rx_lbreq, "DMAC_RX_LBREQ", 1);

	qdev_init_gpio_in_named(dev, dif_handle_gpio_data_input, "D0_IN", 1);
	qdev_init_gpio_in_named(dev, dif_handle_gpio_data_input, "D1_IN", 1);
	qdev_init_gpio_in_named(dev, dif_handle_gpio_data_input, "D2_IN", 1);
	qdev_init_gpio_in_named(dev, dif_handle_gpio_data_input, "D3_IN", 1);
	qdev_init_gpio_in_named(dev, dif_handle_gpio_data_input, "D4_IN", 1);
	qdev_init_gpio_in_named(dev, dif_handle_gpio_data_input, "D5_IN", 1);
	qdev_init_gpio_in_named(dev, dif_handle_gpio_data_input, "D6_IN", 1);
	qdev_init_gpio_in_named(dev, dif_handle_gpio_data_input, "D7_IN", 1);

	qdev_init_gpio_out_named(dev, &p->gpio_data[0], "D0_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_data[1], "D1_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_data[2], "D2_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_data[3], "D3_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_data[4], "D4_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_data[5], "D5_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_data[6], "D6_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_data[7], "D7_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_cd, "CD_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_wr, "WR_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_rd, "RD_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_cs[0], "CS1_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_cs[1], "CS2_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_cs[2], "CS3_OUT", 1);
}

static void dif_realize(DeviceState *dev, Error **errp) {
	pmb887x_dif_t *p = PMB887X_DIF(dev);

	pmb887x_clc_set(&p->clc, 1U << MOD_CLC_RMC_SHIFT);
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
	pmb887x_srb_set_irq_router(&p->srb, p, dif_irq_router);
	pmb887x_srb_set_event_handler(&p->srb, p, dif_event_handler);
	pmb887x_srb_ext_init(&p->srb_err, &p->srb, DIFv2_ISR_ERR);

	dif_update_gpio_state(p);

	pmb887x_fifo32_init(&p->tx_fifo, FIFO_SIZE);
	pmb887x_fifo32_init(&p->tx_csreg_fifo, FIFO_SIZE);
	pmb887x_fifo32_init(&p->rx_fifo, FIFO_SIZE);
	for (uint32_t i = 0; i < ARRAY_SIZE(p->bit_mux); i++) {
		uint32_t reg_index = i / 6;
		uint32_t shift = (i % 6) * 5;
		if (shift >= 15)
			shift++;
		p->bmreg[reg_index] |= (i << shift);
	}
	dif_update_mux(p);

	p->rx_refill_bh = qemu_bh_new(dif_rx_refill, p);
	p->tx_bus_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dif_tx_bus_wake, p);
}

static void dif_unrealize(DeviceState *dev) {
	pmb887x_dif_t *p = PMB887X_DIF(dev);

	qemu_bh_delete(p->rx_refill_bh);
	timer_free(p->tx_bus_timer);
}

static void dif_reset(DeviceState *dev) {
	pmb887x_dif_t *p = PMB887X_DIF(dev);

	qemu_bh_cancel(p->rx_refill_bh);

	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	pmb887x_srb_reset(&p->srb);
	pmb887x_srb_ext_reset(&p->srb_err);
	pmb887x_srb_ext_set_imsc(&p->srb_err, DIFv2_ERRIRQSM_RXFUFL | DIFv2_ERRIRQSM_RXFOFL |
		DIFv2_ERRIRQSM_TXFOFL | DIFv2_ERRIRQSM_PHASE);
	pmb887x_fifo_reset(&p->tx_fifo);
	pmb887x_fifo_reset(&p->tx_csreg_fifo);
	pmb887x_fifo_reset(&p->rx_fifo);
	dif_reset_tx_bus(p);

	p->in_schedule = false;
	p->transfer_pending = false;
	p->tx_words_preloaded = 0;
	p->tx_csreg = 0;
	p->is_tx_csreg_active = false;
	p->rx_words_in_fifo = 0;
	p->pbc_word = 0;
	p->is_pbc_word_valid = false;
	p->is_pbc_pair_completed = false;

	p->con = 0;
	p->perreg = 0;
	p->csreg = 0;
	memset(p->lcdtim, 0, sizeof(p->lcdtim));
	p->runctrl = 0;
	p->startlcdrd = 0;
	memset(p->coeff, 0, sizeof(p->coeff));
	p->pbccon = 0;
	memset(p->bmreg, 0, sizeof(p->bmreg));
	memset(p->bcsel, 0, sizeof(p->bcsel));
	p->bcreg = 0;
	p->invert_bit = 0;
	p->sync_config = 0;
	p->sync_count = 0;
	p->br = 0;
	p->fdiv = 0;
	p->debug = 0;
	p->rxfifo_cfg = DIFv2_RXFIFO_CFG_RXBS_4_WORD;
	p->mrps_ctrl = 0;
	p->txfifo_cfg = DIFv2_TXFIFO_CFG_TXBS_8_WORD;
	p->tps_ctrl = 0;
	p->rx_packet_words = 0;
	p->rx_buffer = 0;
	p->rx_buffer_word_count = 0;
	p->dmac_tx_clr = 0;
	p->dmac_rx_clr = 0;

	for (uint32_t i = 0; i < ARRAY_SIZE(p->bit_mux); i++) {
		uint32_t reg_index = i / 6;
		uint32_t shift = (i % 6) * 5;
		if (shift >= 15)
			shift++;
		p->bmreg[reg_index] |= (i << shift);
	}
	dif_update_mux(p);

	p->state = DIF_STATE_NONE;
	p->tx_words_remaining = 0;
	p->rx_words_remaining = 0;
	p->tx_fifo_req = false;
	p->rx_fifo_req = false;
	p->is_tx_started = false;

	p->are_pins_driven = false;
	dif_update_gpio_state(p);
	p->are_dma_requests_driven = false;
	dif_trigger_dma(p);
}

static const Property dif_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_dif_t, revision, 0),
	DEFINE_PROP_LINK("bus", pmb887x_dif_t, bus, "SSI", SSIBus *),
};

static void dif_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, dif_properties);
	device_class_set_legacy_reset(dc, dif_reset);
	dc->realize = dif_realize;
	dc->unrealize = dif_unrealize;

	pmb887x_dma_sink_class_t *dma_sink = PMB887X_DMA_SINK_CLASS(klass);
	dma_sink->write_block = dif_dma_write_block;
}

static const TypeInfo dif_info = {
    .name          	= TYPE_PMB887X_DIF,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_dif_t),
    .instance_init 	= dif_init,
    .class_init    	= dif_class_init,
    .interfaces		= (const InterfaceInfo[]) {
        { TYPE_PMB887X_DMA_SINK },
        { }
    },
};

static void dif_register_types(void) {
	type_register_static(&dif_info);
}
type_init(dif_register_types)
