#include "hw/arm/pmb887x/board/cpu_module.h"

#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/board/gpio.h"
#include "hw/arm/pmb887x/gen/cpu_modules.h"

#include "hw/arm/pmb887x/utils/strings.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "qom/object.h"

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

	sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, mod->base);

	if (mod->irqs_count > 0) {
		DeviceState *vic = qdev_find_recursive(sysbus_get_default(), "VIC");
		g_assert(vic != NULL);

		for (int i = 0; i < mod->irqs_count; i++)
			sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, qdev_get_gpio_in(vic, mod->irqs[i]));
	}

	return dev;
}

static void pmb887x_cpu_module_post_init(DeviceState *dev, const pmb887x_cpu_module_t *mod) {
	if (mod->gpios_count > 0) {
		DeviceState *gpio = qdev_find_recursive(sysbus_get_default(), "GPIO");
		g_assert(gpio != NULL);

		// GPIO
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

	if (mod->dma_count > 0) {
		DeviceState *dmac = qdev_find_recursive(sysbus_get_default(), "DMAC");
		g_assert(dmac != NULL);

		const char *module_signals[] = { "SREQ", "LSREQ", "BREQ", "LBREQ" };
		const char *dmac_signals[] = { "CLR", "TC" };

		// DMA signals
		for (int i = 0; i < mod->dma_count; i++) {
			const pmb887x_cpu_module_dma_t *dma_channel = &mod->dma[i];

			for (int j = 0; j < ARRAY_SIZE(module_signals); j++) {
				char module_signal_out[128];
				sprintf(module_signal_out, "DMAC_%s_%s", dma_channel->channel, module_signals[j]);

				if (!pmb887x_qdev_is_gpio_out_exists(dev, module_signal_out, 0))
					continue;

				char dmac_signal_in[128];
				sprintf(dmac_signal_in, "SEL%d_%s", dma_channel->sel, module_signals[j]);

				qemu_irq signal = qdev_get_gpio_in_named(dmac, dmac_signal_in, dma_channel->request);
				qdev_connect_gpio_out_named(dev, module_signal_out, 0, signal);
			}

			for (int j = 0; j < ARRAY_SIZE(dmac_signals); j++) {
				char module_signal_in[128];
				sprintf(module_signal_in, "DMAC_%s_%s", dma_channel->channel, dmac_signals[j]);

				if (!pmb887x_qdev_is_gpio_out_exists(dev, module_signal_in, 0))
					continue;

				char dmac_signal_out[128];
				sprintf(dmac_signal_out, "SEL%d_%s", dma_channel->sel, dmac_signals[j]);

				qemu_irq signal = qdev_get_gpio_in_named(dev, module_signal_in, 0);
				qdev_connect_gpio_out_named(dmac, dmac_signal_out, dma_channel->request, signal);
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
