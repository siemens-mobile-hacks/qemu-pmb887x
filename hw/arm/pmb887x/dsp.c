/*
 * DSP
 */
#define PMB887X_TRACE_ID		DSP
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/audio/i2s.h"
#include "hw/ssi/ssi.h"
#include "system/runstate.h"

#include "hw/arm/pmb887x/dsp/runtime.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/gen/dsp_rom.h"
#include "hw/arm/pmb887x/dsp.h"
#include "hw/arm/pmb887x/dsp/config.h"
#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/rf.h"
#include "hw/arm/pmb887x/trace.h"

#define DSP_RAM_SIZE		(DSP_IO_SIZE - DSP_RAM0)
#define DSP_BOOT_DATA_OFFSET	2
#define DSP_RUNTIME_PIPE_OFFSET	5
#define DSP_RUNTIME_PIPE_STRIDE	0x1C
#define DSP_OUTPUT_COUNT	3
#define DSP_SYNC_PERIOD_NS	(100 * SCALE_US)
#define DSP_RESET_START_CYCLES	512
#define DSP_RUN_CYCLES		16384
#define DSP_AFE_CLOCK_HZ	8000000
#define DSP_SSC_BUS_NAME	"pmb887x-dsp-ssc"
#define DSP_I2S1_TX_BUS_NAME	"pmb887x-dsp-i2s1-tx"
#define DSP_I2S1_RX_BUS_NAME	"pmb887x-dsp-i2s1-rx"
#define DSP_I2S2_TX_BUS_NAME	"pmb887x-dsp-i2s2-tx"
#define DSP_I2S2_RX_BUS_NAME	"pmb887x-dsp-i2s2-rx"
#define DSP_IQ_RX_BUS_NAME	"pmb887x-dsp-iq-rx"
#define PMB887X_DSP(obj)	OBJECT_CHECK(dsp_state_t, (obj), TYPE_PMB887X_DSP)

enum {
	DSP_BOOT_PLOAD,
	DSP_BOOT_DLOAD,
	DSP_BOOT_BRANCH,
	DSP_BOOT_PREAD,
	DSP_BOOT_DREAD,
};

typedef struct dsp_state_t dsp_state_t;
typedef struct dsp_events_t dsp_events_t;

struct dsp_events_t {
	uint16_t interrupts;
	uint16_t output_events;
	uint16_t outputs;
};

struct dsp_state_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	MemoryRegion regs;
	MemoryRegion ram;
	uint32_t revision;
	uint32_t rom_version;
	const pmb887x_dsp_config_t *config;
	bool trace_boot_mode;
	pmb887x_clc_reg_t clc;
	dsp_runtime_t *runtime;
	CPUState *cpu;
	QEMUTimer *sync_timer;
	VMChangeStateEntry *vmstate;
	uint64_t pending_cycles;
	uint64_t cycle_debt;
	uint64_t cycle_remainder;
	uint64_t afe_remainder;
	size_t pending_afe_cycles;
	int64_t sync_time_ns;
	uint32_t clock_hz;
	uint16_t comm_status;
	uint16_t comm_pending;
	Clock *gsm_clock;
	Clock *ms_clock;
	bool vm_running;
	bool comm_transition_pending;
	bool sync_queued;
	bool syncing;
	qemu_irq mcu_interrupts[PMB887X_DSP_MCU_INT_COUNT];
	qemu_irq outputs[DSP_OUTPUT_COUNT];
	qemu_irq i2s2_outputs[PMB887X_DSP_I2S_OUTPUT_COUNT];
	qemu_irq active_output;
	dsp_i2s_port_t i2s1;
	dsp_i2s_port_t i2s2;
	pmb887x_iq_bus_t *iq_rx_bus;
	SSIBus *ssc_bus;
};

static void dsp_clock_update(void *opaque);

