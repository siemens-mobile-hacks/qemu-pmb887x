/*
 * SCCU
 * */
#define PMB887X_TRACE_ID		SCCU
#define PMB887X_TRACE_PREFIX	"pmb887x-sccu"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/hw-error.h"
#include "system/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/core/qdev-properties.h"

#include "hw/arm/pmb887x/sccu.h"
#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_SCCU	"pmb887x-sccu"
#define PMB887X_SCCU(obj)	OBJECT_CHECK(pmb887x_sccu_t, (obj), TYPE_PMB887X_SCCU)

enum {
	SCCU_IRQ_UNK = 0,
	SCCU_IRQ_WAKE
};

typedef struct pmb887x_sccu_t pmb887x_sccu_t;

struct pmb887x_sccu_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	uint32_t revision;

	qemu_irq irq[2];

	pmb887x_clc_reg_t clc;
	pmb887x_src_reg_t src[2];

	bool irq_fired;
	uint32_t timer_freq;
	int64_t start;
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
	uint32_t timer_cnt;
	uint32_t tdmini;

	QEMUTimer *timer;
	QEMUTimer *cal_timer;
	QEMUTimer *sc_timer;
	pmb887x_pll_t *pll;
};

static uint32_t sccu_get_nqtz(pmb887x_sccu_t *p) {
	uint32_t nqtz = (p->nqtz & SCCU_NQTZ_NQTZ) >> SCCU_NQTZ_NQTZ_SHIFT;
	return nqtz ? nqtz : 1;
}

static uint64_t sccu_get_counter(pmb887x_sccu_t *p, bool real) {
	uint64_t next = p->timer_cnt;

	if (p->enabled) {
		uint64_t delta_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - p->start;
		next += muldiv64(delta_ns, p->timer_freq, NANOSECONDS_PER_SECOND);
	}

	return real ? next : MIN(next, p->tdmini);
}

static int64_t sccu_ticks_to_ns(pmb887x_sccu_t *p, uint64_t ticks) {
	return (int64_t) muldiv64(ticks, NANOSECONDS_PER_SECOND, p->timer_freq) + (ticks ? 1 : 0);
}

static void sccu_cal_timer_reset(void *opaque) {
	pmb887x_sccu_t *p = opaque;
	uint32_t frtc = pmb887x_pll_get_frtc(p->pll);
	uint32_t rmc = pmb887x_clc_get_rmc(&p->clc);
	uint32_t sccu_freq = pmb887x_pll_get_fosc(p->pll) / (rmc ? rmc : 1);
	uint64_t standby_cycles = (uint64_t) sccu_get_nqtz(p) * 16 * sccu_freq / frtc;
	uint32_t refout = standby_cycles < 960000 ? 960000 - standby_cycles : 0;
	uint32_t refpos = 128;

	p->ref = ((refout << SCCU_REF_REFOUT_SHIFT) & SCCU_REF_REFOUT) |
		((refpos << SCCU_REF_REFPOS_SHIFT) & SCCU_REF_REFPOS);
	p->slpctrl &= ~(SCCU_SLPCTRL_REFEN | SCCU_SLPCTRL_REFERR);
	if (refout <= 16 || refout > (SCCU_REF_REFOUT >> SCCU_REF_REFOUT_SHIFT))
		p->slpctrl |= SCCU_SLPCTRL_REFERR;
}

static void sccu_set_active_state(pmb887x_sccu_t *p) {
	p->sccuclksta = SCCU_SCCUCLKSTA_CPUCLK | SCCU_SCCUCLKSTA_GSMCLK;
	p->sccumsta = SCCU_SCCUMSTA_UC_ON | SCCU_SCCUMSTA_TCXO_ON | SCCU_SCCUMSTA_SHAP_ON;
}

static void sccu_sc_timer_reset(void *opaque) {
	pmb887x_sccu_t *p = opaque;
	uint32_t command = p->scctrl;

	p->scctrl = 0;
	if ((command & (SCCU_SCCTRL_UCWUP | SCCU_SCCTRL_SSCRST))) {
		sccu_set_active_state(p);
	} else if ((command & SCCU_SCCTRL_UCSLP)) {
		p->sccuclksta &= ~(SCCU_SCCUCLKSTA_CPUCLK | SCCU_SCCUCLKSTA_GSMCLK);
		p->sccumsta = SCCU_SCCUMSTA_TCXO_OFF;
	}
}

