#pragma once

#include "qemu/osdep.h"

#define PMB887X_REG_IS_IRQ_NUM		1
#define PMB887X_REG_IS_GPIO_PIN		2
#define PMB887X_REG_IS_IRQ_CON		3
#define PMB887X_REG_IS_I2C_TXD		4

typedef struct pmb887x_cpu_meta_gpio_t pmb887x_cpu_meta_gpio_t;
typedef struct pmb887x_cpu_meta_irq_t pmb887x_cpu_meta_irq_t;
typedef struct pmb887x_cpu_meta_t pmb887x_cpu_meta_t;
typedef struct pmb887x_module_t pmb887x_module_t;
typedef struct pmb887x_module_reg_t pmb887x_module_reg_t;
typedef struct pmb887x_module_field_t pmb887x_module_field_t;
typedef struct pmb887x_module_value_t pmb887x_module_value_t;

struct pmb887x_module_value_t {
	const char *name;
	uint32_t value;
};

struct pmb887x_module_field_t {
	const char *name;
	uint32_t mask;
	uint32_t shift;
	const pmb887x_module_value_t *values;
	int values_count;
};

struct pmb887x_module_reg_t {
	const char *name;
	uint32_t addr;
	const pmb887x_module_field_t *fields;
	int fields_count;
	int special;
};

struct pmb887x_module_t {
	const char *name;
	uint32_t base;
	uint32_t size;
	const pmb887x_module_reg_t *regs;
	int regs_count;
};

struct pmb887x_cpu_meta_irq_t {
	const char *name;
	uint32_t id;
	uint32_t addr;
};

struct pmb887x_cpu_meta_gpio_t {
	const char *name;
	const char *func_name;
	const char *full_name;
	uint32_t id;
};

struct pmb887x_cpu_meta_t {
	const char *name;

	const pmb887x_cpu_meta_irq_t *irqs;
	int irqs_count;

	const pmb887x_cpu_meta_gpio_t *gpios;
	int gpios_count;

	const pmb887x_module_t *modules;
	int modules_count;
};

const pmb887x_cpu_meta_t *pmb887x_get_cpu_meta(int cpu);
