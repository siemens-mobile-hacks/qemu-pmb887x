#pragma once
#include "qemu/osdep.h"

void pmb887x_board_gpio_init(void);
void pmb887x_board_gpio_init_fixed_inputs(void);

void pmb887x_gpio_connect(const char *gpio_out_name, const char *gpio_in_name);
qemu_irq pmb887x_gpio_get_input(const char *name);
int pmb887x_get_gpio_id_by_name(const char *name);

bool pmb887x_qdev_is_gpio_in_exists(DeviceState *dev, const char *name, int n);
bool pmb887x_qdev_is_gpio_out_exists(DeviceState *dev, const char *name, int n);
