/*
 * Capture/Compare
 * */
#define PMB887X_TRACE_ID		CAPCOM
#define PMB887X_TRACE_PREFIX	"pmb887x-capcom"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_CAPCOM	"pmb887x-capcom"
#define PMB887X_CAPCOM(obj)	OBJECT_CHECK(pmb887x_capcom_t, (obj), TYPE_PMB887X_CAPCOM)

typedef struct pmb887x_capcom_t pmb887x_capcom_t;
typedef struct pmb887x_capcom_cc_t pmb887x_capcom_cc_t;
typedef enum pmb887x_capcom_cc_mode_t pmb887x_capcom_cc_mode_t;

struct pmb887x_capcom_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	qemu_irq t_irq[2];
	qemu_irq cc_irq[8];
	
	pmb887x_src_reg_t t_src[2];
	pmb887x_src_reg_t cc_src[8];
	
	pmb887x_clc_reg_t clc;
	
	uint32_t pisel;
	uint32_t t01con;
	uint32_t ccm[2];
	uint32_t out;
	uint32_t ioc;
	uint32_t sem;
	uint32_t see;
	uint32_t drm;
	uint32_t whbssee;
	uint32_t whbcsee;
	uint32_t t0;
	uint32_t t0rel;
	uint32_t t1;
	uint32_t t1rel;
	uint32_t t01ocr;
	uint32_t whbsout;
	uint32_t whbcout;
};

struct pmb887x_capcom_cc_t {
	uint32_t ccm_index;
	uint32_t acc_mask;
	uint32_t acc_shift;
	uint32_t mod_mask;
	uint32_t mod_shift;
};

enum pmb887x_capcom_cc_mode_t {
	CAPCOM_CC_MODE_DISABLED = 0,
	CAPCOM_CC_MODE_RISING_EDGE,
	CAPCOM_CC_MODE_FALLING_EDGE,
	CAPCOM_CC_MODE_BOTH_EDGES,
	CAPCOM_CC_MODE_0,
	CAPCOM_CC_MODE_1,
	CAPCOM_CC_MODE_2,
	CAPCOM_CC_MODE_3,
};

const pmb887x_capcom_cc_t capcom_cc_list[] = {
	{ 0, CAPCOM_CCM0_ACC0, CAPCOM_CCM0_ACC0_SHIFT, CAPCOM_CCM0_MOD0, CAPCOM_CCM0_MOD0_SHIFT },
	{ 0, CAPCOM_CCM0_ACC1, CAPCOM_CCM0_ACC1_SHIFT, CAPCOM_CCM0_MOD1, CAPCOM_CCM0_MOD1_SHIFT },
	{ 0, CAPCOM_CCM0_ACC2, CAPCOM_CCM0_ACC2_SHIFT, CAPCOM_CCM0_MOD2, CAPCOM_CCM0_MOD2_SHIFT },
	{ 0, CAPCOM_CCM0_ACC3, CAPCOM_CCM0_ACC3_SHIFT, CAPCOM_CCM0_MOD3, CAPCOM_CCM0_MOD3_SHIFT },
	{ 1, CAPCOM_CCM1_ACC4, CAPCOM_CCM1_ACC4_SHIFT, CAPCOM_CCM1_MOD4, CAPCOM_CCM1_MOD4_SHIFT },
	{ 1, CAPCOM_CCM1_ACC5, CAPCOM_CCM1_ACC5_SHIFT, CAPCOM_CCM1_MOD5, CAPCOM_CCM1_MOD5_SHIFT },
	{ 1, CAPCOM_CCM1_ACC6, CAPCOM_CCM1_ACC6_SHIFT, CAPCOM_CCM1_MOD6, CAPCOM_CCM1_MOD6_SHIFT },
	{ 1, CAPCOM_CCM1_ACC7, CAPCOM_CCM1_ACC7_SHIFT, CAPCOM_CCM1_MOD7, CAPCOM_CCM1_MOD7_SHIFT },
};

static enum pmb887x_capcom_cc_mode_t capcom_get_mode(pmb887x_capcom_t *p, int id) {
	const pmb887x_capcom_cc_t *cc = &capcom_cc_list[id];
	uint32_t ccm = p->ccm[cc->ccm_index];
	return (ccm & cc->mod_mask) >> cc->mod_shift;
}

static void capcom_update_state(pmb887x_capcom_t *p) {
	// TODO
}

