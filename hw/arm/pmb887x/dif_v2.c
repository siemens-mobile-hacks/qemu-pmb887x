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
#include "hw/arm/pmb887x/dmac.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/ssc/lcd_common.h"

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

enum DIFIrqType {
	DIF_RX_SINGLE_IRQ = 0,
	DIF_RX_BURST_IRQ,
	DIF_TX_IRQ,
	DIF_ERROR_IRQ,
};

enum DIFFifoType {
	DIF_FIFO_RX,
	DIF_FIFO_TX
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

	bool fifo_req;
	uint32_t tx_remaining;
	uint32_t rx_remaining;

	uint32_t rx_total_bytes;
	uint32_t rx_bytes_in_fifo;

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

	uint32_t mask;
	uint32_t bits;

	pmb887x_fifo32_t tx_fifo_buffered;
	pmb887x_fifo32_t rx_fifo_buffered;

	pmb887x_fifo32_t tx_fifo_single;
	pmb887x_fifo32_t rx_fifo_single;

	pmb887x_fifo32_t *rx_fifo;
	pmb887x_fifo32_t *tx_fifo;

	uint32_t rx_cnt;
	uint32_t rx_buffer_cnt;

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

static inline uint32_t dif_get_bsconf_size(pmb887x_dif_t *p) {
	switch (p->csreg & DIFv2_CSREG_BSCONF) {
		case DIFv2_CSREG_BSCONF_OFF:
			return 0;
		case DIFv2_CSREG_BSCONF_1x8BIT:
			return 1;
		case DIFv2_CSREG_BSCONF_2x8BIT:
			return 2;
		case DIFv2_CSREG_BSCONF_3x8BIT:
			return 3;
		case DIFv2_CSREG_BSCONF_4x8BIT:
			return 4;
	}
	hw_error("Invalid bsconf value: %08X", (p->csreg & DIFv2_CSREG_BSCONF) >> DIFv2_CSREG_BSCONF_SHIFT);
	return 0;
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

static void dif_fifo_req(pmb887x_dif_t *p) {
	if (p->fifo_req)
		return;

	if (p->state == DIF_STATE_TX) {
		uint32_t burst_req_size = dif_get_tx_burst_size(p);
		uint32_t single_req_size = (4 / dif_get_tx_align(p));
		uint32_t burst_req_count = burst_req_size / single_req_size;

		uint32_t tx_remaining = p->tx_remaining - MIN(p->tx_remaining, pmb887x_fifo_count(p->tx_fifo));
		if (!tx_remaining)
			return;
		if (tx_remaining >= burst_req_size && pmb887x_fifo_free_count(p->tx_fifo) < burst_req_count)
			return;
		if (tx_remaining < burst_req_size && pmb887x_fifo_free_count(p->tx_fifo) < 1)
			return;

		bool is_fc = (p->txfifo_cfg & DIFv2_TXFIFO_CFG_TXFC) != 0;
		if (is_fc) {
			if (tx_remaining > burst_req_size) {
				pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_TXBREQ);
			} else if (tx_remaining == burst_req_size) {
				pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_TXLBREQ);
			} else if (tx_remaining > single_req_size) {
				pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_TXSREQ);
			} else {
				pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_TXLSREQ);
			}
			p->fifo_req = true;
		} else {
			if (pmb887x_fifo_free_count(p->tx_fifo) >= burst_req_size) {
				pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_BREQ_INT);
				p->fifo_req = true;
			}
		}
	} else if (p->state == DIF_STATE_RX) {
		uint32_t burst_req_size = dif_get_rx_burst_size(p);
		uint32_t single_req_size = (4 / dif_get_rx_align(p));

		if (!p->rx_bytes_in_fifo)
			return;

		bool is_fc = (p->rxfifo_cfg & DIFv2_RXFIFO_CFG_RXFC) != 0;
		if (is_fc) {
			if (p->rx_bytes_in_fifo > burst_req_size) {
				pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_RXBREQ);
			} else if (p->rx_bytes_in_fifo == burst_req_size) {
				pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_RXLBREQ);
			} else if (p->rx_bytes_in_fifo > single_req_size) {
				pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_RXSREQ);
			} else {
				pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_RXLSREQ);
			}
		} else {
			if (p->rx_bytes_in_fifo >= burst_req_size) {
				pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_RXBREQ);
			} else {
				pmb887x_srb_set_isr(&p->srb, DIFv2_ISR_RXSREQ);
			}
		}

		p->fifo_req = true;
	}
}

static void dif_fifo_clr_req(pmb887x_dif_t *p) {
	p->fifo_req = false;
}

static void dif_kernel_reset(pmb887x_dif_t *p, uint32_t new_state) {
	p->state = new_state;
	p->tx_remaining = 0;
	p->rx_remaining = 0;
	p->rx_total_bytes = 0;
	p->rx_bytes_in_fifo = 0;
	dif_fifo_clr_req(p);
	dif_update_gpio_state(p);
	dif_schedule(p);
}

