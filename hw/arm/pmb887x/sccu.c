/*
 * SCCU
 * */
#define PMB887X_TRACE_ID		SCCU
#define PMB887X_TRACE_PREFIX	"pmb887x-sccu"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "system/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"

#include "hw/arm/pmb887x/sccu.h"
#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_RTC	"pmb887x-sccu"
#define PMB887X_RTC(obj)	OBJECT_CHECK(pmb887x_sccu_t, (obj), TYPE_PMB887X_RTC)

enum {
	SCCU_IRQ_UNK = 0,
	SCCU_IRQ_WAKE
};

typedef struct pmb887x_sccu_t pmb887x_sccu_t;

struct pmb887x_sccu_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;

	qemu_irq irq[2];

	pmb887x_clc_reg_t clc;
	pmb887x_src_reg_t src[2];

	bool irq_fired;
	uint32_t timer_freq;
	int64_t start;
	int64_t next;
	bool enabled;

	uint32_t spcr;
	uint32_t slpctrl;
	uint32_t refin;
	uint32_t ref;
	uint32_t nqtz;
	uint32_t scctrl;
	uint32_t wait;
	uint32_t hwwakeup;
	uint32_t sccuclksta;
	uint32_t sccumsta;
	uint32_t timer_int;
	uint32_t timer_cnt;
	uint32_t tdmini;

	QEMUTimer *timer;
	QEMUTimer *cal_timer;
	pmb887x_pll_t *pll;
};

static uint64_t sccu_get_counter(pmb887x_sccu_t *p, bool real) {
	uint64_t next = p->timer_cnt;

	if (p->enabled) {
		uint64_t delta_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - p->start;
		next += muldiv64(delta_ns, p->timer_freq, NANOSECONDS_PER_SECOND);
	}

	return real ? next : MIN(next, p->tdmini);
}

static int64_t sccu_ticks_to_ns(pmb887x_sccu_t *p, uint64_t ticks) {
    return (int64_t) muldiv64(ticks, NANOSECONDS_PER_SECOND, p->timer_freq);
}

static void sccu_cal_timer_reset(void *opaque) {
	// nothing
}

static void sccu_ptimer_reset(void *opaque) {
	pmb887x_sccu_t *p = opaque;

	if (!p->enabled)
		return;

	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	uint32_t overflow = p->tdmini + 1;

	if (!p->start) {
		p->sccuclksta &= ~SCCU_SCCUCLKSTA_GSMCLK;
		p->sccumsta = SCCU_SCCUMSTA_UC_OFF;

		p->start = now;
		p->timer_cnt = 0;

		DPRINTF("sleep timer start %d ms\n", (uint32_t) (sccu_ticks_to_ns(p, p->tdmini) / 1000000));
		DPRINTF("now %u\n", (uint32_t) (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000));
	}

	uint64_t counter = sccu_get_counter(p, true);
	if (counter >= p->timer_int && !p->irq_fired) {
		p->irq_fired = true;
		pmb887x_src_update(&p->src[SCCU_IRQ_WAKE], 0, MOD_SRC_SETR);
	}

	if (counter >= p->tdmini) {
		p->start = 0;
		p->timer_cnt = p->timer_cnt % overflow;
		p->slpctrl &= ~SCCU_SLPCTRL_SLPEN;

		p->sccuclksta |= SCCU_SCCUCLKSTA_GSMCLK;
		p->sccumsta = SCCU_SCCUMSTA_UC_ON |
			SCCU_SCCUMSTA_TCXO_ON | SCCU_SCCUMSTA_SHAP_ON;

		DPRINTF("now %u\n", (uint32_t) (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000));
		DPRINTF("sleep timer done\n");
	} else {
		p->next = now + sccu_ticks_to_ns(p, overflow - counter);
		timer_mod(p->timer, p->next);
	}
}

