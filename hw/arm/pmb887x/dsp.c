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
#include "qemu/atomic.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/ssi/ssi.h"
#include "system/runstate.h"

#include "hw/arm/pmb887x/dsp/runtime.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/gen/dsp_rom.h"
#include "hw/arm/pmb887x/dsp.h"
#include "hw/arm/pmb887x/dsp/config.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define DSP_RAM_SIZE		(DSP_IO_SIZE - DSP_RAM0)
#define DSP_BOOT_DATA_OFFSET	2
#define DSP_RUNTIME_PIPE_OFFSET	5
#define DSP_RUNTIME_PIPE_STRIDE	0x1C
#define DSP_OUTPUT_COUNT	3
#define DSP_BASEBAND_SYNC_TIMEOUT_MS	50
#define DSP_BASEBAND_IRQ_MASK	(TEAK_INT_FINTA0_BBHI | TEAK_INT_FINTA0_BBLO | TEAK_INT_FINTA0_BB_FULL)
#define DSP_SSC_BUS_NAME	"pmb887x-dsp-ssc"
#define TYPE_PMB887X_DSP	"pmb887x-dsp"
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
typedef struct dsp_worker_t dsp_worker_t;

struct dsp_events_t {
	uint16_t interrupts;
	uint16_t output_events;
	uint16_t outputs;
};

struct dsp_worker_t {
	QEMUBH *bh;
	QemuThread thread;
	QemuMutex mutex;
	QemuCond cond;
	QemuCond idle_cond;
	QemuEvent event;
	uint16_t interrupt_events;
	uint16_t output_events;
	uint16_t outputs;
	uint64_t command_sequence;
	bool enabled;
	bool busy;
	bool sync_requested;
	bool reset;
	bool stop;
	bool created;
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
	dsp_worker_t worker;
	bool runtime_running;
	VMChangeStateEntry *vmstate;
	uint16_t comm_status;
	uint16_t reset_comm_flags;
	uint16_t reset_requests;
	uint64_t command_sequence;
	int64_t command_host_time;
	int64_t command_virtual_time;
	uint32_t command_status_reads;
	uint16_t command_flags;
	Clock *gsm_clock;
	bool reset_pending;
	bool vm_running;
	qemu_irq mcu_interrupts[PMB887X_DSP_MCU_INT_COUNT];
	qemu_irq outputs[DSP_OUTPUT_COUNT];
	SSIBus *ssc_bus;
};

static uint32_t dsp_ssc_transfer(void *opaque, uint32_t value) {
	dsp_state_t *p = opaque;
	bool locked = bql_locked();
	uint32_t received;

	if (!locked)
		bql_lock();
	received = ssi_transfer(p->ssc_bus, value);
	if (!locked)
		bql_unlock();
	return received;
}

static void dsp_run(dsp_state_t *p, dsp_events_t *events) {
	p->runtime_running = dsp_runtime_run(p->runtime);
	events->interrupts = dsp_runtime_take_mcu_irqs(p->runtime);
	events->output_events = dsp_runtime_take_output_events(p->runtime);
	events->outputs = dsp_runtime_get_outputs(p->runtime);
}

static bool dsp_runnable(dsp_state_t *p) {
	if (!p->runtime_running)
		return false;
	return !dsp_runtime_is_idle(p->runtime);
}

static void dsp_worker_bh(void *opaque) {
	dsp_state_t *p = opaque;
	uint16_t events = qatomic_xchg(&p->worker.interrupt_events, 0);
	uint16_t output_events = qatomic_xchg(&p->worker.output_events, 0);
	uint16_t outputs = qatomic_read(&p->worker.outputs);
	bool locked = bql_locked();

	events &= MAKE_64BIT_MASK(0, PMB887X_DSP_MCU_INT_COUNT);

	if (!locked)
		bql_lock();
	for (size_t i = 0; i < ARRAY_SIZE(p->mcu_interrupts); i++)
		if ((events & BIT(i)) != 0)
			qemu_irq_raise(p->mcu_interrupts[i]);

	for (size_t i = 0; i < ARRAY_SIZE(p->outputs); i++)
		if ((output_events & BIT(i)) != 0)
			qemu_set_irq(p->outputs[i], (outputs & BIT(i)) != 0);
	if (!locked)
		bql_unlock();
}

