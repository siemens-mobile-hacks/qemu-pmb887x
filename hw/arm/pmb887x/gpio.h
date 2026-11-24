#pragma once

#include "qemu/osdep.h"
#include "hw/core/qdev.h"
#include "hw/arm/pmb887x/gen/cpu_modules.h"

void pmb887x_gpio_set_input_pull(DeviceState *dev, int pin, int alt, enum pmb887x_gpio_pull pull);
