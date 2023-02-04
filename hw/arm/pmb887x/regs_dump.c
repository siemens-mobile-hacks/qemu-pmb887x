#include "regs_dump.h"

#include "target/arm/cpu.h"
#include "qemu/log.h"
#include "sysemu/cpu-timers.h"

static QemuMutex io_dump_queue_lock;
static GQueue *io_dump_queue = NULL;
static QemuThread io_dump_thread_id;
static GCond io_dump_cond = {};
static pmb887x_board_meta_t *current_board = NULL;

typedef struct {
	uint32_t addr;
	uint32_t value;
	uint8_t size;
	uint32_t pc;
	uint32_t lr;
	bool is_write;
} pmb887x_io_operation_t;

static pmb887x_module_t *find_cpu_module(pmb887x_cpu_meta_t *cpu, uint32_t addr) {
	for (int i = 0; i < cpu->modules_count; i++) {
		pmb887x_module_t *module = &cpu->modules[i];
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

static const char *find_cpu_irq_num_name(pmb887x_cpu_meta_t *cpu, uint32_t field_value) {
	for (int i = 0; i < cpu->irqs_count; i++) {
		pmb887x_cpu_meta_irq_t *v = &cpu->irqs[i];
		if (field_value == v->id)
			return v->name;
	}
	return NULL;
}

static const char *find_cpu_irq_name(pmb887x_cpu_meta_t *cpu, uint32_t field_value) {
	for (int i = 0; i < cpu->irqs_count; i++) {
		pmb887x_cpu_meta_irq_t *v = &cpu->irqs[i];
		if (field_value == v->addr)
			return v->name;
	}
	return NULL;
}

static const char *find_cpu_gpio_name(pmb887x_board_meta_t *board, uint32_t field_value) {
	for (int i = 0; i < board->gpios_count; i++) {
		pmb887x_cpu_meta_gpio_t *v = &board->gpios[i];
		if (field_value == v->addr)
			return v->name;
	}
	return NULL;
}

static void *_dump_io_thread(void *arg) {
	while (true) {
		pmb887x_io_operation_t *entry = NULL;
		size_t queue_size;
		
		qemu_mutex_lock(&io_dump_queue_lock);
		queue_size = g_queue_get_length(io_dump_queue);
		if (queue_size > 0)
			entry = g_queue_pop_head(io_dump_queue);
		qemu_mutex_unlock(&io_dump_queue_lock);
		
		if (entry) {
			pmb887x_print_dump_io(entry->addr, entry->size, entry->value, entry->is_write, entry->pc, entry->lr);
			g_free(entry);
		}
		
		if (queue_size > 200000) {
			error_report("IO dump queue overflow (%d)! Something wrong!\n", g_queue_get_length(io_dump_queue));
			exit(1);
		}
	}
	return NULL;
}

void pmb887x_io_dump_init(int id) {
	io_dump_queue = g_queue_new();
	qemu_mutex_init(&io_dump_queue_lock);
	current_board = pmb887x_get_board_meta(id);
	g_cond_init(&io_dump_cond);
	
	qemu_thread_create(&io_dump_thread_id, "io_dump", _dump_io_thread, NULL, QEMU_THREAD_JOINABLE);
}

void pmb887x_dump_io(uint32_t addr, uint32_t size, uint32_t value, bool is_write) {
	if (!current_board)
		return;
	
	pmb887x_io_operation_t *entry = g_new0(pmb887x_io_operation_t, 1);
	entry->addr = addr;
	entry->size = size;
	entry->value = value;
	entry->is_write = is_write;
	entry->pc = ARM_CPU(qemu_get_cpu(0))->env.regs[15];
	entry->lr = ARM_CPU(qemu_get_cpu(0))->env.regs[14];
	
	qemu_mutex_lock(&io_dump_queue_lock);
	g_queue_push_tail(io_dump_queue, entry);
	qemu_mutex_unlock(&io_dump_queue_lock);
}

void pmb887x_print_dump_io(uint32_t addr, uint32_t size, uint32_t value, bool is_write, uint32_t pc, uint32_t lr) {
	pmb887x_cpu_meta_t *cpu = current_board->cpu;
	pmb887x_module_t *module = find_cpu_module(cpu, addr);
	g_autoptr(GString) s = g_string_new("");
	
	if (is_write) {
		g_string_append_printf(s, "WRITE[%d] %08X: %08X", size, addr, value);
	} else {
		g_string_append_printf(s, " READ[%d] %08X: %08X", size, addr, value);
	}
	
	if (module) {
		pmb887x_module_reg_t *reg = find_cpu_module_reg(module, addr);
		if (reg) {
			if (reg->special == PMB887X_REG_IS_GPIO_PIN) {
				const char *gpio_name = find_cpu_gpio_name(current_board, addr - module->base);
				if (gpio_name) {
					g_string_append_printf(s, " (%s)", gpio_name);
				} else {
					g_string_append_printf(s, " (%s_%s)", module->name, reg->name);
				}
			} else if (reg->special == PMB887X_REG_IS_IRQ_CON) {
				const char *irq_name = find_cpu_irq_name(cpu, addr - module->base);
				if (irq_name) {
					g_string_append_printf(s, " (%s_%s_%s)",  module->name, reg->name, irq_name);
				} else {
					g_string_append_printf(s, " (%s_%s)", module->name, reg->name);
				}
			} else {
				g_string_append_printf(s, " (%s_%s)", module->name, reg->name);
			}
			
			if (reg->special == PMB887X_REG_IS_IRQ_NUM) {
				const char *irq_name = find_cpu_irq_num_name(cpu, value);
				if (irq_name) {
					g_string_append_printf(s, ": NUM(0x%02X)=%s", value, irq_name);
				} else {
					g_string_append_printf(s, ": NUM(0x%02X)", value);
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
						g_string_append_printf(s, ": ");
						first = false;
					} else {
						g_string_append_printf(s, " | ");
					}
					
					if (enum_name) {
						g_string_append_printf(s, "%s(%s)", field->name, enum_name);
					} else if ((field->mask >> field->shift) == 1) {
						g_string_append_printf(s, "%s", field->name);
					} else {
						g_string_append_printf(s, "%s(0x%02X)", field->name, field_value);
					}
				}
				
				uint32_t unknown_bits = (value & ~known_bits);
				if (unknown_bits) {
					for (int i = 0; i < 32; i++) {
						if ((unknown_bits & (1 << i))) {
							if (first) {
								g_string_append_printf(s, ": ");
								first = false;
							} else {
								g_string_append_printf(s, " | ");
							}
							g_string_append_printf(s, "UNK_%d", i);
						}
					}
				}
			}
		} else {
			g_string_append_printf(s, " (%s_*)", module->name);
		}
	}
	
	qemu_log_mask(LOG_TRACE, "%s (PC: %08X, LR: %08X)\n", s->str, pc, lr);
}
