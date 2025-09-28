#pragma once

#include "qemu/osdep.h"

typedef struct pmb887x_cpu_module_t pmb887x_cpu_module_t;

struct pmb887x_cpu_module_t {
	const char name[64];
	const char dev[64];
	uint32_t base;
	const int irqs[32];
};

const pmb887x_cpu_module_t *pmb887x_cpu_get_modules_list(int cpu_id);