static void sccu_update_timer_timer(struct pmb887x_sccu_t *p) {
	uint32_t nqtz = (p->nqtz & SCCU_NQTZ_NQTZ) >> SCCU_NQTZ_NQTZ_SHIFT;
	uint32_t prewup = (p->wait & SCCU_WAIT_PREWUP) >> SCCU_WAIT_PREWUP_SHIFT;

	if (!nqtz) {
		nqtz = 1;
	}

	p->timer_freq = pmb887x_pll_get_frtc(p->pll) / nqtz;
	p->enabled = (p->slpctrl & SCCU_SLPCTRL_SLPEN) != 0 && pmb887x_clc_is_enabled(&p->clc);
	p->timer_int = p->tdmini > prewup ? p->tdmini - prewup : 0;

	// Calibration
	if ((p->slpctrl & SCCU_SLPCTRL_REFEN)) {
		DPRINTF("SCCU_SLPCTRL_REFEN\n");
		// Unknown magic, similar to hardware value
		uint32_t timer_freq = pmb887x_pll_get_frtc(p->pll) / nqtz;
		uint32_t sccu_freq = pmb887x_pll_get_fosc(p->pll) / pmb887x_clc_get_rmc(&p->clc);
		uint32_t ratio = sccu_freq / timer_freq;
		uint32_t cal = (ratio >= 60000 ? 0 : 60000 - ratio) << 4;
		p->ref = (cal << SCCU_REF_REFOUT_SHIFT) | (0x400 << SCCU_REF_REFPOS_SHIFT);
	}

	// Reset
	if ((p->slpctrl & SCCU_SLPCTRL_SLPRST)) {
		DPRINTF("SCCU_SLPCTRL_SLPRST\n");
		p->timer_cnt = 0;
		p->start = 0;
		p->slpctrl &= ~SCCU_SLPCTRL_SLPRST;
	}

	if ((p->slpctrl & SCCU_SLPCTRL_SLPSTP)) {
		DPRINTF("SCCU_SLPCTRL_SLPSTP\n");
		p->slpctrl &= ~(SCCU_SLPCTRL_SLPEN | SCCU_SLPCTRL_SLPSTP);
		p->sccuclksta |= SCCU_SCCUCLKSTA_GSMCLK;
		p->sccumsta = SCCU_SCCUMSTA_UC_ON |
			SCCU_SCCUMSTA_TCXO_ON | SCCU_SCCUMSTA_SHAP_ON;
	}

	sccu_ptimer_reset(p);
}

uint32_t pmb887x_sccu_clc_get(pmb887x_sccu_t *p) {
	return pmb887x_clc_get(&p->clc);
}

void pmb887x_sccu_clc_set(pmb887x_sccu_t *p, uint32_t value) {
	pmb887x_clc_set(&p->clc, value);
	sccu_update_timer_timer(p);

}

static int sccu_get_reg_index(hwaddr haddr) {
	switch (haddr) {
		case SCCU_UNK_SRC:	return 0;
		case SCCU_WAKE_SRC:	return 1;
		default:			abort();
	}
}

static uint64_t sccu_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_sccu_t *p = opaque;

	uint64_t value = 0;

	switch (haddr) {
		case SCCU_SPCR:
			value = p->spcr;
			break;

		case SCCU_TDMINI:
			value = p->tdmini;
			break;

		case SCCU_TDMOUT:
			value = sccu_get_counter(p, false);
			break;

		case SCCU_SLPCTRL:
			value = p->slpctrl;
			break;

		case SCCU_REFIN:
			value = p->refin;
			break;

		case SCCU_REF:
			value = p->ref;
			break;

		case SCCU_NQTZ:
			value = p->nqtz;
			break;

		case SCCU_SCCTRL:
			value = p->scctrl;
			break;

		case SCCU_WAIT:
			value = p->wait;
			break;

		case SCCU_HWWAKEUP:
			value = p->hwwakeup;
			break;

		case SCCU_SCCUCLKSTA:
			value = p->sccuclksta;
			break;

		case SCCU_SCCUMSTA:
			value = p->sccumsta;
			break;

		case SCCU_WAKE_SRC:
		case SCCU_UNK_SRC:
			value = pmb887x_src_get(&p->src[sccu_get_reg_index(haddr)]);
			break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}

	IO_DUMP(haddr + p->mmio.addr, size, value, false);

	return value;
}

