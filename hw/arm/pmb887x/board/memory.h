#pragma once

#include "qemu/osdep.h"

void pmb887x_board_ebu_connect(DeviceState *ebuc, int cs, MemoryRegion *region);
MemoryRegion *pmb887x_board_create_sdram(const char *id, uint32_t size);
MemoryRegion *pmb887x_board_create_nor_flash(const char *id, uint32_t vid, uint32_t pid, uint32_t offset, uint32_t *size);
