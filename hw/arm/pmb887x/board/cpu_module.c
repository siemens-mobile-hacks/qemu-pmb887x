#include "hw/arm/pmb887x/board/cpu_module.h"

#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/board/timings/bus.h"
#include "hw/arm/pmb887x/board/gpio.h"
#include "hw/arm/pmb887x/gen/cpu_modules.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"

#include "hw/core/hw-error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"

static const struct pmb887x_cpu_module_t *get_cpu_module_definition(const char *name) {
	const pmb887x_cpu_t *cpu = pmb887x_cpu_get(pmb887x_board()->cpu);

	for (size_t i = 0; i < cpu->modules_count; i++) {
		const pmb887x_cpu_module_t *mod = &cpu->modules[i];
		if (strcmp(name, mod->name) == 0)
			return mod;
	}
	hw_error("Can't find CPU module: %s\n", name);
}

DeviceState *pmb887x_new_cpu_module(const char *name) {
	const pmb887x_cpu_module_t *mod = get_cpu_module_definition(name);

	DeviceState *dev = qdev_new(mod->dev);
	dev->id = g_strdup(name);
	qdev_prop_set_uint32(dev, "revision", mod->id & MOD_ID_REV);
	if (object_property_find(OBJECT(dev), "peripheral-id"))
		qdev_prop_set_uint32(dev, "peripheral-id", mod->id);

	sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, mod->base);
	if (mod->bus[0])
		pmb887x_bus_add_region(mod->bus, sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));

	if (mod->clock[0]) {
		DeviceState *cgu = qdev_find_recursive(sysbus_get_default(), "CGU");
		g_assert(cgu != NULL);
		qdev_connect_clock_in(dev, "clk", qdev_get_clock_out(cgu, mod->clock));
	}

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

		for (int i = 0; i < mod->gpios_count; i++) {
			const pmb887x_cpu_module_gpio_t *gpio_link = &mod->gpios[i];
			char alt_name[16];
			sprintf(alt_name, "pin_alt%d", gpio_link->alt);

			if (gpio_link->directions & PMB887X_GPIO_DIR_OUT) {
				qemu_irq gpio_in = pmb887x_qdev_get_gpio_in(gpio, alt_name, gpio_link->pin);
				pmb887x_qdev_connect_gpio_out(dev, gpio_link->name, 0, gpio_in);
			}

			if (gpio_link->directions & PMB887X_GPIO_DIR_IN) {
				qemu_irq dev_in = pmb887x_qdev_get_gpio_in(dev, gpio_link->name, 0);
				pmb887x_qdev_connect_gpio_out(gpio, alt_name, gpio_link->pin, dev_in);
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

				if (!qdev_has_gpio_out_named(dev, module_signal_out, 0))
					continue;

				char dmac_signal_in[128];
				sprintf(dmac_signal_in, "SEL%d_%s", dma_channel->sel, module_signals[j]);

				qemu_irq signal = qdev_get_gpio_in_named(dmac, dmac_signal_in, dma_channel->request);
				qdev_connect_gpio_out_named(dev, module_signal_out, 0, signal);
			}

			for (int j = 0; j < ARRAY_SIZE(dmac_signals); j++) {
				char module_signal_in[128];
				sprintf(module_signal_in, "DMAC_%s_%s", dma_channel->channel, dmac_signals[j]);

				if (!qdev_has_gpio_in_named(dev, module_signal_in, 0))
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
	const pmb887x_cpu_t *cpu = pmb887x_cpu_get(pmb887x_board()->cpu);

	for (size_t i = 0; i < cpu->modules_count; i++) {
		const pmb887x_cpu_module_t *mod = &cpu->modules[i];
		DeviceState *dev = qdev_find_recursive(sysbus_get_default(), mod->name);
		if (dev)
			pmb887x_cpu_module_post_init(dev, mod);
	}
}
