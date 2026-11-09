/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "qemu/aiocb.h"

#define TYPE_CFI_FLASH "cfi-flash"
#define TYPE_CFI_FLASH_STORAGE "cfi-flash-storage"

typedef struct CFIFlashStorage CFIFlashStorage;

int cfi_flash_storage_pread(CFIFlashStorage *flash, int64_t offset, int64_t size, void *storage);
void cfi_flash_storage_aio_pwrite(
    CFIFlashStorage *flash,
    int64_t offset,
    int64_t size,
    void *data,
    BlockCompletionFunc *cb,
    void *opaque
);
bool cfi_flash_storage_is_rw(CFIFlashStorage *flash);
int64_t cfi_flash_storage_size(CFIFlashStorage *flash);
const char *cfi_flash_storage_filename(CFIFlashStorage *flash);
CFIFlashStorage *cfi_flash_storage_self(DeviceState *dev);
