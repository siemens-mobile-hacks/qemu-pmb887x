/*
 * I2C
 * */
#define PMB887X_TRACE_ID		I2C
#define PMB887X_TRACE_PREFIX	"pmb887x-i2c"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"
#include "hw/i2c/i2c.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/fifo.h"

#define TYPE_PMB887X_I2C	"pmb887x-i2c-v2"
OBJECT_DECLARE_SIMPLE_TYPE(pmb887x_i2c_t, PMB887X_I2C);

#define I2C_TX_BYTE_TIME	100000
#define FIFO_IO_SIZE		0x3FFF
#define FIFO_SIZE			8

#define FIFO_ICR_MASK		(I2Cv2_ICR_BREQ_INT | I2Cv2_ICR_LBREQ_INT | I2Cv2_ICR_SREQ_INT | I2Cv2_ICR_LSREQ_INT)

enum {
	I2C_STATE_NONE,
	I2C_STATE_MASTER_TX,
	I2C_STATE_MASTER_RX,
	I2C_STATE_MASTER_RESTART,
	I2C_STATE_MASTER_TX_DONE,
	I2C_STATE_DONE,
};

enum {
	I2C_SINGLE_REQ_IRQ = 0,
	I2C_BURST_REQ_IRQ,
	I2C_ERROR_IRQ,
	I2C_PROTOCOL_IRQ,
};

struct pmb887x_i2c_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
    I2CBus *bus;
	QEMUTimer *timer;
	bool wait_for_next_tick;
    
	pmb887x_clc_reg_t clc;
	pmb887x_srb_reg_t srb;
	pmb887x_srb_ext_reg_t srb_proto;
	pmb887x_srb_ext_reg_t srb_err;
	
	int state;
	int next_byte_action;
	
	pmb887x_fifo32_t fifo;
	
	bool is_read;
	uint8_t addr;
	
	qemu_irq irq[4];
	bool busy;
	bool addr_is_sent;
	bool enddctrl_end;
	bool enddctrl_restart;
	uint32_t rx_total_bytes;
	uint32_t rx_bytes_in_fifo;
	
	bool fifo_req;
	uint32_t tx_remaining;
	uint32_t rx_remaining;

	uint32_t runctrl;
	uint32_t fdivcfg;
	uint32_t fdivhighcfg;
	uint32_t addrcfg;
	uint32_t mrpsctrl;
	uint32_t fifocfg;
	uint32_t tpsctrl;
	uint32_t timcfg;
	uint32_t dma_control;

	qemu_irq gpio_scl;
	qemu_irq gpio_sda;
};

static void i2c_work(pmb887x_i2c_t *p);
static void i2c_transfer_done(pmb887x_i2c_t *p);
static void i2c_transfer_error(pmb887x_i2c_t *p);

static inline bool i2c_is_running(pmb887x_i2c_t *p) {
	return pmb887x_clc_is_enabled(&p->clc) && p->runctrl;
}

static int i2c_irq_router(void *opaque, int event_id) {
	switch ((1 << event_id)) {
		case I2Cv2_ISR_LSREQ_INT:
		case I2Cv2_ISR_SREQ_INT:
		case I2Cv2_ISR_LBREQ_INT:
			return I2C_SINGLE_REQ_IRQ;
		case I2Cv2_ISR_BREQ_INT:
			return I2C_BURST_REQ_IRQ;
		case I2Cv2_ISR_I2C_P_INT:
			return I2C_PROTOCOL_IRQ;
		case I2Cv2_ISR_I2C_ERR_INT:
			return I2C_ERROR_IRQ;
		default:
			hw_error("Unknown event id: %d\n", event_id);
	}
}

static inline uint32_t i2c_get_rx_align(pmb887x_i2c_t *p) {
	return 1 << ((p->fifocfg & I2Cv2_FIFOCFG_RXFA) >> I2Cv2_FIFOCFG_RXFA_SHIFT);
}

static inline uint32_t i2c_get_tx_align(pmb887x_i2c_t *p) {
	return 1 << ((p->fifocfg & I2Cv2_FIFOCFG_TXFA) >> I2Cv2_FIFOCFG_TXFA_SHIFT);
}

