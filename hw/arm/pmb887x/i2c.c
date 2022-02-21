/*
 * I2C
 * */
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
#include "qapi/error.h"
#include "hw/i2c/i2c.h"

#include "hw/arm/pmb887x/i2c.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"

#define I2C_DEBUG

#ifdef I2C_DEBUG
#define DPRINTF(fmt, ...) do { fprintf(stderr, "[pmb887x-i2c]: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#define TYPE_PMB887X_I2C	"pmb887x-i2c"
#define PMB887X_I2C(obj)	OBJECT_CHECK(pmb887x_i2c_t, (obj), TYPE_PMB887X_I2C)
#define I2C_TX_BYTE_TIME	40000

enum {
	I2C_SINGLE_REQ_IRQ = 0,
	I2C_BURST_REQ_IRQ,
	I2C_ERROR_IRQ,
	I2C_PROTOCOL_IRQ,
};

typedef struct {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
    I2CBus *bus;
	QEMUTimer *timer;
    
	pmb887x_clc_reg_t clc;
	pmb887x_srb_reg_t srb;
	pmb887x_srb_ext_reg_t srb_proto;
	pmb887x_srb_ext_reg_t srb_err;
	
	qemu_irq irq[4];
	bool wait_for_sreq;
	uint32_t tx_cnt;
	
	uint32_t runctrl;
	uint32_t enddctrl;
	uint32_t fdivcfg;
	uint32_t fdivhighcfg;
	uint32_t addrcfg;
	uint32_t busstat;
	uint32_t mrpsctrl;
	uint32_t fifocfg;
	uint32_t rpsstat;
	uint32_t tpsctrl;
	uint32_t ffsstat;
	uint32_t timcfg;
} pmb887x_i2c_t;

static void i2c_update_state(pmb887x_i2c_t *p) {
	// TODO
}

static int i2c_irq_router(void *opaque, int event_id) {
	switch ((1 << event_id)) {
		case I2C_ISR_LSREQ_INT:		return I2C_SINGLE_REQ_IRQ;
		case I2C_ISR_SREQ_INT:		return I2C_SINGLE_REQ_IRQ;
		case I2C_ISR_LBREQ_INT:		return I2C_SINGLE_REQ_IRQ;
		case I2C_ISR_BREQ_INT:		return I2C_BURST_REQ_IRQ;
		case I2C_ISR_I2C_P_INT:		return I2C_PROTOCOL_IRQ;
		case I2C_ISR_I2C_ERR_INT:	return I2C_ERROR_IRQ;
	}
	
	error_report("Unknown event id: %d\n", event_id);
	abort();
	
	return 0;
}

static uint32_t i2c_get_txbs(pmb887x_i2c_t *p) {
	switch (p->fifocfg & I2C_FIFOCFG_TXBS) {
		case I2C_FIFOCFG_TXBS_4_WORD:		return 4;
		case I2C_FIFOCFG_TXBS_2_WORD:		return 2;
		case I2C_FIFOCFG_TXBS_1_WORD:		return 1;
	}
	
	error_report("Unknown TXSB value: %08X\n", (p->fifocfg & I2C_FIFOCFG_TXBS));
	abort();
}

static uint32_t i2c_get_rxbs(pmb887x_i2c_t *p) {
	switch (p->fifocfg & I2C_FIFOCFG_RXBS) {
		case I2C_FIFOCFG_RXBS_4_WORD:		return 4;
		case I2C_FIFOCFG_RXBS_2_WORD:		return 2;
		case I2C_FIFOCFG_RXBS_1_WORD:		return 1;
	}
	
	error_report("Unknown RXBS value: %08X\n", (p->fifocfg & I2C_FIFOCFG_RXBS));
	abort();
}

static uint32_t i2c_get_rx_align(pmb887x_i2c_t *p) {
	switch (p->fifocfg & I2C_FIFOCFG_RXFA) {
		case I2C_FIFOCFG_RXFA_BYTE:			return 1;
		case I2C_FIFOCFG_RXFA_HALF_WORLD:	return 2;
		case I2C_FIFOCFG_RXFA_WORD:			return 4;
	}
	
	error_report("Unknown RXFA value: %08X\n", (p->fifocfg & I2C_FIFOCFG_RXFA));
	abort();
}

static uint32_t i2c_get_tx_align(pmb887x_i2c_t *p) {
	switch (p->fifocfg & I2C_FIFOCFG_TXFA) {
		case I2C_FIFOCFG_TXFA_BYTE:			return 1;
		case I2C_FIFOCFG_TXFA_HALF_WORLD:	return 2;
		case I2C_FIFOCFG_TXFA_WORD:			return 4;
	}
	
	error_report("Unknown TXFA value: %08X\n", (p->fifocfg & I2C_FIFOCFG_TXFA));
	abort();
}

static uint32_t i2c_get_rx_burst_size(pmb887x_i2c_t *p) {
	return 4 / i2c_get_rx_align(p);
}

