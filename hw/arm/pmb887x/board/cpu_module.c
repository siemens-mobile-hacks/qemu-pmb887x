#include "hw/arm/pmb887x/board/cpu_module.h"

#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/board/gpio.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/gen/cpu_modules.h"

#include "hw/arm/pmb887x/utils/strings.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "qemu/error-report.h"
#include "qom/object.h"
#include "qapi/error.h"

static const struct pmb887x_cpu_module_t *get_cpu_module_definition(const char *name) {
	int i = 0;
	const pmb887x_cpu_module_t *modules = pmb887x_cpu_get_modules_list(pmb887x_board()->cpu);
	while (modules[i].name[0]) {
		const pmb887x_cpu_module_t *mod = &modules[i];
		if (strcmp(name, mod->name) == 0)
			return mod;
		i++;
	}
	hw_error("Can't find CPU module: %s\n", name);
}

DeviceState *pmb887x_new_cpu_module(const char *name) {
	const pmb887x_cpu_module_t *mod = get_cpu_module_definition(name);

	DeviceState *dev = qdev_new(mod->dev);
	dev->id = g_strdup(name);

	if (object_property_find(OBJECT(dev), "cpu_type"))
		qdev_prop_set_uint32(dev, "cpu_type", pmb887x_board()->cpu);

	sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, mod->base);

	if (mod->irqs_count > 0) {
		DeviceState *vic = qdev_find_recursive(sysbus_get_default(), "VIC");
		g_assert(vic != NULL);

		for (int i = 0; i < mod->irqs_count; i++)
			sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, qdev_get_gpio_in(vic, mod->irqs[i]));
	}

	return dev;
}

static void pmb887x_cpu_module_post_init(DeviceState *dev, const pmb887x_cpu_module_t * mod) {
	if (mod->gpios_count > 0) {
		DeviceState *gpio = qdev_find_recursive(sysbus_get_default(), "GPIO");
		g_assert(gpio != NULL);

		for (int i = 0; i < mod->gpios_count; i++) {
			const pmb887x_cpu_module_gpio_t *gpio_link = &mod->gpios[i];

			// PERIPHERAL -> GPIO_OUT
			if (str_ends_with(gpio_link->name, "_OUT")) {
				char gpio_in_name[64];
				sprintf(gpio_in_name, "pin_alt%d_in", gpio_link->alt);

				if (!pmb887x_qdev_is_gpio_in_exists(gpio, gpio_in_name, gpio_link->pin))
					hw_error("GPIO_IN '%s[%d]' not found in '%s'", gpio_in_name, gpio_link->pin, object_get_typename(OBJECT(gpio)));

				if (!pmb887x_qdev_is_gpio_out_exists(dev, gpio_link->name, 0))
					hw_error("GPIO_OUT '%s[%d]' not found in '%s'", gpio_link->name, 0, object_get_typename(OBJECT(dev)));

				qemu_irq gpio_in = qdev_get_gpio_in_named(gpio, gpio_in_name, gpio_link->pin);
				if (!object_get_canonical_path(OBJECT(gpio_in)))
					hw_error("GPIO_IN '%s[%d]' is not attached to dev!", gpio_in_name, gpio_link->pin);
				if (qdev_get_gpio_out_connector(dev, gpio_link->name, 0) != NULL)
					hw_error("GPIO_OUT '%s:%s[0]' is already connected!", dev->id, gpio_link->name);
				qdev_connect_gpio_out_named(dev, gpio_link->name, 0, gpio_in);
			}

			// GPIO_IN -> PERIPHERAL
			if (str_ends_with(gpio_link->name, "_IN")) {
				char gpio_out_name[64];
				sprintf(gpio_out_name, "pin_alt%d_out", gpio_link->alt);

				if (!pmb887x_qdev_is_gpio_in_exists(dev, gpio_link->name, 0))
					hw_error("GPIO_IN '%s[%d]' not found in '%s'", gpio_link->name, 0, object_get_typename(OBJECT(dev)));

				if (!pmb887x_qdev_is_gpio_out_exists(gpio, gpio_out_name, gpio_link->pin))
					hw_error("GPIO_OUT '%s[%d]' not found in '%s'", gpio_out_name, gpio_link->pin, object_get_typename(OBJECT(gpio)));

				qemu_irq gpio_in = qdev_get_gpio_in_named(dev, gpio_link->name, 0);
				if (!object_get_canonical_path(OBJECT(gpio_in)))
					hw_error("GPIO_IN '%s:%s[0]' is not attached to dev!", dev->id, gpio_link->name);
				if (qdev_get_gpio_out_connector(dev, gpio_out_name, gpio_link->pin) != NULL)
					hw_error("GPIO_OUT '%s[%d]' is already connected!", gpio_out_name, gpio_link->pin);
				qdev_connect_gpio_out_named(gpio, gpio_out_name, gpio_link->pin, gpio_in);
			}
		}
	}
}

void pmb887x_cpu_modules_post_init(void) {
	int i = 0;
	const pmb887x_cpu_module_t *modules = pmb887x_cpu_get_modules_list(pmb887x_board()->cpu);
	while (modules[i].name[0]) {
		const pmb887x_cpu_module_t *mod = &modules[i];
		DeviceState *dev = qdev_find_recursive(sysbus_get_default(), mod->name);
		if (dev)
			pmb887x_cpu_module_post_init(dev, mod);
		i++;
	}
}