static inline uint32_t i2c_get_rx_burst_size(pmb887x_i2c_t *p) {
	uint32_t bs = 1 << ((p->fifocfg & I2Cv2_FIFOCFG_RXBS) >> I2Cv2_FIFOCFG_RXBS_SHIFT);
	return bs * (4 / i2c_get_rx_align(p));
}

static uint32_t i2c_get_tx_burst_size(pmb887x_i2c_t *p) {
	uint32_t bs = 1 << ((p->fifocfg & I2Cv2_FIFOCFG_TXBS) >> I2Cv2_FIFOCFG_TXBS_SHIFT);
	return bs * (4 / i2c_get_tx_align(p));
}

static void i2c_fifo_req(pmb887x_i2c_t *p) {
	if (p->fifo_req)
		return;

	if (p->state == I2C_STATE_MASTER_TX) {
		uint32_t burst_req_size = i2c_get_tx_burst_size(p);
		uint32_t single_req_size = (4 / i2c_get_tx_align(p));

		if (!p->tx_remaining)
			return;

		bool is_fc = (p->fifocfg & I2Cv2_FIFOCFG_TXFC) != 0;
		if (is_fc) {
			if (p->tx_remaining > burst_req_size) {
				pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_BREQ_INT);
			} else if (p->tx_remaining == burst_req_size) {
				pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_LBREQ_INT);
			} else if (p->tx_remaining > single_req_size) {
				pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_SREQ_INT);
			} else if (p->tx_remaining > 0) {
				pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_LSREQ_INT);
			}
		} else {
			if (pmb887x_fifo_free_count(&p->fifo) >= burst_req_size) {
				pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_BREQ_INT);
			}
		}

		p->fifo_req = true;
	} else if (p->state == I2C_STATE_MASTER_RX) {
		uint32_t burst_req_size = i2c_get_rx_burst_size(p);
		uint32_t single_req_size = (4 / i2c_get_rx_align(p));

		if (!p->rx_bytes_in_fifo)
			return;

		bool is_fc = (p->fifocfg & I2Cv2_FIFOCFG_RXFC) != 0;
		if (is_fc) {
			if (p->rx_bytes_in_fifo > burst_req_size) {
				pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_BREQ_INT);
			} else if (p->rx_bytes_in_fifo == burst_req_size) {
				pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_LBREQ_INT);
			} else if (p->rx_bytes_in_fifo > single_req_size) {
				pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_SREQ_INT);
			} else {
				pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_LSREQ_INT);
			}
		} else {
			if (p->rx_bytes_in_fifo >= burst_req_size) {
				pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_BREQ_INT);
			} else {
				pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_SREQ_INT);
			}
		}

		p->fifo_req = true;
	}
}

static void i2c_fifo_clr_req(pmb887x_i2c_t *p) {
	p->fifo_req = false;
}

static void i2c_kernel_reset(pmb887x_i2c_t *p, uint32_t new_state) {
	p->state = new_state;
	p->addr_is_sent = false;
	p->tx_remaining = 0;
	p->rx_remaining = 0;
	p->rx_total_bytes = 0;
	p->rx_bytes_in_fifo = 0;
	p->enddctrl_end = false;
	p->enddctrl_restart = false;
	i2c_fifo_clr_req(p);
}

static void i2c_timer_reset(void *opaque) {
	pmb887x_i2c_t *p = (pmb887x_i2c_t *) opaque;
	p->wait_for_next_tick = false;
	
	if (p->runctrl)
		i2c_work(p);
}

static void i2c_timer_schedule(pmb887x_i2c_t *p) {
	if (!p->wait_for_next_tick) {
		timer_mod(p->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + I2C_TX_BYTE_TIME);
		p->wait_for_next_tick = true;
	}
}

static void i2c_fifo_write(pmb887x_i2c_t *p, uint64_t value) {
	if (p->state == I2C_STATE_MASTER_RX)
		hw_error("Wriring to FIFO in MASTER_RX is not allowed!");

	if (!pmb887x_fifo_free_count(&p->fifo)) {
		DPRINTF("TX FIFO overflow!\n");
		pmb887x_srb_ext_set_isr(&p->srb_err, I2Cv2_ERRIRQSS_TXF_OFL);
		i2c_transfer_error(p);
		return;
	}
	
	pmb887x_fifo32_push(&p->fifo, value);
	i2c_timer_schedule(p);
}

