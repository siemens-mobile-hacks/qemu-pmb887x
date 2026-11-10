#pragma once

#include "qemu/osdep.h"

void pmb887x_board_init_devices(DeviceState *ebuc);
Object *pmb887x_board_find_bus(GHashTable *devices, const char *name);
