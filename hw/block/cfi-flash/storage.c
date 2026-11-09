/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "block/block_int-common.h"
#include "hw/core/sysbus.h"
#include "system/block-backend.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/debug-channel.h"
#include "hw/block/cfi-flash.h"

struct CFIFlashStorage {
    SysBusDevice parent_obj;
    DebugChannel dbg;
    BlockBackend *blk;
};

typedef struct CFIFlashStorageWrite CFIFlashStorageWrite;

struct CFIFlashStorageWrite {
    QEMUIOVector qiov;
    BlockCompletionFunc *cb;
    void *opaque;
};

int cfi_flash_storage_pread(CFIFlashStorage *flash, int64_t offset, int64_t size, void *storage) {
    return blk_pread(flash->blk, offset, size, storage, 0);
}

static void cfi_flash_storage_aio_pwrite_done(void *opaque, int ret) {
    CFIFlashStorageWrite *write = opaque;
    write->cb(write->opaque, ret);
    g_free(write);
}

void cfi_flash_storage_aio_pwrite(
    CFIFlashStorage *flash,
    int64_t offset,
    int64_t size,
    void *data,
    BlockCompletionFunc *cb,
    void *opaque
) {
    CFIFlashStorageWrite *write = g_new(CFIFlashStorageWrite, 1);
    write->cb = cb;
    write->opaque = opaque;
    qemu_iovec_init_buf(&write->qiov, data, size);
    blk_aio_pwritev(flash->blk, offset, &write->qiov, 0, cfi_flash_storage_aio_pwrite_done, write);
}

bool cfi_flash_storage_is_rw(CFIFlashStorage *flash) {
    return blk_supports_write_perm(flash->blk);
}

int64_t cfi_flash_storage_size(CFIFlashStorage *flash) {
    return blk_co_getlength(flash->blk);
}

const char *cfi_flash_storage_filename(CFIFlashStorage *flash) {
    BlockDriverState *bs = blk_bs(flash->blk);
    return bs->exact_filename[0] ? bs->exact_filename : bs->filename;
}

static void cfi_flash_storage_realize(DeviceState *dev, Error **errp) {
    CFIFlashStorage *flash = CFI_FLASH_STORAGE(dev);
    debug_channel_init_device(&flash->dbg, dev);

    if (!flash->blk) {
        error_setg(errp, "Flash property 'drive' is not set");
        return;
    }

    DEBUG_LOG(&flash->dbg, "Drive size: %08"PRIX64"\n", blk_co_getlength(flash->blk));

    if (cfi_flash_storage_is_rw(flash)) {
        blk_set_perm(flash->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE, BLK_PERM_ALL, errp);
    } else {
        blk_set_perm(flash->blk, BLK_PERM_CONSISTENT_READ, BLK_PERM_ALL, errp);
    }
}

static const Property cfi_flash_storage_properties[] = {
    DEFINE_PROP_DRIVE("drive", CFIFlashStorage, blk)
};

static void cfi_flash_storage_class_init(ObjectClass *klass, const void *data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, cfi_flash_storage_properties);
    dc->realize = cfi_flash_storage_realize;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo cfi_flash_storage_info = {
    .name           = TYPE_CFI_FLASH_STORAGE,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(CFIFlashStorage),
    .class_init     = cfi_flash_storage_class_init,
};

static void cfi_flash_storage_register_types(void) {
    type_register_static(&cfi_flash_storage_info);
}
type_init(cfi_flash_storage_register_types)
