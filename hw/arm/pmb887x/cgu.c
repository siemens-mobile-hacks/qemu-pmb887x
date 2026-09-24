/*
 * CGU
 * */
#define PMB887X_TRACE_ID		CGU
#define PMB887X_TRACE_PREFIX	"pmb887x-cgu"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/hw-error.h"
#include "system/memory.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "cpu.h"
#include "qemu/timer.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_CGU	"pmb887x-cgu"
#define PMB887X_CGU(obj)	OBJECT_CHECK(pmb887x_cgu_t, (obj), TYPE_PMB887X_CGU)
#define PLL_LOCK_DELAY_NS	(10 * SCALE_US)

typedef struct pmb887x_cgu_t pmb887x_cgu_t;

struct pmb887x_cgu_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	uint32_t revision;
	
	pmb887x_src_reg_t src;
	qemu_irq irq;
	QEMUTimer *lock_timer;
	bool locked;
	
	uint32_t xtal;
	uint32_t hw_ns_div;
	
	uint32_t frtc;
	uint32_t fsys;
	uint32_t fpi2;
	uint32_t fahb;
	uint32_t fcpu;
	bool fpi2_pll_selected;
	Clock *osc_clock;
	Clock *rtc_clock;
	Clock *sys_clock;
	Clock *fpi2_clock;
	Clock *wdt_clock;
	Clock *fpi1_clock;
	Clock *dsp_clock;
	Clock *dma_clock;
	Clock *mmci_clock;
	
	uint32_t osc;
	uint32_t con0;
	uint32_t con1;
	uint32_t con2;
	uint32_t con3;

	qemu_irq gpio_clk32;
	qemu_irq gpio_fpi2_pll_selected;
};

static void cgu_update_state(struct pmb887x_cgu_t *p);

static void cgu_pll_lock_timer_expired(void *opaque) {
	pmb887x_cgu_t *p = opaque;

	if (!(p->osc & CGU_OSC_PLL_POWER_UP))
		return;

	p->locked = true;
	cgu_update_state(p);
	pmb887x_src_update(&p->src, 0, MOD_SRC_SETR);
}

// Freq after PLL
static uint32_t cgu_get_pll_core_freq(pmb887x_cgu_t *p) {
	if ((p->osc & CGU_OSC_PLL_POWER_UP) == 0 || !p->locked)
		return 0;

	// fPLL = fOSC * (NDIV + 1) / (MDIV + 1)
	uint32_t ndiv = (p->osc & CGU_OSC_NDIV) >> CGU_OSC_NDIV_SHIFT;
	uint32_t mdiv = (p->osc & CGU_OSC_MDIV) >> CGU_OSC_MDIV_SHIFT;
	return muldiv64(p->xtal, ndiv + 1, mdiv + 1);
}

static uint32_t cgu_get_pll_freq(pmb887x_cgu_t *p) {
	if ((p->osc & CGU_OSC_PLL_BYPASS_N) == 0)
		return p->xtal;

	return cgu_get_pll_core_freq(p);
}

static uint32_t cgu_get_phase_freq(pmb887x_cgu_t *p, uint32_t phase) {
	uint32_t power_up;
	uint32_t bypass_n;
	uint32_t k1;
	uint32_t k2;

	switch (phase) {
		case 1:
			power_up = CGU_OSC_PHASE1_POWER_UP;
			bypass_n = CGU_OSC_PHASE1_BYPASS_N;
			k1 = (p->con0 & CGU_CON0_PHASE1_K1) >> CGU_CON0_PHASE1_K1_SHIFT;
			k2 = (p->con0 & CGU_CON0_PHASE1_K2) >> CGU_CON0_PHASE1_K2_SHIFT;
			break;
		case 2:
			power_up = CGU_OSC_PHASE2_POWER_UP;
			bypass_n = CGU_OSC_PHASE2_BYPASS_N;
			k1 = (p->con0 & CGU_CON0_PHASE2_K1) >> CGU_CON0_PHASE2_K1_SHIFT;
			k2 = (p->con0 & CGU_CON0_PHASE2_K2) >> CGU_CON0_PHASE2_K2_SHIFT;
			break;
		case 3:
			power_up = CGU_OSC_PHASE3_POWER_UP;
			bypass_n = CGU_OSC_PHASE3_BYPASS_N;
			k1 = (p->con0 & CGU_CON0_PHASE3_K1) >> CGU_CON0_PHASE3_K1_SHIFT;
			k2 = (p->con0 & CGU_CON0_PHASE3_K2) >> CGU_CON0_PHASE3_K2_SHIFT;
			break;
		case 4:
			power_up = CGU_OSC_PHASE4_POWER_UP;
			bypass_n = CGU_OSC_PHASE4_BYPASS_N;
			k1 = (p->con0 & CGU_CON0_PHASE4_K1) >> CGU_CON0_PHASE4_K1_SHIFT;
			k2 = (p->con0 & CGU_CON0_PHASE4_K2) >> CGU_CON0_PHASE4_K2_SHIFT;
			break;
		default:
			return 0;
	}

	if ((p->osc & bypass_n) == 0)
		return cgu_get_pll_freq(p);
	if ((p->osc & power_up) == 0 || k1 == 0 || k2 > 5)
		return 0;

	return muldiv64(cgu_get_pll_core_freq(p), 12, k1 * 6 + k2);
}

