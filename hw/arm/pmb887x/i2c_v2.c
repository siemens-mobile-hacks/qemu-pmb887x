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
#define FIFO_SIZE 8

enum {
	I2C_STATE_NONE	= 0,
	I2C_STATE_TX	= 1,
	I2C_STATE_RX	= 2,
	I2C_STATE_DONE	= 3,
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
    
	pmb887x_clc_reg_t clc;
	pmb887x_srb_reg_t srb;
	pmb887x_srb_ext_reg_t srb_proto;
	pmb887x_srb_ext_reg_t srb_err;
	
	int state;
	
	pmb887x_fifo32_t fifo;
	
	bool last_mode;
	uint8_t last_addr;
	
	qemu_irq irq[4];
	bool busy;
	bool wait_for_next_tick;
	uint32_t tx_cnt;
	uint32_t rx_cnt;
	uint32_t rx_buffer_cnt;
	
	uint32_t runctrl;
	uint32_t enddctrl;
	uint32_t fdivcfg;
	uint32_t fdivhighcfg;
	uint32_t addrcfg;
	uint32_t mrpsctrl;
	uint32_t fifocfg;
	uint32_t tpsctrl;
	uint32_t timcfg;

	qemu_irq gpio_scl;
	qemu_irq gpio_sda;
};

static void i2c_work(pmb887x_i2c_t *p);

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

static uint32_t i2c_get_rx_align(pmb887x_i2c_t *p) {
	switch (p->fifocfg & I2Cv2_FIFOCFG_RXFA) {
		case I2Cv2_FIFOCFG_RXFA_BYTE:			return 1;
		case I2Cv2_FIFOCFG_RXFA_HALF_WORLD:		return 2;
		case I2Cv2_FIFOCFG_RXFA_WORD:			return 4;
		default:
			hw_error("Unknown RXFA value: %08X\n", (p->fifocfg & I2Cv2_FIFOCFG_RXFA));
	}
}

static uint32_t i2c_get_tx_align(pmb887x_i2c_t *p) {
	switch (p->fifocfg & I2Cv2_FIFOCFG_TXFA) {
		case I2Cv2_FIFOCFG_TXFA_BYTE:			return 1;
		case I2Cv2_FIFOCFG_TXFA_HALF_WORLD:		return 2;
		case I2Cv2_FIFOCFG_TXFA_WORD:			return 4;
		default:
			hw_error("Unknown TXFA value: %08X\n", (p->fifocfg & I2Cv2_FIFOCFG_TXFA));
	}
}

static uint32_t i2c_get_rx_burst_size(pmb887x_i2c_t *p) {
	switch (p->fifocfg & I2Cv2_FIFOCFG_RXBS) {
		case I2Cv2_FIFOCFG_RXBS_1_WORD:		return 1 * (4 / i2c_get_rx_align(p));
		case I2Cv2_FIFOCFG_RXBS_2_WORD:		return 2 * (4 / i2c_get_rx_align(p));
		case I2Cv2_FIFOCFG_RXBS_4_WORD:		return 4 * (4 / i2c_get_rx_align(p));
		default:
			hw_error("Unknown RXBS value: %08X\n", (p->fifocfg & I2Cv2_FIFOCFG_RXBS));
	}
}

static uint32_t i2c_get_tx_burst_size(pmb887x_i2c_t *p) {
	switch (p->fifocfg & I2Cv2_FIFOCFG_TXBS) {
		case I2Cv2_FIFOCFG_TXBS_1_WORD:		return 1 * (4 / i2c_get_tx_align(p));
		case I2Cv2_FIFOCFG_TXBS_2_WORD:		return 2 * (4 / i2c_get_tx_align(p));
		case I2Cv2_FIFOCFG_TXBS_4_WORD:		return 4 * (4 / i2c_get_tx_align(p));
		default:
			hw_error("Unknown TXBS value: %08X\n", (p->fifocfg & I2Cv2_FIFOCFG_TXBS));
	}
}

