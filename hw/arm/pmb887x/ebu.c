/*
 * External Bus Unit
 * */
#define PMB887X_TRACE_ID		EBU
#define PMB887X_TRACE_PREFIX	"pmb887x-ebu"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "hw/qdev-properties.h"
#include "cpu.h"

#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_EBU	"pmb887x-ebu"
#define PMB887X_EBU(obj)	OBJECT_CHECK(struct pmb887x_ebu_t, (obj), TYPE_PMB887X_EBU)

typedef struct pmb887x_ebu_t pmb887x_ebu_t;
typedef struct pmb887x_ebu_regions_t pmb887x_ebu_regions_t;
typedef struct pmb887x_ebu_user_data_t pmb887x_ebu_user_data_t;

struct pmb887x_ebu_regions_t {
	MemoryRegion memory;
	size_t addr;
};

struct pmb887x_ebu_user_data_t {
	pmb887x_ebu_t *p;
	int cs;
};

struct pmb887x_ebu_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	MemoryRegion *cs[8];
	MemoryRegion regions[8];
	pmb887x_ebu_user_data_t user_data[8];
	pmb887x_clc_reg_t clc;
	
	uint32_t con;
	uint32_t bfcon;
	uint32_t emuovl;
	uint32_t usercon;
	uint32_t addrsel[8];
	uint32_t buscon[8];
	uint32_t busap[8];
	uint32_t sdrmcon[2];
	uint32_t sdrmref[2];
	uint32_t sdrmod[2];
};

static void ebu_update_state(pmb887x_ebu_t *p) {
	bool is_ebu_enabled = pmb887x_clc_is_enabled(&p->clc);
	
	for (int i = 0; i < 8; ++i) {
		MemoryRegion *region = &p->regions[i];
		
		uint32_t base = (p->addrsel[i] & EBU_ADDRSEL_BASE) >> EBU_ADDRSEL_BASE_SHIFT;
		uint32_t mask = (p->addrsel[i] & EBU_ADDRSEL_MASK) >> EBU_ADDRSEL_MASK_SHIFT;
		
		bool is_enabled = is_ebu_enabled && (p->addrsel[i] & EBU_ADDRSEL_REGENAB);
		bool is_ro = (p->buscon[i] & EBU_BUSCON_WRITE) != 0;
		uint32_t addr = base << 12;
		uint32_t size = (1 << (27 - mask));
		
		bool state_changed = memory_region_size(region) != size || region->addr != addr ||
			region->enabled != is_enabled || region->readonly != is_ro;
		
		if (state_changed) {
			if (is_enabled && !region->enabled) {
				DPRINTF("CS%d enable region %08X-%08X%s [%dM]\n", i, addr, addr + size - 1, is_ro ? " [RO]" : " [RW]", size / 1024 / 1024);
			} else if (!is_enabled && region->enabled) {
				DPRINTF("CS%d disable region %08X-%08X [%dM]\n", i, addr, addr + size - 1, size / 1024 / 1024);
			} else if (is_enabled) {
				DPRINTF("CS%d update region %08X-%08X%s [%dM]\n", i, addr, addr + size - 1, is_ro ? " [RO]" : " [RW]", size / 1024 / 1024);
			}
			
			if (region->addr != addr && memory_region_is_mapped(region)) {
				// Remove memory from CPU space
				memory_region_del_subregion(get_system_memory(), region);
			}
			
			memory_region_set_enabled(region, is_enabled);
			memory_region_set_size(region, size);
			memory_region_set_readonly(region, is_ro);
			
			if (!memory_region_is_mapped(region)) {
				// Add memory to CPU space
				memory_region_add_subregion_overlap(get_system_memory(), addr, region, 1007 - i);
			}
		}
	}
}