// Get AHB bus freq
static uint32_t cgu_get_ahb_freq(pmb887x_cgu_t *p) {
	switch ((p->con1 & CGU_CON1_AHB_CLKSEL)) {
		case CGU_CON1_AHB_CLKSEL_BYPASS:
			// fAHB = fOSC
			return p->xtal;
		
		case CGU_CON1_AHB_CLKSEL_PLL:
			// fAHB = fPLL
			return cgu_get_pll_freq(p);
		
		case CGU_CON1_AHB_CLKSEL_PHASE1:
			return cgu_get_phase_freq(p, 1);
		
		case CGU_CON1_AHB_CLKSEL_PHASE2:
			return cgu_get_phase_freq(p, 2);
		
		case CGU_CON1_AHB_CLKSEL_PHASE3:
			return cgu_get_phase_freq(p, 3);
		
		case CGU_CON1_AHB_CLKSEL_PHASE4:
			return cgu_get_phase_freq(p, 4);
	}
	return 0;
}

static uint32_t cgu_get_dsp_freq(pmb887x_cgu_t *p) {
	if ((p->con2 & CGU_CON2_DSP_CLKSEL) == CGU_CON2_DSP_CLKSEL_PHASE1)
		return cgu_get_phase_freq(p, 1);

	return 0;
}

static uint32_t cgu_get_dma_freq(pmb887x_cgu_t *p) {
	if ((p->con3 & CGU_CON3_DMA_CLK_DISABLE) != 0)
		return 0;

	return cgu_get_pll_freq(p);
}

static uint32_t cgu_get_mmci_freq(pmb887x_cgu_t *p) {
	uint32_t frequency;

	switch (p->con3 & CGU_CON3_MMCI_CLKSEL) {
		case CGU_CON3_MMCI_CLKSEL_OSC:
			frequency = p->xtal;
			break;
		case CGU_CON3_MMCI_CLKSEL_CLK32K:
			frequency = p->frtc;
			break;
		case CGU_CON3_MMCI_CLKSEL_PHASE4:
			frequency = cgu_get_phase_freq(p, 4);
			break;
		default:
			return 0;
	}

	uint32_t divider = (p->con3 & CGU_CON3_MMCI_CLKDIV) >> CGU_CON3_MMCI_CLKDIV_SHIFT;
	return frequency >> divider;
}

static uint32_t cgu_get_sys_freq(pmb887x_cgu_t *p) {
	uint32_t freq = cgu_get_pll_freq(p);
	uint32_t clksel = p->con1 & CGU_CON1_FSYS_CLKSEL;
	
	// fSYS=0
	if (clksel == CGU_CON1_FSYS_CLKSEL_DISABLE)
		return 0;
	
	if (clksel == CGU_CON1_FSYS_CLKSEL_PLL) {
		// fSYS = fPLL / 2
		return freq / 2;
	}
	
	// fSYS = fOSC
	return p->xtal;
}

static uint32_t cgu_get_fpi2_freq(pmb887x_cgu_t *p) {
	if ((p->con1 & CGU_CON1_FPI2_CLKSEL) == CGU_CON1_FPI2_CLKSEL_PLL) {
		uint32_t divider = (p->con1 & CGU_CON1_FPI2_CLKDIV) >> CGU_CON1_FPI2_CLKDIV_SHIFT;

		return cgu_get_pll_freq(p) >> (divider + 1);
	}
	if ((p->con1 & CGU_CON1_FPI2_OSC_DISABLE) != 0)
		return 0;

	return p->xtal;
}