static void i2c_fifo_read(pmb887x_i2c_t *p, uint64_t *value) {
	if (pmb887x_fifo_is_empty(&p->fifo)) {
		DPRINTF("RX FIFO underflow!\n");
		pmb887x_srb_ext_set_isr(&p->srb_err, I2Cv2_ERRIRQSS_RXF_UFL);
		i2c_transfer_error(p);
		return;
	}
	
	*value = pmb887x_fifo32_pop(&p->fifo);
	p->rx_bytes_in_fifo -= MIN(p->rx_bytes_in_fifo, 4 / i2c_get_rx_align(p));

	if (!p->rx_bytes_in_fifo)
		i2c_timer_schedule(p);
}

static bool i2c_tx_from_fifo(pmb887x_i2c_t *p) {
	uint32_t align = i2c_get_tx_align(p);
	while (pmb887x_fifo_count(&p->fifo) > 0) {
		uint32_t value = pmb887x_fifo32_pop(&p->fifo);

		uint32_t bytes_in_fifo_reg = MIN(4, p->tx_remaining);
		if (bytes_in_fifo_reg == 0)
			bytes_in_fifo_reg = 4; // shit from real HW

		for (uint32_t i = 0; i < bytes_in_fifo_reg; i += align) {
			uint8_t byte = (value >> (8 * i)) & 0xFF;
			if (p->tx_remaining > 0)
				p->tx_remaining--;

			if (!p->addr_is_sent) {
				p->addr_is_sent = true;
				p->addr = byte >> 1;
				p->is_read = (byte & 1) != 0;

				if (p->is_read) {
					DPRINTF("TX: %02X (read)\n", p->addr);
				} else {
					DPRINTF("TX: %02X (write)\n", p->addr);
				}

				if (i2c_start_transfer(p->bus, p->addr, p->is_read) != 0)
					return false;

				if (p->enddctrl_end) {
					i2c_nack(p->bus);
					return true;
				}

				if (p->enddctrl_restart)
					return true;
			} else {
				DPRINTF("TX: %02X\n", byte);
				if (i2c_send(p->bus, byte) != 0)
					return false;

				if (p->enddctrl_end) {
					i2c_nack(p->bus);
					return true;
				}

				if (p->enddctrl_restart)
					return true;
			}
		}
	}
	return true;
}

static void i2c_rx_to_fifo(pmb887x_i2c_t *p) {
	uint32_t align = i2c_get_rx_align(p);
	uint32_t rx_remaining = MIN(p->rx_remaining, i2c_get_rx_burst_size(p));

	uint32_t bytes_read = 0;
	while (bytes_read < rx_remaining && !pmb887x_fifo_is_full(&p->fifo)) {
		uint32_t value = 0;
		uint32_t bytes_in_fifo_reg = MIN(4, rx_remaining - bytes_read);
		for (uint32_t i = 0; i < bytes_in_fifo_reg; i += align) {
			uint8_t byte = i2c_recv(p->bus);
			DPRINTF("RX: %02X\n", byte);
			value |= (byte << (8 * i));
			bytes_read++;

			if (p->enddctrl_end)
				break;
		}
		pmb887x_fifo32_push(&p->fifo, value);

		if (p->enddctrl_end)
			break;
	}

	p->rx_remaining -= bytes_read;
	p->rx_total_bytes += bytes_read;
	p->rx_bytes_in_fifo += bytes_read;
}

static void i2c_start_tx(pmb887x_i2c_t *p) {
	if (!i2c_is_running(p))
		return;

	if (p->state != I2C_STATE_MASTER_RESTART && p->state != I2C_STATE_NONE)
		return;

	if (p->tpsctrl > 0) {
		i2c_kernel_reset(p, I2C_STATE_MASTER_TX);
		p->tx_remaining = p->tpsctrl;
		p->rx_remaining = p->mrpsctrl;

		DPRINTF("new transfer: tx=%d, rx=%d\n", p->tx_remaining, p->rx_remaining);

		i2c_fifo_req(p);
		p->tpsctrl = 0;
		p->mrpsctrl = 0;

		i2c_timer_schedule(p);
	}
}

