#pragma once

#include "qemu/osdep.h"
#include "regs.h"

#define PMB887X_REG_IS_IRQ_NUM		1
#define PMB887X_REG_IS_GPIO_PIN		2
#define PMB887X_REG_IS_IRQ_CON		3
#define PMB887X_REG_IS_I2C_TXD		4

typedef struct {
	const char *name;
	uint32_t value;
} pmb887x_module_value_t;

typedef struct {
	const char *name;
	uint32_t mask;
	uint32_t shift;
	pmb887x_module_value_t *values;
	int values_count;
} pmb887x_module_field_t;

typedef struct {
	const char *name;
	uint32_t addr;
	pmb887x_module_field_t *fields;
	int fields_count;
	int special;
} pmb887x_module_reg_t;

typedef struct {
	const char *name;
	uint32_t base;
	uint32_t size;
	pmb887x_module_reg_t *regs;
	int regs_count;
} pmb887x_module_t;

typedef struct {
	const char *name;
	uint32_t id;
	uint32_t addr;
} pmb887x_cpu_meta_irq_t;

typedef struct {
	const char *name;
	uint32_t id;
	uint32_t addr;
} pmb887x_cpu_meta_gpio_t;

typedef struct {
	const char *name;
	
	pmb887x_cpu_meta_irq_t *irqs;
	int irqs_count;
	
	pmb887x_module_t *modules;
	int modules_count;
} pmb887x_cpu_meta_t;

typedef struct {
	const char *name;
	pmb887x_cpu_meta_t *cpu;
	
	pmb887x_cpu_meta_gpio_t *gpios;
	int gpios_count;
} pmb887x_board_meta_t;

void pmb887x_dump_io(uint32_t addr, uint32_t size, uint32_t value, bool is_w);
pmb887x_cpu_meta_t *pmb887x_get_cpu_meta(int cpu);
pmb887x_board_meta_t *pmb887x_get_board_meta(int cpu);
void pmb887x_dump_set_board(int id);