static void sccu_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_sccu_t *p = opaque;

	IO_DUMP(haddr + p->mmio.addr, size, value, true);

	switch (haddr) {
		case SCCU_SPCR:
			p->spcr = value;
			break;

		case SCCU_TDMINI:
			p->tdmini = value;
			sccu_update_timer_timer(p);
			break;

		case SCCU_SLPCTRL:
			p->slpctrl = value;
			sccu_update_timer_timer(p);
			break;

		case SCCU_REFIN:
			p->refin = value;
			break;

		case SCCU_NQTZ:
			p->nqtz = value;
			sccu_update_timer_timer(p);
			break;

		case SCCU_SCCTRL:
			p->scctrl = value;
			if (value & SCCU_SCCTRL_UCSLP) {
				p->slpctrl |= SCCU_SLPCTRL_SLPEN;
			}
			if (value & SCCU_SCCTRL_UCWUP) {
				p->slpctrl &= ~SCCU_SLPCTRL_SLPEN;
				p->sccuclksta |= SCCU_SCCUCLKSTA_GSMCLK;
				p->sccumsta = SCCU_SCCUMSTA_UC_ON |
					SCCU_SCCUMSTA_TCXO_ON | SCCU_SCCUMSTA_SHAP_ON;
			}
			if (value & SCCU_SCCTRL_SSCRST) {
				p->slpctrl |= SCCU_SLPCTRL_SLPRST;
			}
			sccu_update_timer_timer(p);
			break;

		case SCCU_WAIT:
			p->wait = value;
			sccu_update_timer_timer(p);
			break;

		case SCCU_HWWAKEUP:
			p->hwwakeup = value;
			break;

		case SCCU_WAKE_SRC:
		case SCCU_UNK_SRC:
			pmb887x_src_set(&p->src[sccu_get_reg_index(haddr)], value);
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
}

static const MemoryRegionOps io_ops = {
	.read			= sccu_io_read,
	.write			= sccu_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void sccu_init(Object *obj) {
	pmb887x_sccu_t *p = PMB887X_RTC(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-sccu", SCCU_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	for (int i = 0; i < ARRAY_SIZE(p->src); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void sccu_realize(DeviceState *dev, Error **errp) {
	pmb887x_sccu_t *p = PMB887X_RTC(dev);

	pmb887x_clc_init(&p->clc);

	for (int i = 0; i < ARRAY_SIZE(p->src); i++) {
		if (!p->irq[i])
			hw_error("pmb887x-tpu: irq %d not set", i);

		pmb887x_src_init(&p->src[i], p->irq[i]);
	}

    p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sccu_ptimer_reset, p);
    p->cal_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sccu_cal_timer_reset, p);

	p->nqtz = 0x97;
	p->wait = 3 << SCCU_WAIT_PREWUP_SHIFT;
	p->sccuclksta = SCCU_SCCUCLKSTA_CPUCLK | SCCU_SCCUCLKSTA_GSMCLK;
	p->sccumsta = SCCU_SCCUMSTA_UC_ON |
		SCCU_SCCUMSTA_TCXO_ON | SCCU_SCCUMSTA_SHAP_ON;

	sccu_update_timer_timer(p);
}

static const Property sccu_properties[] = {
	DEFINE_PROP_LINK("pll", struct pmb887x_sccu_t, pll, "pmb887x-pll", struct pmb887x_pll_t *),
};

static void sccu_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, sccu_properties);
	dc->realize = sccu_realize;
}

static const TypeInfo sccu_info = {
    .name          	= TYPE_PMB887X_RTC,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_sccu_t),
    .instance_init 	= sccu_init,
    .class_init    	= sccu_class_init,
};

static void sccu_register_types(void) {
	type_register_static(&sccu_info);
}
type_init(sccu_register_types)