static void i2c_handle_enddctrl(pmb887x_i2c_t *p, uint32_t value) {
	if (p->state == I2C_STATE_MASTER_TX || p->state == I2C_STATE_MASTER_RX) {
		if ((value & I2Cv2_ENDDCTRL_SETEND))
			p->enddctrl_end = true;
		if ((value & I2Cv2_ENDDCTRL_SETRSC))
			p->enddctrl_restart = true;
	} else if (p->state == I2C_STATE_MASTER_RESTART) {
		if ((value & I2Cv2_ENDDCTRL_SETEND)) {
			p->enddctrl_end = true;
			i2c_transfer_done(p);
		}
	}
	i2c_timer_schedule(p);
}

static void i2c_transfer_error(pmb887x_i2c_t *p) {
	pmb887x_srb_ext_set_isr(&p->srb_proto, I2Cv2_PIRQSS_TX_END);
	DPRINTF("stop\n");
	i2c_end_transfer(p->bus);
	i2c_kernel_reset(p, I2C_STATE_NONE);
	i2c_timer_schedule(p);
}

static void i2c_transfer_done(pmb887x_i2c_t *p) {
	pmb887x_srb_ext_set_isr(&p->srb_proto, I2Cv2_PIRQSS_TX_END);

	if (p->enddctrl_end) {
		DPRINTF("stop\n");
		i2c_end_transfer(p->bus);
		i2c_kernel_reset(p, I2C_STATE_NONE);
	} else if (p->enddctrl_restart) {
		i2c_kernel_reset(p, I2C_STATE_MASTER_RESTART);
	} else {
		if ((p->addrcfg & I2Cv2_ADDRCFG_SONA)) {
			DPRINTF("stop\n");
			i2c_end_transfer(p->bus);
			i2c_kernel_reset(p, I2C_STATE_NONE);
		} else {
			i2c_kernel_reset(p, I2C_STATE_MASTER_RESTART);
		}
	}
	i2c_timer_schedule(p);
}

static void i2c_work(pmb887x_i2c_t *p) {
	if (p->state == I2C_STATE_MASTER_TX) {
		if (!i2c_tx_from_fifo(p)) {
			DPRINTF("NACK!\n");
			if ((p->addrcfg & I2Cv2_ADDRCFG_SONA))
				pmb887x_fifo_reset(&p->fifo);
			pmb887x_srb_ext_set_isr(&p->srb_proto, I2Cv2_PIRQSS_NACK);
			i2c_transfer_done(p);
			return;
		}

		if (p->enddctrl_restart) {
			pmb887x_fifo_reset(&p->fifo);
			i2c_transfer_done(p);
			return;
		}

		if (p->enddctrl_end) {
			i2c_transfer_done(p);
			return;
		}

		i2c_fifo_req(p);

		if (p->tx_remaining == 0) {
			if (p->is_read) {
				p->state = I2C_STATE_MASTER_RX;
				pmb887x_srb_ext_set_isr(&p->srb_proto, I2Cv2_PIRQSS_RX);
				i2c_fifo_clr_req(p);
			} else {
				i2c_transfer_done(p);
			}
			i2c_timer_schedule(p);
		}
	} else if (p->state == I2C_STATE_MASTER_RX) {
		if (p->enddctrl_restart) {
			i2c_nack(p->bus);
			i2c_transfer_done(p);
			return;
		}

		i2c_rx_to_fifo(p);
		if (p->enddctrl_end) {
			i2c_nack(p->bus);
			i2c_transfer_done(p);
			return;
		}

		i2c_fifo_req(p);

		if (p->rx_remaining == 0 && pmb887x_fifo_is_empty(&p->fifo))
			i2c_transfer_done(p);
	} else if (p->state == I2C_STATE_MASTER_TX_DONE) {

	} else if (p->state == I2C_STATE_NONE) {
		if (p->tpsctrl > 0)
			i2c_start_tx(p);
	} else if (p->state == I2C_STATE_MASTER_RESTART) {
		if (p->enddctrl_end) {
			i2c_transfer_done(p);
		} else if (p->tpsctrl > 0) {
			i2c_start_tx(p);
		}
	}
}

