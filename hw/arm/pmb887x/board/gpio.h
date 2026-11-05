#pragma once
#include "qemu/osdep.h"

void pmb887x_board_gpio_init(void);
void pmb887x_board_gpio_init_fixed_inputs(void);

void pmb887x_gpio_connect(const char *output_spec, const char *input_spec);
void pmb887x_qdev_connect_gpio_out(DeviceState *dev, const char *name, int n, qemu_irq gpio_in);
void pmb887x_qdev_connect_gpio_outputs(void);
qemu_irq pmb887x_gpio_get_input(const char *spec);
qemu_irq pmb887x_qdev_get_gpio_in(DeviceState *dev, const char *name, int n);
