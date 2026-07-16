/*
 * sci-worx USB Device Controller
 */
#define PMB887X_TRACE_ID USB
#define PMB887X_TRACE_PREFIX "pmb887x-usb"

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_USB "pmb887x-usb"
#define PMB887X_USB(obj) OBJECT_CHECK(pmb887x_usb_t, (obj), TYPE_PMB887X_USB)

#define USB_ID_BASE 0xF047C000
#define USB_PMB8876_REVISION 0x12
#define USB_ENDPOINT_COUNT 11
#define USB_ENDPOINT_CONFIG_STRIDE (USB_EP_CONFIG1 - USB_EP_CONFIG0)
#define USB_ENDPOINT_STRIDE (USB_EP_DATA1 - USB_EP_DATA0)

typedef struct pmb887x_usb_t pmb887x_usb_t;

struct pmb887x_usb_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	uint32_t revision;

	qemu_irq irq;
	pmb887x_clc_reg_t clc;
	uint8_t registers[USB_IO_SIZE];
};

static uint64_t usb_register_read(pmb887x_usb_t *p, hwaddr haddr, unsigned int size) {
	uint64_t value = 0;

	for (uint32_t index = 0; index < size; index++)
		value |= (uint64_t) p->registers[haddr + index] << (index * CHAR_BIT);
	return value;
}

static void usb_register_write(pmb887x_usb_t *p, hwaddr haddr, uint64_t value, unsigned int size) {
	for (uint32_t index = 0; index < size; index++)
		p->registers[haddr + index] = (uint8_t) (value >> (index * CHAR_BIT));
}

static void usb_reset_internal_state(pmb887x_usb_t *p) {
	static const uint8_t endpoint_config_reset[USB_ENDPOINT_COUNT] = {
		0x2C, 0x28, 0x20, 0x28, 0x20, 0x28, 0x20, 0x28, 0x20, 0x28, 0x20,
	};
	static const uint8_t endpoint_count_low_reset[USB_ENDPOINT_COUNT] = {
		0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	static const uint8_t endpoint_count_high_reset[USB_ENDPOINT_COUNT] = {
		0x50, 0x50, 0xA0, 0x50, 0xA0, 0x50, 0xA0, 0x50, 0xA0, 0x50, 0xA0,
	};

	memset(p->registers, 0, sizeof(p->registers));
	if (p->revision != USB_PMB8876_REVISION)
		return;

	p->registers[USB_CONTROL] = USB_CONTROL_ENABLE;
	p->registers[USB_EP0_STATUS] = 0x24;
	p->registers[USB_EVENT_INT_STATUS] = BIT(3);
	for (uint32_t endpoint = 0; endpoint < USB_ENDPOINT_COUNT; endpoint++) {
		p->registers[USB_EP_CONFIG0 + endpoint * USB_ENDPOINT_CONFIG_STRIDE] = endpoint_config_reset[endpoint];
		p->registers[USB_EP_COUNT_LOW0 + endpoint * USB_ENDPOINT_STRIDE] = endpoint_count_low_reset[endpoint];
		p->registers[USB_EP_COUNT_HIGH0 + endpoint * USB_ENDPOINT_STRIDE] = endpoint_count_high_reset[endpoint];
	}
}

static void usb_reset_input(void *opaque, int id, int level) {
	if (level)
		usb_reset_internal_state(opaque);
}

static uint64_t usb_io_read(void *opaque, hwaddr haddr, unsigned int size) {
	pmb887x_usb_t *p = opaque;
	uint64_t value;

	switch (haddr) {
		case USB_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case USB_ID:
			value = USB_ID_BASE | p->revision;
			break;

		default:
			value = usb_register_read(p, haddr, size);
			break;
	}

	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	return value;
}

static void usb_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned int size) {
	pmb887x_usb_t *p = opaque;

	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	if (haddr == USB_CLC) {
		pmb887x_clc_set(&p->clc, value);
		return;
	}
	if (haddr != USB_ID)
		usb_register_write(p, haddr, value, size);
}

static const MemoryRegionOps usb_io_ops = {
	.read = usb_io_read,
	.write = usb_io_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = {
		.min_access_size = 1,
		.max_access_size = 4,
		.unaligned = true,
	},
	.impl = {
		.min_access_size = 1,
		.max_access_size = 4,
		.unaligned = true,
	},
};

static void usb_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_usb_t *p = PMB887X_USB(obj);

	memory_region_init_io(&p->mmio, obj, &usb_io_ops, p, TYPE_PMB887X_USB, USB_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq);
	qdev_init_gpio_in_named(dev, usb_reset_input, "RESET_IN", 1);
}

static void usb_reset(DeviceState *dev) {
	pmb887x_usb_t *p = PMB887X_USB(dev);

	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	usb_reset_internal_state(p);
	qemu_set_irq(p->irq, 0);
}

static void usb_realize(DeviceState *dev, Error **errp) {
	usb_reset(dev);
}

static const Property usb_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_usb_t, revision, 0),
};

static void usb_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);

	device_class_set_props(dc, usb_properties);
	device_class_set_legacy_reset(dc, usb_reset);
	dc->realize = usb_realize;
}

static const TypeInfo usb_info = {
	.name = TYPE_PMB887X_USB,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(pmb887x_usb_t),
	.instance_init = usb_init,
	.class_init = usb_class_init,
};

static void usb_register_types(void) {
	type_register_static(&usb_info);
}

type_init(usb_register_types)
