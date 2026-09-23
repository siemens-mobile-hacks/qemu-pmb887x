#define PMB887X_TRACE_ID		DSP_TCG
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-tcg"

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "qemu/rcu.h"
#include "qemu/timer.h"

#include "tcg/startup.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/dsp/tcg.h"
#include "hw/arm/pmb887x/dsp/runtime.h"
#include "hw/arm/pmb887x/trace.h"

/* Longest run between two looks at the interrupt lines and the peripherals. */
#define DSP_SLICE_CYCLES	4096

struct dsp_runtime_t {
	const pmb887x_dsp_config_t *config;
	const uint8_t *program_rom;
	const uint8_t *data_rom;
	void *device_opaque;
	void (*events_changed)(void *opaque);
	teak_tcg_core_t core;
	dsp_bus_t *bus;
	uint16_t *program;
	uint16_t *data;
	size_t active_program_bank;
	size_t active_data_bank;
	uint16_t rom_version;
	bool halted;
	bool core_disabled;
	bool pram_cache_active;
	bool program_dirty;
	/* The core's clock: cycles run so far and when it last changed frequency. */
	uint64_t cycles;
	uint64_t base_cycles;
	int64_t base_ns;
	uint32_t frequency;
};

static uint16_t dsp_runtime_read_u16(const uint8_t *data) {
	return data[0] | (uint16_t) data[1] << 8;
}

static void dsp_runtime_load_words(uint16_t *destination, const uint8_t *source, size_t words) {
	for (size_t i = 0; i < words; i++)
		qatomic_set(&destination[i], dsp_runtime_read_u16(source + i * sizeof(uint16_t)));
}

static void dsp_runtime_map_program_bank(dsp_runtime_t *runtime, size_t bank) {
	const pmb887x_dsp_config_t *config = runtime->config;
	const uint8_t *bank_data;
	size_t fixed_words = config->program_bank_base - config->program_rom_base;
	size_t bank_words = PMB887X_DSP_ADDRESS_SPACE_WORDS - config->program_bank_base;

	if (runtime->active_program_bank == bank)
		return;

	if (bank >= config->program_bank_count) {
		DPRINTF("unknown program ROM bank: cpu=%s bank=%zu count=%zu\n", config->name, bank, config->program_bank_count);
		return;
	}

	bank_data = runtime->program_rom + (fixed_words + bank * bank_words) * sizeof(uint16_t);
	dsp_runtime_load_words(runtime->program + config->program_bank_base, bank_data, bank_words);
	teak_tcg_request_exit(&runtime->core);
	teak_tcg_invalidate_program_range(&runtime->core, config->program_bank_base, bank_words);
	runtime->active_program_bank = bank;
}

static void dsp_runtime_map_data_bank(dsp_runtime_t *runtime, size_t bank) {
	const pmb887x_dsp_config_t *config = runtime->config;
	const uint8_t *bank_data;
	size_t fixed_words = config->data_bank_base - config->data_rom_base;
	size_t bank_words = config->shared_base - config->data_bank_base;

	if (runtime->active_data_bank == bank)
		return;

	if (bank >= config->data_bank_count) {
		DPRINTF("unknown data ROM bank: cpu=%s bank=%zu count=%zu\n", config->name, bank, config->data_bank_count);
		return;
	}

	bank_data = runtime->data_rom + (fixed_words + bank * bank_words) * sizeof(uint16_t);
	dsp_runtime_load_words(runtime->data + config->data_bank_base, bank_data, bank_words);
	runtime->active_data_bank = bank;
}

static void dsp_runtime_set_page(void *opaque, uint16_t page) {
	dsp_runtime_t *runtime = opaque;
	const pmb887x_dsp_config_t *config = runtime->config;
	uint16_t program_page_mask = config->page_field_mask << config->program_page_shift;
	uint16_t page_mask = program_page_mask | config->page_field_mask;

	if ((page & ~page_mask) != 0)
		DPRINTF("unknown DSP page bits: cpu=%s value=%04X unknown=%04X\n", config->name, page, page & ~page_mask);

	dsp_runtime_map_program_bank(runtime, page >> config->program_page_shift & config->page_field_mask);
	dsp_runtime_map_data_bank(runtime, page & config->page_field_mask);
}

