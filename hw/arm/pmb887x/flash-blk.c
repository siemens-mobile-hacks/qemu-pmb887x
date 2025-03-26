#define PMB887X_TRACE_ID		FLASH
#define PMB887X_TRACE_PREFIX	"pmb887x-flash-blk"

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/sysbus.h"
#include "system/block-backend.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/flash-blk.h"

#define TYPE_PMB887X_FLASH_BLK	"pmb887x-flash-blk"
#define PMB887X_FLASH_BLK(obj)	OBJECT_CHECK(struct pmb887x_flash_blk_t, (obj), TYPE_PMB887X_FLASH_BLK)

struct pmb887x_flash_blk_t {
	SysBusDevice parent_obj;
	DeviceState *dev;
	BlockBackend *blk;
};

int pmb887x_flash_blk_pread(pmb887x_flash_blk_t *flash, int64_t offset, int64_t size, void *storage) {
	return blk_pread(flash->blk, offset, size, storage, 0);
}

int pmb887x_flash_blk_pwrite(pmb887x_flash_blk_t *flash, int64_t offset, int64_t size, void *value) {
	return blk_pwrite(flash->blk, offset, size, value, 0);
}

bool pmb887x_flash_blk_is_rw(pmb887x_flash_blk_t *flash) {
	return blk_supports_write_perm(flash->blk);
}

int64_t pmb887x_flash_blk_size(pmb887x_flash_blk_t *flash) {
	return blk_co_getlength(flash->blk);
}

pmb887x_flash_blk_t *pmb887x_flash_blk_self(DeviceState *dev) {
	return PMB887X_FLASH_BLK(dev);
}

static void flash_blk_realize(DeviceState *dev, Error **errp) {
	pmb887x_flash_blk_t *flash = PMB887X_FLASH_BLK(dev);
	
	if (!flash->blk) {
		EPRINTF("Property 'drive' is not set");
		exit(1);
	}
	
	DPRINTF("Drive size: %08"PRIX64"\n", blk_co_getlength(flash->blk));
	
	if (pmb887x_flash_blk_is_rw(flash)) {
		int ret = blk_set_perm(flash->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE, BLK_PERM_ALL, errp);
		if (ret < 0) {
			EPRINTF("Failed to set block dev permissions");
			exit(1);
		}
	} else {
		int ret = blk_set_perm(flash->blk, BLK_PERM_CONSISTENT_READ, BLK_PERM_ALL, errp);
		if (ret < 0) {
			EPRINTF("Failed to set block dev permissions");
			exit(1);
		}
	}
}

static const Property flash_blk_properties[] = {
	DEFINE_PROP_DRIVE("drive", pmb887x_flash_blk_t, blk),
};

static void flash_blk_class_init(ObjectClass *klass, void *data) {
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