static void sccu_ptimer_reset(void *opaque) {
	pmb887x_sccu_t *p = opaque;

	if (!p->enabled)
		return;

	uint32_t overflow = p->tdmini + 1;
	uint64_t counter = sccu_get_counter(p, true);

	if (counter >= p->tdmini && !p->irq_fired) {
		p->irq_fired = true;
		pmb887x_src_update(&p->src[SCCU_IRQ_WAKE], 0, MOD_SRC_SETR);
	}

	if (counter >= overflow) {
		p->start = 0;
		p->timer_cnt = p->tdmini;
		p->enabled = false;
		p->slpctrl &= ~SCCU_SLPCTRL_SLPEN;
		sccu_set_active_state(p);

		DPRINTF("now %u\n", (uint32_t) (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000));
		DPRINTF("sleep timer done\n");
	} else {
		timer_mod(p->timer, p->start + sccu_ticks_to_ns(p, overflow));
	}
}

static void sccu_start_sleep(pmb887x_sccu_t *p) {
	p->timer_freq = pmb887x_pll_get_frtc(p->pll) / sccu_get_nqtz(p);
	p->timer_cnt = 0;
	p->start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	p->enabled = true;
	p->irq_fired = false;
	p->slpctrl |= SCCU_SLPCTRL_SLPEN;
	p->sccuclksta &= ~SCCU_SCCUCLKSTA_GSMCLK;

	uint32_t first_event = p->tdmini ? p->tdmini : p->tdmini + 1;
	timer_mod(p->timer, p->start + sccu_ticks_to_ns(p, first_event));
	DPRINTF("sleep timer start %d ms\n", (uint32_t) (sccu_ticks_to_ns(p, p->tdmini + 1) / 1000000));
}

uint32_t pmb887x_sccu_clc_get(pmb887x_sccu_t *p) {
	return pmb887x_clc_get(&p->clc);
}