static void dsp_runtime_set_core_disabled(void *opaque, bool disabled) {
	dsp_runtime_t *runtime = opaque;

	qatomic_set(&runtime->core_disabled, disabled);
	if (disabled)
		teak_tcg_request_exit(&runtime->core);
}

static void dsp_runtime_set_interrupt_lines(void *opaque, uint8_t lines) {
	dsp_runtime_t *runtime = opaque;
	teak_tcg_update_irq_lines(&runtime->core, lines);
}

static void dsp_runtime_events_changed(void *opaque) {
	dsp_runtime_t *runtime = opaque;
	runtime->events_changed(runtime->device_opaque);
}

int64_t dsp_runtime_get_time(const dsp_runtime_t *runtime) {
	if (runtime->frequency == 0)
		return runtime->base_ns;
	return runtime->base_ns + muldiv64(runtime->cycles - runtime->base_cycles, NANOSECONDS_PER_SECOND,
		runtime->frequency);
}

static int64_t dsp_runtime_time_ns(void *opaque) {
	return dsp_runtime_get_time(opaque);
}

static uint16_t dsp_runtime_program_read(void *opaque, uint32_t address) {
	dsp_runtime_t *runtime = opaque;

	if (address >= PMB887X_DSP_ADDRESS_SPACE_WORDS) {
		DPRINTF("program read outside address space: address=%05X pc=%05X\n", address, runtime->core.state.pc);
		return UINT16_MAX;
	}
	return qatomic_read(&runtime->program[address]);
}

static void dsp_runtime_program_write(void *opaque, uint32_t address, uint16_t value) {
	dsp_runtime_t *runtime = opaque;

	if (address >= PMB887X_DSP_ADDRESS_SPACE_WORDS) {
		DPRINTF("program write outside address space: address=%05X value=%04X pc=%05X\n",
			address, value, runtime->core.state.pc);
		return;
	}
	if (address >= runtime->config->program_rom_base || qatomic_read(&runtime->program[address]) == value)
		return;

	/* The mask ROM loader is filling P-RAM: the next program must be compiled afresh. */
	if (runtime->core.state.pc >= runtime->config->program_rom_base)
		runtime->program_dirty = true;
	qatomic_set(&runtime->program[address], value);
}

static bool dsp_runtime_program_should_invalidate(void *opaque, uint32_t address) {
	dsp_runtime_t *runtime = opaque;
	return address < runtime->config->program_rom_base && qatomic_read(&runtime->pram_cache_active);
}

static bool dsp_runtime_is_mmio(const dsp_runtime_t *runtime, uint16_t address) {
	return address >= runtime->config->mmio_base && address - runtime->config->mmio_base < runtime->config->mmio_size;
}

static void dsp_runtime_advance_cycles(void *opaque, size_t cycles) {
	dsp_runtime_t *runtime = opaque;

	runtime->cycles += cycles;
	dsp_bus_advance(runtime->bus, cycles);
}

static uint16_t dsp_runtime_data_read(void *opaque, uint32_t address) {
	dsp_runtime_t *runtime = opaque;
	uint16_t data_address = (uint16_t) address;

	if (dsp_runtime_is_mmio(runtime, data_address))
		return dsp_bus_read_at(runtime->bus, data_address, runtime->core.state.trace_pc);
	if (data_address >= runtime->config->shared_base)
		return qatomic_read(&runtime->data[data_address]);
	return runtime->data[data_address];
}