static void dsp_worker_publish_events(dsp_state_t *p, const dsp_events_t *events) {
	if (events->interrupts == 0 && events->output_events == 0)
		return;

	qatomic_or(&p->worker.interrupt_events, events->interrupts);
	qatomic_set(&p->worker.outputs, events->outputs);
	qatomic_or(&p->worker.output_events, events->output_events);
	qemu_bh_schedule(p->worker.bh);
}

static void *dsp_worker(void *opaque) {
	dsp_state_t *p = opaque;

	dsp_runtime_thread_enter();

	qemu_mutex_lock(&p->worker.mutex);
	while (!p->worker.stop) {
		dsp_events_t events = {};
		uint64_t command_sequence;
		bool runnable;

		while (!p->worker.enabled && !p->worker.reset && !p->worker.stop)
			qemu_cond_wait(&p->worker.cond, &p->worker.mutex);

		if (p->worker.stop)
			break;

		if (p->worker.reset) {
			bool run_startup = p->worker.enabled;
			uint16_t comm_flags;
			uint16_t requests;

			p->worker.busy = true;
			p->worker.reset = false;
			qemu_mutex_unlock(&p->worker.mutex);
			dsp_runtime_reset(p->runtime);
			p->runtime_running = true;
			DPRINTF("core reset: startup=%d\n", run_startup);

			if (run_startup)
				dsp_run(p, &events);

			qemu_mutex_lock(&p->worker.mutex);
			p->worker.busy = false;
			p->worker.sync_requested = false;
			qemu_cond_broadcast(&p->worker.idle_cond);
			if (p->worker.reset)
				continue;

			comm_flags = qatomic_read(&p->reset_comm_flags);
			requests = qatomic_read(&p->reset_requests);
			dsp_runtime_set_comm(p->runtime, comm_flags);
			qatomic_set(&p->comm_status, dsp_runtime_get_comm(p->runtime));
			for (size_t i = 0; i < PMB887X_DSP_INT_COUNT; i++)
				if ((requests & BIT(i)) != 0)
					dsp_runtime_set_request(p->runtime, i, true);

			qatomic_set(&p->reset_pending, false);
			qatomic_set(&p->reset_comm_flags, 0);
			qatomic_set(&p->reset_requests, 0);
			qemu_cond_broadcast(&p->worker.idle_cond);
			dsp_worker_publish_events(p, &events);
			continue;
		}

		runnable = dsp_runnable(p);
		if (!runnable) {
			qemu_event_reset(&p->worker.event);
			runnable = dsp_runnable(p);
			if (!runnable) {
				qemu_mutex_unlock(&p->worker.mutex);
				qemu_event_wait(&p->worker.event);
				qemu_mutex_lock(&p->worker.mutex);
				continue;
			}
		}

		p->worker.busy = true;
		qemu_mutex_unlock(&p->worker.mutex);

		command_sequence = qatomic_read(&p->command_sequence);
		if (command_sequence != p->worker.command_sequence) {
			int64_t host_delay = qemu_clock_get_ns(QEMU_CLOCK_HOST) - qatomic_read(&p->command_host_time);
			int64_t virtual_delay = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
				qatomic_read(&p->command_virtual_time);

			p->worker.command_sequence = command_sequence;
			DPRINTF("command start: sequence=%" PRIu64 " flags=%04X host_delay=%" PRId64
				" ns virtual_delay=%" PRId64 " ns\n",
				command_sequence, qatomic_read(&p->command_flags), host_delay, virtual_delay);
		}

		dsp_run(p, &events);

		qemu_mutex_lock(&p->worker.mutex);
		qatomic_set(&p->comm_status, dsp_runtime_get_comm(p->runtime));
		p->worker.busy = false;
		p->worker.sync_requested = false;
		qemu_cond_broadcast(&p->worker.idle_cond);

		if (p->worker.reset)
			continue;
		dsp_worker_publish_events(p, &events);
	}

	qemu_mutex_unlock(&p->worker.mutex);
	dsp_runtime_thread_exit();
	return NULL;
}

static void dsp_worker_set_enabled(dsp_state_t *p, bool enabled) {
	qemu_mutex_lock(&p->worker.mutex);
	p->worker.enabled = enabled;
	if (enabled) {
		qemu_cond_signal(&p->worker.cond);
		qemu_event_set(&p->worker.event);
	}
	qemu_mutex_unlock(&p->worker.mutex);
}

