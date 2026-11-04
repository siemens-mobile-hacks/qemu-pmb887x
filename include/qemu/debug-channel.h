/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef QEMU_DEBUG_CHANNEL_H
#define QEMU_DEBUG_CHANNEL_H

#include "qemu/log.h"

enum {
    DEBUG_CH_LOG = 1 << 0,
    DEBUG_CH_IO  = 1 << 1,
};

typedef struct DebugChannelInfo {
    const char *name;
    const char *const *aliases;
    const char *const *groups;
    bool not_in_all;
} DebugChannelInfo;

typedef struct DebugIoAccess {
    uint64_t addr;
    uint64_t offset;
    uint64_t value;
    uint64_t pc;
    uint64_t lr;
    uint64_t count;
    uint8_t size;
    bool is_write;
    bool has_context;
} DebugIoAccess;

/*
 * Runs on the output thread while a vCPU may hold the BQL and wait for queue
 * space: it must not take the BQL or print to a debug channel.
 */
typedef struct DebugIoFormat {
    void (*print)(const char *name, const void *opaque,
                  const DebugIoAccess *io, GString *out);
} DebugIoFormat;

void debug_io_print_access(const char *name, const DebugIoAccess *io,
                           int addr_digits, GString *out);
void debug_io_print_context(const DebugIoAccess *io, GString *out);

typedef struct DebugChannel {
    uint8_t flags;
    char *name;
    const DebugIoFormat *io_format;
    const void *io_format_opaque;
} DebugChannel;

#define DEBUG_CHANNEL_INFO(var, ...) \
    static DebugChannelInfo var = { __VA_ARGS__ }; \
    static void __attribute__((constructor)) var##_register(void) \
    { \
        debug_channel_info_register(&var); \
    }

void debug_channel_info_register(DebugChannelInfo *info);

bool debug_channels_parse(const char *spec, uint8_t flag, Error **errp);
bool debug_channels_parse_option(const char *str, Error **errp);
bool debug_channels_check_unused(Error **errp);
bool debug_channels_has_enabled(void);
void debug_channels_reset(void);

void debug_channel_init(DebugChannel *ch, const DebugChannelInfo *info,
                        const char *instance);
void debug_channel_cleanup(DebugChannel *ch);

static inline bool debug_channel_is_enabled(const DebugChannel *ch,
                                            uint8_t flag)
{
    return ch->flags & flag;
}

typedef int64_t (*DebugClockFunc)(void);
typedef void (*DebugOutputFunc)(const char *buf, size_t len, void *opaque);

void debug_channels_set_clock(DebugClockFunc clock);
void debug_channels_set_output(DebugOutputFunc output, void *opaque);
void debug_channels_set_time(bool on);
void debug_channels_start(size_t capacity);
void debug_channels_flush(void);
void debug_channels_stop(void);

void __attribute__((cold, noinline))
debug_channel_log(const DebugChannel *ch, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);
void __attribute__((cold, noinline))
debug_channel_io(const DebugChannel *ch, uint64_t addr, uint64_t offset,
                 uint8_t size, uint64_t value, bool is_write, bool has_context,
                 uint64_t pc, uint64_t lr);

#define DEBUG_LOG(ch, fmt, ...) do { \
        if (unlikely(debug_channel_is_enabled(ch, DEBUG_CH_LOG))) { \
            debug_channel_log(ch, fmt, ## __VA_ARGS__); \
        } \
    } while (0)

#define DEBUG_UNIMP(ch, fmt, ...) do { \
        if (unlikely(qemu_loglevel_mask(LOG_UNIMP))) { \
            debug_channel_log(ch, fmt, ## __VA_ARGS__); \
        } \
    } while (0)

#define DEBUG_GUEST_ERROR(ch, fmt, ...) do { \
        if (unlikely(qemu_loglevel_mask(LOG_GUEST_ERROR))) { \
            debug_channel_log(ch, fmt, ## __VA_ARGS__); \
        } \
    } while (0)

#define DEBUG_IO_ACCESS(ch, addr, offset, size, value, is_write, has_context, \
                        pc, lr) \
    do { \
        if (unlikely(debug_channel_is_enabled(ch, DEBUG_CH_IO))) { \
            debug_channel_io(ch, addr, offset, size, value, is_write, \
                             has_context, pc, lr); \
        } \
    } while (0)

#define DEBUG_IO_READ(ch, addr, size, value) \
    DEBUG_IO_ACCESS(ch, addr, addr, size, value, false, false, 0, 0)
#define DEBUG_IO_WRITE(ch, addr, size, value) \
    DEBUG_IO_ACCESS(ch, addr, addr, size, value, true, false, 0, 0)

#endif
