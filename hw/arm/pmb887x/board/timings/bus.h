#pragma once

#include "qemu/osdep.h"
#include "system/memory.h"

void pmb887x_bus_init(DeviceState *cgu);
void pmb887x_bus_add_region(const char *name, MemoryRegion *mr);
