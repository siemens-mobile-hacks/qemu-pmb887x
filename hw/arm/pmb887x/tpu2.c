/*
 * Time Processing Unit
 * */
#define PMB887X_TRACE_ID		TPU
#define PMB887X_TRACE_PREFIX	"pmb887x-tpu"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/ptimer.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"

#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/vic.h"
#include "hw/arm/pmb887x/dyn_timer.h"

#define TYPE_PMB887X_TPU	"pmb887x-tpu"
#define PMB887X_TPU(obj)	OBJECT_CHECK(struct pmb887x_tpu_t, (obj), TYPE_PMB887X_TPU)
#define	TPU_RAM_SIZE		0x2000

struct pmb887x_tpu_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;

	// regs
	pmb887x_clc_reg_t clc;
	uint8_t ram[TPU_RAM_SIZE];
	uint32_t correction;
	uint32_t offset;
	uint32_t param;
	uint32_t skip;
	bool enabled;

	pmb887x_src_reg_t src[2];
	pmb887x_src_reg_t unk_src[6];

	qemu_irq irq[2];
	qemu_irq unk_irq[6];

	uint32_t pllcon0;
	uint32_t pllcon1;
	uint32_t pllcon2;

	uint32_t unk[8];

	pmb887x_dyn_timer_t *timer;

	uint32_t L;
	uint32_t K;

	uint32_t last_fsys;

	struct pmb887x_pll_t *pll;
	pmb887x_vic_t *vic;
};

/*
 * static void tpu_ptimer_reset2(void *opaque) {
 *	struct pmb887x_tpu_t *p = (struct pmb887x_tpu_t *) opaque;
 *
 *	uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
 *	if (p->next && (now - p->next) / 1000000) {
 *		EPRINTF("delta=%"PRId64" ms / %"PRId64" us\n", (now - p->next) / 1000000, (now - p->next) / 1000);
 *		// abort();
 *	}
 *
 *	tpu_ptimer_reset(p);
 * }
 */

static void tpu_timer_control(struct pmb887x_tpu_t *p) {
	bool has_irq = (
		(pmb887x_src_get(&p->src[0]) & MOD_SRC_SRR) != 0 ||
		(pmb887x_src_get(&p->src[1]) & MOD_SRC_SRR) != 0
	);
	DPRINTF("has_irq=%d\n", has_irq);
	if (!has_irq && p->enabled) {
		pmb887x_dyn_timer_start(p->timer);
	} else {
		pmb887x_dyn_timer_stop(p->timer);
	}
}

static void pmb887x_tpu_timer_callback(void *opaque, int irq_id) {
	struct pmb887x_tpu_t *p = (struct pmb887x_tpu_t *) opaque;
	if (irq_id != -1) {
		pmb887x_src_update(&p->src[irq_id], 0, MOD_SRC_SETR);
		tpu_timer_control(p);
	}
}

static void tpu_update_state(struct pmb887x_tpu_t *p) {
	uint32_t div = pmb887x_clc_get_rmc(&p->clc);

	// Input freq for module
	uint32_t ftpu = div > 0 ? pmb887x_pll_get_fsys(p->pll) / div : 0;

	// Update clock
	if ((p->pllcon2 & TPU_PLLCON2_INIT) || (p->pllcon2 & TPU_PLLCON2_LOAD)) {
		p->K = (p->pllcon0 & TPU_PLLCON0_K_DIV) >> TPU_PLLCON0_K_DIV_SHIFT;
		p->L = (p->pllcon1 & TPU_PLLCON1_L_DIV) >> TPU_PLLCON1_L_DIV_SHIFT;
		p->pllcon2 &= ~(TPU_PLLCON2_INIT | TPU_PLLCON2_LOAD);
	}

	// Input freq for TPU counter
	uint32_t freq;
	if (p->L == 0 && p->K == 0) {
		freq = ftpu > 0 ? ftpu / 6 : 0;
	} else {
		freq = ftpu > 0 ? (ftpu / p->L * p->K) / 6 : 0;
	}

	// Check if timer is enabled
	p->enabled = pmb887x_clc_is_enabled(&p->clc) && freq > 0 && (p->param & TPU_PARAM_TINI) != 0 && pmb887x_dyn_timer_get_overflow(p->timer) >= 2;

	pmb887x_dyn_timer_set_freq(p->timer, freq);
	tpu_timer_control(p);

	// Reset counter when TPU_PARAM_TINI=0
	if (!(p->param & TPU_PARAM_TINI))
		pmb887x_dyn_timer_reset(p->timer);

	DPRINTF("fsys=%d, ftpu=%d, fcounter=%d [%s]\n", pmb887x_pll_get_fsys(p->pll), ftpu, freq, p->enabled ? "ON" : "OFF");
	//	pmb887x_dyn_timer_run(p->timer);
}