static void dsp_vm_state_change(void *opaque, bool running, RunState state) {
	dsp_state_t *p = opaque;
	bool enabled = running && pmb887x_clc_is_enabled(&p->clc);

	p->vm_running = running;

	qemu_mutex_lock(&p->worker.mutex);
	p->worker.enabled = enabled;
	if (enabled) {
		qemu_cond_signal(&p->worker.cond);
		qemu_event_set(&p->worker.event);
	}

	while (!running && (p->worker.busy || p->worker.reset))
		qemu_cond_wait(&p->worker.idle_cond, &p->worker.mutex);
	qemu_mutex_unlock(&p->worker.mutex);
}

static void dsp_worker_kick(void *opaque) {
	dsp_state_t *p = opaque;

	if (qemu_thread_is_self(&p->worker.thread)) {
		qemu_event_set(&p->worker.event);
		return;
	}

	qemu_mutex_lock(&p->worker.mutex);
	if (p->worker.busy) {
		dsp_runtime_kick(p->runtime);
	} else {
		dsp_runtime_wake(p->runtime);
	}
	qemu_event_set(&p->worker.event);
	qemu_mutex_unlock(&p->worker.mutex);
}

static void dsp_worker_notify_activity(void *opaque) {
	dsp_state_t *p = opaque;

	if (qemu_thread_is_self(&p->worker.thread)) {
		qemu_event_set(&p->worker.event);
		return;
	}

	qemu_mutex_lock(&p->worker.mutex);
	dsp_runtime_wake(p->runtime);
	qemu_event_set(&p->worker.event);
	qemu_mutex_unlock(&p->worker.mutex);
}

static void dsp_worker_notify_comm(void *opaque, uint16_t flags, bool set) {
	dsp_state_t *p = opaque;

	if (set) {
		qatomic_or(&p->comm_status, flags);
		return;
	}

	qatomic_and(&p->comm_status, (uint16_t) ~flags);

	uint64_t sequence = qatomic_read(&p->command_sequence);
	if (sequence == 0)
		return;

	int64_t host_delay = qemu_clock_get_ns(QEMU_CLOCK_HOST) - qatomic_read(&p->command_host_time);
	int64_t virtual_delay = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - qatomic_read(&p->command_virtual_time);

	DPRINTF("command clear: sequence=%" PRIu64 " flags=%04X host_delay=%" PRId64
		" ns virtual_delay=%" PRId64 " ns status_reads=%u\n",
		sequence, flags, host_delay, virtual_delay, qatomic_read(&p->command_status_reads));
}

static void dsp_worker_synchronize_cold_program(dsp_state_t *p) {
	int64_t start = qemu_clock_get_ns(QEMU_CLOCK_HOST);
	uint64_t cache_compiles = dsp_runtime_get_cache_compiles(p->runtime);
	bool waited = false;

	qemu_mutex_lock(&p->worker.mutex);
	if (p->worker.enabled && !p->worker.stop && dsp_runnable(p)) {
		p->worker.sync_requested = true;
		qemu_event_set(&p->worker.event);
	}
	while (p->worker.sync_requested && p->worker.enabled && !p->worker.stop) {
		waited = true;
		qemu_cond_wait(&p->worker.idle_cond, &p->worker.mutex);
	}
	qemu_mutex_unlock(&p->worker.mutex);

	cache_compiles = dsp_runtime_get_cache_compiles(p->runtime) - cache_compiles;
	if (cache_compiles == 0 && dsp_runtime_get_comm(p->runtime) == 0)
		dsp_runtime_finish_program_warmup(p->runtime);

	DPRINTF("cold program sync: waited=%u host_delay=%" PRId64 " ns compile=%" PRIu64 " warming=%u\n",
		waited, qemu_clock_get_ns(QEMU_CLOCK_HOST) - start, cache_compiles,
		dsp_runtime_is_program_warming(p->runtime));
}

