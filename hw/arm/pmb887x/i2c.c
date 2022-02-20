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
#define PMB887X_I2C(obj)	OBJECT_CHECK(struct pmb887x_i2c_t, (obj), TYPE_PMB887X_I2C)

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
    
	struct pmb887x_clc_reg_t clc;
	struct pmb887x_srb_reg_t srb;
	struct pmb887x_srb_reg_t srb_proto;
	struct pmb887x_srb_reg_t srb_err;
	
	qemu_irq irq[4];
	
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
	uint32_t txd;
	uint32_t rxd;
};

static void i2c_update_state(struct pmb887x_i2c_t *p) {
	// TODO
}

static int i2c_irq_router(void *opaque, int event_id) {
	switch ((1 << event_id)) {
		case I2C_ISR_LSREQ_INT:		return I2C_SINGLE_REQ_IRQ;
		case I2C_ISR_SREQ_INT:		return I2C_SINGLE_REQ_IRQ;
		case I2C_ISR_LBREQ_INT:		return I2C_SINGLE_REQ_IRQ;
		case I2C_ISR_BREQ_INT:		return I2C_BURST_REQ_IRQ;
		case I2C_ISR_I2C_ERR_INT:	return I2C_PROTOCOL_IRQ;
		case I2C_ISR_I2C_P_INT:		return I2C_PROTOCOL_IRQ;
	}
	return I2C_ISR_I2C_P_INT;
}

static int i2c_err_irq_router(void *opaque, int event_id) {
	return I2C_ERROR_IRQ;
}

static int i2c_proto_irq_router(void *opaque, int event_id) {
	return I2C_PROTOCOL_IRQ;
}

static uint64_t i2c_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb887x_i2c_t *p = (struct pmb887x_i2c_t *) opaque;
	
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
			value = p->txd;
		break;
		
		case I2C_RXD:
			value = p->rxd;
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
			value = pmb887x_srb_get_ris(&p->srb_proto);
		break;
		
		case I2C_PIRQSM:
			value = pmb887x_srb_get_imsc(&p->srb_proto);
		break;
		
		case I2C_PIRQSC:
			value = 0;
		break;
		
		case I2C_ERRIRQSS:
			value = pmb887x_srb_get_mis(&p->srb_proto);
		break;
		
		case I2C_ERRIRQSM:
			value = pmb887x_srb_get_imsc(&p->srb_err);
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
	struct pmb887x_i2c_t *p = (struct pmb887x_i2c_t *) opaque;
	
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
			p->tpsctrl = value;
		break;
		
		case I2C_FFSSTAT:
			p->ffsstat = value;
		break;
		
		case I2C_TIMCFG:
			p->timcfg = value;
		break;
		
		case I2C_TXD:
			p->txd = value;
		break;
		
		case I2C_RXD:
			p->rxd = value;
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
			pmb887x_srb_set_imsc(&p->srb_proto, value);
		break;
		
		case I2C_PIRQSC:
			pmb887x_srb_set_icr(&p->srb_proto, value);
		break;
		
		case I2C_ERRIRQSM:
			pmb887x_srb_set_imsc(&p->srb_err, value);
		break;
		
		case I2C_ERRIRQSC:
			pmb887x_srb_set_icr(&p->srb_err, value);
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
    struct pmb887x_i2c_t *p = PMB887X_I2C(dev);
    return p->bus;
}

static void i2c_init(Object *obj) {
	struct pmb887x_i2c_t *p = PMB887X_I2C(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-i2c", I2C_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	p->bus = i2c_init_bus(DEVICE(obj), NULL);
	
	for (int i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void i2c_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_i2c_t *p = PMB887X_I2C(dev);
	
	pmb887x_clc_init(&p->clc);
	
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
	pmb887x_srb_set_irq_router(&p->srb, p, i2c_irq_router);
	
	pmb887x_srb_init(&p->srb_err, p->irq, ARRAY_SIZE(p->irq));
	pmb887x_srb_set_irq_router(&p->srb_proto, p, i2c_err_irq_router);
	
	pmb887x_srb_init(&p->srb_proto, p->irq, ARRAY_SIZE(p->irq));
	pmb887x_srb_set_irq_router(&p->srb_proto, p, i2c_proto_irq_router);
	
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
    .instance_size 	= sizeof(struct pmb887x_i2c_t),
    .instance_init 	= i2c_init,
    .class_init    	= i2c_class_init,
};

static void i2c_register_types(void) {
	type_register_static(&i2c_info);
}
type_init(i2c_register_types)
