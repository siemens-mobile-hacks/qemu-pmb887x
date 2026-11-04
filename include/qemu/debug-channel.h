/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef QEMU_DEBUG_CHANNEL_H
#define QEMU_DEBUG_CHANNEL_H

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

typedef struct DebugChannel {
    uint8_t flags;
    char *name;
} DebugChannel;

#define DEBUG_CHANNEL_INFO(var, ...) \
    static DebugChannelInfo var = { __VA_ARGS__ }; \
    static void __attribute__((constructor)) var##_register(void) \
    { \
        debug_channel_info_register(&var); \
    }

void debug_channel_info_register(DebugChannelInfo *info);

bool debug_channels_parse(const char *spec, uint8_t flag, Error **errp);
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

#endif
