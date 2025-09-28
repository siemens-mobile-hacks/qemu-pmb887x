#include "hw/arm/pmb887x/board/cpu_module.h"

#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/gen/cpu_modules.h"

#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "qemu/error-report.h"
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

	if (mod->irqs[0]) {
		DeviceState *vic = qdev_find_recursive(sysbus_get_default(), "VIC");
		g_assert(vic != NULL);

		int irq_n = 0;
		while (mod->irqs[irq_n]) {
			sysbus_connect_irq(SYS_BUS_DEVICE(dev), irq_n, qdev_get_gpio_in(vic, mod->irqs[irq_n]));
			irq_n++;
		}
	}

	if (object_property_find(OBJECT(dev), "cpu_type"))
		qdev_prop_set_uint32(dev, "cpu_type", pmb887x_board()->cpu);

	sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, mod->base);

	return dev;
}