static void i2c_reset(pmb887x_i2c_t *p) {
	DPRINTF("reset state\n");
	p->state = I2C_STATE_NONE;
	p->tx_cnt = 0;
	p->rx_cnt = 0;
	p->rx_buffer_cnt = 0;
	pmb887x_fifo_reset(&p->fifo);
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
	if (!pmb887x_fifo_free_count(&p->fifo)) {
		DPRINTF("TX fifo overflow!\n");
		pmb887x_srb_ext_set_isr(&p->srb_err, I2Cv2_ERRIRQSS_TXF_OFL);
		i2c_reset(p);
		return;
	}
	
	pmb887x_fifo32_push(&p->fifo, value);
	i2c_timer_schedule(p);
}

static void i2c_fifo_read(pmb887x_i2c_t *p, uint64_t *value) {
	if (!pmb887x_fifo_count(&p->fifo)) {
		DPRINTF("RX fifo underflow!\n");
		pmb887x_srb_ext_set_isr(&p->srb_err, I2Cv2_ERRIRQSS_RXF_UFL);
		i2c_reset(p);
		return;
	}
	
	*value = pmb887x_fifo32_pop(&p->fifo);
	p->rx_buffer_cnt -= MIN(p->rx_buffer_cnt, 4 / i2c_get_rx_align(p));
	i2c_timer_schedule(p);
}

static bool i2c_tx(pmb887x_i2c_t *p, uint8_t byte) {
	if (p->tx_cnt == 0) {
		if ((byte & 1)) {
			DPRINTF("start: %02X (read)\n", byte >> 1);
		} else {
			DPRINTF("start: %02X (write)\n", byte >> 1);
		}
		
		p->last_addr = byte >> 1;
		p->last_mode = (byte & 1) != 0;
		
		bool nack = i2c_start_transfer(p->bus, p->last_addr, p->last_mode);
		if (nack) {
			DPRINTF("NACK for address: %02X\n", p->last_addr);
			pmb887x_srb_ext_set_isr(&p->srb_proto, I2Cv2_PIRQSS_NACK);
			i2c_reset(p);
			return false;
		}
	} else {
		DPRINTF("TX: %02X\n", byte);
		i2c_send(p->bus, byte);
	}
	p->tpsctrl--;
	p->tx_cnt++;
	return true;
}

static void i2c_tx_fifo(pmb887x_i2c_t *p, uint32_t tx_size) {
	uint32_t align = i2c_get_tx_align(p);
	while (tx_size > 0) {
		uint32_t value = pmb887x_fifo32_pop(&p->fifo);
		for (uint32_t i = 0; tx_size > 0 && i < 4; i += align) {
			uint8_t byte = (value >> (8 * i)) & 0xFF;
			if (!i2c_tx(p, byte))
				return;
			tx_size--;
		}
	}
}

static void i2c_rx_fifo(pmb887x_i2c_t *p, uint32_t rx_size) {
	uint32_t align = i2c_get_rx_align(p);
	while (rx_size > 0) {
		uint32_t value = 0;
		for (uint32_t i = 0; rx_size > 0 && i < 4; i += align) {
			uint8_t byte = i2c_recv(p->bus);
			DPRINTF("RX: %02X\n", byte);
			value |= (byte << (8 * i));
			p->mrpsctrl--;
			p->rx_cnt++;
			p->rx_buffer_cnt++;
			rx_size--;
		}
		pmb887x_fifo32_push(&p->fifo, value);
	}
}

static void i2c_trigger_fifo_irq(pmb887x_i2c_t *p, uint32_t avail, uint32_t size, uint32_t burst_size) {
	DPRINTF("avail=%d, size=%d, burst_size=%d\n", avail, size, burst_size);
	if (size < burst_size) {
		if (avail > size) {
			DPRINTF("I2C_ISR_SREQ_INT\n");
			pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_SREQ_INT);
		} else {
			DPRINTF("I2C_ISR_LSREQ_INT\n");
			pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_LSREQ_INT);
		}
	} else {
		if (avail > size) {
			DPRINTF("I2C_ISR_BREQ_INT\n");
			pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_BREQ_INT);
		} else {
			DPRINTF("I2C_ISR_LBREQ_INT\n");
			pmb887x_srb_set_isr(&p->srb, I2Cv2_ISR_LBREQ_INT);
		}
	}
}