static uint64_t i2c_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_i2c_t *p = opaque;

	uint64_t value = 0;

	switch (haddr) {
		case I2Cv2_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case I2Cv2_ID:
			value = 0xF057C000;
			break;

		case I2Cv2_RUNCTRL:
			value = p->runctrl;
			break;

		case I2Cv2_ENDDCTRL:
			value = 0;
			break;

		case I2Cv2_FDIVCFG:
			value = p->fdivcfg;
			break;

		case I2Cv2_FDIVHIGHCFG:
			value = p->fdivhighcfg;
			break;

		case I2Cv2_ADDRCFG:
			value = p->addrcfg;
			break;

		case I2Cv2_BUSSTAT:
			if (p->state != I2C_STATE_NONE) {
				value = I2Cv2_BUSSTAT_BS_BUSY_MASTER;
			} else if (i2c_bus_busy(p->bus)) {
				value = I2Cv2_BUSSTAT_BS_BUSY_OTHER_MASTER;
			} else {
				value = I2Cv2_BUSSTAT_BS_FREE;
			}

			if (p->state == I2C_STATE_MASTER_RX)
				value |= I2Cv2_BUSSTAT_RnW;
			break;

		case I2Cv2_MRPSCTRL:
			value = 0;
			break;

		case I2Cv2_FIFOCFG:
			value = p->fifocfg;
			break;

		case I2Cv2_RPSSTAT:
			value = p->rx_total_bytes;
			break;

		case I2Cv2_TPSCTRL:
			value = 0;
			break;

		case I2Cv2_FFSSTAT:
			value = pmb887x_fifo_count(&p->fifo);
			break;

		case I2Cv2_TIMCFG:
			value = p->timcfg;
			break;

		case I2Cv2_TXD ... (I2Cv2_TXD + FIFO_IO_SIZE):
			value = 0;
			break;

		case I2Cv2_RXD ... (I2Cv2_RXD + FIFO_IO_SIZE):
			i2c_fifo_read(p, &value);
			break;

		case I2Cv2_IMSC:
			value = pmb887x_srb_get_imsc(&p->srb);
			break;

		case I2Cv2_RIS:
			value = pmb887x_srb_get_ris(&p->srb);
			break;

		case I2Cv2_MIS:
			value = pmb887x_srb_get_mis(&p->srb);
			break;

		case I2Cv2_ICR:
		case I2Cv2_ISR:
			value = 0;
			break;

		case I2Cv2_PIRQSS:
			value = pmb887x_srb_ext_get_ris(&p->srb_proto);
			break;

		case I2Cv2_PIRQSM:
			value = pmb887x_srb_ext_get_imsc(&p->srb_proto);
			break;

		case I2Cv2_PIRQSC:
			value = 0;
			break;

		case I2Cv2_ERRIRQSS:
			value = pmb887x_srb_ext_get_mis(&p->srb_err);
			break;

		case I2Cv2_ERRIRQSM:
			value = pmb887x_srb_ext_get_imsc(&p->srb_err);
			break;

		case I2Cv2_ERRIRQSC:
			value = 0;
			break;

		case I2Cv2_DMAE:
			value = p->dma_control;
			break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}

	IO_DUMP(haddr + p->mmio.addr, size, value, false);

	return value;
}