static void dsp_runtime_data_write(void *opaque, uint32_t address, uint16_t value) {
	dsp_runtime_t *runtime = opaque;
	uint16_t data_address = (uint16_t) address;

	if (dsp_runtime_is_mmio(runtime, data_address)) {
		dsp_bus_write_at(runtime->bus, data_address, value, runtime->core.state.trace_pc);
		return;
	}
	if (data_address >= runtime->config->shared_base) {
		uint16_t offset = data_address - runtime->config->shared_base;
		qatomic_set(&runtime->data[data_address], value);
		DPRINTF("shared write: address=%04X offset=%04X value=%04X pc=%05X\n", data_address,
			offset, value, runtime->core.state.trace_pc);
		return;
	}

	if (data_address < runtime->config->data_rom_base)
		runtime->data[data_address] = value;
}

static uint16_t dsp_runtime_bus_data_read(void *opaque, uint16_t address) {
	dsp_runtime_t *runtime = opaque;
	return qatomic_read(&runtime->data[address]);
}

static void dsp_runtime_bus_data_write(void *opaque, uint16_t address, uint16_t value) {
	dsp_runtime_t *runtime = opaque;
	qatomic_set(&runtime->data[address], value);
}

static uint16_t dsp_runtime_external_read(void *opaque, uint32_t index) {
	dsp_runtime_t *runtime = opaque;
	return dsp_bus_external_read(runtime->bus, index);
}

static void dsp_runtime_external_write(void *opaque, uint32_t index, uint16_t value) {
	dsp_runtime_t *runtime = opaque;
	dsp_bus_external_write(runtime->bus, index, value);
}

dsp_runtime_t *dsp_runtime_create(
	const pmb887x_dsp_config_t *config, uint16_t rom_version, const uint8_t *program_rom, const uint8_t *data_rom,
	void *device_opaque, void (*events_changed)(void *opaque), uint32_t (*ssc_transfer)(void *opaque, uint32_t value)
) {
	dsp_runtime_t *runtime;
	teak_memory_t memory;
	dsp_host_t host;

	runtime = g_new0(dsp_runtime_t, 1);
	runtime->config = config;
	runtime->rom_version = rom_version;
	runtime->program_rom = program_rom;
	runtime->data_rom = data_rom;
	runtime->device_opaque = device_opaque;
	runtime->events_changed = events_changed;
	runtime->program = g_new0(uint16_t, PMB887X_DSP_ADDRESS_SPACE_WORDS);
	runtime->data = g_new0(uint16_t, PMB887X_DSP_ADDRESS_SPACE_WORDS);
	runtime->active_program_bank = SIZE_MAX;
	runtime->active_data_bank = SIZE_MAX;

	host = (dsp_host_t) {
		.opaque = runtime,
		.ssc_opaque = device_opaque,
		.set_page = dsp_runtime_set_page,
		.set_core_disabled = dsp_runtime_set_core_disabled,
		.set_interrupt_lines = dsp_runtime_set_interrupt_lines,
		.data_read = dsp_runtime_bus_data_read,
		.data_write = dsp_runtime_bus_data_write,
		.ssc_transfer = ssc_transfer,
		.get_time_ns = dsp_runtime_time_ns,
		.events_changed = dsp_runtime_events_changed,
	};
	runtime->bus = dsp_bus_create(config, &host);

	memory = (teak_memory_t) {
		.program = {
			.opaque = runtime,
			.read = dsp_runtime_program_read,
			.write = dsp_runtime_program_write,
			.should_invalidate = dsp_runtime_program_should_invalidate,
		},
		.data = {
			.opaque = runtime,
			.read = dsp_runtime_data_read,
			.write = dsp_runtime_data_write,
		},
		.external = {
			.opaque = runtime,
			.read = dsp_runtime_external_read,
			.write = dsp_runtime_external_write,
		},
		.direct_data = runtime->data,
		.direct_data_read_size = config->shared_base,
		.direct_data_write_size = config->data_rom_base,
		.cycle_opaque = runtime,
		.advance_cycles = dsp_runtime_advance_cycles,
		.cycle_sensitive_base = config->mmio_base,
		.cycle_sensitive_size = config->mmio_size,
		.y_space_base = config->y_space_base,
	};

	teak_tcg_init(&runtime->core, &memory);
	dsp_runtime_reset(runtime);
	return runtime;
}

