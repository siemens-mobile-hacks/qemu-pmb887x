/*
 * Display Interface
 * */
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

typedef struct pmb887x_dif_t pmb887x_dif_t;

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
	DIF_STATE_TX_DONE,
	DIF_STATE_RX,
	DIF_STATE_RX_DONE,
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
	uint32_t dma_control;

	uint32_t dmac_tx_periph_id;
	pmb887x_dmac_t *dmac;
	pmb887x_lcd_t *lcd;
	
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

static void dif_reset(pmb887x_dif_t *p) {

}

static void dif_work(pmb887x_dif_t *p) {
	if (p->state == DIF_STATE_TX) {
		DPRINTF("DIF_STATE_TX\n");
	}
}

static void dif_transfer(pmb887x_dif_t *p) {
	uint32_t max_tx_size = p->tps_ctrl >= dif_get_tx_burst_size(p) ? dif_get_tx_burst_size(p) : (4 / dif_get_tx_align(p));
	uint32_t tx_fifo_bytes = (4 / dif_get_tx_align(p)) * pmb887x_fifo_count(p->tx_fifo);
	uint32_t tx_size = MIN(max_tx_size, p->tps_ctrl);

	DPRINTF("tx_fifo_bytes=%d\n", tx_fifo_bytes);
	DPRINTF("tx_size=%d\n", tx_size);
	DPRINTF("max_tx_size=%d\n", max_tx_size);

	/*
	if (p->tps_ctrl > 0 && tx_fifo_bytes >= tx_size)
		i2c_tx_fifo(p, tx_size);

	if (p->tps_ctrl > 0) {
		i2c_trigger_fifo_irq(p, p->tps_ctrl, tx_size, dif_get_tx_burst_size(p));
	} else {
		pmb887x_fifo_reset(&p->fifo);

		if (p->mrpsctrl > 0) {
			DPRINTF("switching to RX\n");
			pmb887x_srb_ext_set_isr(&p->srb_proto, I2Cv2_PIRQSS_RX);
			p->state = I2C_STATE_RX;
			i2c_timer_schedule(p);
		} else {
			p->state = I2C_STATE_DONE;
			i2c_timer_schedule(p);
		}
	}
	*/
}

static void dif_fifo_write(pmb887x_dif_t *p, uint32_t value) {
	if (!dif_is_running(p))
		return;

	if (!pmb887x_fifo_free_count(p->tx_fifo)) {
		DPRINTF("TX FIFO overflow!\n");
		pmb887x_srb_ext_set_isr(&p->srb_err, DIFv2_ERRIRQSS_TXFOFL);
		return;
	}
	pmb887x_fifo32_push(p->tx_fifo, value);

	dif_transfer(p);

	// WRITE[4] F71000A0: 00010203 (DIF_TXFIFO_CFG): TXBS(8_WORD) | TXFA(4) | TXFC (PC: A0609E44, LR: A0609CC0)

	uint32_t max_tx_size = p->tps_ctrl >= dif_get_tx_burst_size(p) ? dif_get_tx_burst_size(p) : (4 / dif_get_tx_align(p));
	DPRINTF("max_tx_size=%04X\n", max_tx_size);
}

static void dif_fifo_read(pmb887x_dif_t *p, uint64_t *value) {
	if (!pmb887x_fifo_count(p->rx_fifo)) {
		DPRINTF("RX fifo underflow!\n");
		pmb887x_srb_ext_set_isr(&p->srb_err, DIFv2_ERRIRQSS_RXFUFL);
		return;
	}

	*value = pmb887x_fifo32_pop(p->rx_fifo);
	p->rx_buffer_cnt -= MIN(p->rx_buffer_cnt, 4 / dif_get_rx_align(p));
}

