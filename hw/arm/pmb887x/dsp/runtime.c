#define PMB887X_TRACE_ID		DSP_TCG
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-tcg"

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bitops.h"
#include "qemu/rcu.h"

#include "tcg/startup.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/dsp/tcg.h"
#include "hw/arm/pmb887x/dsp/runtime.h"
#include "hw/arm/pmb887x/trace.h"

#define DSP_ACTIVE_SLICE_CYCLES	32768
#define DSP_STABLE_BLOCK_CYCLES	512

struct dsp_runtime_t {
	const pmb887x_dsp_config_t *config;
	const uint8_t *program_rom;
	const uint8_t *data_rom;
	void *device_opaque;
	void (*notify_activity)(void *opaque);
	void (*notify_comm)(void *opaque, uint16_t flags, bool set);
	teak_tcg_core_t core;
	dsp_bus_t *bus;
	uint16_t *program;
	uint16_t *data;
	size_t active_program_bank;
	size_t active_data_bank;
	uint16_t rom_version;
	bool idle;
	bool halted;
	bool core_disabled;
	bool pram_cache_active;
	bool program_dirty;
	bool program_warming;
	bool mutable_program_started;
	bool program_start;
	bool reschedule;
	uint32_t program_start_pc;
};

static uint16_t dsp_runtime_read_u16(const uint8_t *data) {
	return data[0] | (uint16_t) data[1] << 8;
}

static void dsp_runtime_load_words(uint16_t *destination, const uint8_t *source, size_t words) {
	for (size_t i = 0; i < words; i++)
		qatomic_set(&destination[i], dsp_runtime_read_u16(source + i * sizeof(uint16_t)));
}

void dsp_runtime_wake(dsp_runtime_t *runtime) {
	qatomic_set(&runtime->idle, false);
}

void dsp_runtime_kick(dsp_runtime_t *runtime) {
	dsp_runtime_wake(runtime);
	qatomic_set(&runtime->reschedule, true);
	teak_tcg_request_exit(&runtime->core);
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
	if (lines != 0) {
		dsp_runtime_wake(runtime);
		runtime->notify_activity(runtime->device_opaque);
	}
}

static void dsp_runtime_comm_changed(void *opaque, uint16_t flags, bool set) {
	dsp_runtime_t *runtime = opaque;

	runtime->notify_comm(runtime->device_opaque, flags, set);
	if (set)
		dsp_runtime_kick(runtime);
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

	if (!qatomic_read(&runtime->program_warming))
		qatomic_set(&runtime->program_dirty, true);
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
	void *device_opaque, void (*notify_activity)(void *opaque), void (*notify_comm)(void *opaque, uint16_t flags, bool set),
	uint32_t (*ssc_transfer)(void *opaque, uint32_t value)
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
	runtime->notify_activity = notify_activity;
	runtime->notify_comm = notify_comm;
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
		.comm_changed = dsp_runtime_comm_changed,
		.data_read = dsp_runtime_bus_data_read,
		.data_write = dsp_runtime_bus_data_write,
		.ssc_transfer = ssc_transfer,
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
	dsp_bus_set_core_idle(runtime->bus, false);
	teak_tcg_reset(&runtime->core, config->program_rom_base + 2);

	runtime->data[config->shared_base] = runtime->rom_version;
	if (qatomic_xchg(&runtime->program_warming, false))
		qatomic_set(&runtime->program_dirty, true);
	qatomic_set(&runtime->mutable_program_started, false);
	qatomic_set(&runtime->program_start, false);
	qatomic_set(&runtime->reschedule, false);
	qatomic_set(&runtime->idle, false);
	qatomic_set(&runtime->core_disabled, false);
	runtime->halted = false;
}

