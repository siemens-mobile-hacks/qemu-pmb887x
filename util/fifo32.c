/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qemu/fifo32.h"

const VMStateDescription vmstate_fifo32 = {
    .name = "Fifo32",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VARRAY_UINT32(data, Fifo32, capacity, 1, vmstate_info_uint32,
                              uint32_t),
        VMSTATE_UINT32(head, Fifo32),
        VMSTATE_UINT32(num, Fifo32),
        VMSTATE_END_OF_LIST()
    }
};