static uint32_t i2c_get_tx_burst_size(pmb887x_i2c_t *p) {
	return 4 / i2c_get_tx_align(p);
}

static void i2c_trigger_sreq(pmb887x_i2c_t *p) {
	if (p->wait_for_sreq) {
		DPRINTF("double i2c_trigger_sreq\n");
		abort();
	}
	
	p->wait_for_sreq = true;
	timer_mod(p->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + I2C_TX_BYTE_TIME);
}

static void i2c_fifo_write(pmb887x_i2c_t *p, uint64_t value) {
	uint32_t align = i2c_get_tx_align(p);
	
	if (!p->tpsctrl || p->wait_for_sreq) {
		DPRINTF("TX fifo overflow!\n");
		pmb887x_srb_ext_set_isr(&p->srb_err, I2C_ERRIRQSS_TXF_OFL);
		return;
	}
	
	uint32_t tx_size = MIN(4, p->tpsctrl);
	for (uint32_t i = 0; i < tx_size; i += align) {
		uint8_t byte = (value >> (8 * i)) & 0xFF;
		
		if (p->tx_cnt == 0) {
			if ((value & 1)) {
				DPRINTF("start: %02X (read)\n", byte >> 1);
			} else {
				DPRINTF("start: %02X (write)\n", byte >> 1);
			}
			
			bool nack = i2c_start_transfer(p->bus, byte >> 1, (byte & 1) != 0);
			if (nack) {
				pmb887x_srb_ext_set_isr(&p->srb_proto, I2C_PIRQSS_NACK);
				return;
			}
		} else {
			DPRINTF("TX: %02X\n", byte);
			i2c_send(p->bus, byte);
		}
		
		p->tx_cnt++;
		p->tpsctrl--;
	}
	
	i2c_trigger_sreq(p);
	
	if (!p->tpsctrl && p->mrpsctrl)
		pmb887x_srb_ext_set_isr(&p->srb_proto, I2C_PIRQSS_RX);
}

static void i2c_fifo_read(pmb887x_i2c_t *p, uint64_t *value) {
	uint32_t align = i2c_get_tx_align(p);
	
	if (!p->mrpsctrl || p->wait_for_sreq) {
		DPRINTF("RX fifo overflow!\n");
		pmb887x_srb_ext_set_isr(&p->srb_err, I2C_ERRIRQSS_RXF_UFL);
		return;
	}
	
	uint32_t rx_size = MIN(4, p->mrpsctrl);
	for (uint32_t i = 0; i < rx_size; i += align) {
		uint8_t byte = i2c_recv(p->bus);
		DPRINTF("RX: %02X\n", byte);
		*value |= byte << (8 * i);
		p->mrpsctrl--;
	}
	
	i2c_trigger_sreq(p);
}

static void i2c_timer_reset(void *opaque) {
	pmb887x_i2c_t *p = (pmb887x_i2c_t *) opaque;
	
	p->wait_for_sreq = false;
	
	if (p->tpsctrl > 0) {
		if (p->tpsctrl <= i2c_get_tx_burst_size(p)) {
			pmb887x_srb_set_isr(&p->srb, I2C_ISR_LSREQ_INT);
		} else {
			pmb887x_srb_set_isr(&p->srb, I2C_ISR_SREQ_INT);
		}
	} else if (p->mrpsctrl > 0) {
		if (p->mrpsctrl <= i2c_get_rx_burst_size(p)) {
			pmb887x_srb_set_isr(&p->srb, I2C_ISR_LSREQ_INT);
		} else {
			pmb887x_srb_set_isr(&p->srb, I2C_ISR_SREQ_INT);
		}
	} else {
		pmb887x_srb_ext_set_isr(&p->srb_proto, I2C_PIRQSS_TX_END);
		i2c_end_transfer(p->bus);
		DPRINTF("stop\n");
	}
}