static bool dsp_mcu_interface_is_enabled(dsp_state_t *p) {
	return pmb887x_clc_is_enabled(&p->clc) && pmb887x_clc_get_rmc(&p->clc) != 0;
}

static void dsp_update_active_output(dsp_state_t *p) {
	bool active = dsp_runtime_is_running(p->runtime) && !dsp_runtime_is_idle(p->runtime);

	qemu_set_irq(p->active_output, active);
}

static void dsp_schedule_sync(dsp_state_t *p);

static uint32_t dsp_ssc_transfer(void *opaque, uint32_t value) {
	dsp_state_t *p = opaque;
	return ssi_transfer(p->ssc_bus, value);
}

static void dsp_take_events(dsp_state_t *p, dsp_events_t *events) {
	events->interrupts = dsp_runtime_take_mcu_irqs(p->runtime);
	events->output_events = dsp_runtime_take_output_events(p->runtime);
	events->outputs = dsp_runtime_get_outputs(p->runtime);
}

static void dsp_publish_events(dsp_state_t *p, const dsp_events_t *events) {
	uint16_t interrupts = (events->interrupts & MAKE_64BIT_MASK(0, PMB887X_DSP_MCU_INT_COUNT));

	for (size_t i = 0; i < ARRAY_SIZE(p->mcu_interrupts); i++)
		if ((interrupts & BIT(i)) != 0)
			qemu_irq_pulse(p->mcu_interrupts[i]);

	for (size_t i = 0; i < ARRAY_SIZE(p->outputs); i++)
		if ((events->output_events & BIT(i)) != 0)
			qemu_set_irq(p->outputs[i], (events->outputs & BIT(i)) != 0);
}

