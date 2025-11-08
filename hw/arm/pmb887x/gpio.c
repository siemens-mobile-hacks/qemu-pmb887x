/*
 * Port Control Logic
 * */
#define PMB887X_TRACE_ID		GPIO
#define PMB887X_TRACE_PREFIX	"pmb887x-gpio"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "system/memory.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "cpu.h"

#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"

#define TYPE_PMB887X_GPIO	"pmb887x-gpio"
#define PMB887X_GPIO(obj)	OBJECT_CHECK(pmb887x_gpio_t, (obj), TYPE_PMB887X_GPIO)

#define GPIOS_COUNT ((GPIO_PIN113 - GPIO_PIN0) / 4 + 1)

typedef struct pmb887x_gpio_t pmb887x_gpio_t;

struct pmb887x_gpio_t {
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

static void gpio_update_state(pmb887x_gpio_t *p) {
	// TODO
}

static uint8_t gpio_get_mux_is(pmb887x_gpio_t *p, int pin) {
	if ((p->pins[pin] & GPIO_PS) == GPIO_PS_ALT && (p->pins[pin] & GPIO_IS) != GPIO_IS_NONE)
		return ((p->pins[pin] & GPIO_IS) >> GPIO_IS_SHIFT);
	return 0;
}

static uint8_t gpio_get_mux_os(pmb887x_gpio_t *p, int pin) {
	if ((p->pins[pin] & GPIO_PS) == GPIO_PS_ALT && (p->pins[pin] & GPIO_OS) != GPIO_OS_NONE)
		return ((p->pins[pin] & GPIO_OS) >> GPIO_OS_SHIFT);
	return 0;
}

static void gpio_sync_pin_state(pmb887x_gpio_t *p, int id) {
	if ((p->pins[id] & GPIO_PS) == GPIO_PS_MANUAL) {
		if ((p->pins[id] & GPIO_DIR) == GPIO_DIR_OUT)
			qemu_set_irq(p->pins_out[0][id], (p->pins[id] & GPIO_DATA) == GPIO_DATA_HIGH);
	} else {
		int pin_mux_is = gpio_get_mux_is(p, id);
		int pin_mux_os = gpio_get_mux_os(p, id);

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

static uint64_t gpio_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_gpio_t *p = opaque;
	
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

static void gpio_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_gpio_t *p = opaque;
	
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
			gpio_sync_pin_state(p, id);
			break;
		}
		
		case GPIO_MON_CR1 ... GPIO_MON_CR4:
			p->mon_cr[(haddr - GPIO_MON_CR1) / 4] = value;
			break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	gpio_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= gpio_io_read,
	.write			= gpio_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void gpio_update_pin_state(pmb887x_gpio_t *p, int mux, int id) {
	int pin_mux_is = gpio_get_mux_is(p, id);
	int pin_mux_os = gpio_get_mux_os(p, id);
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

static void gpio_handle_gpio_change(pmb887x_gpio_t *p, int mux, int id, int level) {
	p->input_state[mux][id] = level;
	gpio_update_pin_state(p, mux, id);
}

static void gpio_input_handler(void *opaque, int id, int level) {
	gpio_handle_gpio_change(opaque, 0, id, level);
}

static void gpio_input_alt0_handler(void *opaque, int id, int level) {
	gpio_handle_gpio_change(opaque, 1, id, level);
}

static void gpio_input_alt1_handler(void *opaque, int id, int level) {
	gpio_handle_gpio_change(opaque, 2, id, level);
}

static void gpio_input_alt2_handler(void *opaque, int id, int level) {
	gpio_handle_gpio_change(opaque, 3, id, level);
}

static void gpio_input_alt3_handler(void *opaque, int id, int level) {
	gpio_handle_gpio_change(opaque, 4, id, level);
}

static void gpio_input_alt4_handler(void *opaque, int id, int level) {
	gpio_handle_gpio_change(opaque, 5, id, level);
}

static void gpio_input_alt5_handler(void *opaque, int id, int level) {
	gpio_handle_gpio_change(opaque, 6, id, level);
}

static void gpio_input_alt6_handler(void *opaque, int id, int level) {
	gpio_handle_gpio_change(opaque, 7, id, level);
}

static void gpio_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_gpio_t *p = PMB887X_GPIO(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-gpio", GPIO_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	DPRINTF("gpio count: %d\n", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, gpio_input_handler, "pin_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[0], "pin_out", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, gpio_input_alt0_handler, "pin_alt0_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[1], "pin_alt0_out", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, gpio_input_alt1_handler, "pin_alt1_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[2], "pin_alt1_out", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, gpio_input_alt2_handler, "pin_alt2_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[3], "pin_alt2_out", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, gpio_input_alt3_handler, "pin_alt3_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[4], "pin_alt3_out", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, gpio_input_alt4_handler, "pin_alt4_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[5], "pin_alt4_out", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, gpio_input_alt5_handler, "pin_alt5_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[6], "pin_alt5_out", GPIOS_COUNT);

	qdev_init_gpio_in_named(dev, gpio_input_alt6_handler, "pin_alt6_in", GPIOS_COUNT);
	qdev_init_gpio_out_named(dev, p->pins_out[7], "pin_alt6_out", GPIOS_COUNT);
}

static void gpio_realize(DeviceState *dev, Error **errp) {
	pmb887x_gpio_t *p = PMB887X_GPIO(dev);
	pmb887x_clc_init(&p->clc);
	gpio_update_state(p);
}

static void gpio_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	dc->realize = gpio_realize;
}

static const TypeInfo gpio_info = {
    .name          	= TYPE_PMB887X_GPIO,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_gpio_t),
    .instance_init 	= gpio_init,
    .class_init    	= gpio_class_init,
};

static void gpio_register_types(void) {
	type_register_static(&gpio_info);
}
type_init(gpio_register_types)