static uint32_t cgu_get_fpi1_freq(pmb887x_cgu_t *p) {
	uint32_t freq;

	switch (p->con1 & CGU_CON1_FPI1_CLKSEL) {
		case CGU_CON1_FPI1_CLKSEL_OSC:
			freq = p->xtal;
			break;
		case CGU_CON1_FPI1_CLKSEL_CLK32K:
			return p->frtc;
		case CGU_CON1_FPI1_CLKSEL_PLL_DIV_2:
			freq = cgu_get_pll_freq(p) / 2;
			break;
		default:
			return 0;
	}

	uint32_t div = (p->con1 & CGU_CON1_FPI1_CLKDIV) >> CGU_CON1_FPI1_CLKDIV_SHIFT;
	return freq >> div;
}

// CPU freq from AHB
static uint32_t cgu_get_cpu_freq(pmb887x_cgu_t *p) {
	uint32_t ahb_freq = cgu_get_ahb_freq(p);
	if ((p->con2 & CGU_CON2_CPU_DIV_EN)) {
		// fCPU = fAHB / (CPU_DIV + 1)
		uint32_t div = ((p->con2 & CGU_CON2_CPU_DIV) >> CGU_CON2_CPU_DIV_SHIFT) + 1;
		return ahb_freq / div;
	}
	// fCPU = fAHB
	return ahb_freq;
}

static void cgu_update_state(struct pmb887x_cgu_t *p) {
	uint32_t new_fsys = cgu_get_sys_freq(p);
	uint32_t new_fpi2 = cgu_get_fpi2_freq(p);
	uint32_t new_fcpu = cgu_get_cpu_freq(p);
	uint32_t new_fahb = cgu_get_ahb_freq(p);
	uint32_t new_fwdt = new_fpi2 / 16384;
	bool new_fpi2_pll_selected = (p->con1 & CGU_CON1_FPI2_CLKSEL) == CGU_CON1_FPI2_CLKSEL_PLL;
	
	if (new_fcpu != p->fcpu)
		DPRINTF("fCPU: %u -> %u Hz\n", p->fcpu, new_fcpu);
	if (new_fahb != p->fahb)
		DPRINTF("fAHB: %u -> %u Hz\n", p->fahb, new_fahb);
	if (new_fsys != p->fsys)
		DPRINTF("fSYS: %u -> %u Hz\n", p->fsys, new_fsys);
	if (new_fpi2 != p->fpi2)
		DPRINTF("fPI2: %u -> %u Hz\n", p->fpi2, new_fpi2);

	p->fsys = new_fsys;
	p->fcpu = new_fcpu;
	p->fahb = new_fahb;

	clock_update_hz(p->sys_clock, new_fsys);
	p->fpi2 = new_fpi2;
	clock_update_hz(p->fpi2_clock, new_fpi2);
	if (p->fpi2_pll_selected != new_fpi2_pll_selected) {
		p->fpi2_pll_selected = new_fpi2_pll_selected;
		qemu_set_irq(p->gpio_fpi2_pll_selected, new_fpi2_pll_selected);
	}
	clock_update_hz(p->wdt_clock, new_fwdt);
	clock_update_hz(p->fpi1_clock, cgu_get_fpi1_freq(p));
	clock_update_hz(p->dsp_clock, cgu_get_dsp_freq(p));
	clock_update_hz(p->dma_clock, cgu_get_dma_freq(p));
	clock_update_hz(p->mmci_clock, cgu_get_mmci_freq(p));
}

static uint64_t cgu_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_cgu_t *p = opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case CGU_OSC:
			value = p->osc;
			break;
		
		case CGU_CON0:
			value = p->con0;
			break;
		
		case CGU_CON1:
			value = p->con1;
			break;
		
		case CGU_CON2:
			value = p->con2;
			break;
		
		case CGU_STAT:
			value = p->locked ? CGU_STAT_LOCK : 0;
			break;
		
		case CGU_CON3:
			value = p->con3;
			break;
		
		case CGU_SRC:
			value = pmb887x_src_get(&p->src);
			break;
		
		default:
			IO_DUMP_READ(haddr + p->mmio.addr, size, 0xFFFFFFFF);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	IO_DUMP_READ(haddr + p->mmio.addr, size, value);
	
	return value;
}