static inline uint32_t dif_mux(pmb887x_dif_t *p, uint32_t value) {
	uint32_t new_value = 0;
	for (uint32_t i = 0; i < 32; i++) {
		uint32_t bit = (value >> i) & 1;
		if (p->bit_invert[i])
			bit = bit ? 0 : 1;
		new_value |= bit << p->bit_mux[i];
	}
	return new_value;
}

static void dif_fifo_write(pmb887x_dif_t *p, uint32_t value) {
	if (!pmb887x_fifo_free_count(p->tx_fifo)) {
		DPRINTF("TX FIFO overflow!\n");
		pmb887x_srb_ext_set_isr(&p->srb_err, DIFv2_ERRIRQSS_TXFOFL);
		dif_kernel_reset(p, DIF_STATE_NONE);
		return;
	}

	uint32_t bsconf = (p->csreg & DIFv2_CSREG_BSCONF);
	if (bsconf != DIFv2_CSREG_BSCONF_OFF)
		value = dif_mux(p, value);

	pmb887x_fifo32_push(p->tx_fifo, value);
	dif_schedule(p);
}

static void dif_fifo_read(pmb887x_dif_t *p, uint64_t *value) {
	if (pmb887x_fifo_is_empty(p->rx_fifo)) {
		DPRINTF("RX FIFO underflow!\n");
		pmb887x_srb_ext_set_isr(&p->srb_err, DIFv2_ERRIRQSS_RXFUFL);
		dif_kernel_reset(p, DIF_STATE_NONE);
		return;
	}

	*value = pmb887x_fifo32_pop(p->rx_fifo);
	p->rx_bytes_in_fifo -= MIN(p->rx_bytes_in_fifo, 4 / dif_get_rx_align(p));

	if (!p->rx_bytes_in_fifo)
		dif_schedule(p);
}

static void dif_reset_fifo(pmb887x_dif_t *p, enum DIFFifoType fifo) {
	if (fifo == DIF_FIFO_RX) {
		pmb887x_fifo_reset(p->rx_fifo);
	} else {
		pmb887x_fifo_reset(p->tx_fifo);
	}
}

static void dif_set_fifo(pmb887x_dif_t *p, enum DIFFifoType fifo, bool buffered) {
	if (fifo == DIF_FIFO_RX) {
		DPRINTF("RX FIFO: %s\n", buffered ? "ON" : "OFF");
		p->rx_fifo = buffered ? &p->rx_fifo_buffered : &p->rx_fifo_single;
	} else {
		DPRINTF("TX FIFO: %s\n", buffered ? "ON" : "OFF");
		p->tx_fifo = buffered ? &p->tx_fifo_buffered : &p->tx_fifo_single;
	}
	dif_reset_fifo(p, fifo);
}

static bool dif_is_running(pmb887x_dif_t *p) {
	return pmb887x_clc_is_enabled(&p->clc) && pmb887x_clc_get_rmc(&p->clc) && p->runctrl;
}

static void dif_start_tx(pmb887x_dif_t *p) {
	if (!dif_is_running(p) || p->state != DIF_STATE_NONE)
		return;

	uint32_t write_count = p->tps_ctrl;
	p->tps_ctrl = 0;

	if (!write_count)
		return;

	DPRINTF("new transfer: tx=%d\n", write_count);
	dif_kernel_reset(p, DIF_STATE_TX);
	p->tx_remaining = write_count;
	dif_fifo_req(p);
}

static void dif_start_rx(pmb887x_dif_t *p) {
	if (!dif_is_running(p) || p->state != DIF_STATE_NONE)
		return;

	uint32_t read_count = 0;
	if ((p->startlcdrd & DIFv2_STARTLCDRD_STARTREAD)) {
		read_count = (p->startlcdrd & DIFv2_STARTLCDRD_READBYTES) >> DIFv2_STARTLCDRD_READBYTES_SHIFT;
	} else if (p->mrps_ctrl > 0) {
		read_count = p->mrps_ctrl;
		p->mrps_ctrl = 0;
	}

	if (!read_count)
		return;

	DPRINTF("new transfer: rx=%d\n", read_count);
	dif_kernel_reset(p, DIF_STATE_RX);
	p->rx_remaining = read_count;
	dif_fifo_req(p);
}

