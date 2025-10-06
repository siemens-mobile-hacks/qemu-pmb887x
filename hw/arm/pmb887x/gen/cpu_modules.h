#pragma once

#include "qemu/osdep.h"

typedef struct pmb887x_cpu_module_t pmb887x_cpu_module_t;
typedef struct pmb887x_cpu_module_gpio_t pmb887x_cpu_module_gpio_t;

struct pmb887x_cpu_module_t {
	const char name[64];
	uint32_t id;
	uint32_t base;
	const char dev[64];
	const int *irqs;
	size_t irqs_count;
	const pmb887x_cpu_module_gpio_t *gpios;
	size_t gpios_count;
};

struct pmb887x_cpu_module_gpio_t {
	const char name[64];
	int pin;
	int alt;
};

const pmb887x_cpu_module_t *pmb887x_cpu_get_modules_list(int cpu_id);