bool dsp_runtime_run(dsp_runtime_t *runtime) {
	uint64_t cache_compiles = runtime->core.cache_compiles;
	uint64_t cache_decoded_hits = runtime->core.cache_decoded_hits;
	uint64_t cache_fast_hits = runtime->core.cache_fast_hits;
	uint64_t jit_entries = runtime->core.jit_entries;
	uint64_t chain_links = runtime->core.chain_links;
	uint64_t chain_interrupts = runtime->core.chain_interrupts;
	uint64_t chain_exit_stops = runtime->core.chain_exit_stops;
	uint64_t chain_budget_stops = runtime->core.chain_budget_stops;
	uint64_t chain_cache_stops = runtime->core.chain_cache_stops;
	size_t slices = 0;
	size_t blocks = 0;
	size_t cycles = 0;

	runtime->core.chain_exit_pc = 0;

	qatomic_set(&runtime->idle, false);
	dsp_bus_set_core_idle(runtime->bus, false);

	while (cycles < DSP_ACTIVE_SLICE_CYCLES && !runtime->halted) {
		uint8_t block_repeat_level;
		uint32_t block_pc;
		size_t remaining_cycles = DSP_ACTIVE_SLICE_CYCLES - cycles;
		size_t slice_cycles = MIN(remaining_cycles, (size_t) DSP_STABLE_BLOCK_CYCLES);
		bool mutable_program = runtime->core.state.pc < runtime->config->program_rom_base;
		bool new_program_lifecycle = !qatomic_read(&runtime->mutable_program_started);
		bool program_changed = qatomic_read(&runtime->program_dirty);
		bool first_mutable_execution = mutable_program && (new_program_lifecycle || program_changed);

		if (first_mutable_execution) {
			qatomic_set(&runtime->program_start_pc, runtime->core.state.pc);
			qatomic_set(&runtime->program_start, true);
			qatomic_set(&runtime->program_dirty, false);
			qatomic_set(&runtime->program_warming, true);
			qatomic_set(&runtime->mutable_program_started, true);
			qatomic_set(&runtime->pram_cache_active, true);

			size_t precompiled = teak_tcg_precompile_entry(&runtime->core, runtime->core.state.pc);
			DPRINTF("cold program precompile: pc=%05X blocks=%zu\n", runtime->core.state.pc, precompiled);
		}

		if (qatomic_read(&runtime->core_disabled)) {
			uint8_t active_lines;

			dsp_bus_advance(runtime->bus, 0);
			active_lines = dsp_bus_get_irq_lines(runtime->bus);
			if (active_lines == 0) {
				qatomic_set(&runtime->idle, true);
				dsp_bus_set_core_idle(runtime->bus, true);
				break;
			}
			qatomic_set(&runtime->core_disabled, false);
		}

		qatomic_xchg(&runtime->core.state.interrupt_request, 0);
		teak_tcg_service_interrupt(&runtime->core);
		block_pc = runtime->core.state.pc;
		block_repeat_level = runtime->core.state.bcn;
		runtime->core.state.exit_reason = TEAK_EXIT_NONE;
		if (!teak_tcg_execute_slice(&runtime->core, slice_cycles)) {
			teak_insn_t instruction;
			uint32_t pc = runtime->core.translation_error_address;
			uint16_t word = teak_program_read(&runtime->core, pc);
			bool decoded = teak_decode(&runtime->core, pc, &instruction);

			DPRINTF("native execution stopped: cpu=%s pc=%05X opcode=%04X decoded=%u error=%u lp=%u bcn=%u\n",
				runtime->config->name, pc, word, decoded, runtime->core.translation_error,
				runtime->core.state.lp, runtime->core.state.bcn);
			if (runtime->core.state.bcn != 0) {
				size_t level = runtime->core.state.bcn - 1;
				DPRINTF("active block repeat: level=%zu start=%04X end=%04X lc=%04X\n", level,
					runtime->core.state.block_repeat_start[level], runtime->core.state.block_repeat_end[level],
					runtime->core.state.block_repeat_lc[level]);
			}
			runtime->halted = true;
			break;
		}

		if (qatomic_xchg(&runtime->reschedule, false))
			break;

		cycles += runtime->core.last_block_cycles;
		blocks += runtime->core.last_block_count;
		slices++;

		if (runtime->core.state.bcn != block_repeat_level)
			DPRINTF("block repeat nesting: pc=%05X next=%05X bcn=%u->%u lp=%u\n", block_pc,
				runtime->core.state.pc, block_repeat_level, runtime->core.state.bcn, runtime->core.state.lp);
	}

	if (blocks != 0) {
		DPRINTF("slices=%zu blocks=%zu jit=%"PRIu64" cycles=%zu run=%u idle=%u pc=%04X\n", slices, blocks,
			runtime->core.jit_entries - jit_entries, cycles, !runtime->halted, qatomic_read(&runtime->idle),
			runtime->core.state.pc);
		DPRINTF("cache=%"PRIu64"/%"PRIu64" compile=%"PRIu64" chain=%"PRIu64" irq=%"PRIu64
			" stop=%"PRIu64"/%"PRIu64"/%"PRIu64" exit_pc=%04X\n",
			runtime->core.cache_fast_hits - cache_fast_hits,
			runtime->core.cache_decoded_hits - cache_decoded_hits,
			runtime->core.cache_compiles - cache_compiles, runtime->core.chain_links - chain_links,
			runtime->core.chain_interrupts - chain_interrupts,
			runtime->core.chain_exit_stops - chain_exit_stops,
			runtime->core.chain_budget_stops - chain_budget_stops,
			runtime->core.chain_cache_stops - chain_cache_stops, runtime->core.chain_exit_pc);
	}
	return !runtime->halted;
}

