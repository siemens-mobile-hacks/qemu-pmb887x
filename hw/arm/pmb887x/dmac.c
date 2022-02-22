/*
 * DMA Controller (PL080)
 * */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/ptimer.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"

#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"

#define DMAC_DEBUG

#ifdef DMAC_DEBUG
#define DPRINTF(fmt, ...) do { fprintf(stderr, "[pmb887x-dmac]: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#define TYPE_PMB887X_DMAC	"pmb887x-dmac"
#define PMB887X_DMAC(obj)	OBJECT_CHECK(pmb887x_dmac_t, (obj), TYPE_PMB887X_DMAC)

static const uint32_t PCELL_ID = 0x0A141080;
static const uint32_t PERIPH_ID = 0xB105F00D;

typedef struct {
	uint32_t src_addr;
	uint32_t dst_addr;
	uint32_t lli;
	uint32_t control;
	uint32_t config;
	qemu_irq irq;
} pmb887x_dmac_ch_t;

typedef struct {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	MemoryRegion *downstream;
	AddressSpace downstream_as;
	
	qemu_irq irq;
	pmb887x_dmac_ch_t ch[8];
	
	uint32_t config;
} pmb887x_dmac_t;

static int dmac_get_index_by_reg(uint32_t reg) {
	if (reg >= DMAC_CH_SRC_ADDR0 && reg <= DMAC_CH_SRC_ADDR7)
		return (reg - DMAC_CH_SRC_ADDR0) / 0x20;
	
	if (reg >= DMAC_CH_DST_ADDR0 && reg <= DMAC_CH_DST_ADDR7)
		return (reg - DMAC_CH_DST_ADDR0) / 0x20;
	
	if (reg >= DMAC_CH_LLI0 && reg <= DMAC_CH_LLI7)
		return (reg - DMAC_CH_LLI0) / 0x20;
	
	if (reg >= DMAC_CH_CONTROL0 && reg <= DMAC_CH_CONTROL7)
		return (reg - DMAC_CH_CONTROL0) / 0x20;
	
	if (reg >= DMAC_CH_CONFIG0 && reg <= DMAC_CH_CONFIG7)
		return (reg - DMAC_CH_CONFIG0) / 0x20;
	
	switch (reg) {
		case DMAC_PCELL_ID0:	return 0;
		case DMAC_PCELL_ID1:	return 1;
		case DMAC_PCELL_ID2:	return 2;
		case DMAC_PCELL_ID3:	return 3;
		case DMAC_PERIPH_ID0:	return 0;
		case DMAC_PERIPH_ID1:	return 1;
		case DMAC_PERIPH_ID2:	return 2;
		case DMAC_PERIPH_ID3:	return 3;
	}
	error_report("pmb887x-dmac: unknown reg %d", reg);
	abort();
	return -1;
}

static uint64_t dmac_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_dmac_t *p = (pmb887x_dmac_t *) opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case DMAC_CONFIG:
			value = p->config;
		break;
		
		case DMAC_PCELL_ID0:
		case DMAC_PCELL_ID1:
		case DMAC_PCELL_ID2:
		case DMAC_PCELL_ID3:
			value = (PCELL_ID >> dmac_get_index_by_reg(haddr) * 8) & 0xFF;
		break;
		
		case DMAC_PERIPH_ID0:
		case DMAC_PERIPH_ID1:
		case DMAC_PERIPH_ID2:
		case DMAC_PERIPH_ID3:
			value = (PERIPH_ID >> dmac_get_index_by_reg(haddr) * 8) & 0xFF;
		break;
		
		case DMAC_CH_SRC_ADDR0:
		case DMAC_CH_SRC_ADDR1:
		case DMAC_CH_SRC_ADDR2:
		case DMAC_CH_SRC_ADDR3:
		case DMAC_CH_SRC_ADDR4:
		case DMAC_CH_SRC_ADDR5:
		case DMAC_CH_SRC_ADDR6:
		case DMAC_CH_SRC_ADDR7:
			value = p->ch[dmac_get_index_by_reg(haddr)].src_addr;
		break;
		
		case DMAC_CH_DST_ADDR0:
		case DMAC_CH_DST_ADDR1:
		case DMAC_CH_DST_ADDR2:
		case DMAC_CH_DST_ADDR3:
		case DMAC_CH_DST_ADDR4:
		case DMAC_CH_DST_ADDR5:
		case DMAC_CH_DST_ADDR6:
		case DMAC_CH_DST_ADDR7:
			value = p->ch[dmac_get_index_by_reg(haddr)].dst_addr;
		break;
		
		case DMAC_CH_CONFIG0:
		case DMAC_CH_CONFIG1:
		case DMAC_CH_CONFIG2:
		case DMAC_CH_CONFIG3:
		case DMAC_CH_CONFIG4:
		case DMAC_CH_CONFIG5:
		case DMAC_CH_CONFIG6:
		case DMAC_CH_CONFIG7:
			value = p->ch[dmac_get_index_by_reg(haddr)].config;
		break;
		
		case DMAC_CH_CONTROL0:
		case DMAC_CH_CONTROL1:
		case DMAC_CH_CONTROL2:
		case DMAC_CH_CONTROL3:
		case DMAC_CH_CONTROL4:
		case DMAC_CH_CONTROL5:
		case DMAC_CH_CONTROL6:
		case DMAC_CH_CONTROL7:
			value = p->ch[dmac_get_index_by_reg(haddr)].control;
		break;
		
		case DMAC_CH_LLI0:
		case DMAC_CH_LLI1:
		case DMAC_CH_LLI2:
		case DMAC_CH_LLI3:
		case DMAC_CH_LLI4:
		case DMAC_CH_LLI5:
		case DMAC_CH_LLI6:
		case DMAC_CH_LLI7:
			value = p->ch[dmac_get_index_by_reg(haddr)].lli;
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

