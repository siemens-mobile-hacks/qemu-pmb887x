#include "hw/arm/pmb887x/board/gpio.h"

#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"

#include "hw/arm/pmb887x/utils/toml.h"
#include "hw/arm/pmb887x/utils/tomlc17.h"
#include "hw/core/hw-error.h"
#include "hw/core/irq.h"
#include "hw/core/split-irq.h"
#include "hw/ssi/ssi.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"

typedef struct pmb887x_gpio_output_t pmb887x_gpio_output_t;

struct pmb887x_gpio_output_t {
	DeviceState *dev;
	char *name;
	int n;
	GPtrArray *inputs;
};

static GPtrArray *gpio_outputs;

static void pmb887x_gpio_output_free(gpointer data) {
	pmb887x_gpio_output_t *output = data;
	g_free(output->name);
	g_ptr_array_free(output->inputs, true);
	g_free(output);
}

bool pmb887x_qdev_is_gpio_in_exists(DeviceState *dev, const char *name, int n) {
	if (n < 0)
		return false;
	NamedGPIOList *ngl;
	QLIST_FOREACH(ngl, &dev->gpios, node) {
		if (g_strcmp0(name, ngl->name) == 0)
			return n < ngl->num_in;
	}
	return false;
}

bool pmb887x_qdev_is_gpio_out_exists(DeviceState *dev, const char *name, int n) {
	char prop_name[64];
	sprintf(prop_name, "%s[%d]", name ? name : "unnamed-gpio-out", n);
	return object_property_find(OBJECT(dev), prop_name) != NULL;
}

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

static char *find_internal_gpio(bool is_out, const char *name, DeviceState **dev, int *id) {
	char **parts = g_strsplit(name, ":", 2);
	const char *dev_name = "GPIO";
	const char *pin_name = name;

	if (g_strv_length(parts) > 1) {
		dev_name = parts[0];
		pin_name = parts[1];
	}

	*dev = find_qdev(dev_name);
	if (!*dev)
		hw_error("Device not found: %s", dev_name);

	char *internal_name = g_new(char, 128);
	if (strcmp(dev_name, "GPIO") == 0) {
		*id = pmb887x_get_gpio_id_by_name(pin_name);
		if (*id < 0)
			hw_error("GPIO_%s '%s' not found in '%s'", is_out ? "OUT" : "IN", pin_name, dev_name);
		sprintf(internal_name, "pin_%s", is_out ? "out" : "in");
	} else {
		*id = 0;
		if (strcmp(pin_name, "CS") == 0) {
			sprintf(internal_name, "%s", SSI_GPIO_CS);
		} else {
			sprintf(internal_name, "%s_%s", pin_name, is_out ? "OUT" : "IN");
		}

		if (is_out) {
			if (!pmb887x_qdev_is_gpio_out_exists(*dev, internal_name, *id))
				hw_error("GPIO_OUT '%s' not found in '%s'", internal_name, dev_name);
		} else {
			if (!pmb887x_qdev_is_gpio_in_exists(*dev, internal_name, *id))
				hw_error("GPIO_IN '%s' not found in '%s'", internal_name, dev_name);
		}
	}

	g_strfreev(parts);

	return internal_name;
}

static const pmb887x_cpu_meta_gpio_t *find_cpu_gpio_by_name(const char *name) {
	pmb887x_board_t *board = pmb887x_board();
	const pmb887x_cpu_meta_t *cpu_info = pmb887x_get_cpu_meta(board->cpu);
	for (int i = 0; i < cpu_info->gpios_count; i++) {
		const pmb887x_cpu_meta_gpio_t *cpu_gpio = &cpu_info->gpios[i];
		if (strcmp(cpu_gpio->name, name) == 0 || strcmp(cpu_gpio->func_name, name) == 0)
			return cpu_gpio;
	}
	return NULL;
}

static pmb887x_cpu_meta_gpio_t *find_board_gpio_by_name(const char *name) {
	pmb887x_board_t *board = pmb887x_board();
	for (int i = 0; i < board->gpios_count; i++) {
		pmb887x_cpu_meta_gpio_t *board_gpio = &board->gpios[i];
		if (strcmp(board_gpio->name, name) == 0 || strcmp(board_gpio->func_name, name) == 0)
			return board_gpio;
	}
	return NULL;
}

int pmb887x_get_gpio_id_by_name(const char *name) {
	const pmb887x_cpu_meta_gpio_t *board_pin = find_board_gpio_by_name(name);
	if (board_pin)
		return board_pin->id;
	const pmb887x_cpu_meta_gpio_t *cpu_pin = find_cpu_gpio_by_name(name);
	if (cpu_pin) {
		pmb887x_board_t *board = pmb887x_board();
		return board->gpios[cpu_pin->id].id;
	}
	return -1;
}