static void dif_tx_from_fifo(pmb887x_dif_t *p) {
	uint32_t align = dif_get_tx_align(p);
	uint32_t bsconf_size = dif_get_bsconf_size(p);

	while (pmb887x_fifo_count(p->tx_fifo) > 0) {
		uint32_t value = pmb887x_fifo32_pop(p->tx_fifo);
		uint32_t bytes_in_fifo_reg = MIN(4 / align, p->tx_remaining);
		p->tx_remaining -= bytes_in_fifo_reg;

		if (bsconf_size != 0) {
			for (uint32_t i = 0; i < bsconf_size; i++) {
				uint8_t byte = (value >> (8 * i)) & 0xFF;
				ssi_transfer(p->bus, byte);
			}
		} else {
			if (bytes_in_fifo_reg == 0)
				bytes_in_fifo_reg = 4 / align; // shit from real HW

			for (uint32_t i = 0; i < bytes_in_fifo_reg; i += align) {
				uint8_t byte = (value >> (8 * i)) & 0xFF;
				DPRINTF("TX: %02X\n", byte);
				ssi_transfer(p->bus, byte);
			}
		}
	}
}

static void dif_work(pmb887x_dif_t *p) {
	if (p->state == DIF_STATE_NONE) {
		dif_tx_from_fifo(p);

		if (p->tps_ctrl > 0) {
			dif_start_tx(p);
		} else if ((p->startlcdrd & DIFv2_STARTLCDRD_STARTREAD)) {
			dif_start_rx(p);
		} else if (p->mrps_ctrl > 0) {
			dif_start_rx(p);
		}
	} else if (p->state == DIF_STATE_TX) {
		dif_tx_from_fifo(p);
		dif_fifo_req(p);

		if (p->tx_remaining == 0) {
			DPRINTF("transfer done\n");
			dif_kernel_reset(p, DIF_STATE_NONE);
		}
	} else if (p->state == DIF_STATE_RX) {
		hw_error("DIF: RX is not supported!");
		if (p->rx_remaining == 0) {
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

static void dif_update_state(pmb887x_dif_t *p) {
	// ???
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
				value |= p->state != DIF_STATE_NONE ? DIFv2_STAT_BSY : 0;
				value |= pmb887x_fifo_count(p->tx_fifo) > 0 ? DIFv2_STAT_BSY : 0;
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
			value = p->bmreg[(haddr - DIFv2_BCSEL0) / 4];
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
			value = p->rx_cnt;
			break;

		case DIFv2_RXFFS_STAT:
			value = pmb887x_fifo_count(p->rx_fifo);
			break;

		case DIFv2_TXFIFO_CFG:
			value = p->txfifo_cfg;
			break;

		case DIFv2_TPS_CTRL:
			value = p->tps_ctrl;
			break;

		case DIFv2_TXFFS_STAT:
			value = pmb887x_fifo_count(p->tx_fifo);
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
			if (!p->runctrl && (p->state == DIF_STATE_RX || p->state == DIF_STATE_TX)) {
				dif_kernel_reset(p, DIF_STATE_NONE);
				pmb887x_fifo_reset(p->tx_fifo);
				pmb887x_fifo_reset(p->rx_fifo);
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

		case DIFv2_STARTLCDRD:
			p->startlcdrd = value;
			dif_start_rx(p);
			break;

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
			p->bmreg[(haddr - DIFv2_BCSEL0) / 4] = value;
			dif_update_mux(p);
			break;

		case DIFv2_BCREG:
			p->bcreg = value;
			dif_update_mux(p);
			break;

		case DIFv2_INVERT_BIT:
			p->invert_bit = value;
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
			if ((value & DIFv2_RXFIFO_CFG_RXFC) != (p->rxfifo_cfg & DIFv2_RXFIFO_CFG_RXFC))
				dif_set_fifo(p, DIF_FIFO_RX, (value & DIFv2_RXFIFO_CFG_RXFC) != 0);
			p->rxfifo_cfg = value;
			break;

		case DIFv2_MRPS_CTRL:
			p->mrps_ctrl = value;
			dif_start_rx(p);
			break;

		case DIFv2_TXFIFO_CFG:
			if ((value & DIFv2_TXFIFO_CFG_TXFC) != (p->rxfifo_cfg & DIFv2_TXFIFO_CFG_TXFC))
				dif_set_fifo(p, DIF_FIFO_TX, (value & DIFv2_TXFIFO_CFG_TXFC) != 0);
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

		case DIFv2_RIS:
			value = pmb887x_srb_get_ris(&p->srb);
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
			dif_fifo_clr_req(p);
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

	pmb887x_fifo32_init(&p->tx_fifo_buffered, FIFO_SIZE);
	pmb887x_fifo32_init(&p->rx_fifo_buffered, FIFO_SIZE);

	pmb887x_fifo32_init(&p->tx_fifo_single, 1);
	pmb887x_fifo32_init(&p->rx_fifo_single, 1);

	dif_set_fifo(p, DIF_FIFO_RX, false);
	dif_set_fifo(p, DIF_FIFO_TX, false);

	p->timer = timer_new_ns(QEMU_CLOCK_REALTIME, dif_timer_reset, p);
}

static const Property dif_properties[] = {
	DEFINE_PROP_LINK("bus", pmb887x_dif_t, bus, "SSI", SSIBus *),
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