static void i2c_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_i2c_t *p = opaque;

	IO_DUMP(haddr + p->mmio.addr, size, value, true);

	switch (haddr) {
		case I2Cv2_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case I2Cv2_RUNCTRL:
			if (p->runctrl != value) {
				p->runctrl = value;
				if (!p->runctrl) {
					if (p->state == I2C_STATE_MASTER_RX || p->state == I2C_STATE_MASTER_TX || p->state == I2C_STATE_MASTER_RESTART) {
						DPRINTF("stop\n");
						i2c_end_transfer(p->bus);
						i2c_kernel_reset(p, I2C_STATE_NONE);
						pmb887x_fifo_reset(&p->fifo);
					}
				}
			}
			break;

		case I2Cv2_ENDDCTRL:
			i2c_handle_enddctrl(p, value);
			break;

		case I2Cv2_FDIVCFG:
			p->fdivcfg = value;
			break;

		case I2Cv2_FDIVHIGHCFG:
			p->fdivhighcfg = value;
			break;

		case I2Cv2_ADDRCFG:
			p->addrcfg = value;
			break;

		case I2Cv2_MRPSCTRL:
			p->rx_total_bytes = 0;
			p->mrpsctrl = value;
			break;

		case I2Cv2_FIFOCFG:
			p->fifocfg = value;
			break;

		case I2Cv2_TPSCTRL:
			p->tpsctrl = value;
			i2c_start_tx(p);
			break;

		case I2Cv2_TIMCFG:
			p->timcfg = value;
			break;

		case I2Cv2_TXD ... (I2Cv2_TXD + FIFO_IO_SIZE):
			i2c_fifo_write(p, value);
			break;

		case I2Cv2_IMSC:
			pmb887x_srb_set_imsc(&p->srb, value);
			break;

		case I2Cv2_ICR:
			value &= ~(I2Cv2_ICR_I2C_ERR_INT | I2Cv2_ICR_I2C_P_INT); // unclearable IRQ's

			if ((value & FIFO_ICR_MASK) && (p->state == I2C_STATE_MASTER_RX || p->state == I2C_STATE_MASTER_TX)) {
				i2c_fifo_clr_req(p);
				i2c_fifo_req(p);
			}

			pmb887x_srb_set_icr(&p->srb, value);
			break;

		case I2Cv2_ISR:
			pmb887x_srb_set_isr(&p->srb, value);
			break;

		case I2Cv2_PIRQSM:
			pmb887x_srb_ext_set_imsc(&p->srb_proto, value);
			break;

		case I2Cv2_PIRQSC:
			pmb887x_srb_ext_set_icr(&p->srb_proto, value);
			break;

		case I2Cv2_ERRIRQSM:
			pmb887x_srb_ext_set_imsc(&p->srb_err, value);
			break;

		case I2Cv2_ERRIRQSC:
			pmb887x_srb_ext_set_icr(&p->srb_err, value);
			break;

		case I2Cv2_DMAE:
			p->dma_control = value;
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
			break;
	}
}

static const MemoryRegionOps io_ops = {
	.read			= i2c_io_read,
	.write			= i2c_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 4,
		.max_access_size	= 4
	}
};

static void i2c_handle_gpio_input(void *opaque, int id, int level) {
	// nothing
}

static void i2c_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_i2c_t *p = PMB887X_I2C(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, TYPE_PMB887X_I2C, I2Cv2_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	for (int i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);

	qdev_init_gpio_in_named(dev, i2c_handle_gpio_input, "SCL_IN", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_scl, "SCL_OUT", 1);
	qdev_init_gpio_in_named(dev, i2c_handle_gpio_input, "SDA_IN", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_sda, "SDA_OUT", 1);
}

static void i2c_realize(DeviceState *dev, Error **errp) {
	pmb887x_i2c_t *p = PMB887X_I2C(dev);

	p->bus = i2c_init_bus(dev, TYPE_PMB887X_I2C);

	pmb887x_clc_init(&p->clc);
	
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
	pmb887x_srb_set_irq_router(&p->srb, p, i2c_irq_router);
	
	pmb887x_srb_ext_init(&p->srb_err, &p->srb, I2Cv2_ISR_I2C_ERR_INT);
	pmb887x_srb_ext_init(&p->srb_proto, &p->srb, I2Cv2_ISR_I2C_P_INT);
	
	pmb887x_fifo32_init(&p->fifo, FIFO_SIZE);
	
	p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, i2c_timer_reset, p);
}

static const Property i2c_properties[] = {
	DEFINE_PROP_LINK("bus", pmb887x_i2c_t, bus, TYPE_I2C_BUS, I2CBus *)
};

static void i2c_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, i2c_properties);
	dc->realize = i2c_realize;
}

static const TypeInfo i2c_info = {
    .name          	= TYPE_PMB887X_I2C,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_i2c_t),
    .instance_init 	= i2c_init,
    .class_init    	= i2c_class_init,
};

static void i2c_register_types(void) {
	type_register_static(&i2c_info);
}
type_init(i2c_register_types)
