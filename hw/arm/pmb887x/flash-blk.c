#define PMB887X_TRACE_ID		FLASH
#define PMB887X_TRACE_PREFIX	"pmb887x-flash-blk"

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "block/block_int-common.h"
#include "hw/core/sysbus.h"
#include "system/block-backend.h"
#include "system/runstate.h"
#include "qemu/main-loop.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/flash-blk.h"

#define TYPE_PMB887X_FLASH_BLK	"pmb887x-flash-blk"
#define PMB887X_FLASH_BLK(obj)	OBJECT_CHECK(struct pmb887x_flash_blk_t, (obj), TYPE_PMB887X_FLASH_BLK)

typedef struct {
	int64_t offset;
	int64_t size;
	const uint8_t *src;
} flash_blk_dirty_t;

struct pmb887x_flash_blk_t {
	SysBusDevice parent_obj;
	DeviceState *dev;
	BlockBackend *blk;
	/* write-behind (wasm): dirty ranges flushed by a main-loop BH */
	GArray *dirty;
	QEMUBH *flush_bh;
	VMChangeStateEntry *vmstate;
};

int pmb887x_flash_blk_pread(pmb887x_flash_blk_t *flash, int64_t offset, int64_t size, void *storage) {
	return blk_pread(flash->blk, offset, size, storage, 0);
}

#ifdef __EMSCRIPTEN__
/*
 * A blk_pwrite() from the vCPU thread (the flash MMIO write handler) runs
 * a block-layer coroutine there: on wasm the coroutine switch unwinds the
 * stack with Asyncify, and the JIT'd TB frame plus the invoke_* wrapper
 * under the helper cannot be instrumented, so the vCPU derailed at the
 * first flash write in rw mode.  Record the range instead and write it
 * from a main-loop bottom half (the coroutine-capable thread); the
 * storage array always holds the latest data, so writing later is exact.
 */
static void flash_blk_flush(pmb887x_flash_blk_t *flash) {
	GArray *dirty = flash->dirty;

	if (!dirty->len)
		return;
	flash->dirty = g_array_new(false, false, sizeof(flash_blk_dirty_t));
	for (guint i = 0; i < dirty->len; i++) {
		flash_blk_dirty_t *d = &g_array_index(dirty, flash_blk_dirty_t, i);
		int ret = blk_pwrite(flash->blk, d->offset, d->size, d->src, 0);
		if (ret < 0) {
			EPRINTF("Can't write to flash file: %d, %s", ret, strerror(-ret));
			exit(1);
		}
	}
	g_array_free(dirty, true);
}

static void flash_blk_flush_bh(void *opaque) {
	flash_blk_flush(opaque);
}

static void flash_blk_vm_state(void *opaque, bool running, RunState state) {
	if (!running)
		flash_blk_flush(opaque);
}

int pmb887x_flash_blk_pwrite(pmb887x_flash_blk_t *flash, int64_t offset, int64_t size, void *value) {
	flash_blk_dirty_t *last = flash->dirty->len ?
		&g_array_index(flash->dirty, flash_blk_dirty_t, flash->dirty->len - 1) : NULL;

	if (last && offset == last->offset + last->size && (const uint8_t *) value == last->src + last->size) {
		last->size += size;
	} else if (last && offset >= last->offset && offset + size <= last->offset + last->size &&
			(const uint8_t *) value == last->src + (offset - last->offset)) {
		/* rewrite inside the pending range: the storage already has it */
	} else {
		flash_blk_dirty_t d = { .offset = offset, .size = size, .src = value };
		g_array_append_val(flash->dirty, d);
	}
	qemu_bh_schedule(flash->flush_bh);
	return 0;
}
#else
int pmb887x_flash_blk_pwrite(pmb887x_flash_blk_t *flash, int64_t offset, int64_t size, void *value) {
	return blk_pwrite(flash->blk, offset, size, value, 0);
}
#endif

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
	
	if (!flash->blk) {
		EPRINTF("Property 'drive' is not set");
		exit(1);
	}
	
	DPRINTF("Drive size: %08"PRIX64"\n", blk_co_getlength(flash->blk));

#ifdef __EMSCRIPTEN__
	flash->dirty = g_array_new(false, false, sizeof(flash_blk_dirty_t));
	flash->flush_bh = qemu_bh_new(flash_blk_flush_bh, flash);
	flash->vmstate = qemu_add_vm_change_state_handler(flash_blk_vm_state, flash);
#endif

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
