#pragma once
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"

I2CBus *pmb887x_i2c_v1_bus(DeviceState *dev);
