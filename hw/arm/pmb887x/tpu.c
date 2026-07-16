/*
 * Time Processing Unit
 * */
#define PMB887X_TRACE_ID		TPU
#define PMB887X_TRACE_PREFIX	"pmb887x-tpu"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/hw-error.h"
#include "system/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"

#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_TPU	"pmb887x-tpu"
#define PMB887X_TPU(obj)	OBJECT_CHECK(pmb887x_tpu_t, (obj), TYPE_PMB887X_TPU)
#define	TPU_RAM_SIZE		0x2000

#define TPU_OVERFLOW_RESET 0x270F
#define TPU_FADE_RESET 0x0700
#define TPU_GSMCLK1_RESET (1U << TPU_GSMCLK1_K_SHIFT)
#define TPU_GSMCLK2_RESET (2U << TPU_GSMCLK2_L_SHIFT)

typedef struct pmb887x_tpu_t pmb887x_tpu_t;

struct pmb887x_tpu_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	uint32_t revision;
	
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
	pmb887x_src_reg_t gp_src[5];
	pmb887x_src_reg_t rfssc_src;
	
	qemu_irq irq[2];
	qemu_irq gp_irq[6];
	qemu_irq rfssc_irq;
	
	uint32_t gsmclk1;
	uint32_t gsmclk2;
	uint32_t gsmclk3;
	
	uint32_t ceap;
	uint32_t eapt;
	uint32_t eapb;
	uint32_t tger;
	uint32_t rfcon1;
	uint32_t rfcon2;
	uint32_t fade;

	uint32_t irq_fired;
	QEMUTimer *timer;
	
	bool enabled;
	uint32_t freq;
	uint32_t counter;
	int64_t start;
	int64_t next;
	uint32_t frame_ticks;
	uint32_t next_frame_ticks;
	bool skip_extended;
	bool offset_pending;
	
	uint32_t L;
	uint32_t K;
	
	uint32_t last_fsys;
	uint32_t unk;
	
	pmb887x_pll_t *pll;
};

static uint64_t tpu_get_counter(pmb887x_tpu_t *p) {
	uint64_t counter = p->counter;

	if (p->enabled) {
		int64_t delta_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - p->start;
		counter += muldiv64(delta_ns, p->freq, NANOSECONDS_PER_SECOND);
	}

	return counter;
}

static int64_t tpu_ticks_to_ns(pmb887x_tpu_t *p, uint64_t ticks) {
	return (int64_t) muldiv64_round_up(ticks, NANOSECONDS_PER_SECOND, p->freq);
}

