#include "hw/arm/pmb887x/board/board.h"

#include "hw/arm/pmb887x/board/gpio.h"
#include "hw/arm/pmb887x/board/keyboard.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/hw.h"
#include "qemu/error-report.h"

static pmb887x_board_t *board = NULL;

static int find_cpu_by_name(const char *cpu_name);

void pmb887x_board_init(const char *config_file) {
	board = g_new0(pmb887x_board_t, 1);

	board->config = pmb887x_cfg_parse(config_file);
	if (!board->config)
		hw_error("Invalid config: %s", config_file);

	board->vendor = pmb887x_cfg_get(board->config, "device", "vendor", NULL, true);
	board->model = pmb887x_cfg_get(board->config, "device", "model", NULL, true);
	board->cpu = find_cpu_by_name(pmb887x_cfg_get(board->config, "device", "cpu", NULL, true));
	board->cpu_rev = pmb887x_cfg_get_int(board->config, "device", "cpu_rev", 0xFFFFFFFF, true);
	board->cpu_uid[0] = pmb887x_cfg_get_int(board->config, "device", "cpu_uid0", 0, false);
	board->cpu_uid[1] = pmb887x_cfg_get_int(board->config, "device", "cpu_uid1", 0, false);
	board->cpu_uid[2] = pmb887x_cfg_get_int(board->config, "device", "cpu_uid2", 0, false);

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
	exit(1);
	return -1;
}