static void dif_update_state(pmb887x_dif_t *p) {
	uint32_t bits = ((p->con & DIFv2_CON_BM) >> DIFv2_CON_BM_SHIFT) + 1;
	p->bits = bits;
	p->mask = (1 << bits) - 1;

	if (dif_is_running(p) && !pmb887x_fifo_is_empty(p->tx_fifo)) {
		dif_transfer(p);
	}

	if ((p->dma_control & DIFv2_DMAE_TX)) {
		// if (!pmb887x_dmac_is_busy(p->dmac))
		//	pmb887x_dmac_request(p->dmac, p->dmac_tx_periph_id, pmb887x_fifo_free_count(p->tx_fifo));
	//	hw_error("TX DMA not supported.");
	}

	if ((p->dma_control & DIFv2_DMAE_RX)) {
	//	hw_error("RX DMA not supported.");
	}
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
}

static int dif_get_bmreg_index_from_reg(uint32_t reg) {
	switch (reg) {
		case DIFv2_BMREG0:		return 0;
		case DIFv2_BMREG1:		return 1;
		case DIFv2_BMREG2:		return 2;
		case DIFv2_BMREG3:		return 3;
		case DIFv2_BMREG4:		return 4;
		case DIFv2_BMREG5:		return 5;
	};
	abort();
	return -1;
}

static int dif_get_coeff_index_from_reg(uint32_t reg) {
	switch (reg) {
		case DIFv2_COEFF_REG1:		return 0;
		case DIFv2_COEFF_REG2:		return 1;
		case DIFv2_COEFF_REG3:		return 2;
		case DIFv2_OFFSET:			return 3;
	};
	abort();
	return -1;
}

static int dif_get_lcdtim_index_from_reg(uint32_t reg) {
	switch (reg) {
		case DIFv2_LCDTIM1:		return 0;
		case DIFv2_LCDTIM2:		return 1;
	};
	abort();
	return -1;
}

static int dif_get_bcsel_index_from_reg(uint32_t reg) {
	switch (reg) {
		case DIFv2_BCSEL0:		return 0;
		case DIFv2_BCSEL1:		return 1;
	};
	abort();
	return -1;
}

