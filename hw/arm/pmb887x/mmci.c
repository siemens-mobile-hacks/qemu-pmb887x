/*
 * MMC Interface
 * */
#define PMB887X_TRACE_ID		MMCI
#define PMB887X_TRACE_PREFIX	"pmb887x-mmci"

#define MMCI_EXTI_COUNT 7

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "exec/memory.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "cpu.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_MMCI	"pmb887x-mmci"
#define PMB887X_MMCI(obj)	OBJECT_CHECK(pmb887x_mmci_t, (obj), TYPE_PMB887X_MMCI)

typedef struct pmb887x_mmci_t pmb887x_mmci_t;

struct pmb887x_mmci_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	pmb887x_clc_reg_t clc;
	qemu_irq gpio_dat0;
	qemu_irq gpio_dat1;
	qemu_irq gpio_cmd;
	qemu_irq gpio_clk;
};

static uint64_t mmci_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_mmci_t *p = opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case MMCI_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;
		
		case MMCI_ID:
			value = 0xF041C022;
			break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void mmci_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_mmci_t *p = opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case GPIO_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
}

static const MemoryRegionOps io_ops = {
	.read			= mmci_io_read,
	.write			= mmci_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void mmci_handle_gpio_input(void *opaque, int id, int level) {
	// nothing
}

static void mmci_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_mmci_t *p = PMB887X_MMCI(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-mmci", MMCI_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	qdev_init_gpio_in_named(dev, mmci_handle_gpio_input, "DAT0_IN", 1);
	qdev_init_gpio_in_named(dev, mmci_handle_gpio_input, "DAT1_IN", 1);
	qdev_init_gpio_in_named(dev, mmci_handle_gpio_input, "CMD_IN", 1);
	qdev_init_gpio_in_named(dev, mmci_handle_gpio_input, "CLK_IN", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_dat0, "DAT0_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_dat1, "DAT1_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_cmd, "CMD_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_clk, "CLK_OUT", 1);
}

static void mmci_realize(DeviceState *dev, Error **errp) {
	pmb887x_mmci_t *p = PMB887X_MMCI(dev);
	pmb887x_clc_init(&p->clc);
}

static void mmci_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	dc->realize = mmci_realize;
}

static const TypeInfo mmci_info = {
    .name          	= TYPE_PMB887X_MMCI,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_mmci_t),
    .instance_init 	= mmci_init,
    .class_init    	= mmci_class_init,
};

static void mmci_register_types(void) {
	type_register_static(&mmci_info);
}
type_init(mmci_register_types)
