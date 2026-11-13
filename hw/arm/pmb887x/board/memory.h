#pragma once

#include "qemu/osdep.h"

#define PMB887X_NOR_FLASH_PID_WORDS 3

/* Memory kinds that index the code fetch and literal load costs of the board memory timings. */
enum {
	PMB887X_FETCH_SDRAM = 1,
	PMB887X_FETCH_FLASH,
	PMB887X_FETCH_SRAM,
};

void pmb887x_board_ebu_connect(DeviceState *ebuc, int cs, MemoryRegion *region);
MemoryRegion *pmb887x_board_create_sdram(const char *id, uint32_t size);
MemoryRegion *pmb887x_board_create_nor_flash(
	const char *id,
	uint32_t vid,
	const uint32_t pid[PMB887X_NOR_FLASH_PID_WORDS],
	uint32_t offset
);
