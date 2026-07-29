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
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/gen/dsp_rom.h"
#include "hw/arm/pmb887x/dsp.h"
#include "hw/arm/pmb887x/dsp_core.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define DSP_RAM_SIZE		(DSP_IO_SIZE - DSP_RAM0)
#define DSP_SLICE_CYCLES	256
#define DSP_SLICE_NS		10000
#define DSP_BOOT_DATA_OFFSET	2
#define DSP_RUNTIME_PIPE_OFFSET	5
#define DSP_RUNTIME_PIPE_STRIDE	0x1C
#define TYPE_PMB887X_DSP	"pmb887x-dsp"
#define PMB887X_DSP(obj)	OBJECT_CHECK(pmb887x_dsp_t, (obj), TYPE_PMB887X_DSP)

typedef struct pmb887x_dsp_t pmb887x_dsp_t;

struct pmb887x_dsp_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	MemoryRegion regs;
	MemoryRegion ram;
	uint32_t revision;
	uint32_t rom_version;
	uint32_t com_status;
	uint16_t dsp_interrupts;
	bool boot_mode;
	pmb887x_clc_reg_t clc;
	pmb887x_dsp_core_t *core;
	QEMUTimer *timer;
	qemu_irq mcu_interrupts[PMB887X_DSP_MCU_INT_COUNT];
};

static void dsp_schedule(pmb887x_dsp_t *p) {
	timer_del(p->timer);
	if (pmb887x_clc_is_enabled(&p->clc))
		timer_mod_ns(p->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DSP_SLICE_NS);
}

static void dsp_update_interrupts(pmb887x_dsp_t *p) {
	uint16_t interrupts = pmb887x_dsp_core_get_mcu_interrupts(p->core) & MAKE_64BIT_MASK(0, PMB887X_DSP_MCU_INT_COUNT);
	if (interrupts == p->dsp_interrupts)
		return;
	for (size_t i = 0; i < ARRAY_SIZE(p->mcu_interrupts); i++)
		qemu_set_irq(p->mcu_interrupts[i], interrupts & BIT(i));
	p->dsp_interrupts = interrupts;
}

static void dsp_timer_callback(void *opaque) {
	pmb887x_dsp_t *p = opaque;

	pmb887x_dsp_core_run(p->core, DSP_SLICE_CYCLES);
	uint16_t clear = pmb887x_dsp_core_take_com_clear(p->core) & p->com_status;
	if (clear) {
		p->com_status &= ~clear;
		DPRINTF("command acknowledged: mask=%04X status=%04X pc=%05X\n", clear, p->com_status,
			pmb887x_dsp_core_get_pc(p->core));
	}
	dsp_update_interrupts(p);
	dsp_schedule(p);
}

static void dsp_reset_internal_state(pmb887x_dsp_t *p) {
	p->com_status = 0;
	p->dsp_interrupts = 0;
	p->boot_mode = true;
	for (size_t i = 0; i < ARRAY_SIZE(p->mcu_interrupts); i++)
		qemu_set_irq(p->mcu_interrupts[i], 0);
	pmb887x_dsp_core_reset(p->core);
	dsp_schedule(p);
}

static void dsp_reset_input(void *opaque, int id, int level) {
	if (level)
		dsp_reset_internal_state(opaque);
}

static const char *dsp_boot_command_name(uint16_t command) {
	switch (command) {
		case 0:
			return "PLOAD";
		case 1:
			return "DLOAD";
		case 2:
			return "BRANCH";
		case 3:
			return "PREAD";
		case 4:
			return "DREAD";
		default:
			return "UNKNOWN";
	}
}

static void dsp_trace_command(pmb887x_dsp_t *p, size_t pipe) {
	if (p->boot_mode) {
		uint16_t command = pmb887x_dsp_core_shared_read(p->core, DSP_BOOT_DATA_OFFSET);
		uint16_t address = pmb887x_dsp_core_shared_read(p->core, DSP_BOOT_DATA_OFFSET + 1);
		uint16_t words = command == 2 ? 0 : pmb887x_dsp_core_shared_read(p->core, DSP_BOOT_DATA_OFFSET + 2);
		DPRINTF("boot command: %s(%u) address=%04X words=%u\n", dsp_boot_command_name(command), command, address, words);
		return;
	}

	uint16_t offset = DSP_RUNTIME_PIPE_OFFSET + pipe * DSP_RUNTIME_PIPE_STRIDE;
	uint16_t command = pmb887x_dsp_core_shared_read(p->core, offset);
	DPRINTF("runtime command: pipe=%zu command=%u (0x%04X)\n", pipe, command, command);
}

static void dsp_interrupt_input(void *opaque, int id, int level) {
	pmb887x_dsp_t *p = opaque;

	if (level)
		dsp_trace_command(p, id);
	pmb887x_dsp_core_set_request(p->core, id, level);
	if (level && p->boot_mode && pmb887x_dsp_core_shared_read(p->core, DSP_BOOT_DATA_OFFSET) == 2) {
		p->boot_mode = false;
		pmb887x_dsp_core_set_boot_mode(p->core, false);
	}
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
			value = p->com_status;
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
			dsp_schedule(p);
			break;

		case DSP_COM_SET:
			p->com_status |= value & DSP_COM_SET_FLAGS;
			break;

		case DSP_COM_CLEAR:
			p->com_status &= ~(value & DSP_COM_CLEAR_FLAGS);
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
	uint64_t value = 0;

	for (size_t i = 0; i < size; i++) {
		uint16_t word = pmb887x_dsp_core_shared_read(p->core, (haddr + i) / 2);
		value |= (uint64_t) ((word >> (((haddr + i) & 1) * 8)) & 0xFF) << (i * 8);
	}
	IO_DUMP_READ(haddr + p->mmio.addr + DSP_RAM0, size, value);
	return value;
}

static void dsp_ram_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_dsp_t *p = opaque;

	IO_DUMP_WRITE(haddr + p->mmio.addr + DSP_RAM0, size, value);
	for (size_t i = 0; i < size; i++) {
		uint16_t offset = (haddr + i) / 2;
		uint16_t shift = ((haddr + i) & 1) * 8;
		uint16_t word = pmb887x_dsp_core_shared_read(p->core, offset);
		word = (word & ~(0xFF << shift)) | ((value >> (i * 8) & 0xFF) << shift);
		pmb887x_dsp_core_shared_write(p->core, offset, word);
	}
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
	p->core = pmb887x_dsp_core_create(p->revision, p->rom_version, rom->data, rom->size);
	if (p->core == NULL) {
		error_setg(errp, "DSP MASK ROM version %04X has an invalid DSP1 image", p->rom_version);
		return;
	}
	p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dsp_timer_callback, p);
	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	dsp_reset_internal_state(p);
	DPRINTF("core initialized: revision=%02X family=r%02X rom_version=%04X\n",
		p->revision, revision_family, p->rom_version);
}

static void dsp_unrealize(DeviceState *dev) {
	pmb887x_dsp_t *p = PMB887X_DSP(dev);

	if (p->timer)
		timer_free(p->timer);
	if (p->core)
		pmb887x_dsp_core_destroy(p->core);
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