void pmb887x_qdev_connect_gpio_out(DeviceState *dev, const char *name, int n, qemu_irq gpio_in) {
	if (!gpio_outputs)
		gpio_outputs = g_ptr_array_new_with_free_func(pmb887x_gpio_output_free);

	for (size_t i = 0; i < gpio_outputs->len; i++) {
		pmb887x_gpio_output_t *output = g_ptr_array_index(gpio_outputs, i);

		if (output->dev == dev && output->n == n && g_strcmp0(output->name, name) == 0) {
			g_ptr_array_add(output->inputs, gpio_in);
			return;
		}
	}

	pmb887x_gpio_output_t *output = g_new0(pmb887x_gpio_output_t, 1);
	output->dev = dev;
	output->name = g_strdup(name);
	output->n = n;
	output->inputs = g_ptr_array_new();
	g_ptr_array_add(output->inputs, gpio_in);
	g_ptr_array_add(gpio_outputs, output);
}

void pmb887x_qdev_connect_gpio_outputs(void) {
	if (!gpio_outputs)
		return;

	for (size_t i = 0; i < gpio_outputs->len; i++) {
		pmb887x_gpio_output_t *output = g_ptr_array_index(gpio_outputs, i);

		if (qdev_get_gpio_out_connector(output->dev, output->name, output->n))
			hw_error("GPIO_OUT '%s:%s[%d]' is already connected!", output->dev->id, output->name, output->n);

		if (output->inputs->len == 1) {
			qdev_connect_gpio_out_named(output->dev, output->name, output->n, g_ptr_array_index(output->inputs, 0));
			continue;
		}

		DeviceState *splitter = qdev_new(TYPE_SPLIT_IRQ);
		qdev_prop_set_uint16(splitter, "num-lines", output->inputs->len);
		qdev_realize_and_unref(splitter, NULL, &error_abort);
		for (size_t j = 0; j < output->inputs->len; j++)
			qdev_connect_gpio_out(splitter, j, g_ptr_array_index(output->inputs, j));
		qdev_connect_gpio_out_named(output->dev, output->name, output->n, qdev_get_gpio_in(splitter, 0));
	}

	g_ptr_array_free(gpio_outputs, true);
	gpio_outputs = NULL;
}

void pmb887x_gpio_connect(const char *gpio_out_name, const char *gpio_in_name) {
	int gpio_out_id;
	DeviceState *dev;
	qemu_irq gpio_in = pmb887x_gpio_get_input(gpio_in_name);
	char *gpio_out_internal_name = find_internal_gpio(true, gpio_out_name, &dev, &gpio_out_id);
	pmb887x_qdev_connect_gpio_out(dev, gpio_out_internal_name, gpio_out_id, gpio_in);
	g_free(gpio_out_internal_name);
}

qemu_irq pmb887x_gpio_get_input(const char *name) {
	int id;
	DeviceState *dev;
	char *internal_name = find_internal_gpio(false, name, &dev, &id);
	qemu_irq found_irq = qdev_get_gpio_in_named(dev, internal_name, id);
	g_free(internal_name);
	return found_irq;
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

void pmb887x_board_gpio_init_fixed_connections(void) {
	pmb887x_board_t *board = pmb887x_board();
	toml_datum_t table = toml_table_get(board->config, TOML_TABLE, "gpio.connections", false);
	if (table.type == TOML_UNKNOWN)
		return;
	for (size_t i = 0; i < table.u.tab.size; i++) {
		const char *value = toml_table_get_string(table, table.u.tab.key[i], false, true);
		pmb887x_gpio_connect(table.u.tab.key[i], value);
	}
}

static void _gpio_init_aliases(void) {
	pmb887x_board_t *board = pmb887x_board();
	toml_datum_t table = toml_table_get(board->config, TOML_TABLE, "gpio.aliases", false);
	if (table.type == TOML_UNKNOWN)
		return;
	for (size_t i = 0; i < table.u.tab.size; i++) {
		const char *key = table.u.tab.key[i];
		const char *value = toml_table_get_string(table, key, false, true);
		const pmb887x_cpu_meta_gpio_t *cpu_gpio = find_cpu_gpio_by_name(key);
		if (cpu_gpio) {
			pmb887x_cpu_meta_gpio_t *board_gpio = &board->gpios[cpu_gpio->id];
			board_gpio->name = g_strdup(cpu_gpio->name);
			board_gpio->func_name = g_strdup(value);
			board_gpio->full_name = g_strdup_printf("GPIO_PIN%d_%s", cpu_gpio->id, value);
		} else {
			warn_report("Unknown cpu GPIO '%s'", key);
		}
	}
}

void pmb887x_board_gpio_init(void) {
	pmb887x_board_t *board = pmb887x_board();
	const pmb887x_cpu_meta_t *cpu_info = pmb887x_get_cpu_meta(board->cpu);
	board->gpios_count = cpu_info->gpios_count;
	board->gpios = g_new0(pmb887x_cpu_meta_gpio_t, board->gpios_count);

	// Init default GPIO's
	for (int i = 0; i < cpu_info->gpios_count; i++) {
		const pmb887x_cpu_meta_gpio_t *cpu_gpio = &cpu_info->gpios[i];
		board->gpios[i].id = cpu_gpio->id;
		board->gpios[i].name = g_strdup(cpu_gpio->name);
		board->gpios[i].func_name = g_strdup(cpu_gpio->func_name);
		board->gpios[i].full_name = g_strdup(cpu_gpio->full_name);
	}

	// Set board-specific GPIO
	_gpio_init_aliases();
}