static uint32_t ebu_get_index_from_reg(hwaddr haddr) {
	switch (haddr) {
		case EBU_ADDRSEL0:
		case EBU_BUSCON0:
		case EBU_BUSAP0:
			return 0;

		case EBU_ADDRSEL1:
		case EBU_BUSCON1:
		case EBU_BUSAP1:
			return 1;

		case EBU_ADDRSEL2:
		case EBU_BUSCON2:
		case EBU_BUSAP2:
			return 2;

		case EBU_ADDRSEL3:
		case EBU_BUSCON3:
		case EBU_BUSAP3:
			return 3;

		case EBU_ADDRSEL4:
		case EBU_BUSCON4:
		case EBU_BUSAP4:
			return 4;

		case EBU_ADDRSEL5:
		case EBU_BUSCON5:
		case EBU_BUSAP5:
			return 5;

		case EBU_ADDRSEL6:
		case EBU_BUSCON6:
		case EBU_BUSAP6:
			return 6;

		case EBU_EMUAS:
		case EBU_EMUBC:
		case EBU_EMUBAP:
			return 7;

		case EBU_SDRMCON0:
		case EBU_SDRMREF0:
		case EBU_SDRSTAT0:
		case EBU_SDRMOD0:
			return 0;

		case EBU_SDRMCON1:
		case EBU_SDRMREF1:
		case EBU_SDRSTAT1:
		case EBU_SDRMOD1:
			return 1;

		default:
			hw_error("Invalid reg: %08lX", haddr);
			exit(0);
	}
}

static uint64_t ebu_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_ebu_t *p = opaque;
	
	uint64_t value = 0;
	switch (haddr) {
		case EBU_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case EBU_ID:
			value = 0x0014C004;
			break;

		case EBU_CON:
			value = p->con;
			break;

		case EBU_BFCON:
			value = p->bfcon;
			break;

		case EBU_EMUOVL:
			value = p->emuovl;
			break;

		case EBU_USERCON:
			value = p->usercon;
			break;

		case EBU_ADDRSEL0:
		case EBU_ADDRSEL1:
		case EBU_ADDRSEL2:
		case EBU_ADDRSEL3:
		case EBU_ADDRSEL4:
		case EBU_ADDRSEL5:
		case EBU_ADDRSEL6:
		case EBU_EMUAS:
			value = p->addrsel[ebu_get_index_from_reg(haddr)];
			break;

		case EBU_BUSCON0:
		case EBU_BUSCON1:
		case EBU_BUSCON2:
		case EBU_BUSCON3:
		case EBU_BUSCON4:
		case EBU_BUSCON5:
		case EBU_BUSCON6:
		case EBU_EMUBC:
			value = p->buscon[ebu_get_index_from_reg(haddr)];
			break;

		case EBU_BUSAP0:
		case EBU_BUSAP1:
		case EBU_BUSAP2:
		case EBU_BUSAP3:
		case EBU_BUSAP4:
		case EBU_BUSAP5:
		case EBU_BUSAP6:
		case EBU_EMUBAP:
			value = p->busap[ebu_get_index_from_reg(haddr)];
			break;

		case EBU_SDRMCON0:
		case EBU_SDRMCON1:
			value = p->sdrmcon[ebu_get_index_from_reg(haddr)];
			break;

		case EBU_SDRMREF0:
		case EBU_SDRMREF1:
			value = p->sdrmref[ebu_get_index_from_reg(haddr)] | EBU_SDRMREF_SELFRENST | EBU_SDRMREF_SELFREXST;
			break;

		case EBU_SDRSTAT0:
		case EBU_SDRSTAT1:
			value = 0; // no errors
			break;

		case EBU_SDRMOD0:
		case EBU_SDRMOD1:
			value = p->sdrmod[ebu_get_index_from_reg(haddr)];
			break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	#if PMB887X_IO_BRIDGE
	pmb8876_io_bridge_write(haddr + p->mmio.addr, size, value);
	#endif
	
	return value;
}

