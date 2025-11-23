#include "hw/arm/pmb887x/board/board.h"

#include "hw/arm/pmb887x/board/gpio.h"
#include "hw/arm/pmb887x/board/keyboard.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/utils/toml.h"
#include "hw/arm/pmb887x/utils/tomlc17.h"
#include "hw/hw.h"
#include "qemu/error-report.h"

static pmb887x_board_t *board = NULL;

static int find_cpu_by_name(const char *cpu_name);

static struct {
	uint32_t rev;
	uint32_t uid0;
	uint32_t uid1;
	uint32_t uid2;
} default_cpu_id[] = {
	[CPU_PMB8875] = { 0x05, 0x00000000, 0x00000000, 0x00000000 },
	[CPU_PMB8876] = { 0x10, 0x006025DB, 0x180121A5, 0x00071303 },
};

static toml_result_t load_board_config(const char *config_file) {
	toml_result_t board_config = toml_parse_file_ex(config_file);
	if (!board_config.ok) {
		error_report("Invalid board config %s: %s", config_file, board_config.errmsg);
		exit(EXIT_FAILURE);
	}
	toml_init_datum_location_info(&board_config, config_file);

	const char *base_config_name = toml_table_get_string(board_config.toptab, "board.extends", NULL, false);
	if (base_config_name) {
		char base_config_file[4096];

		const char *config_dir = g_path_get_dirname(config_file);
		snprintf(base_config_file, sizeof(base_config_file) - 1, "%s/%s", config_dir, base_config_name);

		toml_result_t base_config = toml_parse_file_ex(base_config_file);
		if (!base_config.ok) {
			error_report("Invalid board config %s: %s", base_config_file, base_config.errmsg);
			exit(EXIT_FAILURE);
		}
		toml_init_datum_location_info(&base_config, base_config_file);

		toml_result_t merged_config = toml_merge(&base_config, &board_config);
		toml_free(base_config);
		toml_free(board_config);
		return merged_config;
	}

	return board_config;
}

void pmb887x_board_init(const char *config_file) {
	board = g_new0(pmb887x_board_t, 1);

	board->config_result = load_board_config(config_file);
	board->config = board->config_result.toptab;

	board->vendor = toml_table_get_string(board->config, "board.vendor", NULL, true);
	board->model = toml_table_get_string(board->config, "board.model", NULL, true);
	board->cpu = find_cpu_by_name(toml_table_get_string(board->config, "board.cpu.type", NULL, true));

	fprintf(stderr, "Board: %s %s\n", board->vendor, board->model);

	board->cpu_rev = toml_table_get_uint32(board->config, "board.cpu.rev", default_cpu_id[board->cpu].rev, false);
	board->cpu_uid[0] = toml_table_get_uint32(board->config, "board.cpu.uid0", default_cpu_id[board->cpu].uid0, false);
	board->cpu_uid[1] = toml_table_get_uint32(board->config, "board.cpu.uid1", default_cpu_id[board->cpu].uid1, false);
	board->cpu_uid[2] = toml_table_get_uint32(board->config, "board.cpu.uid2", default_cpu_id[board->cpu].uid2, false);

	pmb887x_board_keymap_init();
	pmb887x_board_gpio_init();
}

pmb887x_board_t *pmb887x_board(void) {
	return board;
}

static int find_cpu_by_name(const char *cpu_name) {
	static struct {
		uint32_t id;
		char name[64];
	} cpu_list[] = {
		{ CPU_PMB8875, "pmb8875" },
		{ CPU_PMB8876, "pmb8876" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cpu_list); i++) {
		if (strcmp(cpu_name, cpu_list[i].name) == 0)
			return cpu_list[i].id;
	}
	error_report("Invalid CPU type: %s", cpu_name);
	exit(EXIT_FAILURE);
	return -1;
}
