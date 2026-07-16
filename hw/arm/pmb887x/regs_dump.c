#include "regs_dump.h"

#include "board/board.h"
#include "target/arm/cpu.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "system/system.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"

typedef struct pmb887x_io_operation_t pmb887x_io_operation_t;
typedef struct pmb887x_io_dump_buffer_t pmb887x_io_dump_buffer_t;

#define IO_DUMP_QUEUE_CAPACITY 65536
#define IO_DUMP_BATCH_SIZE 256
#define IO_DUMP_DEDUPLICATION_INTERVAL_MS 100
#define IO_DUMP_BUFFER_SIZE 4096

struct pmb887x_io_operation_t {
	uint32_t addr;
	uint32_t value;
	uint8_t size;
	uint32_t pc;
	uint32_t lr;
	bool is_write;
	uint64_t count;
	int64_t deadline_ms;
};

struct pmb887x_io_dump_buffer_t {
	char data[IO_DUMP_BUFFER_SIZE];
	size_t length;
};

static bool io_dump_stopping;
static QemuMutex io_dump_lock;
static QemuThread io_dump_thread_id;
static QemuCond io_dump_ready;
static pmb887x_io_operation_t io_dump_queue[IO_DUMP_QUEUE_CAPACITY];
static size_t io_dump_queue_head;
static size_t io_dump_queue_count;
static ARMCPU *io_dump_cpu;
static bool io_dump_log_enabled;
static Notifier io_dump_exit_notifier;
static uint32_t gpio_base = 0;
static struct {
	uint32_t count;
	const pmb887x_module_t **modules;
} addr2modules[0xFFF] = {0};

static void regs_dump_buffer_append(pmb887x_io_dump_buffer_t *buffer, const char *format, ...) G_GNUC_PRINTF(2, 3);
static void regs_dump_print_io(FILE *log_file, uint32_t addr, uint32_t size, uint32_t value, bool is_write, uint32_t pc, uint32_t lr, uint64_t count);

static void regs_dump_buffer_append(pmb887x_io_dump_buffer_t *buffer, const char *format, ...) {
	size_t available = sizeof(buffer->data) - buffer->length;
	if (available <= 1)
		return;

	va_list args;
	va_start(args, format);
	int length = vsnprintf(&buffer->data[buffer->length], available, format, args);
	va_end(args);

	if (length < 0)
		return;
	buffer->length += MIN((size_t) length, available - 1);
}

static const pmb887x_module_t *regs_dump_find_cpu_module(uint32_t addr) {
	uint32_t prefix = (addr & 0xFFF00000) >> 20;
	for (int i = 0; i < addr2modules[prefix].count; i++) {
		const pmb887x_module_t *module = addr2modules[prefix].modules[i];
		if (addr >= module->base && addr <= (module->base + module->size))
			return module;
	}
	return NULL;
}

static const pmb887x_module_reg_t *regs_dump_find_cpu_module_reg(const pmb887x_module_t *module, uint32_t addr,
	uint32_t *reg_offset) {
	for (int i = 0; i < module->regs_count; i++) {
		const pmb887x_module_reg_t *reg = &module->regs[i];
		uint32_t reg_addr = module->base + reg->addr;
		if (addr >= reg_addr && addr < reg_addr + sizeof(uint32_t)) {
			*reg_offset = addr - reg_addr;
			return reg;
		}
	}
	return NULL;
}

static const char *regs_dump_find_cpu_module_field_enum(const pmb887x_module_field_t *field, uint32_t field_value) {
	for (int i = 0; i < field->values_count; i++) {
		const pmb887x_module_value_t *v = &field->values[i];
		if (field_value == v->value)
			return v->name;
	}
	return NULL;
}

static const char *regs_dump_find_cpu_irq_num_name(const pmb887x_cpu_meta_t *cpu, uint32_t field_value) {
	for (int i = 0; i < cpu->irqs_count; i++) {
		const pmb887x_cpu_meta_irq_t *v = &cpu->irqs[i];
		if (field_value == v->id)
			return v->name;
	}
	return NULL;
}

static const char *regs_dump_find_cpu_irq_name(const pmb887x_cpu_meta_t *cpu, uint32_t field_value) {
	for (int i = 0; i < cpu->irqs_count; i++) {
		const pmb887x_cpu_meta_irq_t *v = &cpu->irqs[i];
		if (field_value == v->addr)
			return v->name;
	}
	return NULL;
}

