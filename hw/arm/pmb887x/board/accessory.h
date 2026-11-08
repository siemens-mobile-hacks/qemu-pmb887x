#pragma once

#include "qemu/osdep.h"

#include "tomlc17.h"

void pmb887x_board_accessory_configure(GHashTable *devices, DeviceState *dev, toml_datum_t table);
