#pragma once

#include "qemu/osdep.h"

enum {
	PMB887X_DMAC_BUS_AHB1 = 0,
	PMB887X_DMAC_BUS_AHB2,
};

typedef struct pmb887x_cpu_module_t pmb887x_cpu_module_t;
typedef struct pmb887x_cpu_module_gpio_t pmb887x_cpu_module_gpio_t;
typedef struct pmb887x_cpu_module_dma_t pmb887x_cpu_module_dma_t;

struct pmb887x_cpu_module_t {
	const char name[64];
	uint32_t id;
	uint32_t base;
	const char dev[64];

	const int *irqs;
	size_t irqs_count;

	const pmb887x_cpu_module_gpio_t *gpios;
	size_t gpios_count;

	const pmb887x_cpu_module_dma_t *dma;
	size_t dma_count;
};

struct pmb887x_cpu_module_gpio_t {
	const char name[64];
	int pin;
	int alt;
};

struct pmb887x_cpu_module_dma_t {
	const char channel[64];
	int bus;
	int request;
	int sel;
};

const pmb887x_cpu_module_t *pmb887x_cpu_get_modules_list(int cpu_id);
