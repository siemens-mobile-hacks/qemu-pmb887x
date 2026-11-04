/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "qemu/debug-channel.h"

#define DEBUG_MERGE_WINDOW_NS   (100 * SCALE_MS)
#define DEBUG_POLL_MS           10
#define DEBUG_BATCH             256

typedef enum DebugRecordKind {
    DEBUG_RECORD_TEXT,
    DEBUG_RECORD_IO,
} DebugRecordKind;

typedef struct DebugRecord {
    DebugRecordKind kind;
    const DebugChannel *ch;
    int64_t time_ns;
    DebugIoAccess io;
    char *text;
} DebugRecord;

static struct {
    QemuMutex lock;
    QemuCond ready;
    QemuCond space;
    QemuThread thread;
    DebugRecord *ring;
    size_t capacity;
    size_t head;
    size_t count;
    uint64_t pushed;
    uint64_t written;
    unsigned flushing;
    bool running;
    bool stopping;
    bool print_time;
    DebugClockFunc clock;
    DebugOutputFunc output;
    void *output_opaque;
} dq;

static void __attribute__((constructor)) debug_queue_init(void)
{
    qemu_mutex_init(&dq.lock);
    qemu_cond_init(&dq.ready);
    qemu_cond_init(&dq.space);
}

void debug_channels_set_clock(DebugClockFunc clock)
{
    dq.clock = clock;
}

void debug_channels_set_output(DebugOutputFunc output, void *opaque)
{
    dq.output = output;
    dq.output_opaque = opaque;
}

void debug_channels_set_time(bool on)
{
    dq.print_time = on;
}

static int64_t debug_now(void)
{
    return dq.clock ? dq.clock() : 0;
}

static void debug_write(const GString *out)
{
    if (dq.output) {
        dq.output(out->str, out->len, dq.output_opaque);
        return;
    }
    FILE *f = qemu_log_trylock();
    if (f) {
        fwrite(out->str, 1, out->len, f);
    }
    qemu_log_unlock(f);
}

static void debug_print_io_default(const char *name, const DebugIoAccess *io,
                                   GString *out)
{
    g_string_append_printf(out, "[%s] %s[%u] %08" PRIX64 ": %0*" PRIX64,
                           name, io->is_write ? "WRITE" : " READ", io->size,
                           io->addr, io->size * 2, io->value);
    if (io->has_context) {
        g_string_append_printf(out, " (PC: %08" PRIX64 ", LR: %08" PRIX64 ")",
                               io->pc, io->lr);
    }
}

static void debug_format(const DebugRecord *rec, GString *out)
{
    if (dq.print_time && dq.clock) {
        int64_t seconds = rec->time_ns / NANOSECONDS_PER_SECOND;
        int64_t ns = rec->time_ns % NANOSECONDS_PER_SECOND;

        g_string_append_printf(out, "[%4" PRId64 ".%09" PRId64 "] ", seconds,
                               ns);
    }
    const DebugChannel *ch = rec->ch;

    if (rec->kind == DEBUG_RECORD_TEXT) {
        g_string_append_printf(out, "[%s]: %s", ch->name, rec->text);
        return;
    }
    if (ch->io_format) {
        ch->io_format->print(ch->name, ch->io_format_opaque, &rec->io, out);
    } else {
        debug_print_io_default(ch->name, &rec->io, out);
    }
    if (rec->io.count > 1) {
        g_string_append_printf(out, " [x%" PRIu64 "]", rec->io.count);
    }
    g_string_append_c(out, '\n');
}

static void debug_print_now(DebugRecord *rec)
{
    g_autoptr(GString) out = g_string_new(NULL);

    debug_format(rec, out);
    debug_write(out);
    g_clear_pointer(&rec->text, g_free);
}

static DebugRecord *debug_ring_at(size_t index)
{
    return &dq.ring[(dq.head + index) % dq.capacity];
}

static bool debug_head_is_held(void)
{
    const DebugRecord *rec = debug_ring_at(0);

    if (dq.count > 1 || dq.flushing || dq.stopping) {
        return false;
    }
    return rec->kind == DEBUG_RECORD_IO &&
           debug_now() < rec->time_ns + DEBUG_MERGE_WINDOW_NS;
}