static void ebu_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_ebu_t *p = opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case EBU_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case EBU_CON:
			p->con = value;
			break;

		case EBU_BFCON:
			p->bfcon = value;
			break;

		case EBU_EMUOVL:
			p->emuovl = value;
			break;

		case EBU_USERCON:
			p->usercon = value;
			break;

		case EBU_ADDRSEL0:
		case EBU_ADDRSEL1:
		case EBU_ADDRSEL2:
		case EBU_ADDRSEL3:
		case EBU_ADDRSEL4:
		case EBU_ADDRSEL5:
		case EBU_ADDRSEL6:
		case EBU_EMUAS:
			p->addrsel[ebu_get_index_from_reg(haddr)] = value;
			break;

		case EBU_BUSCON0:
		case EBU_BUSCON1:
		case EBU_BUSCON2:
		case EBU_BUSCON3:
		case EBU_BUSCON4:
		case EBU_BUSCON5:
		case EBU_BUSCON6:
		case EBU_EMUBC:
			p->buscon[ebu_get_index_from_reg(haddr)] = value;
			break;

		case EBU_BUSAP0:
		case EBU_BUSAP1:
		case EBU_BUSAP2:
		case EBU_BUSAP3:
		case EBU_BUSAP4:
		case EBU_BUSAP5:
		case EBU_BUSAP6:
		case EBU_EMUBAP:
			p->busap[ebu_get_index_from_reg(haddr)] = value;
			break;

		case EBU_SDRMCON0:
		case EBU_SDRMCON1:
			p->sdrmcon[ebu_get_index_from_reg(haddr)] = value;
			break;

		case EBU_SDRMREF0:
		case EBU_SDRMREF1:
			p->sdrmref[ebu_get_index_from_reg(haddr)] = value;
			break;

		case EBU_SDRMOD0:
		case EBU_SDRMOD1:
			p->sdrmod[ebu_get_index_from_reg(haddr)] = value;
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	#if PMB887X_IO_BRIDGE
	pmb8876_io_bridge_read(haddr + p->mmio.addr, size);
	#endif
	
	ebu_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= ebu_io_read,
	.write			= ebu_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static uint64_t ebu_unmapped_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_ebu_user_data_t *user_data = opaque;
	
	DPRINTF("read[%d] undefined memory at %08"PRIX64" [CS%d]\n", size, user_data->p->regions[user_data->cs].addr + haddr, user_data->cs);
	
	switch (size) {
		case 1:		return 0xFF;
		case 2:		return 0xFFFF;
		case 3:		return 0xFFFFFF;
		default:	return 0xFFFFFFFF;
	}
	return 0xFFFFFFFF;
}

static void ebu_unmapped_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_ebu_user_data_t *user_data = opaque;
	DPRINTF("write[%d] undefined memory at %08"PRIX64" [CS%d]\n", size, user_data->p->regions[user_data->cs].addr + haddr, user_data->cs);
}

static const MemoryRegionOps unmapped_io_ops = {
	.read			= ebu_unmapped_io_read,
	.write			= ebu_unmapped_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void ebu_init(Object *obj) {
	struct pmb887x_ebu_t *p = PMB887X_EBU(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-ebu", EBU_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
}

static void ebu_realize(DeviceState *dev, Error **errp) {
	pmb887x_ebu_t *p = PMB887X_EBU(dev);
	
	pmb887x_clc_init(&p->clc);

	for (int i = 0; i < 8; i++) {
		char memory_region_name[32];
		sprintf(memory_region_name, "EBU_CS%d", i);
		
		p->user_data[i].p = p;
		p->user_data[i].cs = i;
		
		if (p->cs[i]) {
			memory_region_init_alias(&p->regions[i], OBJECT(dev), memory_region_name, p->cs[i], 0, memory_region_size(p->cs[i]));
		} else {
			memory_region_init_io(&p->regions[i], OBJECT(dev), &unmapped_io_ops, &p->user_data[i], memory_region_name, 1 << 27);
		}
		
		memory_region_set_enabled(&p->regions[i], false);
	}
	
	ebu_update_state(p);
}

static const Property ebu_properties[] = {
	DEFINE_PROP_LINK("cs0", pmb887x_ebu_t, cs[0], TYPE_MEMORY_REGION, MemoryRegion *),
	DEFINE_PROP_LINK("cs1", pmb887x_ebu_t, cs[1], TYPE_MEMORY_REGION, MemoryRegion *),
	DEFINE_PROP_LINK("cs2", pmb887x_ebu_t, cs[2], TYPE_MEMORY_REGION, MemoryRegion *),
	DEFINE_PROP_LINK("cs3", pmb887x_ebu_t, cs[3], TYPE_MEMORY_REGION, MemoryRegion *),
	DEFINE_PROP_LINK("cs4", pmb887x_ebu_t, cs[4], TYPE_MEMORY_REGION, MemoryRegion *),
	DEFINE_PROP_LINK("cs5", pmb887x_ebu_t, cs[5], TYPE_MEMORY_REGION, MemoryRegion *),
	DEFINE_PROP_LINK("cs6", pmb887x_ebu_t, cs[6], TYPE_MEMORY_REGION, MemoryRegion *),
	DEFINE_PROP_LINK("cs7", pmb887x_ebu_t, cs[7], TYPE_MEMORY_REGION, MemoryRegion *),
};

static void ebu_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, ebu_properties);
	dc->realize = ebu_realize;
}

static const TypeInfo ebu_info = {
    .name          	= TYPE_PMB887X_EBU,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_ebu_t),
    .instance_init 	= ebu_init,
    .class_init    	= ebu_class_init,
};

static void ebu_register_types(void) {
	type_register_static(&ebu_info);
}
type_init(ebu_register_types)