static void dsp_reset_internal_state(dsp_state_t *p) {
	qemu_bh_cancel(p->worker.bh);

	qemu_mutex_lock(&p->worker.mutex);
	p->worker.reset = true;
	qatomic_set(&p->reset_pending, true);
	p->worker.enabled = p->vm_running && pmb887x_clc_is_enabled(&p->clc);
	qatomic_set(&p->worker.interrupt_events, 0);
	qatomic_set(&p->worker.output_events, 0);
	qatomic_set(&p->worker.outputs, 0);
	qatomic_set(&p->comm_status, 0);
	qatomic_set(&p->reset_comm_flags, 0);
	qatomic_set(&p->reset_requests, 0);
	for (size_t i = 0; i < ARRAY_SIZE(p->outputs); i++)
		qemu_irq_lower(p->outputs[i]);
	p->trace_boot_mode = true;
	qemu_cond_signal(&p->worker.cond);
	qemu_event_set(&p->worker.event);
	qemu_mutex_unlock(&p->worker.mutex);
}

static void dsp_reset_input(void *opaque, int id, int level) {
	if (level)
		dsp_reset_internal_state(opaque);
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

	if (!qatomic_read(&p->reset_pending)) {
		dsp_runtime_set_request(p->runtime, id, true);
		dsp_worker_kick(p);
		return;
	}

	qemu_mutex_lock(&p->worker.mutex);
	if (qatomic_read(&p->reset_pending)) {
		qatomic_or(&p->reset_requests, BIT(id));
	} else {
		dsp_runtime_set_request(p->runtime, id, true);
	}
	qemu_mutex_unlock(&p->worker.mutex);
	dsp_worker_kick(p);
}

static void dsp_set_input(dsp_state_t *p, size_t index, int level) {
	dsp_runtime_set_input(p->runtime, index, level != 0);
}

static void dsp_input0(void *opaque, int id, int level) {
	dsp_set_input(opaque, 0, level);
}

static void dsp_input1(void *opaque, int id, int level) {
	dsp_set_input(opaque, 1, level);
}

static bool dsp_baseband_event_blocked(dsp_state_t *p) {
	uint16_t flags = dsp_runtime_get_irq_flags(p->runtime, 0);

	return dsp_runtime_is_maskable_interrupt_active(p->runtime) || (flags & DSP_BASEBAND_IRQ_MASK) != 0;
}

static void dsp_wait_baseband_irq(dsp_state_t *p, int signal, int level) {
	if (!dsp_baseband_event_blocked(p))
		return;

	int64_t start = qemu_clock_get_ns(QEMU_CLOCK_HOST);
	int64_t deadline = start + DSP_BASEBAND_SYNC_TIMEOUT_MS * SCALE_MS;
	bool timed_out = false;

	dsp_worker_kick(p);
	qemu_mutex_lock(&p->worker.mutex);
	while (dsp_baseband_event_blocked(p)) {
		bool worker_stopped = !p->worker.enabled || !p->runtime_running || p->worker.stop;

		if (worker_stopped)
			break;

		int64_t remaining = deadline - qemu_clock_get_ns(QEMU_CLOCK_HOST);

		if (remaining <= 0) {
			timed_out = true;
			break;
		}
		qemu_cond_timedwait(&p->worker.idle_cond, &p->worker.mutex, DIV_ROUND_UP(remaining, SCALE_MS));
	}
	bool ready = !dsp_baseband_event_blocked(p);
	qemu_mutex_unlock(&p->worker.mutex);

	int64_t host_wait = qemu_clock_get_ns(QEMU_CLOCK_HOST) - start;
	uint16_t flags = dsp_runtime_get_irq_flags(p->runtime, 0) & DSP_BASEBAND_IRQ_MASK;
	uint32_t pc = dsp_runtime_get_pc(p->runtime);

	DPRINTF("ARM waited for DSP Baseband ISR: signal=%d level=%d flags=%04X active=%u pc=%05X host_wait=%" PRId64
		" ns ready=%u timeout=%u\n", signal, level, flags, dsp_runtime_is_maskable_interrupt_active(p->runtime), pc,
		host_wait, ready, timed_out);
}

static void dsp_gsm_input(void *opaque, int signal, int level) {
	dsp_state_t *p = opaque;
	uint32_t gsm_frequency = clock_get_hz(p->gsm_clock);
	bool baseband_irq = signal < PMB887X_DSP_GSM_SIGNAL_RXON;

	dsp_runtime_set_gsm_clock(p->runtime, gsm_frequency);
	if (baseband_irq)
		dsp_wait_baseband_irq(p, signal, level);

	dsp_runtime_set_gsm_signal(p->runtime, signal, level != 0);
}