static int64_t dsp_get_time_ns(void *_opaque) {
	return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void dsp_account_time(dsp_state_t *p) {
	int64_t now = dsp_get_time_ns(p);
	uint64_t elapsed = MAX(now - p->sync_time_ns, 0);
	uint64_t scaled_cycles = elapsed * p->clock_hz + p->cycle_remainder;
	uint64_t scaled_afe_cycles = p->afe_remainder;
	uint64_t elapsed_cycles = scaled_cycles / NANOSECONDS_PER_SECOND;
	if (p->clock_hz != 0)
		scaled_afe_cycles += elapsed * DSP_AFE_CLOCK_HZ;

	p->sync_time_ns = now;
	if (p->cycle_debt >= elapsed_cycles) {
		p->cycle_debt -= elapsed_cycles;
	} else {
		p->pending_cycles += elapsed_cycles - p->cycle_debt;
		p->cycle_debt = 0;
	}
	p->cycle_remainder = scaled_cycles % NANOSECONDS_PER_SECOND;
	p->pending_afe_cycles += scaled_afe_cycles / NANOSECONDS_PER_SECOND;
	p->afe_remainder = scaled_afe_cycles % NANOSECONDS_PER_SECOND;
}

static void dsp_sync(dsp_state_t *p) {
	dsp_events_t events = {};
	uint64_t cache_compiles = dsp_runtime_get_cache_compiles(p->runtime);

	dsp_account_time(p);
	if (!p->vm_running || p->clock_hz == 0)
		return;
	dsp_runtime_update_time(p->runtime);

	size_t remaining_cycles = p->pending_cycles;
	size_t peripheral_cycles = 0;

	p->pending_cycles = 0;
	/*
	 * Account every elapsed cycle in peripherals, but do not let a
	 * polling program monopolize the main loop while catching up.
	 * The core gets a complete maximum-frequency sync slice. Independent
	 * peripherals can advance in bulk without making it carry their backlog.
	 */
	bool core_catchup = dsp_runtime_needs_core_catchup(p->runtime);

	if (remaining_cycles > DSP_RUN_CYCLES) {
		if (core_catchup) {
			p->pending_cycles += remaining_cycles - DSP_RUN_CYCLES;
		} else {
			peripheral_cycles = remaining_cycles - DSP_RUN_CYCLES;
		}
		remaining_cycles = DSP_RUN_CYCLES;
	}
	if (peripheral_cycles != 0)
		dsp_runtime_advance_idle(p->runtime, peripheral_cycles, 0);
	p->syncing = true;
	while (remaining_cycles != 0 && dsp_runtime_is_running(p->runtime)) {
		if (dsp_runtime_is_idle(p->runtime)) {
			remaining_cycles -= dsp_runtime_advance_idle_until_event(p->runtime, remaining_cycles);
			continue;
		}

		bool deferred;
		size_t executed = dsp_runtime_run(p->runtime, remaining_cycles, &deferred);

		if (executed > remaining_cycles) {
			p->cycle_debt += executed - remaining_cycles;
			remaining_cycles = 0;
		} else {
			remaining_cycles -= executed;
		}
		if (executed == 0 && !deferred && dsp_runtime_is_idle(p->runtime))
			continue;
		if (executed == 0 || deferred || p->comm_transition_pending) {
			p->pending_cycles += remaining_cycles;
			remaining_cycles = 0;
			break;
		}
	}
	size_t advanced_afe_cycles = dsp_runtime_advance_idle(p->runtime, remaining_cycles, p->pending_afe_cycles);
	p->pending_afe_cycles -= advanced_afe_cycles;
	p->syncing = false;

	p->comm_status = dsp_runtime_get_comm(p->runtime);
	dsp_update_active_output(p);
	dsp_take_events(p, &events);
	dsp_publish_events(p, &events);

	cache_compiles = dsp_runtime_get_cache_compiles(p->runtime) - cache_compiles;
	if (dsp_runtime_is_program_warming(p->runtime) && dsp_runtime_is_idle(p->runtime) &&
		cache_compiles == 0 && p->comm_status == 0) {
		dsp_runtime_finish_program_warmup(p->runtime);
	}

	if (cache_compiles != 0) {
		/* Do not carry a cold-compilation pause into the next DSP interval. */
		p->sync_time_ns = dsp_get_time_ns(p);
	}
}

static void dsp_sync_on_cpu(CPUState *_cpu, run_on_cpu_data data) {
	dsp_state_t *p = data.host_ptr;

	p->sync_queued = false;
	if (!p->comm_transition_pending)
		dsp_sync(p);
}

static void dsp_schedule_sync(dsp_state_t *p) {
	if (!p->vm_running || p->clock_hz == 0 || p->syncing || p->sync_queued)
		return;

	p->sync_queued = true;
	async_run_on_cpu(p->cpu, dsp_sync_on_cpu, RUN_ON_CPU_HOST_PTR(p));
}

static void dsp_sync_access(dsp_state_t *p) {
	if (current_cpu == p->cpu) {
		dsp_sync(p);
	} else {
		dsp_schedule_sync(p);
	}
}

static void dsp_sync_timer(void *opaque) {
	dsp_state_t *p = opaque;

	p->comm_transition_pending = false;
	dsp_sync_access(p);
	if (p->vm_running && p->clock_hz != 0)
		timer_mod(p->sync_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DSP_SYNC_PERIOD_NS);
}

static void dsp_update_timer(dsp_state_t *p) {
	if (p->vm_running && p->clock_hz != 0) {
		timer_mod(p->sync_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DSP_SYNC_PERIOD_NS);
	} else {
		timer_del(p->sync_timer);
	}
}

static void dsp_clock_update(void *opaque) {
	dsp_state_t *p = opaque;
	uint32_t clock_hz = pmb887x_clc_get_input_hz(&p->clc);

	if (!dsp_mcu_interface_is_enabled(p) && dsp_runtime_is_idle(p->runtime))
		clock_hz = 0;
	if (clock_hz != p->clock_hz) {
		p->comm_transition_pending = false;
		dsp_sync(p);
	}
	p->clock_hz = clock_hz;
	dsp_runtime_set_clock_hz(p->runtime, p->clock_hz);
	dsp_runtime_set_clock_divider(p->runtime, 1);
	dsp_update_active_output(p);
	dsp_update_timer(p);
	dsp_schedule_sync(p);
}

static void dsp_ms_clock_update(void *opaque, ClockEvent _event) {
	dsp_state_t *p = opaque;
	if (p->runtime == NULL)
		return;
	dsp_account_time(p);
	dsp_runtime_set_ms_clock_hz(p->runtime, clock_get_hz(p->ms_clock));
	dsp_schedule_sync(p);
}

static void dsp_vm_state_change(void *opaque, bool running, RunState _state) {
	dsp_state_t *p = opaque;

	dsp_account_time(p);
	p->vm_running = running;
	dsp_update_timer(p);
	dsp_schedule_sync(p);
}

static void dsp_notify_activity(void *opaque) {
	dsp_state_t *p = opaque;

	dsp_runtime_wake(p->runtime);
	dsp_update_active_output(p);
	dsp_schedule_sync(p);
}

static void dsp_notify_comm(void *opaque, uint16_t flags, bool set) {
	dsp_state_t *p = opaque;
	DPRINTF("comm: flags=%04X set=%u pc=%05X time=%" PRId64 "\n", flags, set,
		dsp_runtime_get_pc(p->runtime), dsp_get_time_ns(p));

	if (set) {
		p->comm_status |= flags;
	} else {
		p->comm_status &= (uint16_t) ~flags;
		p->comm_pending &= (uint16_t) ~flags;
	}
	p->comm_transition_pending = true;
	dsp_runtime_request_reschedule(p->runtime);
	if (p->vm_running && p->clock_hz != 0)
		timer_mod(p->sync_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DSP_SYNC_PERIOD_NS);
}

static void dsp_reset_internal_state(dsp_state_t *p) {
	dsp_runtime_reset(p->runtime);
	p->comm_status = 0;
	p->comm_pending = 0;
	p->comm_transition_pending = false;
	p->pending_cycles = 0;
	p->cycle_debt = 0;
	p->pending_afe_cycles = 0;
	p->cycle_remainder = 0;
	p->afe_remainder = 0;
	p->sync_time_ns = dsp_get_time_ns(p);
	for (size_t i = 0; i < ARRAY_SIZE(p->outputs); i++)
		qemu_irq_lower(p->outputs[i]);
	dsp_update_active_output(p);
	p->trace_boot_mode = true;
}

static void dsp_reset_input(void *opaque, int _id, int level) {
	dsp_state_t *p = opaque;

	if (!level)
		return;

	dsp_reset_internal_state(p);
	if (p->vm_running && p->clock_hz != 0) {
		/* Let the reset vector start before the MCU resumes. */
		p->pending_cycles = DSP_RESET_START_CYCLES;
		dsp_sync(p);
	} else {
		dsp_schedule_sync(p);
	}
}

static const char *dsp_boot_command_name(uint16_t command) {
	switch (command) {
		case DSP_BOOT_PLOAD:
			return "PLOAD";
		case DSP_BOOT_DLOAD:
			return "DLOAD";
		case DSP_BOOT_BRANCH:
			return "BRANCH";
		case DSP_BOOT_PREAD:
			return "PREAD";
		case DSP_BOOT_DREAD:
			return "DREAD";
		default:
			return "UNKNOWN";
	}
}

static void dsp_trace_command(dsp_state_t *p, size_t pipe) {
	if (p->trace_boot_mode) {
		uint16_t command = dsp_runtime_shared_read(p->runtime, DSP_BOOT_DATA_OFFSET);
		uint16_t address = dsp_runtime_shared_read(p->runtime, DSP_BOOT_DATA_OFFSET + 1);
		uint16_t words = 0;

		if (command != DSP_BOOT_BRANCH)
			words = dsp_runtime_shared_read(p->runtime, DSP_BOOT_DATA_OFFSET + 2);

		DPRINTF("boot command: %s(%u) address=%04X words=%u\n", dsp_boot_command_name(command), command, address, words);

		if (command == DSP_BOOT_BRANCH)
			p->trace_boot_mode = false;
		return;
	}

	uint16_t offset = DSP_RUNTIME_PIPE_OFFSET + pipe * DSP_RUNTIME_PIPE_STRIDE;
	uint16_t command = dsp_runtime_shared_read(p->runtime, offset);
	DPRINTF("runtime command: pipe=%zu command=%u (0x%04X)\n", pipe, command, command);
}

static void dsp_interrupt_input(void *opaque, int id, int level) {
	dsp_state_t *p = opaque;

	if (!level)
		return;

	if (pmb887x_trace_log_enabled(PMB887X_TRACE_DSP))
		dsp_trace_command(p, id);

	dsp_runtime_set_request(p->runtime, id, true);
	p->comm_transition_pending = false;
	dsp_update_active_output(p);
	dsp_schedule_sync(p);
}

static void dsp_set_input(dsp_state_t *p, size_t index, int level) {
	p->comm_transition_pending = false;
	dsp_runtime_set_input(p->runtime, index, level != 0);
	dsp_schedule_sync(p);
}

static void dsp_input0(void *opaque, int _id, int level) {
	dsp_set_input(opaque, 0, level);
}

static void dsp_input1(void *opaque, int _id, int level) {
	dsp_set_input(opaque, 1, level);
}

void pmb887x_dsp_set_gsm_signal(
	DeviceState *dev,
	pmb887x_dsp_gsm_signal_t signal,
	bool level,
	int64_t timestamp_ns
) {
	dsp_state_t *p = PMB887X_DSP(dev);

	p->comm_transition_pending = false;
	dsp_sync_access(p);
	dsp_runtime_set_gsm_signal(p->runtime, signal, level, timestamp_ns);
	dsp_schedule_sync(p);
}

static void dsp_gsm_input(void *opaque, int signal, int level) {
	pmb887x_dsp_set_gsm_signal(DEVICE(opaque), signal, level != 0,
		qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void dsp_i2s2_input(void *_opaque, int _signal, int _level) {
	/* The GPIOs describe pin state; audio frames use the I2S bus. */
}

static uint64_t dsp_io_read(void *opaque, hwaddr haddr, unsigned size) {
	dsp_state_t *p = opaque;
	uint64_t value = 0;
	bool observe_comm_transition = haddr == DSP_COM_STATUS && p->comm_transition_pending;

	if (!observe_comm_transition) {
		p->comm_transition_pending = false;
		dsp_sync_access(p);
	}

	switch (haddr) {
		case DSP_CLC:
			value = pmb887x_clc_get(&p->clc);
			if (pmb887x_clc_get_rmc(&p->clc) == 0)
				value |= MOD_CLC_DISS;
			break;

		case DSP_ID:
			value = (0xF022C000 | p->revision);
			break;

		case DSP_COM_STATUS: {
			uint32_t program_start_pc;

			value = p->comm_status;
			if (observe_comm_transition) {
				p->comm_transition_pending = false;
				dsp_schedule_sync(p);
			}
			if (dsp_runtime_take_program_start(p->runtime, &program_start_pc))
				DPRINTF("cold program start: pc=%05X flags=%04" PRIX64 "\n", program_start_pc, value);
			break;
		}

		case DSP_COM_SET:
		case DSP_COM_CLEAR:
		case DSP_SEM_SET:
		case DSP_SEM_CLEAR:
			break;

		case DSP_SEM_STATUS:
			value = dsp_runtime_get_mcu_semaphores(p->runtime);
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}

	IO_DUMP_READ(haddr + p->mmio.addr, size, value);
	return value;
}

static void dsp_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	dsp_state_t *p = opaque;

	p->comm_transition_pending = false;
	dsp_sync_access(p);

	switch (haddr) {
		case DSP_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case DSP_COM_SET:
			dsp_runtime_set_comm(p->runtime, (value & DSP_COM_SET_FLAGS));
			p->comm_status |= (value & DSP_COM_SET_FLAGS);
			p->comm_pending |= (value & DSP_COM_SET_FLAGS);
			break;

		case DSP_COM_CLEAR:
			dsp_runtime_clear_comm(p->runtime, (value & DSP_COM_CLEAR_FLAGS));
			p->comm_status &= (uint16_t) ~(value & DSP_COM_CLEAR_FLAGS);
			p->comm_pending &= (uint16_t) ~(value & DSP_COM_CLEAR_FLAGS);
			break;

		case DSP_SEM_SET:
			dsp_runtime_request_mcu_semaphores(p->runtime, (value & DSP_SEM_SET_FLAGS));
			break;

		case DSP_SEM_CLEAR:
			dsp_runtime_release_mcu_semaphores(p->runtime, (value & DSP_SEM_CLEAR_FLAGS));
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}

	dsp_schedule_sync(p);
	IO_DUMP_WRITE(haddr + p->mmio.addr, size, value);
}

static const MemoryRegionOps io_ops = {
	.read			= dsp_io_read,
	.write			= dsp_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 4,
		.max_access_size	= 4,
	},
};

static uint64_t dsp_ram_read(void *opaque, hwaddr haddr, unsigned size) {
	dsp_state_t *p = opaque;
	uint64_t value;

	if (!dsp_mcu_interface_is_enabled(p)) {
		IO_DUMP_READ(haddr + p->mmio.addr + DSP_RAM0, size, 0);
		return 0;
	}
	if (!p->comm_transition_pending)
		dsp_sync_access(p);
	value = dsp_runtime_shared_read_bytes(p->runtime, haddr, size);

	IO_DUMP_READ(haddr + p->mmio.addr + DSP_RAM0, size, value);
	return value;
}

static void dsp_ram_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	dsp_state_t *p = opaque;

	if (!dsp_mcu_interface_is_enabled(p)) {
		IO_DUMP_WRITE(haddr + p->mmio.addr + DSP_RAM0, size, value);
		return;
	}
	p->comm_transition_pending = false;
	dsp_sync_access(p);

	dsp_runtime_shared_write_bytes(p->runtime, haddr, value, size);
	dsp_schedule_sync(p);

	IO_DUMP_WRITE(haddr + p->mmio.addr + DSP_RAM0, size, value);
}

