/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PMB887X-WEB WASM PORT (porting necessity), a file of the fork: the block
 * layer runs coroutines, which only the main loop thread can (asyncify), and
 * a card controller calls the card from the vCPU thread (MMIO, its transfer
 * timers). There sd.c's block I/O runs here: as a BH on the main loop, while
 * the vCPU waits with the BQL dropped. The main loop may unbind the card
 * meanwhile (sd-slot "inserted"): the card and its BlockBackend are held
 * until the BH is done.
 */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qom/object.h"
#include "sd-wasm.h"

typedef struct {
    BlockBackend *blk;
    int64_t addr;
    int64_t len;
    uint8_t *buf;
    bool is_write;
    int ret;
    QemuEvent done;
} SDWasmBlkIO;

static void sd_wasm_blk_io_bh(void *opaque)
{
    SDWasmBlkIO *io = opaque;

    io->ret = io->is_write ? blk_pwrite(io->blk, io->addr, io->len, io->buf, 0)
                           : blk_pread(io->blk, io->addr, io->len, io->buf, 0);
    qemu_event_set(&io->done);
}

int sd_wasm_blk_io(Object *owner, BlockBackend *blk, int64_t addr, int64_t len,
                   uint8_t *buf, bool is_write)
{
    SDWasmBlkIO io = { .blk = blk, .addr = addr, .len = len, .buf = buf,
                       .is_write = is_write };
    bool locked = bql_locked();
    bool lazy = false;

    object_ref(owner);
    blk_ref(blk);
    qemu_event_init(&io.done, false);
    aio_bh_schedule_oneshot(qemu_get_aio_context(), sd_wasm_blk_io_bh, &io);
    if (locked) {
        lazy = bql_unlock_for_wait();
    }
    qemu_event_wait(&io.done);
    if (locked) {
        bql_lock_after_wait(lazy);
    }
    qemu_event_destroy(&io.done);
    blk_unref(blk);
    object_unref(owner);
    return io.ret;
}