static void dmac_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_dmac_t *p = (pmb887x_dmac_t *) opaque;
	
	pmb887x_dump_io(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case DMAC_CONFIG:
			p->config = value;
		break;
		
		case DMAC_CH_SRC_ADDR0:
		case DMAC_CH_SRC_ADDR1:
		case DMAC_CH_SRC_ADDR2:
		case DMAC_CH_SRC_ADDR3:
		case DMAC_CH_SRC_ADDR4:
		case DMAC_CH_SRC_ADDR5:
		case DMAC_CH_SRC_ADDR6:
		case DMAC_CH_SRC_ADDR7:
			p->ch[dmac_get_index_by_reg(haddr)].src_addr = value;
		break;
		
		case DMAC_CH_DST_ADDR0:
		case DMAC_CH_DST_ADDR1:
		case DMAC_CH_DST_ADDR2:
		case DMAC_CH_DST_ADDR3:
		case DMAC_CH_DST_ADDR4:
		case DMAC_CH_DST_ADDR5:
		case DMAC_CH_DST_ADDR6:
		case DMAC_CH_DST_ADDR7:
			p->ch[dmac_get_index_by_reg(haddr)].dst_addr = value;
		break;
		
		case DMAC_CH_CONFIG0:
		case DMAC_CH_CONFIG1:
		case DMAC_CH_CONFIG2:
		case DMAC_CH_CONFIG3:
		case DMAC_CH_CONFIG4:
		case DMAC_CH_CONFIG5:
		case DMAC_CH_CONFIG6:
		case DMAC_CH_CONFIG7:
			p->ch[dmac_get_index_by_reg(haddr)].config = value;
		break;
		
		case DMAC_CH_CONTROL0:
		case DMAC_CH_CONTROL1:
		case DMAC_CH_CONTROL2:
		case DMAC_CH_CONTROL3:
		case DMAC_CH_CONTROL4:
		case DMAC_CH_CONTROL5:
		case DMAC_CH_CONTROL6:
		case DMAC_CH_CONTROL7:
			p->ch[dmac_get_index_by_reg(haddr)].control = value;
		break;
		
		case DMAC_CH_LLI0:
		case DMAC_CH_LLI1:
		case DMAC_CH_LLI2:
		case DMAC_CH_LLI3:
		case DMAC_CH_LLI4:
		case DMAC_CH_LLI5:
		case DMAC_CH_LLI6:
		case DMAC_CH_LLI7:
			p->ch[dmac_get_index_by_reg(haddr)].lli = value;
		break;
		
		default:
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
}

static const MemoryRegionOps io_ops = {
	.read			= dmac_io_read,
	.write			= dmac_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void dmac_init(Object *obj) {
	pmb887x_dmac_t *p = PMB887X_DMAC(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-dmac", DMAC_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq);
	
	for (int i = 0; i < ARRAY_SIZE(p->ch); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->ch[i].irq);
}

static void dmac_realize(DeviceState *dev, Error **errp) {
	pmb887x_dmac_t *p = PMB887X_DMAC(dev);
	
	if (!p->downstream) {
		error_setg(errp, "DMAC 'downstream' link not set");
		return;
	}
	
	address_space_init(&p->downstream_as, p->downstream, "pl080-downstream");
	
	if (!p->irq) {
		error_report("pmb887x-scu: irq not set");
		abort();
	}
	
	for (int i = 0; i < ARRAY_SIZE(p->ch); i++) {
		if (!p->ch[i].irq) {
			error_report("pmb887x-scu: channel%d irq not set", i);
			abort();
		}
	}
}

static Property dmac_properties[] = {
	DEFINE_PROP_LINK("downstream", pmb887x_dmac_t, downstream, TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_END_OF_LIST(),
};

static void dmac_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, dmac_properties);
	dc->realize = dmac_realize;
}

static const TypeInfo dmac_info = {
    .name          	= TYPE_PMB887X_DMAC,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_dmac_t),
    .instance_init 	= dmac_init,
    .class_init    	= dmac_class_init,
};

static void dmac_register_types(void) {
	type_register_static(&dmac_info);
}
type_init(dmac_register_types)
