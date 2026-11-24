#pragma once

#include "qemu/osdep.h"
#include "hw/core/qdev.h"
#include "qapi/qapi-types-ui.h"
#include "tomlc17.h"

bool pmb887x_board_find_keycode(const char *name, QKeyCode *qcode);
void pmb887x_board_key_matrix_configure(DeviceState *dev, toml_datum_t table);
void pmb887x_board_key_switch_configure(DeviceState *dev, toml_datum_t table);
