#include "hw/arm/pmb887x/board/gpio.h"

#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"

#include "hw/arm/pmb887x/utils/toml.h"
#include "tomlc17.h"
#include "hw/core/hw-error.h"
#include "hw/core/irq.h"
#include "hw/core/split-irq.h"
#include "hw/core/qdev.h"
#include "qemu/error-report.h"

typedef struct pmb887x_qdev_search_result_t pmb887x_qdev_search_result_t;

struct pmb887x_qdev_search_result_t {
	const char *id;
	DeviceState *device;
};

static int find_qdev_by_id(Object *object, void *opaque) {
	pmb887x_qdev_search_result_t *result = opaque;
	DeviceState *device = (DeviceState *) object_dynamic_cast(object, TYPE_DEVICE);
	if (device && g_strcmp0(device->id, result->id) == 0) {
		result->device = device;
		return 1;
	}
	return 0;
}

static DeviceState *find_qdev(const char *id) {
	DeviceState *device = qdev_find_recursive(sysbus_get_default(), id);
	if (device)
		return device;

	pmb887x_qdev_search_result_t result = { .id = id };
	object_child_foreach_recursive(object_get_root(), find_qdev_by_id, &result);
	return result.device;
}

static const pmb887x_cpu_meta_gpio_t *find_cpu_gpio(const char *name) {
	const pmb887x_cpu_meta_t *cpu_info = pmb887x_get_cpu_meta(pmb887x_board()->cpu);
	for (int i = 0; i < cpu_info->gpios_count; i++) {
		const pmb887x_cpu_meta_gpio_t *cpu_gpio = &cpu_info->gpios[i];
		if (strcmp(cpu_gpio->name, name) == 0 || strcmp(cpu_gpio->func_name, name) == 0)
			return cpu_gpio;
	}
	return NULL;
}

static int find_gpio_pin(const char *name) {
	const pmb887x_board_t *board = pmb887x_board();
	const pmb887x_cpu_meta_t *cpu_info = pmb887x_get_cpu_meta(board->cpu);
	for (int i = 0; i < cpu_info->gpios_count; i++) {
		if (strcmp(cpu_info->gpios[i].name, name) == 0 || strcmp(board->gpio_names[i], name) == 0)
			return cpu_info->gpios[i].id;
	}

	const pmb887x_cpu_meta_gpio_t *cpu_gpio = find_cpu_gpio(name);
	return cpu_gpio ? cpu_gpio->id : -1;
}

static DeviceState *find_gpio_line(const char *spec, const char **name, int *n) {
	const char *separator = strchr(spec, ':');
	if (!separator) {
		*name = "pin";
		*n = find_gpio_pin(spec);
		if (*n < 0)
			hw_error("GPIO pin '%s' not found", spec);
		return find_qdev("GPIO");
	}

	g_autofree char *dev_id = g_strndup(spec, separator - spec);
	DeviceState *dev = find_qdev(dev_id);
	if (!dev)
		hw_error("Device not found: %s", dev_id);
	*name = separator + 1;
	*n = 0;
	return dev;
}

qemu_irq pmb887x_qdev_get_gpio_in(DeviceState *dev, const char *name, int n) {
	if (!qdev_has_gpio_in_named(dev, name, n))
		hw_error("GPIO input '%s[%d]' not found in '%s'", name, n, object_get_typename(OBJECT(dev)));
	return qdev_get_gpio_in_named(dev, name, n);
}

void pmb887x_qdev_connect_gpio_out(DeviceState *dev, const char *name, int n, qemu_irq gpio_in) {
	if (!qdev_has_gpio_out_named(dev, name, n))
		hw_error("GPIO output '%s[%d]' not found in '%s'", name, n, object_get_typename(OBJECT(dev)));
	split_irq_connect_gpio_out(dev, name, n, gpio_in);
}

qemu_irq pmb887x_gpio_get_input(const char *spec) {
	const char *name;
	int n;
	DeviceState *dev = find_gpio_line(spec, &name, &n);
	return pmb887x_qdev_get_gpio_in(dev, name, n);
}

void pmb887x_gpio_connect(const char *output_spec, const char *input_spec) {
	qemu_irq gpio_in = pmb887x_gpio_get_input(input_spec);
	const char *name;
	int n;
	DeviceState *dev = find_gpio_line(output_spec, &name, &n);
	pmb887x_qdev_connect_gpio_out(dev, name, n, gpio_in);
}

void pmb887x_board_gpio_init_fixed_inputs(void) {
	pmb887x_board_t *board = pmb887x_board();
	toml_datum_t table = toml_table_get(board->config, TOML_TABLE, "gpio.inputs", false);
	if (table.type == TOML_UNKNOWN)
		return;
	for (size_t i = 0; i < table.u.tab.size; i++) {
		uint32_t gpio_value = toml_table_get_uint32(table, table.u.tab.key[i], false, true);
		qemu_set_irq(pmb887x_gpio_get_input(table.u.tab.key[i]), gpio_value);
	}
}

void pmb887x_board_gpio_init(void) {
	pmb887x_board_t *board = pmb887x_board();
	const pmb887x_cpu_meta_t *cpu_info = pmb887x_get_cpu_meta(board->cpu);
	board->gpio_names = g_new(const char *, cpu_info->gpios_count);
	for (int i = 0; i < cpu_info->gpios_count; i++)
		board->gpio_names[i] = cpu_info->gpios[i].func_name;

	toml_datum_t aliases = toml_table_get(board->config, TOML_TABLE, "gpio.aliases", false);
	if (aliases.type == TOML_UNKNOWN)
		return;
	for (size_t i = 0; i < aliases.u.tab.size; i++) {
		const char *key = aliases.u.tab.key[i];
		const pmb887x_cpu_meta_gpio_t *cpu_gpio = find_cpu_gpio(key);
		if (!cpu_gpio) {
			warn_report("Unknown cpu GPIO '%s'", key);
			continue;
		}
		board->gpio_names[cpu_gpio->id] = toml_table_get_string(aliases, key, NULL, true);
	}
}
