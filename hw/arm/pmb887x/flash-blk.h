#pragma once

#include "qemu/osdep.h"

struct pmb887x_flash_blk_t;
typedef struct pmb887x_flash_blk_t pmb887x_flash_blk_t;

int pmb887x_flash_blk_pread(pmb887x_flash_blk_t *flash, int64_t offset, int64_t size, void *storage);
bool pmb887x_flash_blk_is_raw(pmb887x_flash_blk_t *flash);
