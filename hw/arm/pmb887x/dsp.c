/*
 * DSP
 * */
#define PMB887X_TRACE_ID		DSP
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "hw/core/qdev-properties.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/gen/dsp_rom.h"
#include "hw/arm/pmb887x/dsp.h"
#include "hw/arm/pmb887x/dsp_core.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define DSP_RAM_SIZE		(DSP_IO_SIZE - DSP_RAM0)
#define DSP_BACKGROUND_SLICE_CYCLES	256
#define DSP_COMMAND_CHUNK_CYCLES	4096
#define DSP_BACKGROUND_WAIT_MS	1
#define DSP_BOOT_DATA_OFFSET	2
#define DSP_RUNTIME_PIPE_OFFSET	5
#define DSP_RUNTIME_PIPE_STRIDE	0x1C
#define TYPE_PMB887X_DSP	"pmb887x-dsp"
#define PMB887X_DSP(obj)	OBJECT_CHECK(pmb887x_dsp_t, (obj), TYPE_PMB887X_DSP)

enum {
	DSP_BOOT_PLOAD,
	DSP_BOOT_DLOAD,
	DSP_BOOT_BRANCH,
	DSP_BOOT_PREAD,
	DSP_BOOT_DREAD,
};

typedef struct pmb887x_dsp_t pmb887x_dsp_t;

struct pmb887x_dsp_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	MemoryRegion regs;
	MemoryRegion ram;
	uint32_t revision;
	uint32_t rom_version;
	uint16_t dsp_interrupts;
	bool trace_boot_mode;
	pmb887x_clc_reg_t clc;
	pmb887x_dsp_core_t *core;
	QEMUBH *worker_bh;
	QemuThread worker_thread;
	QemuMutex worker_mutex;
	QemuCond worker_cond;
	QemuCond reset_cond;
	uint16_t worker_active_flags;
	uint16_t worker_interrupts;
	uint32_t worker_generation;
	bool worker_enabled;
	bool worker_reset;
	bool worker_starting;
	bool worker_stop;
	bool worker_created;
	qemu_irq mcu_interrupts[PMB887X_DSP_MCU_INT_COUNT];
};

static void dsp_update_interrupts(pmb887x_dsp_t *p, uint16_t interrupts) {
	interrupts &= MAKE_64BIT_MASK(0, PMB887X_DSP_MCU_INT_COUNT);
	if (interrupts == p->dsp_interrupts)
		return;
	for (size_t i = 0; i < ARRAY_SIZE(p->mcu_interrupts); i++)
		qemu_set_irq(p->mcu_interrupts[i], interrupts & BIT(i));
	p->dsp_interrupts = interrupts;
}

static void dsp_worker_bh(void *opaque) {
	pmb887x_dsp_t *p = opaque;
	uint16_t interrupts = qatomic_read(&p->worker_interrupts);

	dsp_update_interrupts(p, interrupts);
}

static bool dsp_worker_publish_result(pmb887x_dsp_t *p, uint32_t generation, uint16_t communication_clear,
	uint16_t interrupts)
{
	if (generation != p->worker_generation)
		return false;

	bool notify_interrupts = interrupts != qatomic_read(&p->worker_interrupts);
	qatomic_and(&p->worker_active_flags, (uint16_t) ~communication_clear);
	qatomic_set(&p->worker_interrupts, interrupts);
	if (notify_interrupts)
		qemu_bh_schedule(p->worker_bh);
	return true;
}

static void *dsp_worker(void *opaque) {
	pmb887x_dsp_t *p = opaque;

	qemu_mutex_lock(&p->worker_mutex);
	while (!p->worker_stop) {
		while (!p->worker_enabled && !p->worker_reset && !p->worker_stop)
			qemu_cond_wait(&p->worker_cond, &p->worker_mutex);
		if (p->worker_stop)
			break;
		if (p->worker_reset) {
			uint32_t generation = p->worker_generation;
			bool run_startup = p->worker_enabled;
			uint16_t communication_clear = 0;
			uint16_t interrupts = 0;

			qemu_mutex_unlock(&p->worker_mutex);
			pmb887x_dsp_core_reset(p->core);
			if (run_startup)
				pmb887x_dsp_core_run(p->core, DSP_COMMAND_CHUNK_CYCLES, &communication_clear, &interrupts);
			qemu_mutex_lock(&p->worker_mutex);
			if (!dsp_worker_publish_result(p, generation, communication_clear, interrupts))
				continue;
			p->worker_reset = false;
			p->worker_starting = !run_startup || communication_clear == 0;
			if (!p->worker_enabled || !p->worker_starting)
				qemu_cond_broadcast(&p->reset_cond);
			continue;
		}

		bool run_active = p->worker_starting || qatomic_read(&p->worker_active_flags) != 0;
		if (!run_active) {
			qemu_cond_timedwait(&p->worker_cond, &p->worker_mutex, DSP_BACKGROUND_WAIT_MS);
			if (p->worker_stop || p->worker_reset || !p->worker_enabled)
				continue;
			run_active = p->worker_starting || qatomic_read(&p->worker_active_flags) != 0;
		}
		uint32_t generation = p->worker_generation;
		bool startup_active = p->worker_starting;
		qemu_mutex_unlock(&p->worker_mutex);

		size_t cycles = run_active ? DSP_COMMAND_CHUNK_CYCLES : DSP_BACKGROUND_SLICE_CYCLES;
		uint16_t communication_clear;
		uint16_t interrupts;
		pmb887x_dsp_core_run(p->core, cycles, &communication_clear, &interrupts);

		qemu_mutex_lock(&p->worker_mutex);
		if (!dsp_worker_publish_result(p, generation, communication_clear, interrupts))
			continue;
		if (startup_active && communication_clear != 0) {
			p->worker_starting = false;
			qemu_cond_broadcast(&p->reset_cond);
		}
	}
	qemu_mutex_unlock(&p->worker_mutex);
	return NULL;
}

