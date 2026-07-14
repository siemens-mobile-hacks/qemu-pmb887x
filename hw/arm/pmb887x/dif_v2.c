/*
 * Display Interface
 * */
#include "hw/arm/pmb887x/fifo.h"
#define PMB887X_TRACE_ID		DIF
#define PMB887X_TRACE_PREFIX	"pmb887x-dif"

#define PMB887X_DIF_DUMP_BIT_MUX	0 // print bit mux config

#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/hw.h"
#include "hw/ssi/ssi.h"
#include "hw/sysbus.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/qdev-properties.h"
#include "qemu/timer.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_DIF	"pmb887x-dif-v2"
#define PMB887X_DIF(obj)	OBJECT_CHECK(pmb887x_dif_t, (obj), TYPE_PMB887X_DIF)

#define FIFO_IO_SIZE	0x3FFF
#define FIFO_SIZE		16
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
	SSIBus *bus;

	qemu_irq irq[4];

	qemu_irq gpio_data[8];
	qemu_irq gpio_cs[3];
	qemu_irq gpio_cd;
	qemu_irq gpio_wr;
	qemu_irq gpio_rd;

	QEMUTimer *timer;
	bool transfer_pending;

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
	pmb887x_fifo32_t rx_fifo;

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
};

static inline uint32_t dif_get_rx_align(pmb887x_dif_t *p) {
	return 1 << ((p->rxfifo_cfg & DIFv2_RXFIFO_CFG_RXFA) >> DIFv2_RXFIFO_CFG_RXFA_SHIFT);
}