static bool regs_dump_pop_io(pmb887x_io_operation_t *entry, bool wait) {
	qemu_mutex_lock(&io_dump_lock);
	if (wait) {
		while (!io_dump_stopping) {
			if (io_dump_queue_count == 0) {
				qemu_cond_wait(&io_dump_ready, &io_dump_lock);
			} else if (io_dump_queue_count > 1) {
				break;
			} else {
				int64_t timeout_ms = io_dump_queue[io_dump_queue_head].deadline_ms - qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
				if (timeout_ms <= 0)
					break;
				qemu_cond_timedwait(&io_dump_ready, &io_dump_lock, timeout_ms);
			}
		}
	}

	bool can_pop = io_dump_queue_count > 1 || io_dump_stopping;
	if (io_dump_queue_count == 1 && !can_pop)
		can_pop = io_dump_queue[io_dump_queue_head].deadline_ms <= qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
	if (!can_pop || io_dump_queue_count == 0) {
		qemu_mutex_unlock(&io_dump_lock);
		return false;
	}

	*entry = io_dump_queue[io_dump_queue_head];
	io_dump_queue_head = (io_dump_queue_head + 1) % IO_DUMP_QUEUE_CAPACITY;
	io_dump_queue_count--;
	qemu_mutex_unlock(&io_dump_lock);
	return true;
}

static void *regs_dump_dump_io_thread(void *arg) {
	while (true) {
		pmb887x_io_operation_t entry;
		if (!regs_dump_pop_io(&entry, true))
			break;

		FILE *log_file = io_dump_log_enabled ? qemu_log_trylock_with_context() : NULL;
		for (size_t i = 0; i < IO_DUMP_BATCH_SIZE; i++) {
			if (log_file)
				regs_dump_print_io(log_file, entry.addr, entry.size, entry.value, entry.is_write, entry.pc, entry.lr, entry.count);
			if (i + 1 == IO_DUMP_BATCH_SIZE || !regs_dump_pop_io(&entry, false))
				break;
		}
		if (log_file)
			qemu_log_unlock(log_file);
	}
	return NULL;
}

static void regs_dump_exit_notify(Notifier *notifier, void *data) {
	qemu_mutex_lock(&io_dump_lock);
	io_dump_stopping = true;
	qemu_cond_broadcast(&io_dump_ready);
	qemu_mutex_unlock(&io_dump_lock);
	qemu_thread_join(&io_dump_thread_id);
}

void pmb887x_io_dump_init(void) {
	const pmb887x_cpu_meta_t *cpu_info = pmb887x_get_cpu_meta(pmb887x_board()->cpu);

	io_dump_stopping = false;
	io_dump_queue_head = 0;
	io_dump_queue_count = 0;
	io_dump_cpu = NULL;
	io_dump_log_enabled = qemu_loglevel_mask(LOG_TRACE);
	qemu_mutex_init(&io_dump_lock);
	qemu_cond_init(&io_dump_ready);
	io_dump_exit_notifier.notify = regs_dump_exit_notify;
	qemu_add_exit_notifier(&io_dump_exit_notifier);

	qemu_thread_create(&io_dump_thread_id, "io_dump", regs_dump_dump_io_thread, NULL, QEMU_THREAD_JOINABLE);
	
	// Module search index
	for (int i = 0; i < cpu_info->modules_count; i++) {
		const pmb887x_module_t *module = &cpu_info->modules[i];
		uint32_t prefix = (module->base & 0xFFF00000) >> 20;
		addr2modules[prefix].count++;
		addr2modules[prefix].modules = g_realloc(addr2modules[prefix].modules, sizeof(pmb887x_module_t *) * addr2modules[prefix].count);
		addr2modules[prefix].modules[addr2modules[prefix].count - 1] = module;
		
		if (strcmp(module->name, "GPIO") == 0)
			gpio_base = module->base;
	}
	
	assert(gpio_base != 0);
}

void pmb887x_dump_io(uint32_t addr, uint32_t size, uint32_t value, bool is_write) {
	if (!io_dump_cpu)
		io_dump_cpu = ARM_CPU(qemu_get_cpu(0));

	uint32_t pc = io_dump_cpu->env.regs[15];
	uint32_t lr = io_dump_cpu->env.regs[14];

	qemu_mutex_lock(&io_dump_lock);
	if (io_dump_queue_count > 0) {
		size_t last_index = (io_dump_queue_head + io_dump_queue_count - 1) % IO_DUMP_QUEUE_CAPACITY;
		pmb887x_io_operation_t *last_entry = &io_dump_queue[last_index];
		bool is_duplicate = last_entry->addr == addr && last_entry->value == value && last_entry->size == size && last_entry->pc == pc && last_entry->lr == lr && last_entry->is_write == is_write;
		if (is_duplicate) {
			last_entry->count++;
			qemu_mutex_unlock(&io_dump_lock);
			return;
		}
	}
	if (io_dump_queue_count == IO_DUMP_QUEUE_CAPACITY) {
		qemu_mutex_unlock(&io_dump_lock);
		error_report("IO dump queue overflow (%d)! Something wrong!", IO_DUMP_QUEUE_CAPACITY);
		exit(1);
	}

	size_t tail = (io_dump_queue_head + io_dump_queue_count) % IO_DUMP_QUEUE_CAPACITY;
	io_dump_queue[tail] = (pmb887x_io_operation_t) {
		.addr = addr,
		.size = size,
		.value = value,
		.is_write = is_write,
		.pc = pc,
		.lr = lr,
		.count = 1,
		.deadline_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + IO_DUMP_DEDUPLICATION_INTERVAL_MS,
	};
	io_dump_queue_count++;
	if (io_dump_queue_count <= 2)
		qemu_cond_signal(&io_dump_ready);
	qemu_mutex_unlock(&io_dump_lock);
}

