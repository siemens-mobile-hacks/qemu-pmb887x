/*
 * MMC interface wrapper
 */
#include "qemu/osdep.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"

#define TYPE_PMB887X_MMCI "pmb887x-mmci"
#define PMB887X_MMCI(obj) OBJECT_CHECK(pmb887x_mmci_t, (obj), TYPE_PMB887X_MMCI)

typedef struct pmb887x_mmci_t pmb887x_mmci_t;

struct pmb887x_mmci_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	DebugChannel dbg;
	uint32_t revision;
	pmb887x_clc_reg_t clc;
	Clock *mclk;
	qemu_irq gpio_dat0;
	qemu_irq gpio_dat1;
	qemu_irq gpio_dat2;
	qemu_irq gpio_dat3;
	qemu_irq gpio_cmd;
	qemu_irq gpio_clk;
};

static uint64_t mmci_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_mmci_t *p = opaque;
	uint64_t value = 0;

	switch (haddr) {
		case MMCI_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;
		case MMCI_ID:
			value = 0xF041C000 | p->revision;
			break;
	}

	return value;
}

static void mmci_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_mmci_t *p = opaque;

	switch (haddr) {
		case MMCI_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;
	}
}

static const MemoryRegionOps mmci_io_ops = {
	.read = mmci_io_read,
	.write = mmci_io_write,
	.endianness = DEVICE_NATIVE_ENDIAN,
	.valid = {
		.min_access_size = 4,
		.max_access_size = 4,
	},
};

static void mmci_gpio_input(void *_opaque, int _id, int _level) {
}

/*
 * Measured on the phone with the OSC, CLK32K and PHASE4 sources: CLC.RMC 4 and 5 divide the module clock
 * of the PL180 by 4 and 5, every other value from 1 to 12 divides it by 8.
 */
static uint32_t mmci_get_mclk_divider(uint32_t rmc) {
	return rmc == 4 || rmc == 5 ? rmc : 8;
}

static void mmci_clock_changed(void *opaque) {
	pmb887x_mmci_t *p = opaque;
	uint32_t rmc = pmb887x_clc_get_rmc(&p->clc);
	uint32_t mclk_hz = 0;
	if (pmb887x_clc_is_enabled(&p->clc) && rmc != 0)
		mclk_hz = pmb887x_clc_get_input_hz(&p->clc) / mmci_get_mclk_divider(rmc);
	clock_update_hz(p->mclk, mclk_hz);
}

static void mmci_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_mmci_t *p = PMB887X_MMCI(obj);

	pmb887x_clc_init(&p->clc, dev);
	p->mclk = qdev_init_clock_out(dev, "MCLK");
	memory_region_init_io(&p->mmio, obj, &mmci_io_ops, p, "pmb887x-mmci", MMCI_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	qdev_init_gpio_in_named(dev, mmci_gpio_input, "DAT0", 1);
	qdev_init_gpio_in_named(dev, mmci_gpio_input, "DAT1", 1);
	qdev_init_gpio_in_named(dev, mmci_gpio_input, "DAT2", 1);
	qdev_init_gpio_in_named(dev, mmci_gpio_input, "DAT3", 1);
	qdev_init_gpio_in_named(dev, mmci_gpio_input, "CMD", 1);
	qdev_init_gpio_in_named(dev, mmci_gpio_input, "CLK", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_dat0, "DAT0", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_dat1, "DAT1", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_dat2, "DAT2", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_dat3, "DAT3", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_cmd, "CMD", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_clk, "CLK", 1);
}

static void mmci_reset(DeviceState *dev) {
	pmb887x_mmci_t *p = PMB887X_MMCI(dev);

	pmb887x_clc_set(&p->clc, 1U << MOD_CLC_RMC_SHIFT);
	mmci_clock_changed(p);
}

static void mmci_realize(DeviceState *dev, Error **_errp) {
	pmb887x_mmci_t *p = PMB887X_MMCI(dev);
	pmb887x_mod_debug_init(&p->dbg, dev, &p->mmio, &pmb887x_mmci_regs);

	pmb887x_clc_set(&p->clc, 1U << MOD_CLC_RMC_SHIFT);
	pmb887x_clc_set_callback(&p->clc, mmci_clock_changed, p);
}

static const Property mmci_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_mmci_t, revision, 0),
};

static void mmci_class_init(ObjectClass *klass, const void *_data) {
	DeviceClass *dc = DEVICE_CLASS(klass);

	device_class_set_props(dc, mmci_properties);
	device_class_set_legacy_reset(dc, mmci_reset);
	dc->realize = mmci_realize;
}

static const TypeInfo mmci_info = {
	.name = TYPE_PMB887X_MMCI,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(pmb887x_mmci_t),
	.instance_init = mmci_init,
	.class_init = mmci_class_init,
};

static void mmci_register_types(void) {
	type_register_static(&mmci_info);
}

type_init(mmci_register_types)