static inline uint32_t dif_get_tx_align(pmb887x_dif_t *p) {
	return 1 << ((p->txfifo_cfg & DIFv2_TXFIFO_CFG_TXFA) >> DIFv2_TXFIFO_CFG_TXFA_SHIFT);
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
	switch ((p->csreg & DIFv2_CSREG_BSCONF)) {
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
	hw_error("Invalid bsconf value: %08X", (p->csreg & DIFv2_CSREG_BSCONF) >> DIFv2_CSREG_BSCONF_SHIFT);
	return 0;
}

static inline bool dif_is_bsconf_9bit(pmb887x_dif_t *p) {
	uint32_t bsconf = (p->csreg & DIFv2_CSREG_BSCONF);
	return bsconf == DIFv2_CSREG_BSCONF_1x9BIT || bsconf == DIFv2_CSREG_BSCONF_2x9BIT || bsconf == DIFv2_CSREG_BSCONF_3x9BIT;
}

static inline uint32_t dif_get_word_bits(pmb887x_dif_t *p) {
	return ((p->con & DIFv2_CON_BM) >> DIFv2_CON_BM_SHIFT) + 1;
}

static inline uint32_t dif_get_tx_word_bits(pmb887x_dif_t *p) {
	return MIN(dif_get_word_bits(p), dif_get_tx_align(p) * 8);
}

static inline bool dif_is_pbc_enabled(pmb887x_dif_t *p) {
	return (p->pbccon & DIFv2_PBCCON_PBBCONV_MODE) != 0;
}

static inline bool dif_is_serial(pmb887x_dif_t *p) {
	return (p->perreg & DIFv2_PERREG_DIFPERMODE) == DIFv2_PERREG_DIFPERMODE_SERIAL;
}

static void dif_update_gpio_state(pmb887x_dif_t *p) {
	struct {
		bool value;
		uint32_t perreg;
		qemu_irq pin;
		char name[32];
	} cs_pins[] = {
		{ (p->csreg & DIFv2_CSREG_CS1) != 0, DIFv2_PERREG_CS1POL, p->gpio_cs[0], "CS1" },
		{ (p->csreg & DIFv2_CSREG_CS2) != 0, DIFv2_PERREG_CS2POL, p->gpio_cs[1], "CS2" },
		{ (p->csreg & DIFv2_CSREG_CS3) != 0, DIFv2_PERREG_CS3POL, p->gpio_cs[2], "CS3" },
		{ (p->csreg & DIFv2_CSREG_CD) != 0, DIFv2_PERREG_CDPOL, p->gpio_cd, "CD" },
		{ p->state == DIF_STATE_RX, DIFv2_PERREG_RDPOL, p->gpio_rd, "RD" },
		{ p->state != DIF_STATE_RX, DIFv2_PERREG_WRPOL, p->gpio_wr, "WR" },
	};
	for (int i = 0; i < ARRAY_SIZE(cs_pins); i++) {
		bool polarity = (p->perreg & cs_pins[i].perreg) != 0;
		bool value = cs_pins[i].value;
		if (polarity) {
			// DPRINTF("%s=%d set %s\n", cs_pins[i].name, value, value ? "HIGH" : "LOW");
			qemu_set_irq(cs_pins[i].pin, value ? 1 : 0);
		} else {
			// DPRINTF("%s=%d set %s\n", cs_pins[i].name, value, value ? "LOW" : "HIGH");
			qemu_set_irq(cs_pins[i].pin, value ? 0 : 1);
		}
	}
}

static void dif_schedule(pmb887x_dif_t *p) {
	if (!p->transfer_pending) {
		p->transfer_pending = true;
		timer_mod(p->timer, 0);
	}
}

static void dif_trigger_dma(pmb887x_dif_t *p) {
	uint32_t ris = pmb887x_srb_get_ris_dma(&p->srb);
	if (p->dmac_tx_clr) {
		qemu_set_irq(p->dmac_tx_sreq, 0);
		qemu_set_irq(p->dmac_tx_breq, 0);
		qemu_set_irq(p->dmac_tx_lsreq, 0);
		qemu_set_irq(p->dmac_tx_lbreq, 0);
	} else {
		qemu_set_irq(p->dmac_tx_sreq, (ris & DIFv2_RIS_TXSREQ) != 0);
		qemu_set_irq(p->dmac_tx_breq, (ris & DIFv2_RIS_TXBREQ) != 0);
		qemu_set_irq(p->dmac_tx_lsreq, (ris & DIFv2_RIS_TXLSREQ) != 0);
		qemu_set_irq(p->dmac_tx_lbreq, (ris & DIFv2_RIS_TXLBREQ) != 0);
	}

	if (p->dmac_rx_clr) {
		qemu_set_irq(p->dmac_rx_sreq, 0);
		qemu_set_irq(p->dmac_rx_breq, 0);
		qemu_set_irq(p->dmac_rx_lsreq, 0);
		qemu_set_irq(p->dmac_rx_lbreq, 0);
	} else {
		qemu_set_irq(p->dmac_rx_sreq, (ris & DIFv2_RIS_RXSREQ) != 0);
		qemu_set_irq(p->dmac_rx_breq, (ris & DIFv2_RIS_RXBREQ) != 0);
		qemu_set_irq(p->dmac_rx_lsreq, (ris & DIFv2_RIS_RXLSREQ) != 0);
		qemu_set_irq(p->dmac_rx_lbreq, (ris & DIFv2_RIS_RXLBREQ) != 0);
	}
}

static void dif_tx_fifo_req(pmb887x_dif_t *p) {
	if (p->state != DIF_STATE_TX || p->tx_fifo_req)
		return;

	uint32_t burst_req_size = dif_get_tx_burst_size(p);
	uint32_t single_req_size = 4 / dif_get_tx_align(p);
	uint32_t burst_req_count = burst_req_size / single_req_size;
	uint32_t tx_words_remaining = p->tx_words_remaining - MIN(p->tx_words_remaining,
		pmb887x_fifo_count(&p->tx_fifo) * single_req_size);
	uint32_t pending_req_count = DIV_ROUND_UP(tx_words_remaining, single_req_size);

	if (tx_words_remaining == 0)
		return;
	if (tx_words_remaining >= burst_req_size && pmb887x_fifo_free_count(&p->tx_fifo) < burst_req_count)
		return;
	if (tx_words_remaining < burst_req_size && pmb887x_fifo_free_count(&p->tx_fifo) < 1)
		return;

	if ((p->txfifo_cfg & DIFv2_TXFIFO_CFG_TXFC) != 0) {
		if (tx_words_remaining > burst_req_size) {
			pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_TXBREQ);
		} else if (pending_req_count == burst_req_count) {
			pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_TXLBREQ);
		} else if (tx_words_remaining > single_req_size) {
			pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_TXSREQ);
		} else {
			pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_TXLSREQ);
		}
		p->tx_fifo_req = true;
	} else if (pmb887x_fifo_free_count(&p->tx_fifo) >= burst_req_count) {
		pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_TXBREQ);
		p->tx_fifo_req = true;
	}
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
	bool is_packet_complete = p->state == DIF_STATE_RX && p->rx_words_remaining == 0;

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

