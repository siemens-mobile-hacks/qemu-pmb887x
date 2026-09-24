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
#include "qemu/timer.h"
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
	GArray *queue;
	guint qpos;
	QEMUIOVector qiov;
	QEMUBH *flush_bh;
	QEMUTimer *flush_timer;
	bool inflight;
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
#define FLASH_BLK_FLUSH_MS	50
#define FLASH_BLK_GAP	(64 * 1024)

static void flash_blk_write_range(pmb887x_flash_blk_t *flash, flash_blk_dirty_t *d) {
	int ret = blk_pwrite(flash->blk, d->offset, d->size, d->src, 0);
	if (ret < 0) {
		EPRINTF("Can't write to flash file: %d, %s", ret, strerror(-ret));
		exit(1);
	}
}

static void flash_blk_write_sync(pmb887x_flash_blk_t *flash) {
	for (; flash->qpos < flash->queue->len; flash->qpos++)
		flash_blk_write_range(flash, &g_array_index(flash->queue, flash_blk_dirty_t, flash->qpos));
	for (guint i = 0; i < flash->dirty->len; i++)
		flash_blk_write_range(flash, &g_array_index(flash->dirty, flash_blk_dirty_t, i));
	g_array_set_size(flash->dirty, 0);
}

static void flash_blk_flush_arm(pmb887x_flash_blk_t *flash) {
	timer_mod(flash->flush_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + FLASH_BLK_FLUSH_MS);
}

static void flash_blk_write_done(void *opaque, int ret) {
	pmb887x_flash_blk_t *flash = opaque;

	if (ret < 0) {
		EPRINTF("Can't write to flash file: %d, %s", ret, strerror(-ret));
		exit(1);
	}
	qemu_iovec_destroy(&flash->qiov);
	flash->inflight = false;
	if (flash->qpos < flash->queue->len)
		qemu_bh_schedule(flash->flush_bh);
	else if (flash->dirty->len)
		flash_blk_flush_arm(flash);
}

static int flash_blk_dirty_cmp(const void *a, const void *b) {
	const flash_blk_dirty_t *x = a, *y = b;
	return x->offset < y->offset ? -1 : x->offset > y->offset;
}

/*
 * Sorted and merged across gaps of up to FLASH_BLK_GAP: the storage array
 * holds the gap's bytes too, so writing them is exact.
 */
static void flash_blk_take_dirty(pmb887x_flash_blk_t *flash) {
	GArray *q = flash->dirty;
	guint n = 0;

	flash->dirty = flash->queue;
	g_array_set_size(flash->dirty, 0);
	flash->queue = q;
	flash->qpos = 0;
	qsort(q->data, q->len, sizeof(flash_blk_dirty_t), flash_blk_dirty_cmp);
	for (guint i = 0; i < q->len; i++) {
		flash_blk_dirty_t *d = &g_array_index(q, flash_blk_dirty_t, i);
		flash_blk_dirty_t *m = n ? &g_array_index(q, flash_blk_dirty_t, n - 1) : NULL;
		if (m && d->offset <= m->offset + m->size + FLASH_BLK_GAP && d->src - d->offset == m->src - m->offset)
			m->size = MAX(m->size, d->offset + d->size - m->offset);
		else
			g_array_index(q, flash_blk_dirty_t, n++) = *d;
	}
	g_array_set_size(q, n);
}

/*
 * Asynchronous, one request in flight (a batch can hold hundreds of ranges,
 * and each concurrent request holds a coroutine stack: all at once ran the
 * heap out of memory).  A synchronous blk_pwrite() here
 * polls the thread pool with the BQL held, so every flash MMIO the vCPU made
 * meanwhile waited for the file write: through a KE970 boot the main loop
 * sat in this BH ~240 ms of every second and the vCPU 230-370 ms in the BQL.
 * The first write of a batch arms a FLASH_BLK_FLUSH_MS timer instead of
 * flushing at once: a boot programs ~80k words/s, and flushing each batch
 * as it appeared cost the main loop a block request per ~30 words.
 * Ranges dirtied while a batch is being written wait for all of it, so a
 * later write of a range always lands after an earlier one and the file
 * converges on the storage.
 */
static void flash_blk_flush_bh(void *opaque) {
	pmb887x_flash_blk_t *flash = opaque;

	if (flash->inflight)
		return;
	if (flash->qpos >= flash->queue->len) {
		if (!flash->dirty->len)
			return;
		flash_blk_take_dirty(flash);
	}
	flash_blk_dirty_t *d = &g_array_index(flash->queue, flash_blk_dirty_t, flash->qpos++);
	flash->inflight = true;
	qemu_iovec_init_buf(&flash->qiov, (void *) d->src, d->size);
	blk_aio_pwritev(flash->blk, d->offset, &flash->qiov, 0, flash_blk_write_done, flash);
}

/* the coroutine must start from a BH: timerlist_run_timers() is not on the Asyncify onlylist */
static void flash_blk_flush_timer(void *opaque) {
	pmb887x_flash_blk_t *flash = opaque;

	qemu_bh_schedule(flash->flush_bh);
}

static void flash_blk_vm_state(void *opaque, bool running, RunState state) {
	pmb887x_flash_blk_t *flash = opaque;

	if (!running) {
		blk_drain(flash->blk);
		flash_blk_write_sync(flash);
	}
}

int pmb887x_flash_blk_pwrite(pmb887x_flash_blk_t *flash, int64_t offset, int64_t size, void *value) {
	flash_blk_dirty_t *last = flash->dirty->len ?
		&g_array_index(flash->dirty, flash_blk_dirty_t, flash->dirty->len - 1) : NULL;
	/*
	 * A non-empty list always has its flush pending: either the timer is
	 * armed, or a batch is being written and its last completion arms it
	 * (the BH swaps the list out before writing it, and flash MMIO runs
	 * under the BQL).  Re-arming a pending timer or BH is not a no-op: it
	 * still wakes the main loop, per programmed word -- ~80k/s through a
	 * KE970 boot.
	 */
	bool schedule = !last;

	if (last && offset == last->offset + last->size && (const uint8_t *) value == last->src + last->size) {
		last->size += size;
	} else if (last && offset >= last->offset && offset + size <= last->offset + last->size &&
			(const uint8_t *) value == last->src + (offset - last->offset)) {
		/* rewrite inside the pending range: the storage already has it */
	} else {
		flash_blk_dirty_t d = { .offset = offset, .size = size, .src = value };
		g_array_append_val(flash->dirty, d);
	}
	if (schedule)
		flash_blk_flush_arm(flash);
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
	flash->queue = g_array_new(false, false, sizeof(flash_blk_dirty_t));
	flash->flush_bh = qemu_bh_new(flash_blk_flush_bh, flash);
	flash->flush_timer = timer_new_ms(QEMU_CLOCK_REALTIME, flash_blk_flush_timer, flash);
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
