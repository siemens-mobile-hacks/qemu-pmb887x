#pragma once

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"

struct pmb887x_dev {
	const char *name;
	const char *dev;
	uint32_t base;
	const uint32_t irqs[32];
};

DeviceState *pmb887x_new_dev(uint32_t cpu_type, const char *name, DeviceState *nvic);
I2CSlave *pmb887x_new_pmic_dev(const char *type, uint8_t addr);
DeviceState *pmb887x_new_lcd_dev(const char *name);
