/*
 * USART
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

#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"

#define USART_DEBUG

#ifdef USART_DEBUG
#define DPRINTF(fmt, ...) do { fprintf(stderr, "[pmb887x-usart]: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#define TYPE_PMB887X_USART	"pmb887x-usart"
#define PMB887X_USART(obj)	OBJECT_CHECK(struct pmb887x_usart_t, (obj), TYPE_PMB887X_USART)

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

struct pmb887x_usart_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	struct pmb887x_clc_reg_t clc;
	struct pmb887x_srb_reg_t srb;
	qemu_irq irq[USART_IRQ_NR];
	
	uint32_t con;
	uint32_t bg;
	uint32_t fdv;
	uint32_t pmw;
	uint32_t txb;
	uint32_t rxb;
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
};

static void usart_update_state(struct pmb887x_usart_t *p) {
	// TODO
}

static uint64_t usart_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb887x_usart_t *p = (struct pmb887x_usart_t *) opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case USART_CLC:
			value = pmb887x_clc_get(&p->clc);
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
			value = p->rxb;
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
			value = p->fstat;
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
		break;
		
		case USART_MIS:
			value = pmb887x_srb_get_mis(&p->srb);
		break;
		
		case USART_ICR:
			value = 0;
		break;
		
		case USART_ISR:
			value = 0;
		break;
		
		case USART_TMO:
			value = p->tmo;
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

static void usart_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct pmb887x_usart_t *p = (struct pmb887x_usart_t *) opaque;
	
	pmb887x_dump_io(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case USART_CLC:
			pmb887x_clc_set(&p->clc, value);
		break;
		
		case USART_ID:
			value = 0x000044F1;
		break;
		
		case USART_CON:
			p->con = value;
		break;
		
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
			p->txb = value;
		break;
		
		case USART_RXB:
			p->rxb = value;
		break;
		
		case USART_ABCON:
			p->abcon = value;
		break;
		
		case USART_ABSTAT:
			p->abstat = value;
		break;
		
		case USART_RXFCON:
			p->rxfcon = value;
		break;
		
		case USART_TXFCON:
			p->txfcon = value;
		break;
		
		case USART_FSTAT:
			p->fstat = value;
		break;
		
		case USART_WHBCON:
			p->whbcon = value;
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
		
		case USART_FCSTAT:
			p->fcstat = value;
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
		
		case USART_TMO:
			p->tmo = value;
		break;
		
		default:
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	usart_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= usart_io_read,
	.write			= usart_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 4,
		.max_access_size	= 4
	}
};

static void usart_init(Object *obj) {
	struct pmb887x_usart_t *p = PMB887X_USART(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-usart", USART_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (int i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void usart_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_usart_t *p = PMB887X_USART(dev);
	
	pmb887x_clc_init(&p->clc);
	
	for (int i = 0; i < ARRAY_SIZE(p->irq); i++) {
		if (!p->irq[i]) {
			error_report("pmb887x-usart: irq %d not set", i);
			abort();
		}
	}
	
	pmb887x_srb_init(&p->srb, p->irq, ARRAY_SIZE(p->irq));
	// pmb887x_srb_set_irq_router(usart_irq_router);
	
	usart_update_state(p);
}

static Property usart_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void usart_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, usart_properties);
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