static int64_t tpu_run_irq(pmb887x_tpu_t *p, int64_t counter, uint64_t now, int64_t next) {
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

static uint32_t tpu_regular_frame_ticks(pmb887x_tpu_t *p) {
	return p->overflow + 1;
}

static void tpu_finish_frame(pmb887x_tpu_t *p) {
	uint32_t regular_frame_ticks = tpu_regular_frame_ticks(p);

	if ((p->skip & TPU_SKIP_SKIPC) && !p->skip_extended) {
		p->frame_ticks += regular_frame_ticks;
		p->skip_extended = true;
		return;
	}

	p->counter -= p->frame_ticks;
	p->irq_fired = 0;
	p->frame_ticks = p->next_frame_ticks ? p->next_frame_ticks : regular_frame_ticks;
	p->next_frame_ticks = 0;

	if (p->skip_extended) {
		p->skip &= ~TPU_SKIP_SKIPC;
		p->skip_extended = false;
	}
	if (p->skip & TPU_SKIP_SKIPN) {
		p->skip &= ~TPU_SKIP_SKIPN;
		p->skip |= TPU_SKIP_SKIPC;
	}
}

static void tpu_update_timer(pmb887x_tpu_t *p) {
	if (!p->enabled) {
		timer_del(p->timer);
		return;
	}

	uint64_t counter = tpu_get_counter(p);
	uint64_t elapsed_ticks = counter - p->counter;
	p->counter = (uint32_t) counter;
	p->start += tpu_ticks_to_ns(p, elapsed_ticks);

	while (p->counter >= p->frame_ticks)
		tpu_finish_frame(p);

	p->next = p->start + tpu_ticks_to_ns(p, p->frame_ticks - p->counter);
	p->next = tpu_run_irq(p, p->counter, p->start, p->next);
	timer_mod(p->timer, p->next);
}

static void tpu_timer_callback(void *opaque) {
	tpu_update_timer(opaque);
}

static void tpu_apply_offset(pmb887x_tpu_t *p) {
	uint32_t offset = p->offset & TPU_OFFSET_VALUE;

	if (offset == 0)
		return;
	if (p->offset & TPU_OFFSET_CTRL) {
		p->frame_ticks += offset + 1;
	} else {
		p->frame_ticks = offset + 1;
	}
}

static void tpu_update_state(pmb887x_tpu_t *p) {
	bool was_enabled = p->enabled;
	if (was_enabled)
		tpu_update_timer(p);

	uint32_t div = pmb887x_clc_get_rmc(&p->clc);
	
	// Input freq for module
	uint32_t ftpu = div > 0 ? pmb887x_pll_get_fsys(p->pll) / div : 0;
	
	// Update clock
	if ((p->gsmclk3 & TPU_GSMCLK3_INIT) || (p->gsmclk3 & TPU_GSMCLK3_LOAD)) {
		p->K = (p->gsmclk1 & TPU_GSMCLK1_K) >> TPU_GSMCLK1_K_SHIFT;
		p->L = (p->gsmclk2 & TPU_GSMCLK2_L) >> TPU_GSMCLK2_L_SHIFT;
		
		p->gsmclk3 &= ~(TPU_GSMCLK3_INIT | TPU_GSMCLK3_LOAD);
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
		p->frame_ticks = tpu_regular_frame_ticks(p);
		p->next_frame_ticks = 0;
		p->skip_extended = false;
	}
	
	bool enabled = pmb887x_clc_is_enabled(&p->clc) && new_freq > 0 && (p->param & TPU_PARAM_TINI) != 0 && p->overflow >= 2;
	if (p->freq != new_freq || p->enabled != enabled) {
		p->freq = new_freq;
		p->enabled = enabled;
		DPRINTF("fsys=%d, ftpu=%d, fcounter=%d [%s]\n", pmb887x_pll_get_fsys(p->pll), ftpu, p->freq, p->enabled ? "ON" : "OFF");
	}

	if (p->enabled && !was_enabled) {
		p->counter = 0;
		p->irq_fired = 0;
		p->frame_ticks = tpu_regular_frame_ticks(p);
		p->next_frame_ticks = 0;
		p->skip_extended = false;
		p->start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
		if (p->offset_pending)
			tpu_apply_offset(p);
		p->offset_pending = false;
	}

	tpu_update_timer(p);
}

static void tpu_update_state_callback(void *opaque) {
	pmb887x_tpu_t *p = opaque;
	uint32_t fsys = pmb887x_pll_get_fsys(p->pll);
	if (p->last_fsys != fsys) {
		tpu_update_state(p);
		p->last_fsys = fsys;
	}
}

static uint32_t tpu_ram_read(pmb887x_tpu_t *p, uint32_t offset, unsigned size) {
	uint8_t *data = p->ram;
	switch (size) {
		case 1:		return data[offset];
		case 2:		return data[offset] | (data[offset + 1] << 8);
		case 4:		return data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) | (data[offset + 3] << 24);
		default:	abort();
	}
}

static void tpu_ram_write(pmb887x_tpu_t *p, uint32_t offset, uint32_t value, unsigned size) {
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

		default:
			abort();
	}
}

