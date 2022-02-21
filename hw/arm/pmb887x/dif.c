/*
 * Display Interface
 * */
#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/qdev-properties.h"

#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"

#define DIF_DEBUG

#ifdef DIF_DEBUG
#define DPRINTF(fmt, ...) do { fprintf(stderr, "[pmb887x-dif]: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#define TYPE_PMB887X_DIF	"pmb887x-dif"
#define PMB887X_DIF(obj)	OBJECT_CHECK(struct pmb887x_dif_t, (obj), TYPE_PMB887X_DIF)

struct pmb887x_dif_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
    QemuConsole *con;
	
	pmb887x_clc_reg_t clc;
};

static void dif_update_state(struct pmb887x_dif_t *p) {
	// TODO
}

static void dif_update_display(void *opaque) {
	struct pmb887x_dif_t *p = (struct pmb887x_dif_t *) opaque;
	// TODO
}

static void dif_invalidate_display(void * opaque) {
	struct pmb887x_dif_t *p = (struct pmb887x_dif_t *) opaque;
	// TODO
}

static uint64_t dif_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb887x_dif_t *p = (struct pmb887x_dif_t *) opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case DIF_CLC:
			value = pmb887x_clc_get(&p->clc);
		break;
		
		case DIF_ID:
			value = 0xF043C012;
		break;
		
		default:
			pmb887x_dump_io(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	pmb887x_dump_io(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void dif_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct pmb887x_dif_t *p = (struct pmb887x_dif_t *) opaque;
	
	pmb887x_dump_io(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case DIF_CLC:
			pmb887x_clc_set(&p->clc, value);
		break;
		
		default:
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	dif_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= dif_io_read,
	.write			= dif_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static const GraphicHwOps dif_gfx_ops = {
	.invalidate = dif_invalidate_display,
	.gfx_update = dif_update_display,
};

static void dif_init(Object *obj) {
	struct pmb887x_dif_t *p = PMB887X_DIF(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-dif", DIF_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
}

static void dif_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_dif_t *p = PMB887X_DIF(dev);
	
	pmb887x_clc_init(&p->clc);
	p->con = graphic_console_init(dev, 0, &dif_gfx_ops, p);
	qemu_console_resize(p->con, 240, 320);
	
	dif_update_state(p);
}

static Property dif_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void dif_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, dif_properties);
	dc->realize = dif_realize;
}

static const TypeInfo dif_info = {
    .name          	= TYPE_PMB887X_DIF,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_dif_t),
    .instance_init 	= dif_init,
    .class_init    	= dif_class_init,
};

static void dif_register_types(void) {
	type_register_static(&dif_info);
}
type_init(dif_register_types)
