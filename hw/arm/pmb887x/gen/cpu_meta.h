#pragma once

#include "qemu/osdep.h"

#define PMB887X_REG_IS_IRQ_NUM		1
#define PMB887X_REG_IS_GPIO_PIN		2
#define PMB887X_REG_IS_IRQ_CON		3
#define PMB887X_REG_IS_I2C_TXD		4

typedef struct pmb887x_cpu_meta_gpio_t pmb887x_cpu_meta_gpio_t;
typedef struct pmb887x_cpu_meta_irq_t pmb887x_cpu_meta_irq_t;
typedef struct pmb887x_cpu_meta_t pmb887x_cpu_meta_t;
typedef struct pmb887x_cpu_io_t pmb887x_cpu_io_t;
typedef struct pmb887x_io_meta_t pmb887x_io_meta_t;
typedef struct pmb887x_io_reg_t pmb887x_io_reg_t;
typedef struct pmb887x_io_field_t pmb887x_io_field_t;
typedef struct pmb887x_io_value_t pmb887x_io_value_t;

typedef enum pmb887x_trace_io_t {
	PMB887X_TRACE_IO_CPU,
	PMB887X_TRACE_IO_DSP,
	PMB887X_TRACE_IO_D1094XX,
	PMB887X_TRACE_IO_D1601XX,
	PMB887X_TRACE_IO_JBT6K71,
	PMB887X_TRACE_IO_PASIC,
	PMB887X_TRACE_IO_PCF8833,
	PMB887X_TRACE_IO_PCF8882,
	PMB887X_TRACE_IO_PMB6812,
	PMB887X_TRACE_IO_SSD1286,
	PMB887X_TRACE_IO_TEA5760UK,
	PMB887X_TRACE_IO_TEA5761UK,
	PMB887X_TRACE_IO_COUNT,
} pmb887x_trace_io_t;

struct pmb887x_io_value_t {
	const char *name;
	uint32_t value;
};

struct pmb887x_io_field_t {
	const char *name;
	uint32_t mask;
	uint32_t shift;
	const pmb887x_io_value_t *values;
	int values_count;
};

struct pmb887x_io_reg_t {
	const char *name;
	uint32_t addr;
	const pmb887x_io_field_t *fields;
	int fields_count;
	int special;
};

struct pmb887x_cpu_io_t {
	const char *name;
	uint32_t base;
	uint32_t size;
	const pmb887x_io_reg_t *regs;
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

	const pmb887x_cpu_io_t *modules;
	int modules_count;

	const pmb887x_cpu_io_t *dsp_modules;
	int dsp_modules_count;
};

struct pmb887x_io_meta_t {
	const char *name;
	const pmb887x_io_reg_t *regs;
	int regs_count;
};

const pmb887x_cpu_meta_t *pmb887x_get_cpu_meta(int cpu);
const pmb887x_io_meta_t *pmb887x_get_io_meta(pmb887x_trace_io_t id);