void dsp_runtime_set_clock(dsp_runtime_t *runtime, bool enabled) {
	dsp_bus_set_clock(runtime->bus, enabled);
}

/* The clock changes now: time already run stays at the old frequency. */
void dsp_runtime_set_frequency(dsp_runtime_t *runtime, uint32_t frequency) {
	if (frequency == runtime->frequency)
		return;

	runtime->base_ns = dsp_runtime_get_time(runtime);
	runtime->base_cycles = runtime->cycles;
	runtime->frequency = frequency;
	dsp_bus_set_frequency(runtime->bus, frequency);
}

void dsp_runtime_destroy(dsp_runtime_t *runtime) {
	if (runtime == NULL)
		return;

	dsp_bus_destroy(runtime->bus);
	g_free(runtime->data);
	g_free(runtime->program);
	g_free(runtime);
}

void dsp_runtime_reset(dsp_runtime_t *runtime) {
	const pmb887x_dsp_config_t *config = runtime->config;
	size_t program_fixed_words = config->program_bank_base - config->program_rom_base;
	size_t data_fixed_words = config->data_bank_base - config->data_rom_base;

	dsp_runtime_load_words(runtime->program + config->program_rom_base, runtime->program_rom, program_fixed_words);
	dsp_runtime_load_words(runtime->data + config->data_rom_base, runtime->data_rom, data_fixed_words);

	dsp_bus_reset(runtime->bus);
	teak_tcg_reset(&runtime->core, config->program_rom_base + 2);

	qatomic_set(&runtime->data[config->shared_base], runtime->rom_version);
	runtime->core_disabled = false;
	runtime->halted = false;
}

static void dsp_runtime_skip(dsp_runtime_t *runtime, size_t cycles) {
	dsp_runtime_advance_cycles(runtime, cycles);
}

static void dsp_runtime_precompile(dsp_runtime_t *runtime) {
	uint32_t pc = runtime->core.state.pc;

	if (!runtime->program_dirty || pc >= runtime->config->program_rom_base)
		return;

	runtime->program_dirty = false;
	qatomic_set(&runtime->pram_cache_active, true);
	DPRINTF("program precompile: pc=%05X blocks=%zu\n", pc, teak_tcg_precompile_entry(&runtime->core, pc));
}

static void dsp_runtime_execute(dsp_runtime_t *runtime, size_t budget) {
	uint8_t block_repeat_level = runtime->core.state.bcn;
	uint32_t block_pc = runtime->core.state.pc;

	dsp_runtime_precompile(runtime);
	qatomic_xchg(&runtime->core.state.interrupt_request, 0);
	teak_tcg_service_interrupt(&runtime->core);
	runtime->core.state.exit_reason = TEAK_EXIT_NONE;

	if (!teak_tcg_execute_slice(&runtime->core, budget)) {
		teak_insn_t instruction;
		uint32_t pc = runtime->core.translation_error_address;
		uint16_t word = teak_program_read(&runtime->core, pc);
		bool decoded = teak_decode(&runtime->core, pc, &instruction);

		DPRINTF("native execution stopped: cpu=%s pc=%05X opcode=%04X op=%u decoded=%u error=%u lp=%u bcn=%u\n",
			runtime->config->name, pc, word, instruction.opcode, decoded, runtime->core.translation_error,
			runtime->core.state.lp, runtime->core.state.bcn);
		if (runtime->core.state.bcn != 0) {
			size_t level = runtime->core.state.bcn - 1;
			DPRINTF("active block repeat: level=%zu start=%04X end=%04X lc=%04X\n", level,
				runtime->core.state.block_repeat_start[level], runtime->core.state.block_repeat_end[level],
				runtime->core.state.block_repeat_lc[level]);
		}
		runtime->halted = true;
		return;
	}

	if (runtime->core.state.bcn != block_repeat_level)
		DPRINTF("block repeat nesting: pc=%05X next=%05X bcn=%u->%u lp=%u\n", block_pc,
			runtime->core.state.pc, block_repeat_level, runtime->core.state.bcn, runtime->core.state.lp);
}