void pmb887x_sccu_clc_set(pmb887x_sccu_t *p, uint32_t value) {
	pmb887x_clc_set(&p->clc, value);
	p->timer_freq = pmb887x_pll_get_frtc(p->pll) / sccu_get_nqtz(p);
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
			p->spcr = value & (SCCU_SPCR_DPDN | SCCU_SPCR_APDN | SCCU_SPCR_DROFF | SCCU_SPCR_DREN);
			break;

		case SCCU_TDMINI:
			p->tdmini = value & SCCU_TDMINI_TDMAIN;
			break;

		case SCCU_SLPCTRL: {
			uint32_t status = p->slpctrl & (SCCU_SLPCTRL_REFEN | SCCU_SLPCTRL_SLPEN | SCCU_SLPCTRL_REFERR);
			p->slpctrl = status | (value & (SCCU_SLPCTRL_SLPRST | SCCU_SLPCTRL_HWACTDI));

			if ((value & SCCU_SLPCTRL_SLPRST)) {
				timer_del(p->timer);
				p->enabled = false;
				p->start = 0;
				p->timer_cnt = 0;
				p->irq_fired = false;
				p->slpctrl &= ~SCCU_SLPCTRL_SLPEN;
				sccu_set_active_state(p);
			}

			if ((value & SCCU_SLPCTRL_REFEN) && !(status & SCCU_SLPCTRL_REFEN)) {
				uint32_t rmc = pmb887x_clc_get_rmc(&p->clc);
				uint32_t sccu_freq = pmb887x_pll_get_fosc(p->pll) / (rmc ? rmc : 1);
				int64_t duration = (int64_t) muldiv64(16 * 60000, NANOSECONDS_PER_SECOND, sccu_freq);

				p->slpctrl = (p->slpctrl | SCCU_SLPCTRL_REFEN) & ~SCCU_SLPCTRL_REFERR;
				timer_mod(p->cal_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + duration);
			}

			if ((value & SCCU_SLPCTRL_SLPEN) && !p->enabled)
				sccu_start_sleep(p);

			if ((value & SCCU_SLPCTRL_SLPSTP)) {
				p->timer_cnt = sccu_get_counter(p, false);
				p->start = 0;
				p->enabled = false;
				p->slpctrl &= ~(SCCU_SLPCTRL_SLPEN | SCCU_SLPCTRL_SLPSTP);
				timer_del(p->timer);
				sccu_set_active_state(p);
			}
			break;
		}

		case SCCU_REFIN:
			p->refin = value & SCCU_REFIN_REFIN;
			break;

		case SCCU_NQTZ:
			p->nqtz = value & SCCU_NQTZ_NQTZ;
			p->timer_freq = pmb887x_pll_get_frtc(p->pll) / sccu_get_nqtz(p);
			break;

		case SCCU_SCCTRL:
			p->scctrl = value & (SCCU_SCCTRL_UCSLP | SCCU_SCCTRL_UCWUP | SCCU_SCCTRL_SSCRST);
			if (p->scctrl) {
				uint32_t frtc = pmb887x_pll_get_frtc(p->pll);
				int64_t duration = (int64_t) muldiv64(3, NANOSECONDS_PER_SECOND, frtc) + 1;
				timer_mod(p->sc_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + duration);
			}
			break;

		case SCCU_WAIT:
			p->wait = value & (SCCU_WAIT_PREWUP | SCCU_WAIT_WAIT);
			break;

		case SCCU_HWWAKEUP:
			p->hwwakeup = value & (SCCU_HWWAKEUP_RTC_EN | SCCU_HWWAKEUP_KPD_EN |
				SCCU_HWWAKEUP_SIM_EN | SCCU_HWWAKEUP_EXT_EN);
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
	pmb887x_sccu_t *p = PMB887X_SCCU(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-sccu", SCCU_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	for (int i = 0; i < ARRAY_SIZE(p->src); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void sccu_realize(DeviceState *dev, Error **errp) {
	pmb887x_sccu_t *p = PMB887X_SCCU(dev);

	pmb887x_clc_init(&p->clc);

	for (int i = 0; i < ARRAY_SIZE(p->src); i++) {
		if (!p->irq[i])
			hw_error("pmb887x-tpu: irq %d not set", i);

		pmb887x_src_init(&p->src[i], p->irq[i]);
	}

	p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sccu_ptimer_reset, p);
	p->cal_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sccu_cal_timer_reset, p);
	p->sc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sccu_sc_timer_reset, p);

	p->nqtz = 0x97;
	p->wait = 3 << SCCU_WAIT_PREWUP_SHIFT;
	p->timer_freq = pmb887x_pll_get_frtc(p->pll) / sccu_get_nqtz(p);
	sccu_set_active_state(p);
}

static void sccu_reset(DeviceState *dev) {
	pmb887x_sccu_t *p = PMB887X_SCCU(dev);

	timer_del(p->timer);
	timer_del(p->cal_timer);
	timer_del(p->sc_timer);

	pmb887x_clc_init(&p->clc);

	for (size_t i = 0; i < ARRAY_SIZE(p->src); i++)
		pmb887x_src_reset(&p->src[i]);

	p->irq_fired = false;
	p->start = 0;
	p->enabled = false;

	p->spcr = 0;
	p->slpctrl = 0;
	p->refin = 0;
	p->ref = 0;
	p->nqtz = 0x97;
	p->scctrl = 0;
	p->wait = 3 << SCCU_WAIT_PREWUP_SHIFT;
	p->hwwakeup = 0;
	p->sccuclksta = 0;
	p->sccumsta = 0;
	p->timer_cnt = 0;
	p->tdmini = 0;
	p->timer_freq = pmb887x_pll_get_frtc(p->pll) / sccu_get_nqtz(p);

	sccu_set_active_state(p);
}

static const Property sccu_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_sccu_t, revision, 0),
	DEFINE_PROP_LINK("pll", struct pmb887x_sccu_t, pll, "pmb887x-pll", struct pmb887x_pll_t *),
};

static void sccu_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, sccu_properties);
	device_class_set_legacy_reset(dc, sccu_reset);
	dc->realize = sccu_realize;
}

static const TypeInfo sccu_info = {
    .name          	= TYPE_PMB887X_SCCU,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_sccu_t),
    .instance_init 	= sccu_init,
    .class_init    	= sccu_class_init,
};

static void sccu_register_types(void) {
	type_register_static(&sccu_info);
}
type_init(sccu_register_types)