static void *debug_output_thread(void *_opaque)
{
    g_autofree DebugRecord *batch = g_new(DebugRecord, DEBUG_BATCH);
    g_autoptr(GString) out = g_string_new(NULL);

    qemu_mutex_lock(&dq.lock);
    for (;;) {
        size_t n = 0;

        while (!dq.count || debug_head_is_held()) {
            if (dq.stopping && !dq.count) {
                dq.running = false;
                qemu_cond_broadcast(&dq.space);
                qemu_mutex_unlock(&dq.lock);
                return NULL;
            }
            if (dq.count) {
                qemu_cond_timedwait(&dq.ready, &dq.lock, DEBUG_POLL_MS);
            } else {
                qemu_cond_wait(&dq.ready, &dq.lock);
            }
        }
        while (n < DEBUG_BATCH && dq.count && !debug_head_is_held()) {
            batch[n++] = *debug_ring_at(0);
            dq.head = (dq.head + 1) % dq.capacity;
            dq.count--;
        }
        qemu_cond_broadcast(&dq.space);
        qemu_mutex_unlock(&dq.lock);

        g_string_truncate(out, 0);
        for (size_t i = 0; i < n; i++) {
            debug_format(&batch[i], out);
            g_free(batch[i].text);
        }
        debug_write(out);

        qemu_mutex_lock(&dq.lock);
        dq.written += n;
        qemu_cond_broadcast(&dq.space);
    }
}

void debug_channels_start(size_t capacity)
{
    g_assert(capacity);

    qemu_mutex_lock(&dq.lock);
    g_assert(!dq.running);
    dq.ring = g_new0(DebugRecord, capacity);
    dq.capacity = capacity;
    dq.head = 0;
    dq.count = 0;
    dq.pushed = 0;
    dq.written = 0;
    dq.flushing = 0;
    dq.stopping = false;
    dq.running = true;
    qemu_mutex_unlock(&dq.lock);
    qemu_thread_create(&dq.thread, "debug-channels", debug_output_thread,
                       NULL, QEMU_THREAD_JOINABLE);
}

void debug_channels_flush(void)
{
    qemu_mutex_lock(&dq.lock);
    uint64_t target = dq.pushed;
    if (dq.running) {
        dq.flushing++;
        qemu_cond_signal(&dq.ready);
        while (dq.running && dq.written < target) {
            qemu_cond_wait(&dq.space, &dq.lock);
        }
        dq.flushing--;
    }
    qemu_mutex_unlock(&dq.lock);
}

void debug_channels_stop(void)
{
    qemu_mutex_lock(&dq.lock);
    if (!dq.running || dq.stopping) {
        qemu_mutex_unlock(&dq.lock);
        return;
    }
    dq.stopping = true;
    qemu_cond_signal(&dq.ready);
    qemu_mutex_unlock(&dq.lock);
    qemu_thread_join(&dq.thread);

    qemu_mutex_lock(&dq.lock);
    dq.stopping = false;
    g_clear_pointer(&dq.ring, g_free);
    qemu_mutex_unlock(&dq.lock);
}

static bool debug_io_is_repeat(const DebugRecord *last,
                               const DebugRecord *rec)
{
    return rec->kind == DEBUG_RECORD_IO && last->kind == DEBUG_RECORD_IO &&
           last->ch == rec->ch &&
           last->io.addr == rec->io.addr && last->io.value == rec->io.value &&
           last->io.size == rec->io.size &&
           last->io.is_write == rec->io.is_write &&
           last->io.has_context == rec->io.has_context &&
           last->io.pc == rec->io.pc && last->io.lr == rec->io.lr &&
           rec->time_ns < last->time_ns + DEBUG_MERGE_WINDOW_NS;
}

static void debug_push(DebugRecord *rec)
{
    qemu_mutex_lock(&dq.lock);
    DebugRecord *last = dq.count ? debug_ring_at(dq.count - 1) : NULL;
    if (last && debug_io_is_repeat(last, rec)) {
        last->io.count++;
        qemu_mutex_unlock(&dq.lock);
        return;
    }
    while (dq.running && dq.count == dq.capacity) {
        qemu_cond_wait(&dq.space, &dq.lock);
    }
    if (!dq.running) {
        qemu_mutex_unlock(&dq.lock);
        debug_print_now(rec);
        return;
    }
    *debug_ring_at(dq.count) = *rec;
    dq.count++;
    dq.pushed++;
    qemu_cond_signal(&dq.ready);
    qemu_mutex_unlock(&dq.lock);
}

static void debug_record_init(DebugRecord *rec, DebugRecordKind kind,
                              const DebugChannel *ch)
{
    *rec = (DebugRecord) {
        .kind = kind,
        .ch = ch,
        .time_ns = debug_now(),
    };
}

void debug_channel_log(const DebugChannel *ch, const char *fmt, ...)
{
    DebugRecord rec;
    va_list ap;

    debug_record_init(&rec, DEBUG_RECORD_TEXT, ch);
    va_start(ap, fmt);
    rec.text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    debug_push(&rec);
}

void debug_channel_io(const DebugChannel *ch, uint64_t addr, uint8_t size,
                      uint64_t value, bool is_write, bool has_context,
                      uint64_t pc, uint64_t lr)
{
    DebugRecord rec;

    debug_record_init(&rec, DEBUG_RECORD_IO, ch);
    rec.io = (DebugIoAccess) {
        .addr = addr,
        .value = value,
        .pc = pc,
        .lr = lr,
        .count = 1,
        .size = size,
        .is_write = is_write,
        .has_context = has_context,
    };
    debug_push(&rec);
}