/*
 * Run the core and its peripherals on the DSP clock until its time reaches
 * @target. A slice ends at the next peripheral event, so interrupts are raised
 * at the cycle they are due; the last slice may pass @target by one block.
 */
void dsp_runtime_run_until(dsp_runtime_t *runtime, int64_t target) {
	while (dsp_runtime_get_time(runtime) < target) {
		uint64_t goal;
		size_t budget;
		size_t next_event;
		bool sleeping;

		if (runtime->frequency == 0) {
			runtime->base_ns = target;
			runtime->base_cycles = runtime->cycles;
			return;
		}

		goal = runtime->base_cycles + muldiv64_round_up(target - runtime->base_ns, runtime->frequency,
			NANOSECONDS_PER_SECOND);
		budget = MAX(goal - runtime->cycles, (uint64_t) 1);
		next_event = MAX(dsp_bus_next_event(runtime->bus), (size_t) 1);

		if (runtime->core_disabled && dsp_bus_get_irq_lines(runtime->bus) != 0)
			runtime->core_disabled = false;
		sleeping = runtime->halted || runtime->core_disabled;

		if (sleeping) {
			dsp_runtime_skip(runtime, MIN(budget, next_event));
		} else {
			dsp_runtime_execute(runtime, MIN(MIN(budget, next_event), (size_t) DSP_SLICE_CYCLES));
		}
	}
}

bool dsp_runtime_code_flush_pending(void) {
	return teak_tcg_flush_pending();
}

bool dsp_runtime_is_sleeping(const dsp_runtime_t *runtime) {
	return runtime->halted || (runtime->core_disabled && dsp_bus_get_irq_lines(runtime->bus) == 0);
}

/* When the DSP next does something: now while the core runs, else its next peripheral event. */
int64_t dsp_runtime_next_event_time(dsp_runtime_t *runtime) {
	int64_t now = dsp_runtime_get_time(runtime);
	size_t cycles;

	if (runtime->frequency == 0)
		return INT64_MAX;
	if (!dsp_runtime_is_sleeping(runtime))
		return now;

	cycles = dsp_bus_next_event(runtime->bus);
	if (cycles == SIZE_MAX)
		return INT64_MAX;
	return runtime->base_ns + muldiv64_round_up(runtime->cycles + cycles - runtime->base_cycles,
		NANOSECONDS_PER_SECOND, runtime->frequency);
}

void dsp_runtime_thread_enter(void) {
	rcu_register_thread();
	tcg_register_thread();
}

void dsp_runtime_thread_exit(void) {
	rcu_unregister_thread();
}

uint16_t dsp_runtime_shared_read(dsp_runtime_t *runtime, uint16_t offset) {
	g_assert(offset < runtime->config->shared_size);
	return qatomic_read(&runtime->data[runtime->config->shared_base + offset]);
}

uint64_t dsp_runtime_shared_read_bytes(dsp_runtime_t *runtime, size_t offset, size_t size) {
	uint64_t value = 0;
	size_t value_shift = 0;

	g_assert(size <= sizeof(value));
	g_assert(offset + size <= runtime->config->shared_size * sizeof(uint16_t));

	while (size != 0) {
		size_t word_offset = offset / sizeof(uint16_t);
		size_t byte_offset = offset % sizeof(uint16_t);
		size_t bytes = MIN(size, sizeof(uint16_t) - byte_offset);
		uint16_t word = dsp_runtime_shared_read(runtime, word_offset);
		uint16_t mask = bytes == sizeof(uint16_t) ? UINT16_MAX : UINT8_MAX;

		value |= (uint64_t) (word >> (byte_offset * 8) & mask) << value_shift;

		offset += bytes;
		size -= bytes;
		value_shift += bytes * 8;
	}
	return value;
}