static void tpu_vic_callback(void *opaque, int action, int irq) {
	// struct pmb887x_tpu_t *p = (struct pmb887x_tpu_t *) opaque;

	DPRINTF("{{{{ VIC }}}} - %d %d\n", action, irq);
}

static void tpu_update_state_callback(void *opaque) {
	struct pmb887x_tpu_t *p = (struct pmb887x_tpu_t *) opaque;
	uint32_t fsys = pmb887x_pll_get_fsys(p->pll);
	if (p->last_fsys != fsys) {
		tpu_update_state(p);
		p->last_fsys = fsys;
	}
}

static uint32_t tpu_ram_read(struct pmb887x_tpu_t *p, uint32_t offset, unsigned size) {
	uint8_t *data = p->ram;
	switch (size) {
		case 1:		return data[offset];
		case 2:		return data[offset] | (data[offset + 1] << 8);
		case 4:		return data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) | (data[offset + 3] << 24);
	}
	return 0;
}

static void tpu_ram_write(struct pmb887x_tpu_t *p, uint32_t offset, uint32_t value, unsigned size) {
	uint8_t *data = p->ram;
	switch (size) {
		case 1:
			data[offset] = value & 0xFF;
			break;

		case 2:
			data[offset] = value & 0xFF;
			data[offset + 1] = (value >> 8) & 0xFF;
			break;

		case 4:
			data[offset] = value & 0xFF;
			data[offset + 1] = (value >> 8) & 0xFF;
			data[offset + 2] = (value >> 16) & 0xFF;
			data[offset + 3] = (value >> 24) & 0xFF;
			break;
	}
}

static int tpu_unk_by_reg(hwaddr haddr) {
	switch (haddr) {
		case TPU_UNK0:		return 0;
		case TPU_UNK1:		return 1;
		case TPU_UNK2:		return 2;
		case TPU_UNK3:		return 3;
		case TPU_UNK4:		return 4;
		case TPU_UNK5:		return 5;
		case TPU_UNK6:		return 6;
		case TPU_UNK7:		return 7;

		case TPU_UNK_SRC0:	return 0;
		case TPU_UNK_SRC1:	return 1;
		case TPU_UNK_SRC2:	return 2;
		case TPU_UNK_SRC3:	return 3;
		case TPU_UNK_SRC4:	return 4;
		case TPU_UNK_SRC5:	return 5;
	}
	return -1;
}

static uint64_t tpu_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct pmb887x_tpu_t *p = (struct pmb887x_tpu_t *) opaque;

	uint64_t value = 0;

	switch (haddr) {
		case TPU_CLC:
			value = pmb887x_clc_get(&p->clc);
		break;

		case TPU_ID:
			value = 0xF021C012;
		break;

		case TPU_CORRECTION:
			value = p->correction;
		break;

		case TPU_OVERFLOW:
			value = pmb887x_dyn_timer_get_overflow(p->timer);
		break;

		case TPU_INT0:
			value = pmb887x_dyn_timer_irq_get_threshold(p->timer, 0);
		break;

		case TPU_INT1:
			value = pmb887x_dyn_timer_irq_get_threshold(p->timer, 1);
		break;

		case TPU_SRC0:
			value = pmb887x_src_get(&p->src[0]);
		break;

		case TPU_SRC1:
			value = pmb887x_src_get(&p->src[1]);
		break;

		case TPU_OFFSET:
			value = p->offset;
		break;

		case TPU_SKIP:
			value = p->skip;
		break;

		case TPU_PARAM:
			value = p->param;
		break;

		case TPU_PLLCON0:
			value = p->pllcon0;
		break;

		case TPU_PLLCON1:
			value = p->pllcon1;
		break;

		case TPU_PLLCON2:
			value = p->pllcon2;
		break;

		case TPU_COUNTER:
			value = pmb887x_dyn_timer_get_counter(p->timer);
			DPRINTF("TPU_COUNTER=%ld\n", value);
		break;

		case TPU_RAM0 ... (TPU_RAM0 + TPU_RAM_SIZE):
			value = tpu_ram_read(p, haddr, size);
		break;

		case TPU_UNK_SRC0:
		case TPU_UNK_SRC1:
		case TPU_UNK_SRC2:
		case TPU_UNK_SRC3:
		case TPU_UNK_SRC4:
		case TPU_UNK_SRC5:
			value = pmb887x_src_get(&p->unk_src[tpu_unk_by_reg(haddr)]);
		break;

		case TPU_UNK0:
		case TPU_UNK1:
		case TPU_UNK2:
		case TPU_UNK3:
		case TPU_UNK4:
		case TPU_UNK5:
		case TPU_UNK6:
		case TPU_UNK7:
			value = p->unk[tpu_unk_by_reg(haddr)];
		break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
		break;
	}

	IO_DUMP(haddr + p->mmio.addr, size, value, false);

	return value;
}

