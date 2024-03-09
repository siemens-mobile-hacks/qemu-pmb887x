#include "hw/arm/pmb887x/boards.h"
#include "hw/arm/pmb887x/config.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"

static struct {
	uint32_t id;
	char name[64];
} cpu_list[] = {
	{CPU_PMB8875, "pmb8875"},
	{CPU_PMB8876, "pmb8876"},
};

static struct {
	char name[64];
	uint32_t id;
} keyboard_map[] = {
	{"NAV_UP",			Q_KEY_CODE_UP},
	{"NAV_RIGHT",		Q_KEY_CODE_RIGHT},
	{"NAV_CENTER",		Q_KEY_CODE_KP_ENTER},
	{"NAV_CENTER",		Q_KEY_CODE_RET},
	{"NAV_LEFT",		Q_KEY_CODE_LEFT},
	{"NAV_DOWN",		Q_KEY_CODE_DOWN},
	
	{"SOFT_LEFT",		Q_KEY_CODE_F1},
	{"SOFT_RIGHT",		Q_KEY_CODE_F2},
	{"SEND",			Q_KEY_CODE_F3},
	{"END_CALL",		Q_KEY_CODE_F4},
	
	{"MUSIC",			Q_KEY_CODE_F5},
	{"PLAY_PAUSE",		Q_KEY_CODE_F6},
	{"PTT",				Q_KEY_CODE_F7},
	{"CAMERA",			Q_KEY_CODE_F8},
	{"BROWSER",			Q_KEY_CODE_F9},
	
	{"VOLUME_UP",		Q_KEY_CODE_KP_ADD},
	{"VOLUME_DOWN",		Q_KEY_CODE_KP_SUBTRACT},
	
	{"NUM0",			Q_KEY_CODE_KP_0},
	{"NUM0",			Q_KEY_CODE_0},
	
	{"NUM1",			Q_KEY_CODE_KP_7},
	{"NUM2",			Q_KEY_CODE_KP_8},
	{"NUM3",			Q_KEY_CODE_KP_9},
	{"NUM1",			Q_KEY_CODE_1},
	{"NUM2",			Q_KEY_CODE_2},
	{"NUM3",			Q_KEY_CODE_3},
	
	{"NUM4",			Q_KEY_CODE_KP_4},
	{"NUM5",			Q_KEY_CODE_KP_5},
	{"NUM6",			Q_KEY_CODE_KP_6},
	{"NUM4",			Q_KEY_CODE_4},
	{"NUM5",			Q_KEY_CODE_5},
	{"NUM6",			Q_KEY_CODE_6},
	
	{"NUM7",			Q_KEY_CODE_KP_1},
	{"NUM8",			Q_KEY_CODE_KP_2},
	{"NUM9",			Q_KEY_CODE_KP_3},
	{"NUM7",			Q_KEY_CODE_7},
	{"NUM8",			Q_KEY_CODE_8},
	{"NUM9",			Q_KEY_CODE_9},
	
	{"STAR",			Q_KEY_CODE_KP_MULTIPLY},
	{"HASH",			Q_KEY_CODE_KP_DIVIDE},
};

static GMatchInfo *_regexp_match(const char *pattern, const char *input) {
	g_autoptr(GRegex) regexp = g_regex_new(pattern, G_REGEX_CASELESS, 0, NULL);
	
	if (!regexp)
		return NULL;
	
	GMatchInfo *m = NULL;
	g_regex_match(regexp, input, 0, &m);
	if (g_match_info_matches(m))
		return m;
	g_match_info_free(m);
	return NULL;
}

static bool _parse_device(pmb887x_board_t *board, pmb887x_cfg_section_t *section) {
	const char *cpu, *vendor, *model;
	
	if (!(cpu = pmb887x_cfg_section_get(section, "cpu", true)))
		return false;
	if (!(vendor = pmb887x_cfg_section_get(section, "vendor", true)))
		return false;
	if (!(model = pmb887x_cfg_section_get(section, "model", true)))
		return false;
	
	for (size_t i = 0; i < ARRAY_SIZE(cpu_list); i++) {
		if (strcmp(cpu, cpu_list[i].name) == 0) {
			board->cpu = cpu_list[i].id;
			break;
		}
	}
	
	strncpy(board->vendor, vendor, sizeof(board->vendor) - 1);
	strncpy(board->model, model, sizeof(board->model) - 1);
	
	return true;
}