static uint64_t dsp_io_read(void *opaque, hwaddr haddr, unsigned size) {
	dsp_state_t *p = opaque;
	uint64_t value = 0;

	switch (haddr) {
		case DSP_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case DSP_ID:
			value = 0xF022C000 | p->revision;
			break;

		case DSP_COM_STATUS: {
			uint32_t program_start_pc;
			bool reset_pending = qatomic_read(&p->reset_pending);

			if (reset_pending) {
				value = qatomic_read(&p->reset_comm_flags) | qatomic_read(&p->comm_status);
			} else {
				value = qatomic_read(&p->comm_status);
			}

			if (value != 0) {
				uint32_t reads = qatomic_fetch_inc(&p->command_status_reads) + 1;

				if (reads == 1 || reads % 100 == 0) {
					int64_t host_delay = qemu_clock_get_ns(QEMU_CLOCK_HOST) -
						qatomic_read(&p->command_host_time);
					int64_t virtual_delay = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
						qatomic_read(&p->command_virtual_time);

					DPRINTF("command pending: sequence=%" PRIu64 " flags=%04" PRIX64
						" reads=%u host_delay=%" PRId64 " ns virtual_delay=%" PRId64 " ns\n",
						qatomic_read(&p->command_sequence), value, reads, host_delay, virtual_delay);
				}
			}

			if (reset_pending || value != 0) {
				g_thread_yield();
				reset_pending = qatomic_read(&p->reset_pending);
				if (reset_pending) {
					value = qatomic_read(&p->reset_comm_flags) | qatomic_read(&p->comm_status);
				} else {
					value = qatomic_read(&p->comm_status);
				}
			}

			if (!reset_pending && dsp_runtime_take_program_start(p->runtime, &program_start_pc))
				DPRINTF("cold program start: pc=%05X flags=%04" PRIX64 "\n", program_start_pc, value);

			if (!reset_pending && dsp_runtime_is_program_warming(p->runtime)) {
				dsp_worker_synchronize_cold_program(p);
				value = qatomic_read(&p->comm_status);
			}
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
			IO_DUMP_READ(haddr + p->mmio.addr, size, 0xFFFFFFFF);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}

	IO_DUMP_READ(haddr + p->mmio.addr, size, value);
	return value;
}

static void dsp_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	dsp_state_t *p = opaque;

	IO_DUMP_WRITE(haddr + p->mmio.addr, size, value);

	switch (haddr) {
		case DSP_CLC:
			pmb887x_clc_set(&p->clc, value);
			dsp_runtime_set_clock(p->runtime, pmb887x_clc_is_enabled(&p->clc));
			dsp_worker_set_enabled(p, p->vm_running && pmb887x_clc_is_enabled(&p->clc));
			break;

		case DSP_COM_SET:
			qatomic_set(&p->command_host_time, qemu_clock_get_ns(QEMU_CLOCK_HOST));
			qatomic_set(&p->command_virtual_time, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
			qatomic_set(&p->command_status_reads, 0);
			qatomic_set(&p->command_flags, value & DSP_COM_SET_FLAGS);
			qatomic_inc(&p->command_sequence);
			DPRINTF("command set: sequence=%" PRIu64 " flags=%04" PRIX64 "\n",
				qatomic_read(&p->command_sequence), value & DSP_COM_SET_FLAGS);

			qemu_mutex_lock(&p->worker.mutex);
			if (qatomic_read(&p->reset_pending)) {
				qatomic_or(&p->reset_comm_flags, value & DSP_COM_SET_FLAGS);
			} else {
				dsp_runtime_set_comm(p->runtime, value & DSP_COM_SET_FLAGS);
				qatomic_or(&p->comm_status, value & DSP_COM_SET_FLAGS);
			}
			qemu_mutex_unlock(&p->worker.mutex);
			dsp_worker_kick(p);
			break;

		case DSP_COM_CLEAR:
			qemu_mutex_lock(&p->worker.mutex);
			if (qatomic_read(&p->reset_pending)) {
				qatomic_and(&p->reset_comm_flags, (uint16_t) ~(value & DSP_COM_CLEAR_FLAGS));
			} else {
				dsp_runtime_clear_comm(p->runtime, value & DSP_COM_CLEAR_FLAGS);
				qatomic_and(&p->comm_status, (uint16_t) ~(value & DSP_COM_CLEAR_FLAGS));
			}
			qemu_mutex_unlock(&p->worker.mutex);
			dsp_worker_kick(p);
			break;

		case DSP_SEM_SET:
			dsp_runtime_request_mcu_semaphores(p->runtime, value & DSP_SEM_SET_FLAGS);
			dsp_worker_kick(p);
			break;

		case DSP_SEM_CLEAR:
			dsp_runtime_release_mcu_semaphores(p->runtime, value & DSP_SEM_CLEAR_FLAGS);
			dsp_worker_kick(p);
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}
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
	uint64_t value = pmb887x_clc_is_enabled(&p->clc) ? dsp_runtime_shared_read_bytes(p->runtime, haddr, size) : 0;

	IO_DUMP_READ(haddr + p->mmio.addr + DSP_RAM0, size, value);
	return value;
}

static void dsp_ram_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	dsp_state_t *p = opaque;

	IO_DUMP_WRITE(haddr + p->mmio.addr + DSP_RAM0, size, value);

	if (!pmb887x_clc_is_enabled(&p->clc))
		return;

	dsp_runtime_shared_write_bytes(p->runtime, haddr, value, size);
	dsp_worker_kick(p);
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
	p->ssc_bus = ssi_create_bus(DEVICE(obj), DSP_SSC_BUS_NAME);
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
	p->gsm_clock = qdev_init_clock_in(DEVICE(obj), "GSM_CLOCK", NULL, p, 0);
	qdev_init_gpio_out_named(DEVICE(obj), &p->outputs[0], "DSPOUT0_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &p->outputs[1], "DSPOUT1_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &p->outputs[2], "DSPOUT2_OUT", 1);
}