static uint64_t tpu_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_tpu_t *p = opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case TPU_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;
		
		case TPU_ID:
			value = 0xF021C000 | p->revision;
			break;

		case TPU_RFCON1:
			value = p->rfcon1;
			break;

		case TPU_RFCON2:
			value = p->rfcon2;
			break;

		case TPU_RFSSCTB:
			value = 0;
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

		case TPU_FADE:
			value = p->fade;
			break;

		case TPU_GSMCLK1:
			value = p->gsmclk1;
			break;
		
		case TPU_GSMCLK2:
			value = p->gsmclk2;
			break;
		
		case TPU_GSMCLK3:
			value = p->gsmclk3;
			break;
		
		case TPU_COUNTER:
			value = tpu_get_counter(p);
			break;

		case TPU_CEAP:
			value = p->ceap;
			break;

		case TPU_EAPT:
			value = p->eapt;
			break;

		case TPU_EAPB:
			value = p->eapb;
			break;

		case TPU_TGER:
			value = p->tger;
			break;

		case TPU_UNK:
			value = p->unk;
			break;

		case TPU_RAM0 ... (TPU_RAM0 + TPU_RAM_SIZE):
			value = tpu_ram_read(p, haddr, size);
			break;

		case TPU_RFSSC_SRC:
			value = pmb887x_src_get(&p->rfssc_src);
			break;

		case TPU_GP_SRC0:
		case TPU_GP_SRC1:
		case TPU_GP_SRC2:
		case TPU_GP_SRC3:
		case TPU_GP_SRC4:
			value = pmb887x_src_get(&p->gp_src[(haddr - TPU_GP_SRC0) / 4]);
			break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void tpu_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_tpu_t *p = (struct pmb887x_tpu_t *) opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case TPU_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case TPU_RFCON1:
			p->rfcon1 = value;
			break;

		case TPU_RFCON2:
			p->rfcon2 = value;
			break;

		case TPU_RFSSCTB:
			DPRINTF("RF control: %04X\n", (uint16_t) value);
			pmb887x_src_set(&p->rfssc_src, MOD_SRC_SETR);
			p->rfcon2 &= ~TPU_RFCON2_SSCEN;
			break;

		case TPU_CORRECTION:
			tpu_update_timer(p);
			p->correction = value;
			if (p->enabled) {
				uint32_t correction_ticks = (p->correction & TPU_CORRECTION_VALUE) + 1;
				if (p->correction & TPU_CORRECTION_CTRL) {
					p->next_frame_ticks = correction_ticks;
				} else {
					p->frame_ticks = correction_ticks;
				}
			}
			break;
		
		case TPU_OVERFLOW:
			tpu_update_timer(p);
			p->overflow = value & TPU_OVERFLOW_VALUE;
			p->frame_ticks = tpu_regular_frame_ticks(p);
			p->next_frame_ticks = 0;
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
			tpu_update_timer(p);
			p->offset = value;
			if (p->enabled) {
				tpu_apply_offset(p);
			} else {
				p->offset_pending = true;
			}
			break;
		
		case TPU_SKIP:
			tpu_update_timer(p);
			p->skip = value & (TPU_SKIP_SKIPN | TPU_SKIP_SKIPC);
			p->skip_extended = false;
			break;
		
		case TPU_PARAM:
			p->param = value;
			break;

		case TPU_FADE:
			p->fade = value;
			break;

		case TPU_GSMCLK1:
			p->gsmclk1 = value;
			break;
		
		case TPU_GSMCLK2:
			p->gsmclk2 = value;
			break;
		
		case TPU_GSMCLK3:
			p->gsmclk3 = value;
			break;

		case TPU_CEAP:
			p->ceap = value;
			break;

		case TPU_EAPT:
			p->eapt = value;
			break;

		case TPU_EAPB:
			p->eapb = value;
			break;

		case TPU_TGER:
			p->tger = value;
			break;

		case TPU_UNK:
			p->unk = value;
			break;

		case TPU_RAM0 ... (TPU_RAM0 + TPU_RAM_SIZE):
			tpu_ram_write(p, haddr, value, size);
			break;

		case TPU_RFSSC_SRC:
			pmb887x_src_set(&p->rfssc_src, value);
			break;

		case TPU_GP_SRC0:
		case TPU_GP_SRC1:
		case TPU_GP_SRC2:
		case TPU_GP_SRC3:
		case TPU_GP_SRC4:
			pmb887x_src_set(&p->gp_src[(haddr - TPU_GP_SRC0) / 4], value);
			break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
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
	pmb887x_tpu_t *p = PMB887X_TPU(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-tpu", TPU_RAM0 + TPU_RAM_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->rfssc_irq);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->gp_irq[0]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->gp_irq[1]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->gp_irq[2]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->gp_irq[3]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->gp_irq[4]);

	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[0]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[1]);
}