static const MemoryRegionOps ram_io_ops = {
	.read			= dsp_ram_read,
	.write			= dsp_ram_write,
	.endianness		= DEVICE_LITTLE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4,
		.unaligned		= true,
	},
	.impl			= {
		.min_access_size	= 1,
		.max_access_size	= 4,
		.unaligned		= true,
	},
};

static void dsp_init(Object *obj) {
	dsp_state_t *p = PMB887X_DSP(obj);

	pmb887x_clc_init(&p->clc, DEVICE(obj));
	pmb887x_clc_set_callback(&p->clc, dsp_clock_update, p);
	p->ssc_bus = ssi_create_bus(DEVICE(obj), DSP_SSC_BUS_NAME);
	p->i2s1.tx_bus = i2s_tx_bus_init(DEVICE(obj), DSP_I2S1_TX_BUS_NAME);
	p->i2s1.rx_bus = i2s_rx_bus_init(DEVICE(obj), DSP_I2S1_RX_BUS_NAME);
	p->i2s2.tx_bus = i2s_tx_bus_init(DEVICE(obj), DSP_I2S2_TX_BUS_NAME);
	p->i2s2.rx_bus = i2s_rx_bus_init(DEVICE(obj), DSP_I2S2_RX_BUS_NAME);
	p->iq_rx_bus = pmb887x_iq_bus_create(DEVICE(obj), DSP_IQ_RX_BUS_NAME);
	memory_region_init(&p->mmio, obj, "pmb887x-dsp", DSP_IO_SIZE);
	memory_region_init_io(&p->regs, obj, &io_ops, p, "pmb887x-dsp-regs", DSP_RAM0);
	memory_region_init_io(&p->ram, obj, &ram_io_ops, p, "pmb887x-dsp-ram", DSP_RAM_SIZE);
	memory_region_add_subregion(&p->mmio, 0, &p->regs);
	memory_region_add_subregion(&p->mmio, DSP_RAM0, &p->ram);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_reset_input, "RESET_IN", 1);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_interrupt_input, "INT_IN", PMB887X_DSP_INT_COUNT);
	qdev_init_gpio_out_named(DEVICE(obj), p->mcu_interrupts, "INT_OUT", ARRAY_SIZE(p->mcu_interrupts));
	qdev_init_gpio_in_named(DEVICE(obj), dsp_input0, "DSPIN0_IN", 1);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_input1, "DSPIN1_IN", 1);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_gsm_input, "GSM_IN", PMB887X_DSP_GSM_SIGNAL_COUNT);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_i2s2_input, "I2S2_RX_IN", 1);
	p->gsm_clock = qdev_init_clock_in(DEVICE(obj), "GSM_CLOCK", NULL, p, 0);
	p->ms_clock = qdev_init_clock_in(DEVICE(obj), "MS_CLOCK", dsp_ms_clock_update, p, ClockUpdate);
	qdev_init_gpio_out_named(DEVICE(obj), &p->outputs[0], "DSPOUT0_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &p->outputs[1], "DSPOUT1_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &p->outputs[2], "DSPOUT2_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &p->i2s2_outputs[PMB887X_DSP_I2S_OUTPUT_CLK0], "I2S2_CLK0_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &p->i2s2_outputs[PMB887X_DSP_I2S_OUTPUT_TX], "I2S2_TX_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &p->i2s2_outputs[PMB887X_DSP_I2S_OUTPUT_WA0], "I2S2_WA0_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &p->i2s2_outputs[PMB887X_DSP_I2S_OUTPUT_CLK1], "I2S2_CLK1_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &p->i2s2_outputs[PMB887X_DSP_I2S_OUTPUT_WA1], "I2S2_WA1_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &p->active_output, "ACTIVE_OUT", 1);
}