static bool _parse_memory(pmb887x_board_t *board, pmb887x_cfg_section_t *section) {
	for (size_t i = 0; i < section->items_count; i++) {
		pmb887x_cfg_item_t *item = &section->items[i];
		
		g_autoptr(GMatchInfo) cs_match = _regexp_match("^cs(\\d+)$", item->key);
		if (cs_match) {
			int cs = strtol(g_match_info_fetch(cs_match, 1), NULL, 10);
			if (cs >= 0 && cs < ARRAY_SIZE(board->cs2memory)) {
				g_autoptr(GMatchInfo) flash_match = _regexp_match("^flash:([a-f0-9]+):([a-f0-9]+)$", item->value);
				g_autoptr(GMatchInfo) ram_match = _regexp_match("^ram:(\\d+)m$", item->value);
				
				if (flash_match) {
					board->cs2memory[cs].type = PMB887X_MEMORY_TYPE_FLASH;
					board->cs2memory[cs].vid = strtol(g_match_info_fetch(flash_match, 1), NULL, 16);
					board->cs2memory[cs].pid = strtol(g_match_info_fetch(flash_match, 2), NULL, 16);
				} else if (ram_match) {
					board->cs2memory[cs].type = PMB887X_MEMORY_TYPE_RAM;
					board->cs2memory[cs].size = strtol(g_match_info_fetch(ram_match, 1), NULL, 10) * 1024 * 1024;
				} else {
					error_report("Unknown memory type: %s", item->value);
					return false;
				}
			} else {
				error_report("Unknown memory chip select: %s", item->key);
				return false;
			}
		} else {
			error_report("Invalid memory chip select: %s", item->key);
			return false;
		}
	}
	return true;
}

static bool _parse_i2c(pmb887x_board_t *board, pmb887x_cfg_section_t *section) {
	const char *type, *addr;
	
	if (!(type = pmb887x_cfg_section_get(section, "type", true)))
		return false;
	if (!(addr = pmb887x_cfg_section_get(section, "addr", true)))
		return false;
	
	board->i2c_devices_count++;
	board->i2c_devices = g_realloc_n(board->i2c_devices, board->i2c_devices_count, sizeof(pmb887x_board_i2c_dev_t));
	
	pmb887x_board_i2c_dev_t *i2c_dev = &board->i2c_devices[board->i2c_devices_count - 1];
	strncpy(i2c_dev->type, type, sizeof(i2c_dev->type) - 1);
	i2c_dev->addr = strtol(addr, NULL, 16);
	
	return true;
}

static bool _parse_analog(pmb887x_board_t *board, pmb887x_cfg_section_t *section) {
	char name[32];
	int channels[] = {
		PMB887X_ADC_INPUT_M0,
		PMB887X_ADC_INPUT_M1,
		PMB887X_ADC_INPUT_M2,
		PMB887X_ADC_INPUT_M3,
		PMB887X_ADC_INPUT_M4,
		PMB887X_ADC_INPUT_M5,
		PMB887X_ADC_INPUT_M6,
		PMB887X_ADC_INPUT_M7,
		PMB887X_ADC_INPUT_M8,
		PMB887X_ADC_INPUT_M9,
		PMB887X_ADC_INPUT_M10,
	};
	for (int i = 0; i < ARRAY_SIZE(channels); i++) {
		int ch = channels[i];
		sprintf(name, "M_%d", ch);
		
		const char *value;
		if (!(value = pmb887x_cfg_section_get(section, name, false)))
			continue;
		
		g_autoptr(GMatchInfo) resistor_match = _regexp_match("^resistor,(\\d+)$", value);
		g_autoptr(GMatchInfo) resistor_divider_match = _regexp_match("^resistor_divider,(\\d+),(\\d+),(-?\\d+)$", value);
		g_autoptr(GMatchInfo) voltage_match = _regexp_match("^(-?\\d+)$", value);
		
		if (resistor_match) {
			uint32_t r1 = strtol(g_match_info_fetch(resistor_match, 1), NULL, 10);
			board->adc_inputs[ch].r1 = r1;
			board->adc_inputs[ch].type = PMB887X_ADC_INPUT_RESISTOR;
		} else if (resistor_divider_match) {
			uint32_t r1 = strtol(g_match_info_fetch(resistor_divider_match, 1), NULL, 10);
			uint32_t r2 = strtol(g_match_info_fetch(resistor_divider_match, 2), NULL, 10);
			uint32_t input = strtol(g_match_info_fetch(resistor_divider_match, 3), NULL, 10);
			board->adc_inputs[ch].r1 = r1;
			board->adc_inputs[ch].r2 = r2;
			board->adc_inputs[ch].value = input;
			board->adc_inputs[ch].type = PMB887X_ADC_INPUT_RESISTOR_DIV;
		} else if (voltage_match) {
			uint32_t input = strtol(g_match_info_fetch(resistor_match, 1), NULL, 10);
			board->adc_inputs[ch].value = input;
			board->adc_inputs[ch].type = PMB887X_ADC_INPUT_VOLTAGE;
		} else {
			error_report("Invalid ADC channel value: %s", value);
			return false;
		}
	}
	return true;
}

