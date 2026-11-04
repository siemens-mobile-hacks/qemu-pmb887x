/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "hw/core/qdev.h"
#include "system/runstate.h"
#include "system/system.h"
#include "system/debug-channel.h"
#include "system/tcg.h"
#include "hw/core/cpu.h"
#ifdef CONFIG_TCG
#include "accel/tcg/cpu-loop.h"
#endif

#define DEBUG_QUEUE_CAPACITY    (64 * 1024)

typedef struct DebugMr {
    MemoryRegionOps ops;
    const MemoryRegionOps *orig_ops;
    void *orig_opaque;
    DebugChannel *ch;
    MemoryRegion *mr;
} DebugMr;

static Notifier debug_exit_notifier;

static bool debug_get_io_context(MemTxAttrs attrs, uint64_t *pc, uint64_t *lr)
{
#ifdef CONFIG_TCG
    if (tcg_enabled() && current_cpu && !attrs.unspecified) {
        return cpu_get_io_context(current_cpu, pc, lr);
    }
#endif
    return false;
}

static void debug_mr_print(DebugMr *d, hwaddr addr, unsigned size,
                           uint64_t value, bool is_write, MemTxAttrs attrs)
{
    uint64_t pc = 0;
    uint64_t lr = 0;
    bool has_context = debug_get_io_context(attrs, &pc, &lr);

    debug_channel_io(d->ch, memory_region_to_absolute_addr(d->mr, addr), size,
                     value, is_write, has_context, pc, lr);
}

static MemTxResult debug_mr_read(void *opaque, hwaddr addr, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    DebugMr *d = opaque;
    MemTxResult r = MEMTX_OK;

    if (d->orig_ops->read) {
        *data = d->orig_ops->read(d->orig_opaque, addr, size);
    } else {
        r = d->orig_ops->read_with_attrs(d->orig_opaque, addr, data, size,
                                         attrs);
    }
    debug_mr_print(d, addr, size, *data, false, attrs);
    return r;
}

static MemTxResult debug_mr_write(void *opaque, hwaddr addr, uint64_t data,
                                  unsigned size, MemTxAttrs attrs)
{
    DebugMr *d = opaque;
    MemTxResult r = MEMTX_OK;

    debug_mr_print(d, addr, size, data, true, attrs);
    if (d->orig_ops->write) {
        d->orig_ops->write(d->orig_opaque, addr, data, size);
    } else {
        r = d->orig_ops->write_with_attrs(d->orig_opaque, addr, data, size,
                                          attrs);
    }
    return r;
}

static bool debug_mr_accepts(void *opaque, hwaddr addr, unsigned size,
                             bool is_write, MemTxAttrs attrs)
{
    DebugMr *d = opaque;

    return d->orig_ops->valid.accepts(d->orig_opaque, addr, size, is_write,
                                      attrs);
}

static void debug_mr_release(Object *_obj, const char *_name, void *opaque)
{
    g_free(opaque);
}

void debug_channel_attach_mr(DebugChannel *ch, MemoryRegion *mr)
{
    Object *owner = memory_region_owner(mr);

    if (!debug_channel_is_enabled(ch, DEBUG_CH_IO)) {
        return;
    }
    g_assert(!phase_check(PHASE_MACHINE_READY));
    g_assert(owner && mr->terminates && !mr->ram && !mr->alias);
    g_assert(mr->ops->read_with_attrs != debug_mr_read);

    DebugMr *d = g_new0(DebugMr, 1);
    d->ops = *mr->ops;
    d->ops.read = NULL;
    d->ops.write = NULL;
    d->ops.read_with_attrs = debug_mr_read;
    d->ops.write_with_attrs = debug_mr_write;
    if (mr->ops->valid.accepts) {
        d->ops.valid.accepts = debug_mr_accepts;
    }
    d->orig_ops = mr->ops;
    d->orig_opaque = mr->opaque;
    d->ch = ch;
    d->mr = mr;
    object_property_add(owner, "debug-channel-mr[*]", "DebugMr", NULL, NULL,
                        debug_mr_release, d);
    memory_region_set_ops(mr, &d->ops, d);
}

static void debug_channels_exit(Notifier *_notifier, void *_data)
{
    debug_channels_stop();
}

static void debug_channels_vm_state(void *_opaque, bool running,
                                    RunState _state)
{
    if (!running) {
        debug_channels_flush();
    }
}

void debug_channels_machine_done(void)
{
    debug_channels_check_unused(&error_fatal);
    if (!debug_channels_has_enabled()) {
        return;
    }
    if (!qemu_loglevel_mask(LOG_TRACE)) {
        qemu_set_log(qemu_loglevel | LOG_TRACE, &error_fatal);
    }
    debug_exit_notifier.notify = debug_channels_exit;
    qemu_add_exit_notifier(&debug_exit_notifier);
    qemu_add_vm_change_state_handler(debug_channels_vm_state, NULL);
    debug_channels_start(DEBUG_QUEUE_CAPACITY);
}
