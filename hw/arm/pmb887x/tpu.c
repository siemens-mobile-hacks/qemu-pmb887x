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
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

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
	uint32_t overflow;
	uint32_t offset;
	uint32_t param;
	uint32_t skip;
	uint32_t intr[2];
	
	pmb887x_src_reg_t src[2];
	pmb887x_src_reg_t unk_src[6];
	
	qemu_irq irq[2];
	qemu_irq unk_irq[6];
	
	uint32_t pllcon0;
	uint32_t pllcon1;
	uint32_t pllcon2;
	
	uint32_t unk[8];
	
	uint32_t irq_fired;
	QEMUTimer *timer;
	
	bool enabled;
	uint32_t freq;
	uint32_t counter;
	uint64_t start;
	uint64_t next;
	
	uint32_t L;
	uint32_t K;
	
	uint32_t last_fsys;
	
	struct pmb887x_pll_t *pll;
};

static uint64_t tpu_get_time(struct pmb887x_tpu_t *p, bool real) {
	uint64_t next = p->counter;
	uint64_t overflow = p->overflow + 1;
	
	if (p->enabled) {
		uint64_t delta_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - p->start;
		next += muldiv64(delta_ns, p->freq, NANOSECONDS_PER_SECOND);
	}
	
	return real ? next : (next % overflow);
}

static uint64_t tpu_ticks_to_ns(struct pmb887x_tpu_t *p, uint64_t ticks) {
    return muldiv64(ticks, NANOSECONDS_PER_SECOND, p->freq);
}

static uint64_t tpu_run_irq(struct pmb887x_tpu_t *p, uint64_t counter, uint64_t now, uint64_t next) {
	for (int i = 0; i < 2; i++) {
		if (!(p->irq_fired & (1 << i))) {
			if (counter >= p->intr[i]) {
				pmb887x_src_update(&p->src[i], 0, MOD_SRC_SETR);
				p->irq_fired |= (1 << i);
			} else {
				next = MIN(next, now + tpu_ticks_to_ns(p, p->intr[i] - counter));
			}
		}
	}
	return next;
}

static void tpu_ptimer_reset(void *opaque) {
	struct pmb887x_tpu_t *p = (struct pmb887x_tpu_t *) opaque;
	
	if (!p->enabled)
		return;
	
	uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	uint64_t overflow = p->overflow + 1;
	
	if (!p->start) {
		p->start = now;
		p->next = 0;
	}
	
	uint64_t counter = tpu_get_time(p, true);
	if (counter >= overflow) {
		p->start = now;
		p->counter = p->counter % overflow;
		p->irq_fired = 0;
		
		counter = p->counter;
	}
	
	p->next = now + tpu_ticks_to_ns(p, overflow - counter);
	p->next = tpu_run_irq(p, counter, now, p->next);
	
	timer_mod(p->timer, p->next);
}

static void tpu_ptimer_reset2(void *opaque) {
	struct pmb887x_tpu_t *p = (struct pmb887x_tpu_t *) opaque;
	
	uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	if (p->next && (now - p->next) / 1000000) {
		EPRINTF("delta=%"PRId64" ms / %"PRId64" us\n", (now - p->next) / 1000000, (now - p->next) / 1000);
		// abort();
	}
	
	tpu_ptimer_reset(p);
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
	uint32_t new_freq;
	if (p->L == 0 && p->K == 0) {
		new_freq = ftpu > 0 ? ftpu / 6 : 0;
	} else {
		new_freq = ftpu > 0 ? (ftpu / p->L * p->K) / 6 : 0;
	}
	
	// new_freq = new_freq / 6;
	
	// Reset counter when TPU_PARAM_TINI=0
	if (!(p->param & TPU_PARAM_TINI)) {
		p->counter = 0;
		p->irq_fired = 0;
		p->next = 0;
	}
	
	bool enabled = pmb887x_clc_is_enabled(&p->clc) && new_freq > 0 && (p->param & TPU_PARAM_TINI) != 0 && p->overflow >= 2;
	if (p->freq != new_freq || p->enabled != enabled) {
		p->freq = new_freq;
		p->enabled = enabled;
		DPRINTF("fsys=%d, ftpu=%d, fcounter=%d [%s]\n", pmb887x_pll_get_fsys(p->pll), ftpu, p->freq, p->enabled ? "ON" : "OFF");
	}
	
	tpu_ptimer_reset(p);
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
			value = p->overflow;
		break;
		
		case TPU_INT0:
			value = p->intr[0];
		break;
		
		case TPU_INT1:
			value = p->intr[1];
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
			value = tpu_get_time(p, false);
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
		break;
		
		case TPU_CORRECTION:
			p->correction = value;
		break;
		
		case TPU_OVERFLOW:
			p->overflow = value;
		break;
		
		case TPU_INT0:
			p->intr[0] = value;
		break;
		
		case TPU_INT1:
			p->intr[1] = value;
		break;
		
		case TPU_SRC0:
			pmb887x_src_set(&p->src[0], value);
		break;
		
		case TPU_SRC1:
			pmb887x_src_set(&p->src[1], value);
		break;
		
		case TPU_OFFSET:
			p->offset = value;
		break;
		
		case TPU_SKIP:
			p->skip = value;
		break;
		
		case TPU_PARAM:
			p->param = value;
		break;
		
		case TPU_PLLCON0:
			p->pllcon0 = value;
		break;
		
		case TPU_PLLCON1:
			p->pllcon1 = value;
		break;
		
		case TPU_PLLCON2:
			p->pllcon2 = value;
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
	
	tpu_update_state(p);
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
	}
	
	for (int i = 0; i < ARRAY_SIZE(p->unk_src); i++) {
		if (!p->unk_irq[i])
			hw_error("pmb887x-tpu: irq %d (TPU_UNK%d) not set", index++, i);
		
		pmb887x_src_init(&p->unk_src[i], p->unk_irq[i]);
	}
	
    p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tpu_ptimer_reset2, p);
	p->enabled = false;
	
	tpu_update_state(p);
	pmb887x_pll_add_freq_update_callback(p->pll, tpu_update_state_callback, p);
}

static Property tpu_properties[] = {
	DEFINE_PROP_LINK("pll", struct pmb887x_tpu_t, pll, "pmb887x-pll", struct pmb887x_pll_t *),
    DEFINE_PROP_END_OF_LIST(),
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