static void dsp_worker_set_enabled(pmb887x_dsp_t *p, bool enabled) {
	qemu_mutex_lock(&p->worker_mutex);
	p->worker_enabled = enabled;
	if (enabled)
		qemu_cond_signal(&p->worker_cond);
	qemu_mutex_unlock(&p->worker_mutex);
}

static void dsp_worker_kick(pmb887x_dsp_t *p) {
	qemu_mutex_lock(&p->worker_mutex);
	qemu_cond_signal(&p->worker_cond);
	qemu_mutex_unlock(&p->worker_mutex);
}

static void dsp_wait_for_startup(pmb887x_dsp_t *p) {
	qemu_mutex_lock(&p->worker_mutex);
	while ((p->worker_reset || (p->worker_enabled && p->worker_starting)) && !p->worker_stop)
		qemu_cond_wait(&p->reset_cond, &p->worker_mutex);
	qemu_mutex_unlock(&p->worker_mutex);
}

static void dsp_reset_internal_state(pmb887x_dsp_t *p) {
	pmb887x_dsp_core_request_reset(p->core);
	qemu_bh_cancel(p->worker_bh);

	qemu_mutex_lock(&p->worker_mutex);
	p->worker_generation++;
	p->worker_reset = true;
	p->worker_starting = true;
	p->worker_enabled = pmb887x_clc_is_enabled(&p->clc);
	qatomic_set(&p->worker_active_flags, 0);
	qatomic_set(&p->worker_interrupts, 0);
	p->dsp_interrupts = 0;
	p->trace_boot_mode = true;
	for (size_t i = 0; i < ARRAY_SIZE(p->mcu_interrupts); i++)
		qemu_set_irq(p->mcu_interrupts[i], 0);
	qemu_cond_signal(&p->worker_cond);
	qemu_mutex_unlock(&p->worker_mutex);
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

static void dsp_trace_command(pmb887x_dsp_t *p, size_t pipe) {
	if (p->trace_boot_mode) {
		uint16_t command = pmb887x_dsp_core_shared_read(p->core, DSP_BOOT_DATA_OFFSET);
		uint16_t address = pmb887x_dsp_core_shared_read(p->core, DSP_BOOT_DATA_OFFSET + 1);
		uint16_t words = 0;
		if (command != DSP_BOOT_BRANCH)
			words = pmb887x_dsp_core_shared_read(p->core, DSP_BOOT_DATA_OFFSET + 2);

		DPRINTF("boot command: %s(%u) address=%04X words=%u\n", dsp_boot_command_name(command), command, address, words);
		if (command == DSP_BOOT_BRANCH)
			p->trace_boot_mode = false;
		return;
	}

	uint16_t offset = DSP_RUNTIME_PIPE_OFFSET + pipe * DSP_RUNTIME_PIPE_STRIDE;
	uint16_t command = pmb887x_dsp_core_shared_read(p->core, offset);

	DPRINTF("runtime command: pipe=%zu command=%u (0x%04X)\n", pipe, command, command);
}

static void dsp_interrupt_input(void *opaque, int id, int level) {
	pmb887x_dsp_t *p = opaque;

	dsp_wait_for_startup(p);
	if (level)
		dsp_trace_command(p, id);
	pmb887x_dsp_core_set_request(p->core, id, level);
	if (level)
		qatomic_or(&p->worker_active_flags, (uint16_t) (1U << id));
	dsp_worker_kick(p);
}

static uint64_t dsp_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_dsp_t *p = opaque;
	uint64_t value = 0;

	switch (haddr) {
		case DSP_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case DSP_ID:
			value = 0xF022C000 | p->revision;
			break;

		case DSP_COM_STATUS:
			dsp_wait_for_startup(p);
			value = pmb887x_dsp_core_get_communication_flags(p->core);
			if (value) {
				g_thread_yield();
				value = pmb887x_dsp_core_get_communication_flags(p->core);
			}
			break;

		case DSP_COM_SET:
		case DSP_COM_CLEAR:
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
	pmb887x_dsp_t *p = opaque;

	IO_DUMP_WRITE(haddr + p->mmio.addr, size, value);
	switch (haddr) {
		case DSP_CLC:
			pmb887x_clc_set(&p->clc, value);
			dsp_worker_set_enabled(p, pmb887x_clc_is_enabled(&p->clc));
			break;

		case DSP_COM_SET:
			dsp_wait_for_startup(p);
			pmb887x_dsp_core_set_communication_flags(p->core, value & DSP_COM_SET_FLAGS);
			break;

		case DSP_COM_CLEAR:
			dsp_wait_for_startup(p);
			pmb887x_dsp_core_clear_communication_flags(p->core, value & DSP_COM_CLEAR_FLAGS);
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
	pmb887x_dsp_t *p = opaque;
	uint64_t value;

	dsp_wait_for_startup(p);
	value = pmb887x_dsp_core_shared_read_bytes(p->core, haddr, size);
	IO_DUMP_READ(haddr + p->mmio.addr + DSP_RAM0, size, value);
	return value;
}

static void dsp_ram_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_dsp_t *p = opaque;

	dsp_wait_for_startup(p);
	IO_DUMP_WRITE(haddr + p->mmio.addr + DSP_RAM0, size, value);
	pmb887x_dsp_core_shared_write_bytes(p->core, haddr, value, size);
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
	pmb887x_dsp_t *p = PMB887X_DSP(obj);
	memory_region_init(&p->mmio, obj, "pmb887x-dsp", DSP_IO_SIZE);
	memory_region_init_io(&p->regs, obj, &io_ops, p, "pmb887x-dsp-regs", DSP_RAM0);
	memory_region_init_io(&p->ram, obj, &ram_io_ops, p, "pmb887x-dsp-ram", DSP_RAM_SIZE);
	memory_region_add_subregion(&p->mmio, 0, &p->regs);
	memory_region_add_subregion(&p->mmio, DSP_RAM0, &p->ram);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_reset_input, "RESET_IN", 1);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_interrupt_input, "INT_IN", PMB887X_DSP_INT_COUNT);
	qdev_init_gpio_out_named(DEVICE(obj), p->mcu_interrupts, "INT_OUT", ARRAY_SIZE(p->mcu_interrupts));
}

