/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef SYSTEM_DEBUG_CHANNEL_H
#define SYSTEM_DEBUG_CHANNEL_H

#include "qemu/debug-channel.h"
#include "system/memory.h"

/*
 * The region must not call its own callbacks through mr->ops or mr->opaque.
 * @offset is the position of the region in the device's register map.
 */
void debug_channel_attach_mr(DebugChannel *ch, MemoryRegion *mr,
                             hwaddr offset);

void debug_channels_machine_done(void);

#endif