static void dsp_reset(DeviceState *dev) {
	dsp_state_t *p = PMB887X_DSP(dev);

	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	p->clock_hz = 0;
	dsp_runtime_set_clock_hz(p->runtime, 0);
	dsp_update_active_output(p);
	dsp_update_timer(p);
	dsp_reset_internal_state(p);
}

void pmb887x_dsp_set_config(DeviceState *dev, const pmb887x_dsp_config_t *config) {
	dsp_state_t *p = PMB887X_DSP(dev);
	p->config = config;
}

static const Property dsp_properties[] = {
	DEFINE_PROP_UINT32("revision", dsp_state_t, revision, 0),
	DEFINE_PROP_UINT32("rom_version", dsp_state_t, rom_version, 0),
	DEFINE_PROP_LINK("cpu", dsp_state_t, cpu, TYPE_CPU, CPUState *),
	DEFINE_PROP_LINK("bus_ssc", dsp_state_t, ssc_bus, "SSI", SSIBus *),
	DEFINE_PROP_LINK("bus_i2s1_tx", dsp_state_t, i2s1.tx_bus, TYPE_I2S_TX_BUS, I2STxBus *),
	DEFINE_PROP_LINK("bus_i2s1_rx", dsp_state_t, i2s1.rx_bus, TYPE_I2S_RX_BUS, I2SRxBus *),
	DEFINE_PROP_LINK("bus_i2s2_tx", dsp_state_t, i2s2.tx_bus, TYPE_I2S_TX_BUS, I2STxBus *),
	DEFINE_PROP_LINK("bus_i2s2_rx", dsp_state_t, i2s2.rx_bus, TYPE_I2S_RX_BUS, I2SRxBus *),
	DEFINE_PROP_LINK("bus_iq_rx", dsp_state_t, iq_rx_bus, TYPE_PMB887X_IQ_BUS, pmb887x_iq_bus_t *),
};

