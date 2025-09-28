#pragma once

#include "qemu/osdep.h"
#include "qapi/qapi-types-ui.h"

#include "hw/arm/pmb887x/gen/cpu_meta.h"
#include "hw/arm/pmb887x/utils/config.h"

typedef struct pmb887x_board_t pmb887x_board_t;

struct pmb887x_board_t {
	const char *vendor;
	const char *model;
	uint32_t cpu;

	pmb887x_cfg_t *config;

	uint32_t keymap[Q_KEY_CODE__MAX];

	pmb887x_cpu_meta_gpio_t *gpios;
	uint32_t gpios_count;

	uint32_t flash_offset;
};

void pmb887x_board_init(const char *config_file);
pmb887x_board_t *pmb887x_board(void);