static int capcom_get_index_from_reg(uint32_t reg) {
	switch (reg) {
		case CAPCOM_CC7_SRC:	return 7;
		case CAPCOM_CC6_SRC:	return 6;
		case CAPCOM_CC5_SRC:	return 5;
		case CAPCOM_CC4_SRC:	return 4;
		case CAPCOM_CC3_SRC:	return 3;
		case CAPCOM_CC2_SRC:	return 2;
		case CAPCOM_CC1_SRC:	return 1;
		case CAPCOM_CC0_SRC:	return 0;
		case CAPCOM_T1_SRC:		return 1;
		case CAPCOM_T0_SRC:		return 0;
		default:				abort();
	};
}

static uint64_t capcom_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_capcom_t *p = opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case CAPCOM_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;
		
		case CAPCOM_ID:
			value = 0x00005011;
			break;
		
		case CAPCOM_PISEL:
			value = p->pisel;
			break;
		
		case CAPCOM_T01CON:
			value = p->t01con;
			break;
		
		case CAPCOM_CCM0:
			value = p->ccm[0];
			break;
		
		case CAPCOM_CCM1:
			value = p->ccm[1];
			break;
		
		case CAPCOM_OUT:
			value = p->out;
			break;
		
		case CAPCOM_IOC:
			value = p->ioc;
			break;
		
		case CAPCOM_SEM:
			value = p->sem;
			break;
		
		case CAPCOM_SEE:
			value = p->see;
			break;
		
		case CAPCOM_DRM:
			value = p->drm;
			break;
		
		case CAPCOM_WHBSSEE:
			value = p->whbssee;
			break;
		
		case CAPCOM_WHBCSEE:
			value = p->whbcsee;
			break;
		
		case CAPCOM_T0:
			value = p->t0;
			break;
		
		case CAPCOM_T0REL:
			value = p->t0rel;
			break;
		
		case CAPCOM_T1:
			value = p->t1;
			break;
		
		case CAPCOM_T1REL:
			value = p->t1rel;
			break;
		
		case CAPCOM_T01OCR:
			value = p->t01ocr;
			break;
		
		case CAPCOM_WHBSOUT:
			value = p->whbsout;
			break;
		
		case CAPCOM_WHBCOUT:
			value = p->whbcout;
			break;
		
		case CAPCOM_CC7_SRC:
		case CAPCOM_CC6_SRC:
		case CAPCOM_CC5_SRC:
		case CAPCOM_CC4_SRC:
		case CAPCOM_CC3_SRC:
		case CAPCOM_CC2_SRC:
		case CAPCOM_CC1_SRC:
		case CAPCOM_CC0_SRC:
			value = pmb887x_src_get(&p->cc_src[capcom_get_index_from_reg(haddr)]);
			break;
		
		case CAPCOM_T1_SRC:
		case CAPCOM_T0_SRC:
			value = pmb887x_src_get(&p->t_src[capcom_get_index_from_reg(haddr)]);
			break;
		
		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void capcom_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_capcom_t *p = opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case CAPCOM_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;
		
		case CAPCOM_PISEL:
			p->pisel = value;
			break;
		
		case CAPCOM_T01CON:
			p->t01con = value;
			break;
		
		case CAPCOM_CCM0:
			p->ccm[0] = value;
			break;
		
		case CAPCOM_CCM1:
			p->ccm[1] = value;
			break;
		
		case CAPCOM_OUT:
			p->out = value;
			break;
		
		case CAPCOM_IOC:
			p->ioc = value;
			break;
		
		case CAPCOM_SEM:
			p->sem = value;
			break;
		
		case CAPCOM_SEE:
			p->see = value;
			break;
		
		case CAPCOM_DRM:
			p->drm = value;
			break;
		
		case CAPCOM_WHBSSEE:
			p->whbssee = value;
			break;
		
		case CAPCOM_WHBCSEE:
			p->whbcsee = value;
			break;
		
		case CAPCOM_T0:
			p->t0 = value;
			break;
		
		case CAPCOM_T0REL:
			p->t0rel = value;
			break;
		
		case CAPCOM_T1:
			p->t1 = value;
			break;
		
		case CAPCOM_T1REL:
			p->t1rel = value;
			break;
		
		case CAPCOM_T01OCR:
			p->t01ocr = value;
			break;
		
		case CAPCOM_WHBSOUT:
			p->whbsout = value;
			break;
		
		case CAPCOM_WHBCOUT:
			p->whbcout = value;
			break;
		
		case CAPCOM_CC7_SRC:
		case CAPCOM_CC6_SRC:
		case CAPCOM_CC5_SRC:
		case CAPCOM_CC4_SRC:
		case CAPCOM_CC3_SRC:
		case CAPCOM_CC2_SRC:
		case CAPCOM_CC1_SRC:
		case CAPCOM_CC0_SRC:
			pmb887x_src_set(&p->cc_src[capcom_get_index_from_reg(haddr)], value);
			break;
		
		case CAPCOM_T1_SRC:
		case CAPCOM_T0_SRC:
			pmb887x_src_set(&p->t_src[capcom_get_index_from_reg(haddr)], value);
			break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}
	
	capcom_update_state(p);
}