static void dsp_realize(DeviceState *dev, Error **errp) {
	dsp_state_t *p = PMB887X_DSP(dev);
	const pmb887x_dsp_config_t *config;
	const pmb887x_dsp_rom_t *rom;
	size_t shared_ram_size;

	if (p->config == NULL) {
		error_setg(errp, "DSP configuration is not set");
		return;
	}
	if (p->cpu == NULL) {
		error_setg(errp, "DSP CPU link is not set");
		return;
	}

	config = p->config;

	shared_ram_size = config->shared_size * sizeof(uint16_t);
	memory_region_set_size(&p->ram, shared_ram_size);
	memory_region_set_size(&p->mmio, DSP_RAM0 + shared_ram_size);
	if (p->rom_version == 0)
		p->rom_version = config->default_rom_version;

	rom = pmb887x_dsp_rom_find(p->rom_version);
	if (rom == NULL) {
		error_setg(errp, "DSP MASK ROM version %04X is not embedded", p->rom_version);
		return;
	}

	const dsp_ports_t ports = {
		.i2s = {
			{
				.tx_bus = p->i2s1.tx_bus,
				.rx_bus = p->i2s1.rx_bus,
			},
			{
				.tx_bus = p->i2s2.tx_bus,
				.rx_bus = p->i2s2.rx_bus,
			},
		},
	};
	p->runtime = dsp_runtime_create(config, rom->program_rom, rom->data_rom,
		p, dsp_notify_activity, dsp_notify_comm, dsp_get_time_ns, dsp_ssc_transfer, &ports);
	dsp_runtime_set_iq_bus(p->runtime, p->iq_rx_bus);
	dsp_runtime_set_ms_clock_hz(p->runtime, clock_get_hz(p->ms_clock));
	p->sync_timer = timer_new_full(NULL, QEMU_CLOCK_VIRTUAL, SCALE_NS, QEMU_TIMER_ATTR_SOFT,
		dsp_sync_timer, p);
	p->sync_time_ns = dsp_get_time_ns(p);

	p->vmstate = qdev_add_vm_change_state_handler(dev, dsp_vm_state_change, NULL, p);
	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	dsp_reset_internal_state(p);
	DPRINTF("core initialized: cpu=%s revision=%02X rom_version=%04X\n", config->name, p->revision, p->rom_version);
}