static void dsp_reset(DeviceState *dev) {
	dsp_state_t *p = PMB887X_DSP(dev);
	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	dsp_runtime_set_clock(p->runtime, false);
	dsp_reset_internal_state(p);
}

void pmb887x_dsp_set_config(DeviceState *dev, const pmb887x_dsp_config_t *config) {
	dsp_state_t *p = PMB887X_DSP(dev);
	p->config = config;
}

static const Property dsp_properties[] = {
	DEFINE_PROP_UINT32("revision", dsp_state_t, revision, 0),
	DEFINE_PROP_UINT32("rom_version", dsp_state_t, rom_version, 0),
	DEFINE_PROP_LINK("bus_ssc", dsp_state_t, ssc_bus, "SSI", SSIBus *),
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

	p->runtime = dsp_runtime_create(config, p->rom_version, rom->program_rom, rom->data_rom,
		p, dsp_worker_notify_activity, dsp_worker_notify_comm, dsp_ssc_transfer);

	p->worker.stop = false;
	p->worker.enabled = false;
	qemu_mutex_init(&p->worker.mutex);
	qemu_cond_init(&p->worker.cond);
	qemu_cond_init(&p->worker.idle_cond);
	qemu_event_init(&p->worker.event, false);
	p->worker.bh = qemu_bh_new(dsp_worker_bh, p);
	qemu_thread_create(&p->worker.thread, "pmb887x-dsp", dsp_worker, p, QEMU_THREAD_JOINABLE);
	p->worker.created = true;

	p->vmstate = qdev_add_vm_change_state_handler(dev, dsp_vm_state_change, NULL, p);
	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	dsp_reset_internal_state(p);
	DPRINTF("core initialized: cpu=%s revision=%02X rom_version=%04X\n", config->name, p->revision, p->rom_version);
}

static void dsp_unrealize(DeviceState *dev) {
	dsp_state_t *p = PMB887X_DSP(dev);

	qemu_del_vm_change_state_handler(p->vmstate);
	p->vmstate = NULL;
	if (p->worker.created) {
		qemu_mutex_lock(&p->worker.mutex);
		p->worker.stop = true;
		qemu_cond_signal(&p->worker.cond);
		qemu_event_set(&p->worker.event);
		qemu_mutex_unlock(&p->worker.mutex);
		qemu_thread_join(&p->worker.thread);
		qemu_bh_delete(p->worker.bh);
		p->worker.bh = NULL;
		p->worker.created = false;
		qemu_cond_destroy(&p->worker.idle_cond);
		qemu_cond_destroy(&p->worker.cond);
		qemu_event_destroy(&p->worker.event);
		qemu_mutex_destroy(&p->worker.mutex);
	}

	dsp_runtime_destroy(p->runtime);
	p->runtime = NULL;
}

static void dsp_class_init(ObjectClass *klass, const void *data) {
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
