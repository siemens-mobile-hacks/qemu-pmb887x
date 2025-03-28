#pragma once

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/pmb887x/boards.h"

struct pmb887x_dev {
	const char *name;
	const char *dev;
	uint32_t base;
	const int irqs[32];
};

DeviceState *pmb887x_new_dev(uint32_t cpu_type, const char *name, DeviceState *nvic);
I2CSlave *pmb887x_new_i2c_dev(const pmb887x_board_i2c_dev_t *i2c_dev);
DeviceState *pmb887x_new_lcd_dev(const char *name);
I2CBus *pmb887x_i2c_bus(DeviceState *dev);
