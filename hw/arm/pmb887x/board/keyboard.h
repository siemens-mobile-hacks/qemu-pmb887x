#pragma once

#include "qemu/osdep.h"
#include "hw/arm/pmb887x/board/board.h"

void pmb887x_board_keymap_init(void);
bool pmb887x_board_find_keycode(const char *name, QKeyCode *qcode);
void pmb887x_board_keyboard_connect_gpios(DeviceState *keypad);
bool pmb887x_board_connect_key_to_gpio(DeviceState *keypad, const char *name, qemu_irq input);
