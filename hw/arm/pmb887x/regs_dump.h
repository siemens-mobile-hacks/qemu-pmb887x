#pragma once

#include "qemu/osdep.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/boards.h"

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
	const pmb887x_module_value_t *values;
	int values_count;
} pmb887x_module_field_t;

typedef struct {
	const char *name;
	uint32_t addr;
	const pmb887x_module_field_t *fields;
	int fields_count;
	int special;
} pmb887x_module_reg_t;

typedef struct {
	const char *name;
	uint32_t base;
	uint32_t size;
	const pmb887x_module_reg_t *regs;
	int regs_count;
} pmb887x_module_t;

typedef struct {
	const char *name;
	uint32_t id;
	uint32_t addr;
} pmb887x_cpu_meta_irq_t;

typedef struct {
	const char *name;
	const char *func_name;
	const char *full_name;
	uint32_t id;
} pmb887x_cpu_meta_gpio_t;

typedef struct {
	const char *name;
	
	const pmb887x_cpu_meta_irq_t *irqs;
	int irqs_count;
	
	const pmb887x_cpu_meta_gpio_t *gpios;
	int gpios_count;
	
	const pmb887x_module_t *modules;
	int modules_count;
} pmb887x_cpu_meta_t;

void pmb887x_dump_io(uint32_t addr, uint32_t size, uint32_t value, bool is_write);
void pmb887x_print_dump_io(uint32_t addr, uint32_t size, uint32_t value, bool is_write, uint32_t pc, uint32_t lr);
const pmb887x_cpu_meta_t *pmb887x_get_cpu_meta(int cpu);
void pmb887x_io_dump_init(const pmb887x_board_t *board);
