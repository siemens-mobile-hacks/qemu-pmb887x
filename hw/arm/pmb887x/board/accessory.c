#include "hw/arm/pmb887x/board/accessory.h"

#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/board/devices.h"
#include "hw/arm/pmb887x/board/gpio.h"
#include "hw/arm/pmb887x/utils/toml.h"
#include "tomlc17.h"

#include "hw/core/qdev.h"
#include "hw/misc/phone/accessory-connector.h"
#include "qapi/error.h"

typedef struct accessory_config_t accessory_config_t;

struct accessory_config_t {
	GPtrArray *inputs;
	GPtrArray *outputs;
	AccessoryProfile *profiles;
	size_t profile_count;
};

static unsigned accessory_add_spec(GPtrArray *specs, const char *spec) {
	for (unsigned i = 0; i < specs->len; i++) {
		if (strcmp(g_ptr_array_index(specs, i), spec) == 0)
			return i;
	}
	g_ptr_array_add(specs, (gpointer) spec);
	return specs->len - 1;
}

static AccessoryGpioRoute accessory_parse_gpio_route(
	accessory_config_t *config,
	const char *name,
	const char *output,
	toml_datum_t value
) {
	unsigned output_index = accessory_add_spec(config->outputs, output);

	switch (value.type) {
		case TOML_INT64:
			if (value.u.int64 != 0 && value.u.int64 != 1)
				break;
			return (AccessoryGpioRoute) { .output = output_index, .level = value.u.int64 == 1 };
		case TOML_STRING: {
			bool inverted = value.u.s[0] == '!';
			const char *input = inverted ? value.u.s + 1 : value.u.s;
			return (AccessoryGpioRoute) {
				.output = output_index,
				.from_input = true,
				.input = accessory_add_spec(config->inputs, input),
				.level = inverted,
			};
		}
		default:
			break;
	}
	toml_fail(value, "accessory '%s': GPIO '%s' must be 0, 1 or an output name with an optional '!'", name, output);
}

static AccessoryPortRoute accessory_parse_port_route(GHashTable *devices, const char *sink, toml_datum_t source) {
	if (source.type != TOML_STRING)
		toml_fail(source, "port '%s' must name a bus", sink);

	const char *separator = strchr(sink, ':');
	if (!separator)
		toml_fail(source, "port '%s' must be DEVICE:PORT", sink);

	g_autofree char *device_id = g_strndup(sink, separator - sink);
	DeviceState *device = g_hash_table_lookup(devices, device_id);
	if (!device)
		toml_fail(source, "port '%s': device '%s' not found", sink, device_id);

	return (AccessoryPortRoute) {
		.device = device,
		.port = separator + 1,
		.bus = BUS(pmb887x_board_find_bus(devices, source.u.s)),
	};
}

static void accessory_parse_profile(
	accessory_config_t *config,
	GHashTable *devices,
	AccessoryProfile *profile,
	const char *name,
	toml_datum_t table
) {
	toml_datum_t gpio = toml_table_get(table, TOML_TABLE, "gpio", false);
	toml_datum_t port = toml_table_get(table, TOML_TABLE, "port", false);
	size_t gpio_count = gpio.type == TOML_TABLE ? gpio.u.tab.size : 0;
	size_t port_count = port.type == TOML_TABLE ? port.u.tab.size : 0;
	AccessoryGpioRoute *gpio_routes = g_new0(AccessoryGpioRoute, gpio_count);
	AccessoryPortRoute *port_routes = g_new0(AccessoryPortRoute, port_count);

	for (size_t i = 0; i < gpio_count; i++)
		gpio_routes[i] = accessory_parse_gpio_route(config, name, gpio.u.tab.key[i], gpio.u.tab.value[i]);

	for (size_t i = 0; i < port_count; i++)
		port_routes[i] = accessory_parse_port_route(devices, port.u.tab.key[i], port.u.tab.value[i]);

	*profile = (AccessoryProfile) {
		.name = name,
		.gpio = gpio_routes,
		.gpio_count = gpio_count,
		.ports = port_routes,
		.port_count = port_count,
	};
}

static void accessory_config_free(accessory_config_t *config) {
	for (size_t i = 0; i < config->profile_count; i++) {
		g_free((gpointer) config->profiles[i].gpio);
		g_free((gpointer) config->profiles[i].ports);
	}
	g_free(config->profiles);
	g_ptr_array_free(config->inputs, true);
	g_ptr_array_free(config->outputs, true);
}

void pmb887x_board_accessory_configure(GHashTable *devices, DeviceState *dev, toml_datum_t table) {
	toml_datum_t accessories = toml_table_get(table, TOML_TABLE, "accessories", true);
	accessory_config_t config = {
		.inputs = g_ptr_array_new(),
		.outputs = g_ptr_array_new(),
		.profile_count = accessories.u.tab.size,
		.profiles = g_new0(AccessoryProfile, accessories.u.tab.size),
	};

	for (size_t i = 0; i < config.profile_count; i++) {
		const char *name = accessories.u.tab.key[i];
		toml_datum_t profile = accessories.u.tab.value[i];

		if (profile.type != TOML_TABLE)
			toml_fail(profile, "accessory '%s' must be a table", name);
		accessory_parse_profile(&config, devices, &config.profiles[i], name, profile);
	}
	accessory_connector_set_profiles(ACCESSORY_CONNECTOR(dev), config.profiles, config.profile_count);

	for (unsigned i = 0; i < config.inputs->len; i++)
		pmb887x_gpio_connect_output(g_ptr_array_index(config.inputs, i), qdev_get_gpio_in_named(dev, "in", i));
	for (unsigned i = 0; i < config.outputs->len; i++) {
		qemu_irq input = pmb887x_gpio_get_input(g_ptr_array_index(config.outputs, i));
		pmb887x_qdev_connect_gpio_out(dev, "out", i, input);
	}

	g_autofree char *requested = object_property_get_str(OBJECT(dev), "accessory", &error_fatal);
	if (!requested[0]) {
		const char *default_accessory = toml_table_get_string(table, "default", NULL, true);
		object_property_set_str(OBJECT(dev), "accessory", default_accessory, &error_fatal);
	}

	accessory_config_free(&config);
}
