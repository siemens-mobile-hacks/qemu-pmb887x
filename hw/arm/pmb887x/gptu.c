/*
 * System Timer (56 bit)
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

#define GPTU_DEBUG

#ifdef GPTU_DEBUG
#define DPRINTF(fmt, ...) do { fprintf(stderr, "[pmb887x-gptu]: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#define TYPE_PMB887X_GPTU	"pmb887x-gptu"
#define PMB887X_GPTU(obj)	OBJECT_CHECK(struct pmb887x_gptu_t, (obj), TYPE_PMB887X_GPTU)

struct pmb887x_gptu_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	qemu_irq irq[8];
	
	pmb887x_clc_reg_t clc;
	pmb887x_src_reg_t src[8];
	
	uint32_t t01irs;
	uint32_t t01ots;
	uint32_t t2con;
	uint32_t t2rccon;
	uint32_t t2ais;
	uint32_t t2bis;
	uint32_t t2es;
	uint32_t osel;
	uint32_t out;
	uint32_t t0dcba;
	uint32_t t0cba;
	uint32_t t0rdcba;
	uint32_t t0rcba;
	uint32_t t1dcba;
	uint32_t t1cba;
	uint32_t t1rdcba;
	uint32_t t1rcba;
	uint32_t t2;
	uint32_t t2rc0;
	uint32_t t2rc1;
	uint32_t t012run;
	uint32_t srsel;
};

static void gptu_update_state(struct pmb887x_gptu_t *p) {
	// TODO
}

static int get_src_index_by_addr(hwaddr haddr) {
	switch (haddr) {
		case GPTU_SRC0:		return 0;
		case GPTU_SRC1:		return 1;
		case GPTU_SRC2:		return 2;
		case GPTU_SRC3:		return 3;
		case GPTU_SRC4:		return 4;
		case GPTU_SRC5:		return 5;
		case GPTU_SRC6:		return 6;
		case GPTU_SRC7:		return 7;
	}
	return -1;
}

static uint64_t gptu_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb887x_gptu_t *p = (struct pmb887x_gptu_t *) opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case GPTU_CLC:
			value = pmb887x_clc_get(&p->clc);
		break;
		
		case GPTU_ID:
			value = 0x0001C011;
		break;
		
		case GPTU_T01IRS:
			value = p->t01irs;
		break;
		
		case GPTU_T01OTS:
			value = p->t01ots;
		break;
		
		case GPTU_T2CON:
			value = p->t2con;
		break;
		
		case GPTU_T2RCCON:
			value = p->t2rccon;
		break;
		
		case GPTU_T2AIS:
			value = p->t2ais;
		break;
		
		case GPTU_T2BIS:
			value = p->t2bis;
		break;
		
		case GPTU_T2ES:
			value = p->t2es;
		break;
		
		case GPTU_OSEL:
			value = p->osel;
		break;
		
		case GPTU_OUT:
			value = p->out;
		break;
		
		case GPTU_T0DCBA:
			value = p->t0dcba;
		break;
		
		case GPTU_T0CBA:
			value = p->t0cba;
		break;
		
		case GPTU_T0RDCBA:
			value = p->t0rdcba;
		break;
		
		case GPTU_T0RCBA:
			value = p->t0rcba;
		break;
		
		case GPTU_T1DCBA:
			value = p->t1dcba;
		break;
		
		case GPTU_T1CBA:
			value = p->t1cba;
		break;
		
		case GPTU_T1RDCBA:
			value = p->t1rdcba;
		break;
		
		case GPTU_T1RCBA:
			value = p->t1rcba;
		break;
		
		case GPTU_T2:
			value = p->t2;
		break;
		
		case GPTU_T2RC0:
			value = p->t2rc0;
		break;
		
		case GPTU_T2RC1:
			value = p->t2rc1;
		break;
		
		case GPTU_T012RUN:
			value = p->t012run;
		break;
		
		case GPTU_SRSEL:
			value = p->srsel;
		break;
		
		case GPTU_SRC0:
		case GPTU_SRC1:
		case GPTU_SRC2:
		case GPTU_SRC3:
		case GPTU_SRC4:
		case GPTU_SRC5:
		case GPTU_SRC6:
		case GPTU_SRC7:
			value = pmb887x_src_get(&p->src[get_src_index_by_addr(haddr)]);
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

static void gptu_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct pmb887x_gptu_t *p = (struct pmb887x_gptu_t *) opaque;
	
	pmb887x_dump_io(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case GPTU_CLC:
			pmb887x_clc_set(&p->clc, value);
		break;
		
		case GPTU_ID:
			value = 0x0001C011;
		break;
		
		case GPTU_T01IRS:
			p->t01irs = value;
		break;
		
		case GPTU_T01OTS:
			p->t01ots = value;
		break;
		
		case GPTU_T2CON:
			p->t2con = value;
		break;
		
		case GPTU_T2RCCON:
			p->t2rccon = value;
		break;
		
		case GPTU_T2AIS:
			p->t2ais = value;
		break;
		
		case GPTU_T2BIS:
			p->t2bis = value;
		break;
		
		case GPTU_T2ES:
			p->t2es = value;
		break;
		
		case GPTU_OSEL:
			p->osel = value;
		break;
		
		case GPTU_OUT:
			p->out = value;
		break;
		
		case GPTU_T0DCBA:
			p->t0dcba = value;
		break;
		
		case GPTU_T0CBA:
			p->t0cba = value;
		break;
		
		case GPTU_T0RDCBA:
			p->t0rdcba = value;
		break;
		
		case GPTU_T0RCBA:
			p->t0rcba = value;
		break;
		
		case GPTU_T1DCBA:
			p->t1dcba = value;
		break;
		
		case GPTU_T1CBA:
			p->t1cba = value;
		break;
		
		case GPTU_T1RDCBA:
			p->t1rdcba = value;
		break;
		
		case GPTU_T1RCBA:
			p->t1rcba = value;
		break;
		
		case GPTU_T2:
			p->t2 = value;
		break;
		
		case GPTU_T2RC0:
			p->t2rc0 = value;
		break;
		
		case GPTU_T2RC1:
			p->t2rc1 = value;
		break;
		
		case GPTU_T012RUN:
			p->t012run = value;
		break;
		
		case GPTU_SRSEL:
			p->srsel = value;
		break;
		
		case GPTU_SRC0:
		case GPTU_SRC1:
		case GPTU_SRC2:
		case GPTU_SRC3:
		case GPTU_SRC4:
		case GPTU_SRC5:
		case GPTU_SRC6:
		case GPTU_SRC7:
			pmb887x_src_set(&p->src[get_src_index_by_addr(haddr)], value);
		break;
		
		default:
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	gptu_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= gptu_io_read,
	.write			= gptu_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 4,
		.max_access_size	= 4
	}
};

static void gptu_init(Object *obj) {
	struct pmb887x_gptu_t *p = PMB887X_GPTU(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-gptu", GPTU_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (int i = 0; i < ARRAY_SIZE(p->src); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void gptu_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_gptu_t *p = PMB887X_GPTU(dev);
	
	pmb887x_clc_init(&p->clc);
	
	for (int i = 0; i < ARRAY_SIZE(p->src); i++) {
		if (!p->irq[i]) {
			error_report("pmb887x-gptu: irq %d not set", i);
			abort();
		}
		pmb887x_src_init(&p->src[i], p->irq[i]);
	}
	
	gptu_update_state(p);
}

static Property gptu_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void gptu_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, gptu_properties);
	dc->realize = gptu_realize;
}

static const TypeInfo gptu_info = {
    .name          	= TYPE_PMB887X_GPTU,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_gptu_t),
    .instance_init 	= gptu_init,
    .class_init    	= gptu_class_init,
};

static void gptu_register_types(void) {
	type_register_static(&gptu_info);
}
type_init(gptu_register_types)