static void dsp_reset(DeviceState *dev) {
	pmb887x_dsp_t *p = PMB887X_DSP(dev);

	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	dsp_reset_internal_state(p);
}

static const Property dsp_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_dsp_t, revision, 0),
	DEFINE_PROP_UINT32("rom_version", pmb887x_dsp_t, rom_version, 0),
};

static void dsp_realize(DeviceState *dev, Error **errp) {
	pmb887x_dsp_t *p = PMB887X_DSP(dev);
	uint32_t revision_family = pmb887x_dsp_core_revision_family(p->revision);
	if (revision_family == 0) {
		error_setg(errp, "DSP revision %02X is not supported", p->revision);
		return;
	}

	if (p->rom_version == 0)
		p->rom_version = pmb887x_dsp_core_default_rom_version(p->revision);
	const pmb887x_dsp_rom_t *rom = pmb887x_dsp_rom_find(p->rom_version);
	if (rom == NULL) {
		error_setg(errp, "DSP MASK ROM version %04X is not embedded", p->rom_version);
		return;
	}
	p->core = pmb887x_dsp_core_create(p->revision, p->rom_version, rom->program_rom, rom->data_rom);
	if (p->core == NULL) {
		error_setg(errp, "DSP MASK ROM version %04X is incompatible with DSP revision %02X", p->rom_version,
			p->revision);
		return;
	}

	p->worker_stop = false;
	p->worker_enabled = false;
	qemu_mutex_init(&p->worker_mutex);
	qemu_cond_init(&p->worker_cond);
	qemu_cond_init(&p->reset_cond);
	p->worker_bh = qemu_bh_new(dsp_worker_bh, p);
	qemu_thread_create(&p->worker_thread, "pmb887x-dsp", dsp_worker, p, QEMU_THREAD_JOINABLE);
	p->worker_created = true;
	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	dsp_reset_internal_state(p);
	DPRINTF("core initialized: revision=%02X family=r%02X rom_version=%04X\n",
		p->revision, revision_family, p->rom_version);
}

static void dsp_unrealize(DeviceState *dev) {
	pmb887x_dsp_t *p = PMB887X_DSP(dev);

	if (p->worker_created) {
		qemu_mutex_lock(&p->worker_mutex);
		p->worker_stop = true;
		qemu_cond_signal(&p->worker_cond);
		qemu_cond_broadcast(&p->reset_cond);
		qemu_mutex_unlock(&p->worker_mutex);
		qemu_thread_join(&p->worker_thread);
		qemu_bh_delete(p->worker_bh);
		p->worker_bh = NULL;
		p->worker_created = false;
		qemu_cond_destroy(&p->reset_cond);
		qemu_cond_destroy(&p->worker_cond);
		qemu_mutex_destroy(&p->worker_mutex);
	}

	if (p->core) {
		pmb887x_dsp_core_destroy(p->core);
		p->core = NULL;
	}
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
	.instance_size	= sizeof(struct pmb887x_dsp_t),
	.instance_init	= dsp_init,
	.class_init		= dsp_class_init,
};

static void dsp_register_types(void) {
	type_register_static(&dsp_info);
}
type_init(dsp_register_types)
