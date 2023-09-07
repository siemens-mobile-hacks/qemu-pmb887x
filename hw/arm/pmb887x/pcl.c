/*
 * Port Control Logic
 * */
#define PMB887X_TRACE_ID		PCL
#define PMB887X_TRACE_PREFIX	"pmb887x-pcl"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/ptimer.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/pll.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "cpu.h"

#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_PCL	"pmb887x-pcl"
#define PMB887X_PCL(obj)	OBJECT_CHECK(struct pmb887x_pcl_t, (obj), TYPE_PMB887X_PCL)

#define GPIOS_COUNT ((GPIO_PIN113 - GPIO_PIN0) / 4 + 1)

struct pmb887x_pcl_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	pmb887x_clc_reg_t clc;
	uint32_t pins[GPIOS_COUNT];
	uint32_t mon_cr[4];
	
	bool pins_input_state[GPIOS_COUNT];
	qemu_irq pins_out[GPIOS_COUNT];
};

static void pcl_update_state(struct pmb887x_pcl_t *p) {
	// TODO
}

static uint64_t pcl_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb887x_pcl_t *p = (struct pmb887x_pcl_t *) opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case GPIO_CLC:
			value = pmb887x_clc_get(&p->clc);
		break;
		
		case GPIO_ID:
			value = 0xF023C032;
		break;
		
		case GPIO_PIN0 ... GPIO_PIN113:
		{
			uint32_t id = (haddr - GPIO_PIN0) / 4;
			value = p->pins[id];
			
			if ((value & GPIO_DIR) == GPIO_DIR_IN) {
				value &= ~GPIO_DATA;
				value |= (p->pins_input_state[id] ? GPIO_DATA_HIGH : GPIO_DATA_LOW);
			}
		}
		break;
		
		case GPIO_MON_CR1 ... GPIO_MON_CR4:
			value = p->mon_cr[(haddr - GPIO_MON_CR1) / 4];
		break;
		
		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void pcl_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct pmb887x_pcl_t *p = (struct pmb887x_pcl_t *) opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case GPIO_CLC:
			pmb887x_clc_set(&p->clc, value);
		break;
		
		case GPIO_PIN0 ... GPIO_PIN113:
		{
			uint32_t id = (haddr - GPIO_PIN0) / 4;
			p->pins[id] = value;
			
			if ((value & GPIO_DIR) == GPIO_DIR_OUT)
				qemu_set_irq(p->pins_out[id], (value & GPIO_DATA) == GPIO_DATA_HIGH);
		}
		break;
		
		case GPIO_MON_CR1 ... GPIO_MON_CR4:
			p->mon_cr[(haddr - GPIO_MON_CR1) / 4] = value;
		break;
		
		default:
			EPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	pcl_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= pcl_io_read,
	.write			= pcl_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void pcl_input_handler(void *opaque, int irq, int level) {
	struct pmb887x_pcl_t *p = (struct pmb887x_pcl_t *) opaque;
	p->pins_input_state[irq] = level;
}

static void pcl_init(Object *obj) {
	struct pmb887x_pcl_t *p = PMB887X_PCL(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-pcl", GPIO_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	DPRINTF("gpio count: %d\n", GPIOS_COUNT);
}

static void pcl_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_pcl_t *p = PMB887X_PCL(dev);
	
	pmb887x_clc_init(&p->clc);
	
	qdev_init_gpio_in(dev, pcl_input_handler, GPIOS_COUNT);
	qdev_init_gpio_out(dev, p->pins_out, GPIOS_COUNT);
	
	pcl_update_state(p);
}

static Property pcl_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void pcl_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, pcl_properties);
	dc->realize = pcl_realize;
}

static const TypeInfo pcl_info = {
    .name          	= TYPE_PMB887X_PCL,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_pcl_t),
    .instance_init 	= pcl_init,
    .class_init    	= pcl_class_init,
};

static void pcl_register_types(void) {
	type_register_static(&pcl_info);
}
type_init(pcl_register_types)