static void capcom_handle_input_change(pmb887x_capcom_t *p, int id, int level) {
	DPRINTF("CC%d MODE=%d\n", id, capcom_get_mode(p, id));

	if (capcom_get_mode(p, id) == CAPCOM_CC_MODE_RISING_EDGE && level == 1)
		pmb887x_src_update(&p->cc_src[id], 0, MOD_SRC_SETR);

	if (capcom_get_mode(p, id) == CAPCOM_CC_MODE_FALLING_EDGE && level == 0)
		pmb887x_src_update(&p->cc_src[id], 0, MOD_SRC_SETR);

	if (capcom_get_mode(p, id) == CAPCOM_CC_MODE_BOTH_EDGES)
		pmb887x_src_update(&p->cc_src[id], 0, MOD_SRC_SETR);
}

static void capcom_input_cc0_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 0, level);
}

static void capcom_input_cc1_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 1, level);
}

static void capcom_input_cc2_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 2, level);
}

static void capcom_input_cc3_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 3, level);
}

static void capcom_input_cc4_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 4, level);
}

static void capcom_input_cc5_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 5, level);
}

static void capcom_input_cc6_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 6, level);
}

static void capcom_input_cc7_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 7, level);
}

static const MemoryRegionOps io_ops = {
	.read			= capcom_io_read,
	.write			= capcom_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void capcom_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_capcom_t *p = PMB887X_CAPCOM(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-capcom", CAPCOM_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (int i = 0; i < ARRAY_SIZE(p->t_src); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->t_irq[i]);
	
	for (int i = 0; i < ARRAY_SIZE(p->cc_src); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->cc_irq[i]);

	qdev_init_gpio_in_named(dev, capcom_input_cc0_handler, "CC0_IN", 1);
	qdev_init_gpio_in_named(dev, capcom_input_cc1_handler, "CC1_IN", 1);
	qdev_init_gpio_in_named(dev, capcom_input_cc2_handler, "CC2_IN", 1);
	qdev_init_gpio_in_named(dev, capcom_input_cc3_handler, "CC3_IN", 1);
	qdev_init_gpio_in_named(dev, capcom_input_cc4_handler, "CC4_IN", 1);
	qdev_init_gpio_in_named(dev, capcom_input_cc5_handler, "CC5_IN", 1);
	qdev_init_gpio_in_named(dev, capcom_input_cc6_handler, "CC6_IN", 1);
	qdev_init_gpio_in_named(dev, capcom_input_cc7_handler, "CC7_IN", 1);
}

static void capcom_realize(DeviceState *dev, Error **errp) {
	pmb887x_capcom_t *p = PMB887X_CAPCOM(dev);
	
	pmb887x_clc_init(&p->clc);
	
	int irqn = 0;
	
	for (int i = 0; i < ARRAY_SIZE(p->t_src); i++) {
		if (!p->t_irq[i])
			hw_error("pmb887x-scu: irq %d (T%d) not set", irqn, i);
		pmb887x_src_init(&p->t_src[i], p->t_irq[i]);
		irqn++;
	}
	
	for (int i = 0; i < ARRAY_SIZE(p->cc_src); i++) {
		if (!p->cc_irq[i])
			hw_error("pmb887x-scu: irq %d (CC%d) not set", irqn, i);
		pmb887x_src_init(&p->cc_src[i], p->cc_irq[i]);
		irqn++;
	}
	
	capcom_update_state(p);
}

static void capcom_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	dc->realize = capcom_realize;
}

static const TypeInfo capcom_info = {
    .name          	= TYPE_PMB887X_CAPCOM,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_capcom_t),
    .instance_init 	= capcom_init,
    .class_init    	= capcom_class_init,
};

static void capcom_register_types(void) {
	type_register_static(&capcom_info);
}
type_init(capcom_register_types)