static uint64_t i2c_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_i2c_t *p = (pmb887x_i2c_t *) opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case I2C_CLC:
			value = pmb887x_clc_get(&p->clc);
		break;
		
		case I2C_ID:
			value = 0xF057C000;
		break;
		
		case I2C_RUNCTRL:
			value = p->runctrl;
		break;
		
		case I2C_ENDDCTRL:
			value = p->enddctrl;
		break;
		
		case I2C_FDIVCFG:
			value = p->fdivcfg;
		break;
		
		case I2C_FDIVHIGHCFG:
			value = p->fdivhighcfg;
		break;
		
		case I2C_ADDRCFG:
			value = p->addrcfg;
		break;
		
		case I2C_BUSSTAT:
			value = p->busstat;
		break;
		
		case I2C_MRPSCTRL:
			value = p->mrpsctrl;
		break;
		
		case I2C_FIFOCFG:
			value = p->fifocfg;
		break;
		
		case I2C_RPSSTAT:
			value = p->rpsstat;
		break;
		
		case I2C_TPSCTRL:
			value = p->tpsctrl;
		break;
		
		case I2C_FFSSTAT:
			value = p->ffsstat;
		break;
		
		case I2C_TIMCFG:
			value = p->timcfg;
		break;
		
		case I2C_TXD:
			value = 0;
		break;
		
		case I2C_RXD:
			i2c_fifo_read(p, &value);
		break;
		
		case I2C_IMSC:
			value = pmb887x_srb_get_imsc(&p->srb);
		break;
		
		case I2C_RIS:
			value = pmb887x_srb_get_ris(&p->srb);
		break;
		
		case I2C_MIS:
			value = pmb887x_srb_get_mis(&p->srb);
		break;
		
		case I2C_ICR:
			value = 0;
		break;
		
		case I2C_ISR:
			value = 0;
		break;
		
		case I2C_PIRQSS:
			value = pmb887x_srb_ext_get_ris(&p->srb_proto);
		break;
		
		case I2C_PIRQSM:
			value = pmb887x_srb_ext_get_imsc(&p->srb_proto);
		break;
		
		case I2C_PIRQSC:
			value = 0;
		break;
		
		case I2C_ERRIRQSS:
			value = pmb887x_srb_ext_get_mis(&p->srb_err);
		break;
		
		case I2C_ERRIRQSM:
			value = pmb887x_srb_ext_get_imsc(&p->srb_err);
		break;
		
		case I2C_ERRIRQSC:
			value = 0;
		break;
		
		default:
			pmb887x_dump_io(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	pmb887x_dump_io(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void i2c_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_i2c_t *p = (pmb887x_i2c_t *) opaque;
	
	pmb887x_dump_io(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case I2C_CLC:
			pmb887x_clc_set(&p->clc, value);
		break;
		
		default:
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
		
		case I2C_RUNCTRL:
			p->runctrl = value;
		break;
		
		case I2C_ENDDCTRL:
			p->enddctrl = value;
		break;
		
		case I2C_FDIVCFG:
			p->fdivcfg = value;
		break;
		
		case I2C_FDIVHIGHCFG:
			p->fdivhighcfg = value;
		break;
		
		case I2C_ADDRCFG:
			p->addrcfg = value;
		break;
		
		case I2C_BUSSTAT:
			p->busstat = value;
		break;
		
		case I2C_MRPSCTRL:
			p->mrpsctrl = value;
		break;
		
		case I2C_FIFOCFG:
			p->fifocfg = value;
		break;
		
		case I2C_RPSSTAT:
			p->rpsstat = value;
		break;
		
		case I2C_TPSCTRL:
			p->tx_cnt = 0;
			p->tpsctrl = value;
			i2c_trigger_sreq(p);
		break;
		
		case I2C_FFSSTAT:
			p->ffsstat = value;
		break;
		
		case I2C_TIMCFG:
			p->timcfg = value;
		break;
		
		case I2C_TXD:
			i2c_fifo_write(p, value);
		break;
		
		case I2C_IMSC:
			pmb887x_srb_set_imsc(&p->srb, value);
		break;
		
		case I2C_ICR:
			pmb887x_srb_set_icr(&p->srb, value);
		break;
		
		case I2C_ISR:
			pmb887x_srb_set_isr(&p->srb, value);
		break;
		
		case I2C_PIRQSM:
			pmb887x_srb_ext_set_imsc(&p->srb_proto, value);
		break;
		
		case I2C_PIRQSC:
			pmb887x_srb_ext_set_icr(&p->srb_proto, value);
		break;
		
		case I2C_ERRIRQSM:
			pmb887x_srb_ext_set_imsc(&p->srb_err, value);
		break;
		
		case I2C_ERRIRQSC:
			pmb887x_srb_ext_set_icr(&p->srb_err, value);
		break;
	}
	
	i2c_update_state(p);
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

I2CBus *pmb887x_i2c_bus(DeviceState *dev) {
    pmb887x_i2c_t *p = PMB887X_I2C(dev);
    return p->bus;
}

static void i2c_init(Object *obj) {
	pmb887x_i2c_t *p = PMB887X_I2C(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-i2c", I2C_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	p->bus = i2c_init_bus(DEVICE(obj), NULL);
	
	for (int i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void i2c_realize(DeviceState *dev, Error **errp) {
	pmb887x_i2c_t *p = PMB887X_I2C(dev);
	
	pmb887x_clc_init(&p->clc);
	
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
	pmb887x_srb_set_irq_router(&p->srb, p, i2c_irq_router);
	
	pmb887x_srb_ext_init(&p->srb_err, &p->srb, I2C_ISR_I2C_ERR_INT);
	pmb887x_srb_ext_init(&p->srb_proto, &p->srb, I2C_ISR_I2C_P_INT);
	
	p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, i2c_timer_reset, p);
	
	i2c_update_state(p);
}

static Property i2c_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
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