static void dsp_unrealize(DeviceState *dev) {
	dsp_state_t *p = PMB887X_DSP(dev);

	qemu_del_vm_change_state_handler(p->vmstate);
	p->vmstate = NULL;
	timer_del(p->sync_timer);
	run_on_cpu(p->cpu, dsp_sync_on_cpu, RUN_ON_CPU_HOST_PTR(p));
	timer_free(p->sync_timer);
	p->sync_timer = NULL;

	dsp_runtime_destroy(p->runtime);
	p->runtime = NULL;
}

static void dsp_class_init(ObjectClass *klass, const void *_data) {
	DeviceClass *dc = DEVICE_CLASS(klass);

	device_class_set_props(dc, dsp_properties);
	device_class_set_legacy_reset(dc, dsp_reset);
	dc->realize = dsp_realize;
	dc->unrealize = dsp_unrealize;
}

static const TypeInfo dsp_info = {
	.name			= TYPE_PMB887X_DSP,
	.parent			= TYPE_SYS_BUS_DEVICE,
	.instance_size	= sizeof(struct dsp_state_t),
	.instance_init	= dsp_init,
	.class_init		= dsp_class_init,
};

static void dsp_register_types(void) {
	type_register_static(&dsp_info);
}
type_init(dsp_register_types)