static void regs_dump_print_io(FILE *log_file, uint32_t addr, uint32_t size, uint32_t value, bool is_write, uint32_t pc, uint32_t lr, uint64_t count) {
	pmb887x_board_t *board = pmb887x_board();
	const pmb887x_cpu_meta_t *cpu_info = pmb887x_get_cpu_meta(board->cpu);

	const pmb887x_module_t *module = regs_dump_find_cpu_module(addr);
	pmb887x_io_dump_buffer_t buffer;
	buffer.length = 0;
	buffer.data[0] = '\0';
	
	if (is_write) {
		regs_dump_buffer_append(&buffer, "WRITE[%d] %08X: %08X", size, addr, value);
	} else {
		regs_dump_buffer_append(&buffer, " READ[%d] %08X: %08X", size, addr, value);
	}
	
	if (module) {
		uint32_t reg_offset;
		const pmb887x_module_reg_t *reg = regs_dump_find_cpu_module_reg(module, addr, &reg_offset);
		if (reg) {
			if (reg_offset) {
				regs_dump_buffer_append(&buffer, " (%s_%s+0x%X)", module->name, reg->name, reg_offset);
			} else if (reg->special == PMB887X_REG_IS_GPIO_PIN) {
				uint32_t gpio_id = (addr - (gpio_base + GPIO_PIN0)) / 4;
				regs_dump_buffer_append(&buffer, " (%s)", board->gpios[gpio_id].full_name);
			} else if (reg->special == PMB887X_REG_IS_IRQ_CON) {
				const char *irq_name = regs_dump_find_cpu_irq_name(cpu_info, addr - module->base);
				if (irq_name) {
					regs_dump_buffer_append(&buffer, " (%s_%s_%s)",  module->name, reg->name, irq_name);
				} else {
					regs_dump_buffer_append(&buffer, " (%s_%s)", module->name, reg->name);
				}
			} else {
				regs_dump_buffer_append(&buffer, " (%s_%s)", module->name, reg->name);
			}
			
			if (!reg_offset && reg->special == PMB887X_REG_IS_IRQ_NUM) {
				const char *irq_name = regs_dump_find_cpu_irq_num_name(cpu_info, value);
				if (irq_name) {
					regs_dump_buffer_append(&buffer, ": NUM(0x%02X)=%s", value, irq_name);
				} else {
					regs_dump_buffer_append(&buffer, ": NUM(0x%02X)", value);
				}
			} else if (!reg_offset && reg->fields_count) {
				bool first = true;
				uint32_t known_bits = 0;
				for (int i = 0; i < reg->fields_count; i++) {
					const pmb887x_module_field_t *field = &reg->fields[i];
					uint32_t field_value = (value & field->mask) >> field->shift;
					const char *enum_name = regs_dump_find_cpu_module_field_enum(field, (value & field->mask));
					uint32_t field_size = (field->mask >> field->shift);
					
					known_bits |= field->mask;
					
					if (field_size == 1 && field_value == 0 && !enum_name)
						continue;
					
					if (first) {
						regs_dump_buffer_append(&buffer, ": ");
						first = false;
					} else {
						regs_dump_buffer_append(&buffer, " | ");
					}
					
					if (enum_name) {
						regs_dump_buffer_append(&buffer, "%s(%s)", field->name, enum_name);
					} else if (field_size == 1) {
						regs_dump_buffer_append(&buffer, "%s", field->name);
					} else {
						regs_dump_buffer_append(&buffer, "%s(0x%02X)", field->name, field_value);
					}
				}
				
				uint32_t unknown_bits = (value & ~known_bits);
				if (unknown_bits) {
					for (int i = 0; i < 32; i++) {
						if ((unknown_bits & (1 << i))) {
							if (first) {
								regs_dump_buffer_append(&buffer, ": ");
								first = false;
							} else {
								regs_dump_buffer_append(&buffer, " | ");
							}
							regs_dump_buffer_append(&buffer, "UNK_%d", i);
						}
					}
				}
			}
		} else {
			regs_dump_buffer_append(&buffer, " (%s_*)", module->name);
		}
	}
	
	regs_dump_buffer_append(&buffer, " (PC: %08X, LR: %08X)", pc, lr);
	if (count > 1)
		regs_dump_buffer_append(&buffer, " [x%" PRIu64 "]", count);
	regs_dump_buffer_append(&buffer, "\n");

	fwrite(buffer.data, 1, buffer.length, log_file);
}