static void i2c_work(pmb887x_i2c_t *p) {
	if (p->state == I2C_STATE_TX) {
		uint32_t max_tx_size = p->tpsctrl >= i2c_get_tx_burst_size(p) ? i2c_get_tx_burst_size(p) : (4 / i2c_get_tx_align(p));
		uint32_t tx_fifo_bytes = (4 / i2c_get_tx_align(p)) * pmb887x_fifo_count(&p->fifo);
		uint32_t tx_size = MIN(max_tx_size, p->tpsctrl);
		
		if (p->tpsctrl > 0 && tx_fifo_bytes >= tx_size)
			i2c_tx_fifo(p, tx_size);
		
		if (p->tpsctrl > 0) {
			i2c_trigger_fifo_irq(p, p->tpsctrl, tx_size, i2c_get_tx_burst_size(p));
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
	} else if (p->state == I2C_STATE_RX) {
		uint32_t max_rx_size = p->mrpsctrl >= i2c_get_rx_burst_size(p) ? i2c_get_rx_burst_size(p) : (4 / i2c_get_rx_align(p));
		uint32_t rx_size = MIN(max_rx_size, p->mrpsctrl);
		if (p->rx_buffer_cnt == 0 && rx_size > 0) {
			i2c_rx_fifo(p, rx_size);
			
			if (p->rx_buffer_cnt > 0) {
				uint32_t next_read_size = MIN(rx_size, p->rx_buffer_cnt);
				i2c_trigger_fifo_irq(p, p->mrpsctrl + p->rx_buffer_cnt, next_read_size, i2c_get_rx_burst_size(p));
			}
		}
		
		if (p->rx_buffer_cnt == 0) {
			p->state = I2C_STATE_DONE;
			i2c_timer_schedule(p);
		}
	} else if (p->state == I2C_STATE_DONE) {
		DPRINTF("stop\n");
		i2c_end_transfer(p->bus);
		pmb887x_srb_ext_set_isr(&p->srb_proto, I2Cv2_PIRQSS_TX_END);
		i2c_reset(p);
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
			value = p->enddctrl;
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

			if (p->state == I2C_STATE_RX)
				value |= I2Cv2_BUSSTAT_RnW;
			break;

		case I2Cv2_MRPSCTRL:
			value = 0;
			break;

		case I2Cv2_FIFOCFG:
			value = p->fifocfg;
			break;

		case I2Cv2_RPSSTAT:
			value = p->rx_cnt;
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

		case I2Cv2_TXD:
			value = 0;
			break;

		case I2Cv2_RXD:
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
			p->runctrl = value;

			if (p->runctrl && p->state == I2C_STATE_NONE && p->tpsctrl > 0) {
				i2c_reset(p);
				p->state = I2C_STATE_TX;
			}

			i2c_timer_schedule(p);
			break;

		case I2Cv2_ENDDCTRL:
			if ((value & I2Cv2_ENDDCTRL_SETEND)) {
				if (p->state != I2C_STATE_NONE && p->state != I2C_STATE_DONE) {
					p->state = I2C_STATE_DONE;
					i2c_work(p);
				} else {
					pmb887x_srb_ext_set_isr(&p->srb_proto, I2Cv2_PIRQSS_TX_END);
				}
			}

			if ((value & I2Cv2_ENDDCTRL_SETRSC)) {
				EPRINTF("I2C_ENDDCTRL_SETRSC not supported!\n");
				exit(1);
			}
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
			p->rx_cnt = 0;
			p->mrpsctrl = value;
			break;

		case I2Cv2_FIFOCFG:
			p->fifocfg = value;
			break;

		case I2Cv2_TPSCTRL:
			if (value > 0) {
				p->tpsctrl = value;

				if (p->runctrl) {
					i2c_reset(p);
					p->state = I2C_STATE_TX;
					i2c_timer_schedule(p);
				}
			}
			break;

		case I2Cv2_TIMCFG:
			p->timcfg = value;
			break;

		case I2Cv2_TXD:
			i2c_fifo_write(p, value);
			break;

		case I2Cv2_IMSC:
			pmb887x_srb_set_imsc(&p->srb, value);
			break;

		case I2Cv2_ICR:
			value &= ~(I2Cv2_ICR_I2C_ERR_INT | I2Cv2_ICR_I2C_P_INT); // unclearable IRQ's
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

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			// exit(1);
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