static void cgu_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_cgu_t *p = opaque;
	
	IO_DUMP_WRITE(haddr + p->mmio.addr, size, value);
	
	switch (haddr) {
		case CGU_OSC: {
			uint32_t old_osc = p->osc;
			p->osc = value;
			bool pll_changed = ((old_osc ^ p->osc) & (CGU_OSC_PLL_POWER_UP | CGU_OSC_NDIV | CGU_OSC_MDIV)) != 0;
			if (!(p->osc & CGU_OSC_PLL_POWER_UP)) {
				timer_del(p->lock_timer);
				p->locked = false;
			} else if (pll_changed) {
				p->locked = false;
				timer_mod(p->lock_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PLL_LOCK_DELAY_NS);
			}
			break;
		}
		
		case CGU_CON0:
			p->con0 = value;
			break;
		
		case CGU_CON1:
			p->con1 = value;
			break;
		
		case CGU_CON2:
			p->con2 = value;
			break;
		
		case CGU_CON3:
			p->con3 = value;
			break;
		
		case CGU_SRC:
			pmb887x_src_set(&p->src, value);
			break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	cgu_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= cgu_io_read,
	.write			= cgu_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void cgu_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_cgu_t *p = PMB887X_CGU(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-cgu", CGU_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq);
	qdev_init_gpio_out_named(dev, &p->gpio_clk32, "CLK32_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->gpio_fpi2_pll_selected, "FPI2_PLL_SELECTED", 1);
	p->osc_clock = qdev_init_clock_out(dev, "OSC");
	p->rtc_clock = qdev_init_clock_out(dev, "RTC");
	p->sys_clock = qdev_init_clock_out(dev, "FSYS");
	p->fpi2_clock = qdev_init_clock_out(dev, "FPI2");
	p->wdt_clock = qdev_init_clock_out(dev, "WDT");
	p->fpi1_clock = qdev_init_clock_out(dev, "FPI1");
	p->dsp_clock = qdev_init_clock_out(dev, "DSP");
	p->dma_clock = qdev_init_clock_out(dev, "DMA");
	p->mmci_clock = qdev_init_clock_out(dev, "MMCI");
	p->lock_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cgu_pll_lock_timer_expired, p);
}

static void cgu_reset(DeviceState *dev) {
	pmb887x_cgu_t *p = PMB887X_CGU(dev);

	timer_del(p->lock_timer);
	pmb887x_src_reset(&p->src);

	p->frtc = 32768;
	clock_update_hz(p->osc_clock, p->xtal);
	clock_update_hz(p->rtc_clock, p->frtc);
	p->fsys = p->xtal;
	p->osc = 0x01070001;
	p->con0 = 0x22000012;
	p->con1 = 0;
	p->con2 = 0;
	p->con3 = 0;
	p->locked = true;

	cgu_update_state(p);
}

static void cgu_realize(DeviceState *dev, Error **errp) {
	pmb887x_cgu_t *p = PMB887X_CGU(dev);
	
	if (!p->irq)
		hw_error("pmb887x-cgu: irq not set");
	
	pmb887x_src_init(&p->src, p->irq);
	
	p->frtc = 32768;
	clock_set_hz(p->osc_clock, p->xtal);
	clock_set_hz(p->rtc_clock, p->frtc);
	p->fsys = p->xtal;
	
	// Initial values
	p->osc	= 0x01070001;
	p->con0	= 0x22000012;
	p->con1	= 0x00000000;
	p->con2	= 0x00000000;
	p->con3	= 0x00000000;
	p->locked = true;
	
	cgu_update_state(p);
}

static const Property cgu_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_cgu_t, revision, 0),
	DEFINE_PROP_UINT32("xtal", struct pmb887x_cgu_t, xtal, 26000000),
	DEFINE_PROP_UINT32("hw-ns-throttle", struct pmb887x_cgu_t, hw_ns_div, 1),
};

static void cgu_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, cgu_properties);
	device_class_set_legacy_reset(dc, cgu_reset);
	dc->realize = cgu_realize;
}

static const TypeInfo cgu_info = {
	.name			= TYPE_PMB887X_CGU,
	.parent			= TYPE_SYS_BUS_DEVICE,
	.instance_size	= sizeof(struct pmb887x_cgu_t),
	.instance_init	= cgu_init,
	.class_init		= cgu_class_init,
};

static void cgu_register_types(void) {
	type_register_static(&cgu_info);
}
type_init(cgu_register_types)
