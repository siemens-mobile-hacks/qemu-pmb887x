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

#define TYPE_PMB887X_FLASH_BLK	"pmb887x-flash-blk"
#define PMB887X_FLASH_BLK(obj)	OBJECT_CHECK(struct pmb887x_flash_blk_t, (obj), TYPE_PMB887X_FLASH_BLK)

struct pmb887x_flash_blk_t {
	SysBusDevice parent_obj;
	DebugChannel dbg;
	DeviceState *dev;
	BlockBackend *blk;
};

typedef struct pmb887x_flash_blk_write_t pmb887x_flash_blk_write_t;

struct pmb887x_flash_blk_write_t {
	QEMUIOVector qiov;
	BlockCompletionFunc *cb;
	void *opaque;
};

int pmb887x_flash_blk_pread(pmb887x_flash_blk_t *flash, int64_t offset, int64_t size, void *storage) {
	return blk_pread(flash->blk, offset, size, storage, 0);
}

static void flash_blk_aio_pwrite_done(void *opaque, int ret) {
	pmb887x_flash_blk_write_t *write = opaque;
	write->cb(write->opaque, ret);
	g_free(write);
}

void pmb887x_flash_blk_aio_pwrite(
	pmb887x_flash_blk_t *flash,
	int64_t offset,
	int64_t size,
	void *data,
	BlockCompletionFunc *cb,
	void *opaque
) {
	pmb887x_flash_blk_write_t *write = g_new(pmb887x_flash_blk_write_t, 1);
	write->cb = cb;
	write->opaque = opaque;
	qemu_iovec_init_buf(&write->qiov, data, size);
	blk_aio_pwritev(flash->blk, offset, &write->qiov, 0, flash_blk_aio_pwrite_done, write);
}

bool pmb887x_flash_blk_is_rw(pmb887x_flash_blk_t *flash) {
	return blk_supports_write_perm(flash->blk);
}

int64_t pmb887x_flash_blk_size(pmb887x_flash_blk_t *flash) {
	return blk_co_getlength(flash->blk);
}

const char *pmb887x_flash_blk_filename(pmb887x_flash_blk_t *flash) {
	BlockDriverState *bs = blk_bs(flash->blk);
	return bs->exact_filename[0] ? bs->exact_filename : bs->filename;
}

pmb887x_flash_blk_t *pmb887x_flash_blk_self(DeviceState *dev) {
	return PMB887X_FLASH_BLK(dev);
}

static void flash_blk_realize(DeviceState *dev, Error **errp) {
	pmb887x_flash_blk_t *flash = PMB887X_FLASH_BLK(dev);
	debug_channel_init_device(&flash->dbg, dev);
	
	if (!flash->blk) {
		error_setg(errp, "Flash property 'drive' is not set");
		return;
	}
	
	DEBUG_LOG(&flash->dbg, "Drive size: %08"PRIX64"\n", blk_co_getlength(flash->blk));
	
	if (pmb887x_flash_blk_is_rw(flash)) {
		blk_set_perm(flash->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE, BLK_PERM_ALL, errp);
	} else {
		blk_set_perm(flash->blk, BLK_PERM_CONSISTENT_READ, BLK_PERM_ALL, errp);
	}
}

static const Property flash_blk_properties[] = {
	DEFINE_PROP_DRIVE("drive", pmb887x_flash_blk_t, blk)
};

static void flash_blk_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, flash_blk_properties);
	dc->realize = flash_blk_realize;
	set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo flash_blk_info = {
    .name          	= TYPE_PMB887X_FLASH_BLK,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_flash_blk_t),
    .class_init    	= flash_blk_class_init,
};

static void flash_blk_register_types(void) {
	type_register_static(&flash_blk_info);
}
type_init(flash_blk_register_types)