static void tpu_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct pmb887x_tpu_t *p = (struct pmb887x_tpu_t *) opaque;

	IO_DUMP(haddr + p->mmio.addr, size, value, true);

	switch (haddr) {
		case TPU_CLC:
			pmb887x_clc_set(&p->clc, value);
			tpu_update_state(p);
		break;

		case TPU_CORRECTION:
			p->correction = value;
		break;

		case TPU_OVERFLOW:
			pmb887x_dyn_timer_set_overflow(p->timer, value);
			tpu_update_state(p);
		break;

		case TPU_INT0:
			pmb887x_dyn_timer_irq_set_threshold(p->timer, 0, value);
		break;

		case TPU_INT1:
			pmb887x_dyn_timer_irq_set_threshold(p->timer, 1, value);
		break;

		case TPU_SRC0:
			pmb887x_src_set(&p->src[0], value);
			tpu_timer_control(p);
		break;

		case TPU_SRC1:
			pmb887x_src_set(&p->src[1], value);
			tpu_timer_control(p);
		break;

		case TPU_OFFSET:
			p->offset = value;
		break;

		case TPU_SKIP:
			p->skip = value;
		break;

		case TPU_PARAM:
			p->param = value;
			tpu_update_state(p);
		break;

		case TPU_PLLCON0:
			p->pllcon0 = value;
			tpu_update_state(p);
		break;

		case TPU_PLLCON1:
			p->pllcon1 = value;
			tpu_update_state(p);
		break;

		case TPU_PLLCON2:
			p->pllcon2 = value;
			tpu_update_state(p);
		break;

		case TPU_RAM0 ... (TPU_RAM0 + TPU_RAM_SIZE):
			tpu_ram_write(p, haddr, value, size);
		break;

		case TPU_UNK_SRC0:
		case TPU_UNK_SRC1:
		case TPU_UNK_SRC2:
		case TPU_UNK_SRC3:
		case TPU_UNK_SRC4:
		case TPU_UNK_SRC5:
			pmb887x_src_set(&p->unk_src[tpu_unk_by_reg(haddr)], value);
		break;

		case TPU_UNK0:
		case TPU_UNK1:
		case TPU_UNK2:
		case TPU_UNK3:
		case TPU_UNK4:
		case TPU_UNK5:
		case TPU_UNK6:
		case TPU_UNK7:
			p->unk[tpu_unk_by_reg(haddr)] = value;
		break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
		break;
	}
}

static const MemoryRegionOps io_ops = {
	.read			= tpu_io_read,
	.write			= tpu_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void tpu_init(Object *obj) {
	struct pmb887x_tpu_t *p = PMB887X_TPU(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-tpu", TPU_RAM0 + TPU_RAM_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	for (int i = 0; i < ARRAY_SIZE(p->src); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
	for (int i = 0; i < ARRAY_SIZE(p->unk_src); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->unk_irq[i]);
}

static void tpu_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_tpu_t *p = PMB887X_TPU(dev);

	pmb887x_clc_init(&p->clc);

	int index = 0;

	for (int i = 0; i < ARRAY_SIZE(p->src); i++) {
		if (!p->irq[i])
			hw_error("pmb887x-tpu: irq %d (TPU_INT%d) not set", index++, i);
		pmb887x_src_init(&p->src[i], p->irq[i]);
		DPRINTF("pmb887x_vic_get_irq_id(p->vic, p->irq[i])=%d\n", pmb887x_vic_get_irq_id(p->vic, p->irq[i]));
		pmb887x_vic_set_callback(p->vic, pmb887x_vic_get_irq_id(p->vic, p->irq[i]), tpu_vic_callback, p);
	}

	for (int i = 0; i < ARRAY_SIZE(p->unk_src); i++) {
		if (!p->unk_irq[i])
			hw_error("pmb887x-tpu: irq %d (TPU_UNK%d) not set", index++, i);
		pmb887x_src_init(&p->unk_src[i], p->unk_irq[i]);
	}

	p->timer = pmb887x_dyn_timer_new(2, pmb887x_tpu_timer_callback, p);
	tpu_update_state(p);

	pmb887x_pll_add_freq_update_callback(p->pll, tpu_update_state_callback, p);
}

static const Property tpu_properties[] = {
	DEFINE_PROP_LINK("pll", struct pmb887x_tpu_t, pll, "pmb887x-pll", struct pmb887x_pll_t *),
	DEFINE_PROP_LINK("vic", struct pmb887x_tpu_t, vic, "pmb887x-vic", struct pmb887x_vic_t *),
};

static void tpu_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, tpu_properties);
	dc->realize = tpu_realize;
}

static const TypeInfo tpu_info = {
	.name          	= TYPE_PMB887X_TPU,
	.parent        	= TYPE_SYS_BUS_DEVICE,
	.instance_size 	= sizeof(struct pmb887x_tpu_t),
	.instance_init 	= tpu_init,
	.class_init    	= tpu_class_init,
};

static void tpu_register_types(void) {
	type_register_static(&tpu_info);
}
type_init(tpu_register_types)