void dsp_runtime_shared_write_bytes(dsp_runtime_t *runtime, size_t offset, uint64_t value, size_t size) {
	size_t value_shift = 0;

	g_assert(size <= sizeof(value));
	g_assert(offset + size <= runtime->config->shared_size * sizeof(uint16_t));

	while (size != 0) {
		size_t word_offset = offset / sizeof(uint16_t);
		size_t byte_offset = offset % sizeof(uint16_t);
		size_t bytes = MIN(size, sizeof(uint16_t) - byte_offset);
		uint16_t field_mask = bytes == sizeof(uint16_t) ? UINT16_MAX : UINT8_MAX;
		uint16_t mask = field_mask << (byte_offset * 8);
		uint16_t field = (uint16_t) (value >> value_shift) << (byte_offset * 8) & mask;
		uint16_t *word = &runtime->data[runtime->config->shared_base + word_offset];
		uint16_t previous;
		uint16_t updated;

		do {
			previous = qatomic_read(word);
			updated = previous & ~mask;
			updated |= field;
		} while (qatomic_cmpxchg(word, previous, updated) != previous);

		offset += bytes;
		size -= bytes;
		value_shift += bytes * 8;
	}
}

void dsp_runtime_set_request(dsp_runtime_t *runtime, size_t index, bool level) {
	if (!level)
		return;

	dsp_bus_set_request(runtime->bus, index, level);
}

void dsp_runtime_set_input(dsp_runtime_t *runtime, size_t index, bool level) {
	dsp_bus_set_input(runtime->bus, index, level);
}

void dsp_runtime_set_gsm_clock(dsp_runtime_t *runtime, uint32_t frequency) {
	dsp_bus_set_gsm_clock(runtime->bus, frequency);
}

void dsp_runtime_set_gsm_signal(dsp_runtime_t *runtime, pmb887x_dsp_gsm_signal_t signal, bool level) {
	dsp_bus_set_gsm_signal(runtime->bus, signal, level);
}

uint16_t dsp_runtime_get_outputs(dsp_runtime_t *runtime) {
	return dsp_bus_get_outputs(runtime->bus);
}

uint32_t dsp_runtime_get_pc(const dsp_runtime_t *runtime) {
	return qatomic_read(&runtime->core.state.pc);
}

uint16_t dsp_runtime_take_output_events(dsp_runtime_t *runtime) {
	return dsp_bus_take_output_events(runtime->bus);
}

uint16_t dsp_runtime_get_comm(dsp_runtime_t *runtime) {
	return dsp_bus_get_comm(runtime->bus);
}

void dsp_runtime_set_comm(dsp_runtime_t *runtime, uint16_t value) {
	dsp_bus_set_comm(runtime->bus, value);
}

void dsp_runtime_clear_comm(dsp_runtime_t *runtime, uint16_t value) {
	dsp_bus_clear_comm(runtime->bus, value);
}

uint16_t dsp_runtime_take_mcu_irqs(dsp_runtime_t *runtime) {
	return dsp_bus_take_mcu_irqs(runtime->bus);
}

uint16_t dsp_runtime_get_mcu_semaphores(dsp_runtime_t *runtime) {
	return dsp_bus_get_mcu_semaphores(runtime->bus);
}

void dsp_runtime_request_mcu_semaphores(dsp_runtime_t *runtime, uint16_t value) {
	dsp_bus_request_mcu_semaphores(runtime->bus, value);
}

void dsp_runtime_release_mcu_semaphores(dsp_runtime_t *runtime, uint16_t value) {
	dsp_bus_release_mcu_semaphores(runtime->bus, value);
}
