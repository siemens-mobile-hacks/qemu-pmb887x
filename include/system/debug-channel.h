/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef SYSTEM_DEBUG_CHANNEL_H
#define SYSTEM_DEBUG_CHANNEL_H

#include "qemu/debug-channel.h"
#include "qemu/debug-regs.h"
#include "system/memory.h"

/*
 * The region must not call its own callbacks through mr->ops or mr->opaque.
 * After machine creation it must not be mapped yet, as for a hotplugged
 * device in realize.
 * @offset is the position of the region in the device's register map.
 */
void debug_channel_attach_mr(DebugChannel *ch, MemoryRegion *mr,
                             hwaddr offset);

void debug_channel_init_device(DebugChannel *ch, DeviceState *dev);
void debug_channel_init_device_regs(DebugChannel *ch, DeviceState *dev,
                                    const DebugRegs *regs);
void debug_channels_machine_done(void);

#endif
