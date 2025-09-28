#include "hw/arm/pmb887x/board/devices.h"

#include "hw/arm/pmb887x/flash-blk.h"
#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/board/gpio.h"
#include "hw/arm/pmb887x/board/memory.h"
#include "hw/arm/pmb887x/utils/regexp.h"

#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/qdev-core.h"
#include "qom/object.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "hw/ssi/ssi.h"
#include "qapi/error.h"

typedef enum pmb887x_dev_prop_type_t pmb887x_dev_prop_type_t;
typedef enum pmb887x_dev_bus_type_t pmb887x_dev_bus_type_t;
typedef struct pmb887x_dev_prop_t pmb887x_dev_prop_t;
typedef struct pmb887x_dev_t pmb887x_dev_t;

enum pmb887x_dev_bus_type_t {
	DEV_BUS_NONE,
	DEV_BUS_I2C,
	DEV_BUS_SSI,
	DEV_BUS_EBU,
};

enum pmb887x_dev_prop_type_t {
	DEV_PROP_INT,
	DEV_PROP_UINT,
	DEV_PROP_STRING,
	DEV_PROP_BOOL,
};

struct pmb887x_dev_prop_t {
	char name[32];
	pmb887x_dev_prop_type_t type;
	bool required;
};

struct pmb887x_dev_t {
	char name[32];
	pmb887x_dev_prop_t props[32];
};

static int global_cs_index = 0;

static pmb887x_dev_t devices_meta[] = {
	// LCD
	{
		.name = "jbt6k71",
		.props = {
			{ "width", DEV_PROP_UINT, true },
			{ "height", DEV_PROP_UINT, true },
			{ "rotation", DEV_PROP_UINT, false },
			{ "flip_horizontal", DEV_PROP_UINT, false },
			{ "flip_vertical", DEV_PROP_UINT, false },
		},
	},
	{
		.name = "ssd1286",
		.props = {
			{ "width", DEV_PROP_UINT, true },
			{ "height", DEV_PROP_UINT, true },
			{ "rotation", DEV_PROP_UINT, false },
			{ "flip_horizontal", DEV_PROP_UINT, false },
			{ "flip_vertical", DEV_PROP_UINT, false },
		},
	},
	{
		.name = "hx5050a",
		.props = {
			{ "width", DEV_PROP_UINT, true },
			{ "height", DEV_PROP_UINT, true },
			{ "rotation", DEV_PROP_UINT, false },
			{ "flip_horizontal", DEV_PROP_UINT, false },
			{ "flip_vertical", DEV_PROP_UINT, false },
		},
	},

	// PMIC
	{
		.name = "d1094xx",
		.props = {
			{ "revision", DEV_PROP_UINT, true },
		},
	},
	{
		.name = "pmb6812",
		.props = {},
	},

	// FM Radio
	{
		.name = "tea5761uk",
		.props = {},
	},

	// Gimmick
	{
		.name = "s1d13705",
		.props = {},
	},

	// Audio Codec
	{
		.name = "b00b10b",
		.props = {},
	},

	// NOR FLASH
	{
		.name = "nor_flash",
		.props = {},
	},

	// SDRAM
	{
		.name = "sdram",
		.props = {},
	},
};

static const pmb887x_dev_t *dev_get_metadata(const char *name) {
	for (size_t i = 0; i < ARRAY_SIZE(devices_meta); i++) {
		if (strcasecmp(devices_meta[i].name, name) == 0)
			return &devices_meta[i];
	}
	hw_error("Unknown device: %s", name);
	return NULL;
}

static Object *bus_find_by_id(const char *name) {
	DeviceState *dev = qdev_find_recursive(sysbus_get_default(), name);
	if (!dev)
		hw_error("Bus not found: %s", name);

	if (object_property_find(OBJECT(dev), "bus")) {
		Object *bus_obj = object_property_get_link(OBJECT(dev), "bus", &error_fatal);
		g_assert(bus_obj);
		return bus_obj;
	}

	return OBJECT(dev);
}

static pmb887x_dev_bus_type_t bus_get_type(Object *dev) {
	const char *typename = object_get_typename(dev);
	if (strcmp(typename, "i2c-bus") == 0)
		return DEV_BUS_I2C;
	if (strcmp(typename, "SSI") == 0)
		return DEV_BUS_SSI;
	if (strcmp(typename, "pmb887x-ebu") == 0)
		return DEV_BUS_EBU;
	hw_error("Unknown bus type: %s", typename);
	return DEV_BUS_NONE;
}

static void device_init_props_from_config(DeviceState *dev, const pmb887x_dev_t *meta, pmb887x_cfg_section_t *section) {
	for (int i = 0; i < ARRAY_SIZE(meta->props); i++) {
		const pmb887x_dev_prop_t *prop = &meta->props[i];
		if (!prop->required && !pmb887x_cfg_section_get(section, prop->name, NULL, false))
			continue;

		switch (prop->type) {
			case DEV_PROP_INT: {
				int value = pmb887x_cfg_section_get_int(section, prop->name, -1, true);
				qdev_prop_set_uint32(dev, prop->name, value);
				break;
			}
			case DEV_PROP_UINT: {
				uint32_t value = pmb887x_cfg_section_get_uint(section, prop->name, 0, true);
				qdev_prop_set_uint32(dev, prop->name, value);
				break;
			}
			case DEV_PROP_STRING: {
				const char *value = pmb887x_cfg_section_get(section, prop->name, NULL, true);
				qdev_prop_set_string(dev, prop->name, value);
				break;
			}
			case DEV_PROP_BOOL: {
				int value = pmb887x_cfg_section_get_int(section, prop->name, -1, true);
				object_property_set_bool(OBJECT(dev), prop->name, value != 0, &error_fatal);
				break;
			}
		}
	}
}