static bool _parse_display(pmb887x_board_t *board, pmb887x_cfg_section_t *section) {
	const char *type, *rotation, *width, *height, *flip_horizontal, *flip_vertical;
	
	if (!(type = pmb887x_cfg_section_get(section, "type", true)))
		return false;
	if (!(width = pmb887x_cfg_section_get(section, "width", true)))
		return false;
	if (!(height = pmb887x_cfg_section_get(section, "height", true)))
		return false;
	
	rotation = pmb887x_cfg_section_get(section, "rotation", false);
	flip_horizontal = pmb887x_cfg_section_get(section, "flip_horizontal", false);
	flip_vertical = pmb887x_cfg_section_get(section, "flip_vertical", false);
	
	strncpy(board->display.type, type, sizeof(board->display.type) - 1);
	
	if (flip_horizontal)
		board->display.flip_horizontal = strtol(flip_horizontal, NULL, 10) != 0;
	
	if (flip_vertical)
		board->display.flip_vertical = strtol(flip_vertical, NULL, 10) != 0;
	
	if (rotation) {
		board->display.rotation = strtol(rotation, NULL, 10);
		if (board->display.rotation != 0 && board->display.rotation != 90 && board->display.rotation != 180 && board->display.rotation == 270) {
			error_report("Invalid display rotation: %s", rotation);
			return false;
		}
	}
	
	board->display.width = strtol(width, NULL, 10);
	board->display.height = strtol(height, NULL, 10);
	
	if (board->display.width < 30 || board->display.width > 1024 || board->display.height < 30 || board->display.height > 1024) {
		error_report("Invalid display resolution: %s x %s", width, height);
		return false;
	}
	
	return true;
}

static const pmb887x_cpu_meta_gpio_t *_find_cpu_gpio_by_name(pmb887x_board_t *board, const char *name) {
	const pmb887x_cpu_meta_t *cpu_info = pmb887x_get_cpu_meta(board->cpu);
	for (int i = 0; i < cpu_info->gpios_count; i++) {
		const pmb887x_cpu_meta_gpio_t *cpu_gpio = &cpu_info->gpios[i];
		if (strcmp(cpu_gpio->name, name) == 0 || strcmp(cpu_gpio->func_name, name) == 0)
			return cpu_gpio;
	}
	return NULL;
}

static pmb887x_board_gpio_t *_find_board_gpio_by_name(pmb887x_board_t *board, const char *name) {
	for (int i = 0; i < board->gpios_count; i++) {
		pmb887x_board_gpio_t *board_gpio = &board->gpios[i];
		if (strcmp(board_gpio->name, name) == 0 || strcmp(board_gpio->func_name, name) == 0)
			return board_gpio;
	}
	return NULL;
}

static bool _parse_gpio_aliases(pmb887x_board_t *board, pmb887x_cfg_section_t *section) {
	// Init default GPIO's
	const pmb887x_cpu_meta_t *cpu_info = pmb887x_get_cpu_meta(board->cpu);
	board->gpios_count = cpu_info->gpios_count;
	board->gpios = g_new0(pmb887x_board_gpio_t, board->gpios_count);
	
	for (int i = 0; i < cpu_info->gpios_count; i++) {
		const pmb887x_cpu_meta_gpio_t *cpu_gpio = &cpu_info->gpios[i];
		board->gpios[i].id = cpu_gpio->id;
		board->gpios[i].value = false;
		strncpy(board->gpios[i].name, cpu_gpio->name, sizeof(board->gpios[i].name) - 1);
		strncpy(board->gpios[i].func_name, cpu_gpio->func_name, sizeof(board->gpios[i].func_name) - 1);
		strncpy(board->gpios[i].full_name, cpu_gpio->full_name, sizeof(board->gpios[i].full_name) - 1);
	}
	
	// Set board-specific gpio names
	for (size_t i = 0; i < section->items_count; i++) {
		pmb887x_cfg_item_t *item = &section->items[i];
		
		const pmb887x_cpu_meta_gpio_t *cpu_gpio = _find_cpu_gpio_by_name(board, item->key);
		if (cpu_gpio) {
			pmb887x_board_gpio_t *board_gpio = &board->gpios[cpu_gpio->id];
			strncpy(board_gpio->func_name, item->value, sizeof(board_gpio->func_name) - 1);
			snprintf(board_gpio->full_name, sizeof(board_gpio->full_name) - 1, "GPIO_PIN%d_%s", cpu_gpio->id, item->value);
		} else {
			warn_report("Unknown cpu GPIO '%s' in %s", item->key, section->parent->file);
		}
	}
	
	return true;
}

