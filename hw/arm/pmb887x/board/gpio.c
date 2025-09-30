#include "hw/arm/pmb887x/board/gpio.h"

#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"
#include "hw/arm/pmb887x/utils/config.h"

#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/qdev-core.h"
#include "qemu/error-report.h"

bool pmb887x_qdev_is_gpio_exists(DeviceState *dev, const char *name, int n) {
	if (n < 0)
		return false;
	NamedGPIOList *ngl;
	QLIST_FOREACH(ngl, &dev->gpios, node) {
		if (g_strcmp0(name, ngl->name) == 0)
			return n < ngl->num_in;
	}
	return false;
}

static char *find_internal_gpio(bool is_out, const char *name, DeviceState **dev, int *id) {
	char **parts = g_strsplit(name, ":", 2);
	const char *dev_name = "GPIO";
	const char *pin_name = name;

	if (g_strv_length(parts) > 1) {
		dev_name = parts[0];
		pin_name = parts[1];
	}

	*dev = qdev_find_recursive(sysbus_get_default(), dev_name);
	if (!*dev)
		hw_error("Device not found: %s", dev_name);

	char *internal_name;
	if (strcmp(dev_name, "GPIO") == 0) {
		*id = pmb887x_get_gpio_id_by_name(pin_name);
		if (*id < 0)
			hw_error("GPIO %s not found in device %s.", pin_name, dev_name);
		internal_name = is_out ? g_strdup("pin_out") : g_strdup("pin_in");
	} else {
		internal_name = g_strdup(pin_name);
	}

	char *prop_name = g_strdup_printf("%s[%d]", internal_name, *id);
	if (!object_property_find(OBJECT(*dev), prop_name))
		hw_error("GPIO %s not found in device %s.", pin_name, dev_name);
	g_free(prop_name);
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

void pmb887x_gpio_connect(const char *gpio_out_name, qemu_irq gpio_in) {
	int id;
	DeviceState *dev;
	char *internal_name = find_internal_gpio(true, gpio_out_name, &dev, &id);
	qdev_connect_gpio_out_named(dev, internal_name, id, gpio_in);
	g_free(internal_name);
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
	pmb887x_cfg_section_t *section = pmb887x_cfg_section(board->config, "gpio-inputs", 0, true);
	for (size_t i = 0; i < section->items_count; i++) {
		pmb887x_cfg_item_t *item = &section->items[i];
		uint32_t gpio_value = strtoll(item->value, NULL, 10) ? 1 : 0;
		qemu_set_irq(pmb887x_gpio_get_input(item->key), gpio_value);
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
	pmb887x_cfg_section_t *section = pmb887x_cfg_section(board->config, "gpio-aliases", 0, true);
	for (size_t i = 0; i < section->items_count; i++) {
		pmb887x_cfg_item_t *item = &section->items[i];

		const pmb887x_cpu_meta_gpio_t *cpu_gpio = find_cpu_gpio_by_name(item->key);
		if (cpu_gpio) {
			pmb887x_cpu_meta_gpio_t *board_gpio = &board->gpios[cpu_gpio->id];
			board_gpio->name = g_strdup(cpu_gpio->name);
			board_gpio->func_name = g_strdup(item->value);
			board_gpio->full_name = g_strdup_printf("GPIO_PIN%d_%s", cpu_gpio->id, item->value);
		} else {
			warn_report("Unknown cpu GPIO '%s' in %s", item->key, section->parent->file);
		}
	}
}
