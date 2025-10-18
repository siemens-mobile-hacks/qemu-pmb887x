/*
 * Port Control Logic
 * */
#define PMB887X_TRACE_ID		PCL
#define PMB887X_TRACE_PREFIX	"pmb887x-pcl"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "exec/memory.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "cpu.h"

#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/gpio.h"
#include "hw/arm/pmb887x/io_bridge.h"

#define TYPE_PMB887X_PCL	"pmb887x-pcl"
#define PMB887X_PCL(obj)	OBJECT_CHECK(pmb887x_pcl_t, (obj), TYPE_PMB887X_PCL)

#define GPIOS_COUNT ((GPIO_PIN113 - GPIO_PIN0) / 4 + 1)

typedef struct pmb887x_pcl_t pmb887x_pcl_t;

struct pmb887x_pcl_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	pmb887x_clc_reg_t clc;
	uint32_t pins[GPIOS_COUNT];
	uint32_t mon_cr[4];
	
	bool input_state[8][GPIOS_COUNT];
	qemu_irq pins_out[8][GPIOS_COUNT];

	const char **names;
	uint32_t names_count;
};

static void pcl_update_state(pmb887x_pcl_t *p) {
	// TODO
}

static uint8_t pcl_get_mux_is(pmb887x_pcl_t *p, int pin) {
	if ((p->pins[pin] & GPIO_PS) == GPIO_PS_ALT && (p->pins[pin] & GPIO_IS) != GPIO_IS_NONE)
		return ((p->pins[pin] & GPIO_IS) >> GPIO_IS_SHIFT);
	return 0;
}

static uint8_t pcl_get_mux_os(pmb887x_pcl_t *p, int pin) {
	if ((p->pins[pin] & GPIO_PS) == GPIO_PS_ALT && (p->pins[pin] & GPIO_OS) != GPIO_OS_NONE)
		return ((p->pins[pin] & GPIO_OS) >> GPIO_OS_SHIFT);
	return 0;
}

static void pcl_sync_pin_state(pmb887x_pcl_t *p, int id) {
	if ((p->pins[id] & GPIO_PS) == GPIO_PS_MANUAL) {
		if ((p->pins[id] & GPIO_DIR) == GPIO_DIR_OUT)
			qemu_set_irq(p->pins_out[0][id], (p->pins[id] & GPIO_DATA) == GPIO_DATA_HIGH);
	} else {
		int pin_mux_is = pcl_get_mux_is(p, id);
		int pin_mux_os = pcl_get_mux_os(p, id);

		if (pin_mux_is != 0) {
			// proxy GPIO_IN -> PERIPHERAL
			qemu_set_irq(p->pins_out[pin_mux_is][id], p->input_state[0][id]);
		}

		if (pin_mux_os != 0) {
			// proxy PERIPHERAL -> GPIO_OUT
			qemu_set_irq(p->pins_out[0][id], p->input_state[pin_mux_os][id]);
		}
	}
}