static void dif_kernel_reset(pmb887x_dif_t *p, uint32_t new_state) {
	p->state = new_state;
	p->tx_words_remaining = 0;
	p->rx_words_remaining = 0;
	p->tx_fifo_req = false;
	p->rx_fifo_req = false;
	p->is_tx_started = false;
	dif_update_gpio_state(p);
	dif_schedule(p);
}

static void dif_reset_rx_fifo(pmb887x_dif_t *p) {
	pmb887x_fifo_reset(&p->rx_fifo);
	p->rx_words_in_fifo = 0;
	p->rx_buffer = 0;
	p->rx_buffer_word_count = 0;
}

static inline uint32_t dif_mux(pmb887x_dif_t *p, uint32_t value) {
	uint32_t new_value = 0;
	for (uint32_t output_bit = 0; output_bit < 32; output_bit++) {
		uint32_t bit = 0;

		if (p->bit_bcsel[output_bit] == 0) {
			bit = ((value >> p->bit_mux[output_bit]) & 1);
		} else if (p->bit_bcsel[output_bit] == 1) {
			bit = p->bit_bcreg[output_bit];
		}
		if (p->bit_invert[output_bit])
			bit = bit ? 0 : 1;
		new_value |= (bit << output_bit);
	}
	return new_value;
}

static void dif_rx_from_bus(pmb887x_dif_t *p);

static void dif_fifo_write(pmb887x_dif_t *p, uint32_t value) {
	if (!pmb887x_fifo_free_count(&p->tx_fifo)) {
		DPRINTF("TX FIFO overflow!\n");
		pmb887x_srb_ext_set_isr(&p->srb_err, DIFv2_ERRIRQSS_TXFOFL);
		dif_kernel_reset(p, DIF_STATE_NONE);
		return;
	}

	pmb887x_fifo32_push(&p->tx_fifo, value);
	dif_schedule(p);
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
		dif_schedule(p);
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
	dif_kernel_reset(p, DIF_STATE_TX);
	p->tx_words_remaining = tx_word_count;
	p->is_tx_started = preloaded_words != 0;
	p->rx_packet_words = 0;
	p->rx_buffer = 0;
	p->rx_buffer_word_count = 0;
	p->is_pbc_pair_completed = false;
	dif_fifo_req(p);
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
	dif_kernel_reset(p, DIF_STATE_RX);
	p->rx_words_remaining = rx_word_count;
	p->rx_packet_words = rx_word_count;
	p->rx_buffer = 0;
	p->rx_buffer_word_count = 0;
	dif_rx_from_bus(p);
	dif_fifo_req(p);
}

static bool dif_push_rx_word(pmb887x_dif_t *p, uint16_t value) {
	uint32_t align = dif_get_rx_align(p);
	bool is_packed_9bit = dif_is_bsconf_9bit(p);
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
			if (!dif_push_rx_word(p, 0))
				return;
		}
		if (word_count != words_per_stage)
			dif_flush_rx_buffer(p);
		words_to_receive -= word_count;
	}
}