static void device_init_gpios_from_config(DeviceState *dev, pmb887x_cfg_section_t *section) {
	g_autoptr(GRegex) regexp = g_regex_new("^gpio_(in|out)\\[(.*?)\\]$", G_REGEX_CASELESS, 0, NULL);
	for (size_t i = 0; i < section->items_count; i++) {
		pmb887x_cfg_item_t *item = &section->items[i];
		g_autoptr(GMatchInfo) match = regexp_match(regexp, item->key);

		if (!match)
			continue;

		const char *direction = g_match_info_fetch(match, 1);
		if (strcmp(direction, "in") == 0) {
			const char *gpio_in_name = g_match_info_fetch(match, 2);
			if (strcmp(gpio_in_name, "cs") == 0)
				gpio_in_name = SSI_GPIO_CS;
			if (!pmb887x_qdev_is_gpio_exists(dev, gpio_in_name, 0))
				hw_error("GPIO '%s' not found in '%s'", gpio_in_name, dev->id);
			pmb887x_gpio_connect(item->value, qdev_get_gpio_in_named(dev, gpio_in_name, 0));
		} else {
			char gpio_out_name[64];
			sprintf(gpio_out_name, "%s:%s", dev->id, g_match_info_fetch(match, 2));
			pmb887x_gpio_connect(gpio_out_name, pmb887x_gpio_get_input(item->value));
		}
	}
}

static void parse_flash_id(const char *flash_id, uint32_t *vid, uint32_t *pid) {
	if (!flash_id)
		hw_error("Invalid flash id: NULL");

	const char *sep = strchr(flash_id, ':');
	if (!sep)
		hw_error("Invalid flash id: %s", flash_id);

	*vid = strtoul(flash_id, NULL, 16);
	*pid = strtoul(sep + 1, NULL, 16);
}

static DeviceState *device_create_from_config(DeviceState *ebuc, pmb887x_cfg_section_t *section) {
	pmb887x_board_t *board = pmb887x_board();
	const char *id = pmb887x_cfg_section_get(section, "id", NULL, true);
	const char *type = pmb887x_cfg_section_get(section, "type", NULL, true);
	const char *bus_id = pmb887x_cfg_section_get(section, "bus", NULL, true);
	const pmb887x_dev_t *meta = dev_get_metadata(type);

	DeviceState *dev = NULL;
	Object *bus = strcmp(bus_id, "EBU") == 0 ? OBJECT(ebuc) : bus_find_by_id(bus_id);
	pmb887x_dev_bus_type_t bus_type = bus_get_type(bus);

	switch (bus_type) {
		case DEV_BUS_I2C: {
			uint32_t addr = pmb887x_cfg_section_get_uint(section, "addr", 0, true);
			dev = DEVICE(i2c_slave_new(type, addr));
			dev->id = g_strdup(id);
			device_init_props_from_config(dev, meta, section);
			i2c_slave_realize_and_unref(I2C_SLAVE(dev), I2C_BUS(bus), &error_fatal);
			device_init_gpios_from_config(dev, section);
			break;
		}
		case DEV_BUS_SSI: {
			dev = qdev_new(type);
			qdev_prop_set_uint8(DEVICE(dev), "cs", global_cs_index++);
			dev->id = g_strdup(id);
			device_init_props_from_config(dev, meta, section);
			qdev_realize_and_unref(dev, BUS(bus), &error_fatal);
			device_init_gpios_from_config(dev, section);
			break;
		}
		case DEV_BUS_EBU: {
			if (strcmp(type, "sdram") == 0) {
				uint32_t size = pmb887x_cfg_section_get_uint(section, "size", 0, true) * 1024 * 1024;
				uint32_t cs = pmb887x_cfg_section_get_uint(section, "cs", 0, true);
				pmb887x_board_ebu_connect(DEVICE(bus), cs, pmb887x_board_create_sdram(id, size));
			} else if (strcmp(type, "nor_flash") == 0) {
				const char *flash_id = pmb887x_cfg_section_get(section, "flash_id", 0, true);
				uint32_t cs = pmb887x_cfg_section_get_uint(section, "cs", 0, true);
				uint32_t pid, vid;
				parse_flash_id(flash_id, &vid, &pid);

				uint32_t bank_size;
				pmb887x_board_ebu_connect(DEVICE(bus), cs, pmb887x_board_create_nor_flash(id, vid, pid, board->flash_offset, &bank_size));
				board->flash_offset += bank_size;
			}
			break;
		}
		default:
			hw_error("Unknown bus type: %s", bus_id);
			break;
	}

	return dev;
}

void pmb887x_board_init_devices(DeviceState *ebuc) {
	pmb887x_board_t *board = pmb887x_board();
	int count = pmb887x_cfg_sections_cnt(board->config, "peripheral");
	for (int i = 0; i < count; i++) {
		pmb887x_cfg_section_t *section = pmb887x_cfg_section(board->config, "peripheral", i, true);
		device_create_from_config(ebuc, section);
	}

	DeviceState *flash_blk = qdev_find_recursive(sysbus_get_default(), "FULLFLASH");
	g_assert(flash_blk != NULL);

	int64_t total_flash_size = pmb887x_flash_blk_size(pmb887x_flash_blk_self(flash_blk));
	if (board->flash_offset != total_flash_size)
		hw_error("Invalid fullflash size=0x%08"PRIX64". Please, specify fullflash with size=0x%08X", total_flash_size, board->flash_offset);

	sysbus_realize_and_unref(SYS_BUS_DEVICE(ebuc), &error_fatal);
}