static void dif_dump_bit_mux(pmb887x_dif_t *p) {
#if PMB887X_DIF_DUMP_BIT_MUX
	g_autoptr(GString) mux_str = g_string_new("");
	g_autoptr(GString) bcsel_str = g_string_new("");
	g_autoptr(GString) bc_str = g_string_new("");
	g_autoptr(GString) invert_str = g_string_new("");

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

		// DIF_INVERT_BIT
		g_string_append_printf(invert_str, " %3d", p->invert_bit & (1 << i) ? 1 : 0);
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

static void dif_update_gpio_state(pmb887x_dif_t *p) {
	uint32_t cs_pins[][2] = {
		{ DIFv2_CSREG_CS1, DIFv2_PERREG_CS1POL },
		{ DIFv2_CSREG_CS2, DIFv2_PERREG_CS2POL },
		{ DIFv2_CSREG_CS3, DIFv2_PERREG_CS3POL },
	};
	for (int i = 0; i < 3; i++) {
		bool polarity = (p->perreg & cs_pins[i][1]) != 0;
		bool value = (p->csreg & cs_pins[i][0]) != 0;
		if (polarity) {
			qemu_set_irq(p->gpio_cs[i], value ? 1 : 0);
		} else {
			qemu_set_irq(p->gpio_cs[i], value ? 0 : 1);
		}
	}
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
			value = p->lcdtim[dif_get_lcdtim_index_from_reg(haddr)];
			break;

		case DIFv2_STARTLCDRD:
			value = p->startlcdrd;
			break;

		case DIFv2_STAT:
			value = 0;
			break;

		case DIFv2_COEFF_REG1:
		case DIFv2_COEFF_REG2:
		case DIFv2_COEFF_REG3:
		case DIFv2_OFFSET:
			value = p->coeff[dif_get_coeff_index_from_reg(haddr)];
			break;

		case DIFv2_BMREG0:
		case DIFv2_BMREG1:
		case DIFv2_BMREG2:
		case DIFv2_BMREG3:
		case DIFv2_BMREG4:
		case DIFv2_BMREG5:
			value = p->bmreg[dif_get_bmreg_index_from_reg(haddr)];
			break;

		case DIFv2_PBCCON:
			value = p->pbccon;
			break;

		case DIFv2_BCSEL0:
		case DIFv2_BCSEL1:
			value = p->bmreg[dif_get_bcsel_index_from_reg(haddr)];
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
			value = p->dma_control;
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
	
	bool supress = (
		(haddr >= DIFv2_TXD && haddr < DIFv2_TXD + FIFO_IO_SIZE) ||
		(haddr >= DIFv2_RXD && haddr < DIFv2_RXD + FIFO_IO_SIZE)
	);
	
	if (!supress)
		IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case DIFv2_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case DIFv2_RUNCTRL:
			p->runctrl = value;
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
			p->lcdtim[dif_get_lcdtim_index_from_reg(haddr)] = value;
			break;

		case DIFv2_STARTLCDRD:
			p->startlcdrd = value;
			break;

		case DIFv2_COEFF_REG1:
		case DIFv2_COEFF_REG2:
		case DIFv2_COEFF_REG3:
		case DIFv2_OFFSET:
			p->coeff[dif_get_coeff_index_from_reg(haddr)] = value;
			break;

		case DIFv2_BMREG0:
		case DIFv2_BMREG1:
		case DIFv2_BMREG2:
		case DIFv2_BMREG3:
		case DIFv2_BMREG4:
		case DIFv2_BMREG5:
			p->bmreg[dif_get_bmreg_index_from_reg(haddr)] = value;
			dif_dump_bit_mux(p);
			break;

		case DIFv2_PBCCON:
			p->pbccon = value;
			break;

		case DIFv2_BCSEL0:
		case DIFv2_BCSEL1:
			p->bmreg[dif_get_bcsel_index_from_reg(haddr)] = value;
			dif_dump_bit_mux(p);
			break;

		case DIFv2_BCREG:
			p->bcreg = value;
			dif_dump_bit_mux(p);
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
			hw_error("DIFv2_MRPS_CTRL not supported!");
			break;

		case DIFv2_TXFIFO_CFG:
			if ((value & DIFv2_TXFIFO_CFG_TXFC) != (p->rxfifo_cfg & DIFv2_TXFIFO_CFG_TXFC))
				dif_set_fifo(p, DIF_FIFO_TX, (value & DIFv2_TXFIFO_CFG_TXFC) != 0);
			p->txfifo_cfg = value;
			break;

		case DIFv2_TPS_CTRL:
			p->tps_ctrl = value;

			if (dif_is_running(p) && value > 0) {
				dif_reset(p);
				p->state = DIF_STATE_TX;
				dif_work(p);
			}
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
			p->dma_control = value;
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

static void dif_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_dif_t *p = PMB887X_DIF(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-dif-v2", DIFv2_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	p->bus = ssi_create_bus(DEVICE(obj), TYPE_PMB887X_DIF);

	for (int i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);

	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
	pmb887x_srb_set_irq_router(&p->srb, p, dif_irq_router);
	pmb887x_srb_ext_init(&p->srb_err, &p->srb, DIFv2_ISR_ERR);

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
	qdev_init_gpio_out_named(dev, &p->gpio_cs[0], "CS0_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_cs[1], "CS1_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_cs[2], "CS2_OUT", 1);
}

static void dif_realize(DeviceState *dev, Error **errp) {
	pmb887x_dif_t *p = PMB887X_DIF(dev);
	pmb887x_clc_init(&p->clc);
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
	dif_update_gpio_state(p);

	pmb887x_fifo32_init(&p->tx_fifo_buffered, FIFO_SIZE);
	pmb887x_fifo32_init(&p->rx_fifo_buffered, FIFO_SIZE);

	pmb887x_fifo32_init(&p->tx_fifo_single, 1);
	pmb887x_fifo32_init(&p->rx_fifo_single, 1);

	dif_set_fifo(p, DIF_FIFO_RX, false);
	dif_set_fifo(p, DIF_FIFO_TX, false);
}

static const Property dif_properties[] = {
	DEFINE_PROP_LINK("bus", pmb887x_dif_t, bus, "SSI", SSIBus *),
	DEFINE_PROP_LINK("dmac", pmb887x_dif_t, dmac, TYPE_PMB887X_DMAC, pmb887x_dmac_t *),
	DEFINE_PROP_LINK("lcd", pmb887x_dif_t, lcd, "pmb887x-lcd", pmb887x_lcd_t *),
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
