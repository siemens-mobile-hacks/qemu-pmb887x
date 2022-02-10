/*
 * Time Processing Unit
 * */
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

#define TPU_DEBUG

#ifdef TPU_DEBUG
#define DPRINTF(fmt, ...) do { fprintf(stderr, "[pmb887x-tpu]: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#define TYPE_PMB887X_TPU	"pmb887x-tpu"
#define PMB887X_TPU(obj)	OBJECT_CHECK(struct tpu_t, (obj), TYPE_PMB887X_TPU)
#define	TPU_RAM_SIZE		512

struct tpu_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	// regs
	struct pmb887x_clc_reg_t clc;
	uint32_t ram[TPU_RAM_SIZE];
	uint32_t correction;
	uint32_t overflow;
	uint32_t offset;
	uint32_t param;
	uint32_t skip;
	uint32_t intr[2];
	
	struct pmb887x_src_reg_t src[2];
	
	uint32_t pllcon0;
	uint32_t pllcon1;
	uint32_t pllcon2;
	
	uint32_t unk[8];
	
	qemu_irq irq[2];
	QEMUTimer *timer;
	
	bool enabled;
	uint32_t freq;
	uint32_t counter;
	uint64_t last_time;
	
	uint32_t L;
	uint32_t K;
	
	struct pmb887x_pll_t *pll;
};

static uint32_t tpu_get_count(struct tpu_t *p) {
	if (p->last_time) {
		uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
		uint64_t counter = p->counter + muldiv64(now - p->last_time, p->freq, NANOSECONDS_PER_SECOND);
		uint32_t overflow = p->overflow + 1;
		
		if (overflow < 2)
			overflow = 2;
		
		while (counter > overflow)
			counter -= overflow;
		return (uint32_t) counter;
	}
	return p->counter;
}

static void tpu_ptimer_reset(void *opaque) {
	struct tpu_t *p = (struct tpu_t *) opaque;
	
	uint32_t div = pmb887x_clc_get_rmc(&p->clc);
	bool is_enabled = pmb887x_clc_is_enabled(&p->clc);
	
	// On real hardware: if TPU_PARAM_TINI = 0, then reset timer counter and stop it
	if (!(p->param & TPU_PARAM_TINI)) {
		p->last_time = 0;
		p->counter = 0;
		return;
	}
	
	// On real hardware: if TPU_CLC_RMC = 0 or TPU_CLC_DISR = 1, then only stop timer
	if (!div || !is_enabled || !p->freq) {
		p->last_time = 0;
		p->counter = tpu_get_count(p);
		return;
	}
	
	uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	uint32_t overflow = p->overflow + 1;
	
	if (overflow < 2)
		overflow = 2;
	
	// Init counter, if first reset
	if (!p->last_time)
		p->last_time = now;
	
	// Get real counter value
	uint64_t counter = p->counter + muldiv64(now - p->last_time, p->freq, NANOSECONDS_PER_SECOND);
	
	// Process interrupts
	for (int i = 0; i < 2; ++i) {
		uint32_t int_overflow = overflow > p->intr[i] ? p->intr[i] : overflow;
		if (counter >= int_overflow) {
			DPRINTF("counter=%ld, overflow=%ld, int_overflow=%ld, elapsed=%ld\n", counter, overflow, int_overflow, (now - p->last_time));
			
			pmb887x_src_update(&p->src[i], 0, MOD_SRC_SETR);
		}
	}
	
	// Reload counter if overflow
	if (counter > overflow) {
		while (counter > overflow)
			counter -= overflow;
		
		p->counter = (uint32_t) counter;
		p->last_time = now;
	}
	
	// Schedule timer for next INTx or overflow
	timer_mod(p->timer, now);
}

static void tpu_update_state(struct tpu_t *p) {
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
	
	if (p->freq != new_freq) {
		p->freq = new_freq;
		DPRINTF("fsys=%d, ftpu=%d, fcounter=%d\n", pmb887x_pll_get_fsys(p->pll), ftpu, p->freq);
	}
	
	tpu_ptimer_reset(p);
}

static void tpu_update_state_callback(void *opaque) {
	tpu_update_state((struct tpu_t *) opaque);
}

static int tpu_unk_by_reg(hwaddr haddr) {
	switch (haddr) {
		case TPU_UNK0:
			return 0;
		case TPU_UNK1:
			return 1;
		case TPU_UNK2:
			return 2;
		case TPU_UNK3:
			return 3;
		case TPU_UNK4:
			return 4;
		case TPU_UNK5:
			return 5;
		case TPU_UNK6:
			return 6;
		case TPU_UNK7:
			return 7;
	}
	return -1;
}

static uint64_t tpu_io_read(void *opaque, hwaddr haddr, unsigned size) {
	struct tpu_t *p = (struct tpu_t *) opaque;
	
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
		
		case TPU_RAM0 ... TPU_RAM0 + (TPU_RAM_SIZE * 4):
			value = p->ram[(haddr - TPU_RAM0) / 4];
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
			pmb887x_dump_io(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			DPRINTF("unknown reg access: %02lX\n", haddr);
			exit(1);
		break;
	}
	
	pmb887x_dump_io(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void tpu_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	struct tpu_t *p = (struct tpu_t *) opaque;
	
	pmb887x_dump_io(haddr + p->mmio.addr, size, value, true);
	
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
		
		case TPU_RAM0 ... TPU_RAM0 + (TPU_RAM_SIZE * 4):
			p->ram[(haddr - TPU_RAM0) / 4] = value;
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
			DPRINTF("unknown reg access: %02lX\n", haddr);
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
		.min_access_size	= 4,
		.max_access_size	= 4
	}
};

static void tpu_init(Object *obj) {
	struct tpu_t *p = PMB887X_TPU(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-tpu", TPU_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[0]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[1]);
}

static void tpu_realize(DeviceState *dev, Error **errp) {
	struct tpu_t *p = PMB887X_TPU(dev);
	
	pmb887x_clc_init(&p->clc);
	
	for (int i = 0; i < ARRAY_SIZE(p->src); i++) {
		if (!p->irq[i]) {
			error_report("pmb887x-tpu: irq %d (TPU_INT%d) not set", i, i);
			abort();
		}
		
		pmb887x_src_init(&p->src[i], p->irq[i]);
	}
	
    p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tpu_ptimer_reset, p);
	p->enabled = false;
	
	tpu_update_state(p);
	pmb887x_pll_add_freq_update_callback(p->pll, tpu_update_state_callback, p);
}

static Property tpu_properties[] = {
	DEFINE_PROP_LINK("pll", struct tpu_t, pll, "pmb887x-pll", struct pmb887x_pll_t *),
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
    .instance_size 	= sizeof(struct tpu_t),
    .instance_init 	= tpu_init,
    .class_init    	= tpu_class_init,
};

static void tpu_register_types(void) {
	type_register_static(&tpu_info);
}
type_init(tpu_register_types)