static bool _parse_gpio_inputs(pmb887x_board_t *board, pmb887x_cfg_section_t *section) {
	for (size_t i = 0; i < section->items_count; i++) {
		pmb887x_cfg_item_t *item = &section->items[i];
		
		pmb887x_board_gpio_t *board_gpio = _find_board_gpio_by_name(board, item->key);
		if (board_gpio) {
			board_gpio->value = strtoll(item->value, NULL, 10) != 0;
		} else {
			warn_report("Unknown board GPIO '%s' in %s", item->key, section->parent->file);
		}
	}
	return true;
}

static bool _parse_keyboard(pmb887x_board_t *board, pmb887x_cfg_section_t *section) {
	for (size_t i = 0; i < section->items_count; i++) {
		pmb887x_cfg_item_t *item = &section->items[i];
		
		char **parts = g_strsplit(item->value, ":", -1);
		if (g_strv_length(parts) != 2) {
			error_report("Invalid [keyboard] config %s=%s", item->key, item->value);
			return false;
		}
		
		uint32_t keycode = 0;
		char **kp_in_arr = g_strsplit(parts[0], ",", -1);
		char **kp_out_arr = g_strsplit(parts[1], ",", -1);
		
		uint32_t kp_in_arr_len = g_strv_length(kp_in_arr);
		for (uint32_t j = 0; j < kp_in_arr_len; j++)
			keycode |= 1 << strtoll(kp_in_arr[j], NULL, 10);
		
		uint32_t kp_out_arr_len = g_strv_length(kp_out_arr);
		for (uint32_t j = 0; j < kp_out_arr_len; j++)
			keycode |= 1 << (8 + strtoll(kp_out_arr[j], NULL, 10));
		
		bool found = false;
		for (size_t j = 0; j < ARRAY_SIZE(keyboard_map); j++) {
			if (strcmp(item->key, keyboard_map[j].name) == 0) {
				board->keymap[keyboard_map[j].id] = keycode;
				found = true;
			}
		}
		
		if (!found)
			warn_report("Unknown key '%s' in %s", item->key, section->parent->file);
	}
	return true;
}

const pmb887x_board_t *pmb887x_get_board(const char *config_file) {
	pmb887x_board_t *board = g_new0(pmb887x_board_t, 1);
	
	pmb887x_cfg_t *cfg = pmb887x_cfg_parse(config_file);
	if (!cfg)
		return NULL;
	
	struct {
		const char *section;
		bool (*parser)(pmb887x_board_t *, pmb887x_cfg_section_t *);
		bool multile;
	} parsers[] = {
		{"device", _parse_device, false},
		{"memory", _parse_memory, false},
		{"i2c", _parse_i2c, true},
		{"analog", _parse_analog, false},
		{"display", _parse_display, false},
		{"gpio-aliases", _parse_gpio_aliases, false},
		{"gpio-inputs", _parse_gpio_inputs, false},
		{"keyboard", _parse_keyboard, false},
	};
	
	for (size_t i = 0; i < ARRAY_SIZE(parsers); i++) {		
		uint32_t sections_n = pmb887x_cfg_sections_cnt(cfg, parsers[i].section);
		
		if (sections_n > 1 && !parsers[i].multile) {
			error_report("[pmb887x-config] %s: multiple [%s] sections is not allowed!", config_file, parsers[i].section);
			pmb887x_cfg_free(cfg);
			return NULL;
		}
		
		for (uint32_t j = 0; j < sections_n; j++) {
			pmb887x_cfg_section_t *section = pmb887x_cfg_section(cfg, parsers[i].section, j, true);
			if (!section) {
				pmb887x_cfg_free(cfg);
				return NULL;
			}
			if (!parsers[i].parser(board, section)) {
				error_report("[pmb887x-config] %s: invalid settings in [%s]", config_file, parsers[i].section);
				pmb887x_cfg_free(cfg);
				return NULL;
			}
		}
	}
	
	pmb887x_cfg_free(cfg);
	
	return board;
}