static uint64_t pcl_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_pcl_t *p = opaque;
	
	uint64_t value = 0;

	#if PMB887X_IO_BRIDGE
	value = pmb8876_io_bridge_read(haddr + p->mmio.addr, size);
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	return value;
	#endif
	
	switch (haddr) {
		case GPIO_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;
		
		case GPIO_ID:
			value = 0xF023C032;
			break;
		
		case GPIO_PIN0 ... GPIO_PIN113: {
			uint32_t id = (haddr - GPIO_PIN0) / 4;
			value = p->pins[id];
			if ((value & GPIO_PS) == GPIO_PS_MANUAL && (value & GPIO_DIR) == GPIO_DIR_IN) {
				value &= ~GPIO_DATA;
				value |= (p->input_state[0][id] ? GPIO_DATA_HIGH : GPIO_DATA_LOW);
			} else if ((value & GPIO_PS) == GPIO_PS_ALT && (value & GPIO_OS) == GPIO_OS_NONE) {
				value &= ~GPIO_DATA;
				value |= (p->input_state[0][id] ? GPIO_DATA_HIGH : GPIO_DATA_LOW);
			}
			break;
		}
		
		case GPIO_MON_CR1 ... GPIO_MON_CR4:
			value = p->mon_cr[(haddr - GPIO_MON_CR1) / 4];
		break;
		
		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void pcl_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_pcl_t *p = opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);

	#if PMB887X_IO_BRIDGE
	pmb8876_io_bridge_write(haddr + p->mmio.addr, size, value);
	return;
	#endif
	
	switch (haddr) {
		case GPIO_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;
		
		case GPIO_PIN0 ... GPIO_PIN113:
		{
			uint32_t id = (haddr - GPIO_PIN0) / 4;
			p->pins[id] = value;
			pcl_sync_pin_state(p, id);
			break;
		}
		
		case GPIO_MON_CR1 ... GPIO_MON_CR4:
			p->mon_cr[(haddr - GPIO_MON_CR1) / 4] = value;
			break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
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

static void pcl_update_pin_state(pmb887x_pcl_t *p, int mux, int id) {
	int pin_mux_is = pcl_get_mux_is(p, id);
	int pin_mux_os = pcl_get_mux_os(p, id);
	if (mux == 0) {
		if (pin_mux_is != 0) {
			// proxy GPIO_IN -> PERIPHERAL
			qemu_set_irq(p->pins_out[pin_mux_is][id], p->input_state[0][id]);
		}
	} else {
		if (pin_mux_os != 0 && pin_mux_os == mux) {
			// proxy PERIPHERAL -> GPIO_OUT
			qemu_set_irq(p->pins_out[0][id], p->input_state[pin_mux_os][id]);
		}
	}
}

static void pcl_handle_gpio_change(pmb887x_pcl_t *p, int mux, int id, int level) {
	p->input_state[mux][id] = level;
	pcl_update_pin_state(p, mux, id);
}

static void pcl_input_handler(void *opaque, int id, int level) {
	pcl_handle_gpio_change(opaque, 0, id, level);
}

static void pcl_input_alt0_handler(void *opaque, int id, int level) {
	pcl_handle_gpio_change(opaque, 1, id, level);
}

static void pcl_input_alt1_handler(void *opaque, int id, int level) {
	pcl_handle_gpio_change(opaque, 2, id, level);
}

static void pcl_input_alt2_handler(void *opaque, int id, int level) {
	pcl_handle_gpio_change(opaque, 3, id, level);
}

static void pcl_input_alt3_handler(void *opaque, int id, int level) {
	pcl_handle_gpio_change(opaque, 4, id, level);
}

static void pcl_input_alt4_handler(void *opaque, int id, int level) {
	pcl_handle_gpio_change(opaque, 5, id, level);
}

static void pcl_input_alt5_handler(void *opaque, int id, int level) {
	pcl_handle_gpio_change(opaque, 6, id, level);
}

static void pcl_input_alt6_handler(void *opaque, int id, int level) {
	pcl_handle_gpio_change(opaque, 7, id, level);
}

static void pcl_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_pcl_t *p = PMB887X_PCL(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-pcl", GPIO_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	DPRINTF("gpio count: %d\n", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, pcl_input_handler, "pin_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[0], "pin_out", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, pcl_input_alt0_handler, "pin_alt0_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[1], "pin_alt0_out", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, pcl_input_alt1_handler, "pin_alt1_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[2], "pin_alt1_out", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, pcl_input_alt2_handler, "pin_alt2_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[3], "pin_alt2_out", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, pcl_input_alt3_handler, "pin_alt3_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[4], "pin_alt3_out", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, pcl_input_alt4_handler, "pin_alt4_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[5], "pin_alt4_out", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, pcl_input_alt5_handler, "pin_alt5_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[6], "pin_alt5_out", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, pcl_input_alt6_handler, "pin_alt6_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[7], "pin_alt6_out", GPIOS_COUNT);
}

static void pcl_realize(DeviceState *dev, Error **errp) {
	pmb887x_pcl_t *p = PMB887X_PCL(dev);
	pmb887x_clc_init(&p->clc);
	pcl_update_state(p);
}

static void pcl_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	dc->realize = pcl_realize;
}

static const TypeInfo pcl_info = {
    .name          	= TYPE_PMB887X_PCL,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_pcl_t),
    .instance_init 	= pcl_init,
    .class_init    	= pcl_class_init,
};

static void pcl_register_types(void) {
	type_register_static(&pcl_info);
}
type_init(pcl_register_types)
