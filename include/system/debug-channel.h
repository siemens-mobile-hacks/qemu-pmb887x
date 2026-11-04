/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef SYSTEM_DEBUG_CHANNEL_H
#define SYSTEM_DEBUG_CHANNEL_H

#include "qemu/debug-channel.h"
#include "system/memory.h"

/* The region must not call its own callbacks through mr->ops or mr->opaque */
void debug_channel_attach_mr(DebugChannel *ch, MemoryRegion *mr);

void debug_channels_machine_done(void);

#endif