bool dsp_runtime_is_idle(const dsp_runtime_t *runtime) {
	return qatomic_read(&runtime->idle);
}

bool dsp_runtime_take_program_start(dsp_runtime_t *runtime, uint32_t *pc) {
	if (!qatomic_xchg(&runtime->program_start, false))
		return false;
	*pc = qatomic_read(&runtime->program_start_pc);
	return true;
}

bool dsp_runtime_is_program_warming(const dsp_runtime_t *runtime) {
	return qatomic_read(&runtime->program_warming);
}

void dsp_runtime_finish_program_warmup(dsp_runtime_t *runtime) {
	qatomic_set(&runtime->program_warming, false);
	qatomic_set(&runtime->program_start, false);
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

void dsp_runtime_shared_write(dsp_runtime_t *runtime, uint16_t offset, uint16_t value) {
	g_assert(offset < runtime->config->shared_size);
	qatomic_set(&runtime->data[runtime->config->shared_base + offset], value);
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
	dsp_runtime_wake(runtime);
}

void dsp_runtime_set_input(dsp_runtime_t *runtime, size_t index, bool level) {
	dsp_bus_set_input(runtime->bus, index, level);
}

void dsp_runtime_set_gsm_signal(dsp_runtime_t *runtime, pmb887x_dsp_gsm_signal_t signal, bool level) {
	dsp_bus_set_gsm_signal(runtime->bus, signal, level);
}

uint16_t dsp_runtime_get_outputs(dsp_runtime_t *runtime) {
	return dsp_bus_get_outputs(runtime->bus);
}

uint64_t dsp_runtime_get_cache_compiles(const dsp_runtime_t *runtime) {
	return qatomic_read(&runtime->core.cache_compiles);
}

uint16_t dsp_runtime_take_output_events(dsp_runtime_t *runtime) {
	return dsp_bus_take_output_events(runtime->bus);
}

uint16_t dsp_runtime_get_comm(dsp_runtime_t *runtime) {
	return dsp_bus_get_comm(runtime->bus);
}

void dsp_runtime_set_comm(dsp_runtime_t *runtime, uint16_t value) {
	dsp_bus_set_comm(runtime->bus, value);

	dsp_runtime_wake(runtime);
}

void dsp_runtime_clear_comm(dsp_runtime_t *runtime, uint16_t value) {
	dsp_bus_clear_comm(runtime->bus, value);

	dsp_runtime_wake(runtime);
}

uint16_t dsp_runtime_take_comm_clear(dsp_runtime_t *runtime) {
	return dsp_bus_take_comm_clear(runtime->bus);
}

uint16_t dsp_runtime_take_mcu_irqs(dsp_runtime_t *runtime) {
	return dsp_bus_take_mcu_irqs(runtime->bus);
}

uint16_t dsp_runtime_get_mcu_semaphores(dsp_runtime_t *runtime) {
	return dsp_bus_get_mcu_semaphores(runtime->bus);
}

void dsp_runtime_request_mcu_semaphores(dsp_runtime_t *runtime, uint16_t value) {
	dsp_bus_request_mcu_semaphores(runtime->bus, value);

	dsp_runtime_wake(runtime);
}

void dsp_runtime_release_mcu_semaphores(dsp_runtime_t *runtime, uint16_t value) {
	dsp_bus_release_mcu_semaphores(runtime->bus, value);

	dsp_runtime_wake(runtime);
}
