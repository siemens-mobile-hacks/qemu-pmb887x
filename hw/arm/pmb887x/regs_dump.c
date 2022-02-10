#include "regs_dump.h"

#include "cpu.h"

static pmb887x_module_t *find_cpu_module(pmb887x_cpu_meta_t *cpu_meta, uint32_t addr) {
	for (int i = 0; i < cpu_meta->modules_count; i++) {
		pmb887x_module_t *module = &cpu_meta->modules[i];
		if (addr >= module->base && addr <= (module->base + module->size))
			return module;
	}
	return NULL;
}

static pmb887x_module_reg_t *find_cpu_module_reg(pmb887x_module_t *module, uint32_t addr) {
	for (int i = 0; i < module->regs_count; i++) {
		pmb887x_module_reg_t *reg = &module->regs[i];
		if (addr == (module->base + reg->addr))
			return reg;
	}
	return NULL;
}

static const char *find_cpu_module_field_enum(pmb887x_module_field_t *field, uint32_t field_value) {
	for (int i = 0; i < field->values_count; i++) {
		pmb887x_module_value_t *v = &field->values[i];
		if (field_value == v->value)
			return v->name;
	}
	return NULL;
}

static const char *find_cpu_irq_num_name(pmb887x_cpu_meta_t *cpu_meta, uint32_t field_value) {
	for (int i = 0; i < cpu_meta->irqs_count; i++) {
		pmb887x_cpu_meta_irq_t *v = &cpu_meta->irqs[i];
		if (field_value == v->id)
			return v->name;
	}
	return NULL;
}

static const char *find_cpu_irq_name(pmb887x_cpu_meta_t *cpu_meta, uint32_t field_value) {
	for (int i = 0; i < cpu_meta->irqs_count; i++) {
		pmb887x_cpu_meta_irq_t *v = &cpu_meta->irqs[i];
		if (field_value == v->addr)
			return v->name;
	}
	return NULL;
}

static const char *find_cpu_gpio_name(pmb887x_cpu_meta_t *cpu_meta, uint32_t field_value) {
	for (int i = 0; i < cpu_meta->gpios_count; i++) {
		pmb887x_cpu_meta_gpio_t *v = &cpu_meta->gpios[i];
		if (field_value == v->addr)
			return v->name;
	}
	return NULL;
}

void pmb887x_dump_io(uint32_t addr, uint32_t size, uint32_t value, bool is_w) {
	pmb887x_cpu_meta_t *cpu_meta = pmb887x_get_metadata(PMB8876);
	pmb887x_module_t *module = find_cpu_module(cpu_meta, addr);
	
	if (is_w) {
		fprintf(stderr, "WRITE[%d] %08X: %08X", size, addr, value);
	} else {
		fprintf(stderr, " READ[%d] %08X: %08X", size, addr, value);
	}
	
	if (module) {
		pmb887x_module_reg_t *reg = find_cpu_module_reg(module, addr);
		if (reg) {
			if (reg->special == PMB887X_REG_IS_GPIO_PIN) {
				const char *gpio_name = find_cpu_gpio_name(cpu_meta, addr - module->base);
				if (gpio_name) {
					fprintf(stderr, " (%s)", gpio_name);
				} else {
					fprintf(stderr, " (%s_%s)", module->name, reg->name);
				}
			} else if (reg->special == PMB887X_REG_IS_IRQ_CON) {
				const char *irq_name = find_cpu_irq_name(cpu_meta, addr - module->base);
				if (irq_name) {
					fprintf(stderr, " (%s_%s_%s)",  module->name, reg->name, irq_name);
				} else {
					fprintf(stderr, " (%s_%s)", module->name, reg->name);
				}
			} else {
				fprintf(stderr, " (%s_%s)", module->name, reg->name);
			}
			
			if (reg->special == PMB887X_REG_IS_IRQ_NUM) {
				const char *irq_name = find_cpu_irq_num_name(cpu_meta, value);
				if (irq_name) {
					fprintf(stderr, ": NUM(0x%02X)=%s", value, irq_name);
				} else {
					fprintf(stderr, ": NUM(0x%02X)", value);
				}
			} else if (reg->fields_count) {
				bool first = true;
				uint32_t known_bits = 0;
				for (int i = 0; i < reg->fields_count; i++) {
					pmb887x_module_field_t *field = &reg->fields[i];
					uint32_t field_value = (value & field->mask) >> field->shift;
					const char *enum_name = find_cpu_module_field_enum(field, (value & field->mask));
					
					known_bits |= field->mask;
					
					if (!field_value && !enum_name)
						continue;
					
					if (first) {
						fprintf(stderr, ": ");
						first = false;
					} else {
						fprintf(stderr, " | ");
					}
					
					if (enum_name) {
						fprintf(stderr, "%s(%s)", field->name, enum_name);
					} else if ((field->mask >> field->shift) == 1) {
						fprintf(stderr, "%s", field->name);
					} else {
						fprintf(stderr, "%s(0x%02X)", field->name, field_value);
					}
				}
				
				uint32_t unknown_bits = (value & ~known_bits);
				if (unknown_bits) {
					for (int i = 0; i < 32; i++) {
						if ((unknown_bits & (1 << i))) {
							if (first) {
								fprintf(stderr, ": ");
								first = false;
							} else {
								fprintf(stderr, " | ");
							}
							fprintf(stderr, "UNK_%d", i);
						}
					}
				}
			}
		} else {
			fprintf(stderr, " (%s_*)", module->name);
		}
	}
	
	fprintf(stderr, " (PC: %08X, LR: %08X)\n", ARM_CPU(qemu_get_cpu(0))->env.regs[15], ARM_CPU(qemu_get_cpu(0))->env.regs[14]);
}