static void tpu_realize(DeviceState *dev, Error **errp) {
	pmb887x_tpu_t *p = PMB887X_TPU(dev);
	
	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	pmb887x_src_init(&p->rfssc_src, p->rfssc_irq);

	for (int i = 0; i < ARRAY_SIZE(p->src); i++) {
		if (!p->irq[i])
			hw_error("pmb887x-tpu: irq %d (INT%d) not set", i, i);
		pmb887x_src_init(&p->src[i], p->irq[i]);
	}
	
	for (int i = 0; i < ARRAY_SIZE(p->gp_src); i++) {
		if (!p->gp_irq[i])
			hw_error("pmb887x-tpu: irq %d (INT_GP%d) not set", i, i);
		pmb887x_src_init(&p->gp_src[i], p->gp_irq[i]);
	}
	
	p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tpu_timer_callback, p);
	p->enabled = false;
	
	tpu_update_state(p);
	pmb887x_pll_add_freq_update_callback(p->pll, tpu_update_state_callback, p);
}

static void tpu_reset(DeviceState *dev) {
	pmb887x_tpu_t *p = PMB887X_TPU(dev);

	timer_del(p->timer);

	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);

	pmb887x_src_reset(&p->rfssc_src);
	for (size_t i = 0; i < ARRAY_SIZE(p->src); i++)
		pmb887x_src_reset(&p->src[i]);
	for (size_t i = 0; i < ARRAY_SIZE(p->gp_src); i++)
		pmb887x_src_reset(&p->gp_src[i]);

	memset(p->ram, 0, sizeof(p->ram));
	p->correction = 0;
	p->overflow = TPU_OVERFLOW_RESET;
	p->offset = 0;
	p->param = 0;
	p->skip = 0;
	for (size_t i = 0; i < ARRAY_SIZE(p->intr); i++)
		p->intr[i] = TPU_INT_VALUE;

	p->gsmclk1 = TPU_GSMCLK1_RESET;
	p->gsmclk2 = TPU_GSMCLK2_RESET;
	p->gsmclk3 = 0;
	p->ceap = 0;
	p->eapt = 0;
	p->eapb = 0;
	p->tger = 0;
	p->rfcon1 = 0;
	p->rfcon2 = 0;
	p->fade = TPU_FADE_RESET;
	p->irq_fired = 0;

	p->enabled = false;
	p->freq = 0;
	p->counter = 0;
	p->start = 0;
	p->next = 0;
	p->frame_ticks = tpu_regular_frame_ticks(p);
	p->next_frame_ticks = 0;
	p->skip_extended = false;
	p->offset_pending = false;
	p->L = 2;
	p->K = 1;
	p->last_fsys = 0;
	p->unk = 0;

	tpu_update_state(p);
}

static const Property tpu_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_tpu_t, revision, 0),
	DEFINE_PROP_LINK("pll", struct pmb887x_tpu_t, pll, "pmb887x-pll", struct pmb887x_pll_t *),
};

static void tpu_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, tpu_properties);
	device_class_set_legacy_reset(dc, tpu_reset);
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