static uint16_t dif_bus_transfer(pmb887x_dif_t *p, uint16_t value) {
	if (dif_is_serial(p) && (p->con & DIFv2_CON_LB) != 0)
		return value;

	return ssi_transfer(p->bus, value);
}

static bool dif_send_word(pmb887x_dif_t *p, uint16_t value) {
	DPRINTF("TX: %03X\n", value);
	uint16_t received = dif_bus_transfer(p, value);
	if (dif_is_serial(p))
		return dif_push_rx_word(p, received);

	return true;
}

static bool dif_convert_word(pmb887x_dif_t *p, uint16_t value, uint16_t *output) {
	if (!dif_is_pbc_enabled(p)) {
		*output = dif_mux(p, value);
		return true;
	}

	if (!p->is_pbc_word_valid) {
		p->pbc_word = value;
		p->is_pbc_word_valid = true;
		return false;
	}

	uint32_t input = (p->pbc_word | ((uint32_t) value << 16));
	uint32_t word_bits = dif_get_tx_word_bits(p);

	*output = (dif_mux(p, input) & ((1 << word_bits) - 1));
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
	uint32_t bsconf_word_count = dif_get_bsconf_word_count(p);
	uint32_t bsconf_word_bits = dif_is_bsconf_9bit(p) ? 9 : 8;
	uint32_t word_bits = dif_get_tx_word_bits(p);

	while (pmb887x_fifo_count(&p->tx_fifo) > 0) {
		uint32_t value = pmb887x_fifo32_pop(&p->tx_fifo);
		uint32_t words_in_fifo_reg = MIN(4 / align, p->tx_words_remaining);

		if (p->state == DIF_STATE_TX)
			p->is_tx_started = true;
		p->tx_words_remaining -= words_in_fifo_reg;
		if (bsconf_word_count == 0 && words_in_fifo_reg == 0)
			words_in_fifo_reg = 4 / align; // Direct TXD writes do not require TPS_CTRL.
		if (p->state == DIF_STATE_NONE)
			p->tx_words_preloaded += bsconf_word_count != 0 ? 1 : words_in_fifo_reg;

		if (bsconf_word_count != 0) {
			if (dif_is_pbc_enabled(p)) {
				uint16_t converted;

				if (!dif_convert_word(p, value, &converted))
					continue;
				value = converted;
			}
			// BSCONF applies bit conversion to the PBCCON output.
			value = dif_mux(p, value);
			for (uint32_t i = 0; i < bsconf_word_count; i++) {
				uint16_t word = (value >> (bsconf_word_bits * i)) & ((1 << bsconf_word_bits) - 1);

				if (!dif_send_word(p, word))
					return;
			}
		} else {
			for (uint32_t i = 0; i < words_in_fifo_reg; i++) {
				uint16_t converted;
				uint16_t word = (value >> (8 * i * align)) & ((1 << word_bits) - 1);

				if (dif_convert_word(p, word, &converted) && !dif_send_word(p, converted))
					return;
			}
		}
	}

	if (p->tx_words_remaining == 0 && dif_is_serial(p))
		dif_flush_rx_buffer(p);
}

