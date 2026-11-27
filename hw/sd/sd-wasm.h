/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PMB887X-WEB WASM PORT: the card's block I/O from the vCPU thread (sd-wasm.c) */
#ifndef HW_SD_SD_WASM_H
#define HW_SD_SD_WASM_H

#include "system/block-backend.h"

int sd_wasm_blk_io(Object *owner, BlockBackend *blk, int64_t addr, int64_t len,
                   uint8_t *buf, bool is_write);

#endif
