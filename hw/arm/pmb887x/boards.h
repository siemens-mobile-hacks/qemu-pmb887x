#pragma once

#include "qemu/osdep.h"
#include "qapi/qapi-types-ui.h"
#include "regs.h"

typedef struct {
	uint32_t id;
	int value;
} pmb887x_fixed_gpio_t;

typedef struct {
	const char *name;
	
	uint32_t width;
	uint32_t height;
	const char *display;
	uint32_t display_rotation;
	
	uint32_t cpu;
	
	uint32_t *flash_banks;
	int flash_banks_cnt;
	
	uint32_t *keymap;
	int keymap_cnt;
	
	pmb887x_fixed_gpio_t *fixed_gpios;
	int fixed_gpios_cnt;
} pmb887x_board_t;

pmb887x_board_t *pmb887x_get_board(int board);
const uint8_t *pmb887x_get_brom_image(uint32_t cpu, size_t *size);