static void dif_work(pmb887x_dif_t *p) {
	if (p->state == DIF_STATE_NONE) {
		if (p->tps_ctrl > 0) {
			dif_start_tx(p);
		} else if (p->mrps_ctrl > 0) {
			dif_start_rx(p);
		} else if (dif_is_running(p) && !dif_is_serial(p) && !pmb887x_fifo_is_empty(&p->tx_fifo)) {
			dif_tx_from_fifo(p);
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

static void dif_timer_reset(void *opaque) {
	pmb887x_dif_t *p = opaque;
	while (p->transfer_pending) {
		p->transfer_pending = false;
		dif_work(p);
		if (pmb887x_srb_get_ris(&p->srb) != 0)
			break;
	}
}

static void dif_update_mux(pmb887x_dif_t *p) {
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
			value = 0xF043C012;
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
			if (dif_is_running(p)) {
				bool is_busy = (
					(p->state == DIF_STATE_TX && p->is_tx_started) ||
					(p->state == DIF_STATE_RX && (p->startlcdrd & DIFv2_STARTLCDRD_STARTREAD) != 0) ||
					pmb887x_fifo_count(&p->tx_fifo) > 0
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
			value = pmb887x_fifo_count(&p->tx_fifo);
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
		case DIFv2_CLC:
			pmb887x_clc_set(&p->clc, value);
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
				dif_reset_rx_fifo(p);
			}
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
			pmb887x_srb_set_isr(&p->srb, value);
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
			exit(1);
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

static void dif_handle_dmac_tx_clr(void *opaque, int id, int level) {
	pmb887x_dif_t *p = opaque;
	p->dmac_tx_clr = level;
	if (level == 1)
		pmb887x_srb_set_icr(&p->srb, DIFv2_ICR_TXSREQ | DIFv2_ICR_TXBREQ | DIFv2_ICR_TXLSREQ | DIFv2_ICR_TXLBREQ);
	dif_trigger_dma(p);
}

static void dif_handle_dmac_rx_clr(void *opaque, int id, int level) {
	pmb887x_dif_t *p = opaque;
	p->dmac_rx_clr = level;
	if (level == 1)
		pmb887x_srb_set_icr(&p->srb, DIFv2_ICR_RXSREQ | DIFv2_ICR_RXBREQ | DIFv2_ICR_RXLSREQ | DIFv2_ICR_RXLBREQ);
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
	if (level == 0 && (mask & FIFO_ICR_MASK) != 0) {
		if (p->state == DIF_STATE_RX || p->state == DIF_STATE_TX) {
			dif_fifo_clr_req(p, mask);
			dif_fifo_req(p);
		}
	}
	if ((mask & pmb887x_srb_get_dmae(&p->srb)) != 0)
		dif_trigger_dma(p);
}

static void dif_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_dif_t *p = PMB887X_DIF(obj);
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

	pmb887x_clc_init(&p->clc);
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
	pmb887x_srb_set_irq_router(&p->srb, p, dif_irq_router);
	pmb887x_srb_set_event_handler(&p->srb, p, dif_event_handler);
	pmb887x_srb_ext_init(&p->srb_err, &p->srb, DIFv2_ISR_ERR);

	dif_update_gpio_state(p);

	pmb887x_fifo32_init(&p->tx_fifo, FIFO_SIZE);
	pmb887x_fifo32_init(&p->rx_fifo, FIFO_SIZE);
	for (uint32_t i = 0; i < ARRAY_SIZE(p->bit_mux); i++) {
		uint32_t reg_index = i / 6;
		uint32_t shift = (i % 6) * 5;
		if (shift >= 15)
			shift++;
		p->bmreg[reg_index] |= (i << shift);
	}
	dif_update_mux(p);

	p->timer = timer_new_ns(QEMU_CLOCK_REALTIME, dif_timer_reset, p);
}

static void dif_reset(DeviceState *dev) {
	pmb887x_dif_t *p = PMB887X_DIF(dev);

	timer_del(p->timer);

	pmb887x_clc_init(&p->clc);
	pmb887x_srb_reset(&p->srb);
	pmb887x_srb_ext_reset(&p->srb_err);
	pmb887x_fifo_reset(&p->tx_fifo);
	pmb887x_fifo_reset(&p->rx_fifo);

	p->transfer_pending = false;
	p->tx_words_preloaded = 0;
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
	p->rxfifo_cfg = 0;
	p->mrps_ctrl = 0;
	p->txfifo_cfg = 0;
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

	dif_update_gpio_state(p);
	dif_trigger_dma(p);
}

static const Property dif_properties[] = {
	DEFINE_PROP_LINK("bus", pmb887x_dif_t, bus, "SSI", SSIBus *),
};

static void dif_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, dif_properties);
	device_class_set_legacy_reset(dc, dif_reset);
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
