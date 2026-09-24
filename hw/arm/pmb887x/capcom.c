/*
 * Capture/Compare
 * */
#define PMB887X_TRACE_ID		CAPCOM
#define PMB887X_TRACE_PREFIX	"pmb887x-capcom"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/hw-error.h"
#include "system/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "hw/core/qdev-properties.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/pll.h"
#include "qemu/timer.h"

#define TYPE_PMB887X_CAPCOM	"pmb887x-capcom"
#define PMB887X_CAPCOM(obj)	OBJECT_CHECK(pmb887x_capcom_t, (obj), TYPE_PMB887X_CAPCOM)

typedef struct pmb887x_capcom_t pmb887x_capcom_t;
typedef struct pmb887x_capcom_cc_t pmb887x_capcom_cc_t;
typedef enum pmb887x_capcom_cc_mode_t pmb887x_capcom_cc_mode_t;

struct pmb887x_capcom_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	uint32_t revision;
	
	qemu_irq t_irq[2];
	qemu_irq cc_irq[8];
	
	pmb887x_src_reg_t t_src[2];
	pmb887x_src_reg_t cc_src[8];
	
	pmb887x_clc_reg_t clc;
	struct pmb887x_cgu_t *cgu;
	
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
	uint32_t t0rel;
	uint32_t t1rel;
	uint32_t t01ocr;
	uint32_t whbsout;
	uint32_t whbcout;
	uint32_t cc[8];
	
	/* The counting engine: T0/T1 are free-running up-counters at
	 * f_capcom = fsys / RMC while their TnR run bit is set, wrapping at
	 * 0xFFFF and reloading from TnREL.  Compares (CCm in MODE0..3 with
	 * ACCm selecting T0/T1) fire cc_src[m] when the accumulated counter
	 * is equal to CCm; wraps fire t_src[n].  One QEMU_CLOCK_VIRTUAL timer
	 * serves the next event that can raise an interrupt (the KE970
	 * firmware boots into a wait for exactly such a compare, and the stub
	 * hung it).  Events whose source cannot raise one (SRE clear, or SRR
	 * already pending) only set bits, so they are applied on the next
	 * access instead: the SL65 runs T0 as a 179 kHz PWM with no interrupt
	 * enabled, and a timer per wrap was a 7x slowdown.  Ticks are counted
	 * from epoch_ns, so the counter values do not depend on when the
	 * accesses sync them. */
	QEMUTimer *timer;
	uint32_t freq;
	int64_t epoch_ns;		/* virtual time of tick 0 at the current freq */
	uint64_t ticks;			/* ticks since epoch_ns applied to count[] */
	uint32_t count[2];
	bool ovf[2];
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

static bool capcom_t_run(pmb887x_capcom_t *p, int n) {
	return (p->t01con & (n ? CAPCOM_T01CON_T1R : CAPCOM_T01CON_T0R)) != 0;
}

static bool capcom_t_timer_mode(pmb887x_capcom_t *p, int n) {
	/* TnM: 0 = timer (module clock), 1 = counter (external input) */
	return (p->t01con & (n ? CAPCOM_T01CON_T1M : CAPCOM_T01CON_T0M)) == 0;
}

static uint32_t capcom_t_rel(pmb887x_capcom_t *p, int n) {
	return (n ? p->t1rel : p->t0rel) & 0xFFFF;
}

static void capcom_update_freq(pmb887x_capcom_t *p) {
	uint8_t rmc = pmb887x_clc_get_rmc(&p->clc);
	uint32_t f = rmc > 0 && p->cgu ? pmb887x_pll_get_fsys(p->cgu) / rmc : 0;
	if (f != p->freq) {
		p->freq = f;
		p->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
		p->ticks = 0;
		DPRINTF("fcapcom=%d %s\n", p->freq,
				pmb887x_clc_is_enabled(&p->clc) && p->freq > 0 ? "[ON]" : "[OFF]");
	}
}

/* Which timer does CCm accumulate, and is it in a compare mode? */
static bool capcom_cc_compare(pmb887x_capcom_t *p, int m, int *acc) {
	const pmb887x_capcom_cc_t *cc = &capcom_cc_list[m];
	uint32_t ccm = p->ccm[cc->ccm_index];
	int mode = (ccm & cc->mod_mask) >> cc->mod_shift;
	if (mode < CAPCOM_CC_MODE_0)
		return false;
	*acc = (ccm & cc->acc_mask) ? 1 : 0;
	return true;
}

/* The compares on Tn that the counter passes moving through (lo, hi] */
static uint32_t capcom_hits(pmb887x_capcom_t *p, int n, uint32_t lo, uint32_t hi) {
	uint32_t hits = 0;
	for (int m = 0; m < 8; m++) {
		int acc;
		if (!capcom_cc_compare(p, m, &acc) || acc != n)
			continue;
		uint32_t ccv = p->cc[m] & 0xFFFF;
		if (ccv > lo && ccv <= hi)
			hits |= 1u << m;
	}
	return hits;
}

/* Only an event that raises the line has to happen on time */
static bool capcom_src_wants_timer(pmb887x_src_reg_t *src) {
	uint32_t v = pmb887x_src_get(src);
	return (v & MOD_SRC_SRE) && !(v & MOD_SRC_SRR);
}

static void capcom_advance(pmb887x_capcom_t *p, int n, uint64_t elapsed) {
	uint32_t rel = capcom_t_rel(p, n);
	uint32_t cur = p->count[n];
	uint32_t pass = MIN((uint64_t) 0x10000 - cur, elapsed);
	uint32_t hits = capcom_hits(p, n, cur, cur + pass);
	bool wrap = cur + pass == 0x10000;

	if (wrap) {
		uint32_t period = 0x10000 - rel;
		elapsed -= pass;
		if (elapsed >= period)
			hits |= capcom_hits(p, n, rel, 0x10000);
		elapsed %= period;
		hits |= capcom_hits(p, n, rel, rel + elapsed);
		cur = rel + elapsed;
	} else {
		cur += pass;
	}
	p->count[n] = cur;

	for (int m = 0; m < 8; m++) {
		if (hits & (1u << m)) {
			DPRINTF("CC%d compare hit (cc=%04x, t=%04x)\n", m, p->cc[m] & 0xFFFF, cur);
			pmb887x_src_update(&p->cc_src[m], 0, MOD_SRC_SETR);
		}
	}
	if (wrap) {
		p->ovf[n] = true;
		DPRINTF("T%d wrap (rel=%04x)\n", n, rel);
		pmb887x_src_update(&p->t_src[n], 0, MOD_SRC_SETR);
	}
}

/* Advance the counters to now, firing every event passed on the way, and
 * re-arm.  Everything else (reads, writes, timer) goes through here first. */
static void capcom_sync(pmb887x_capcom_t *p) {
	if (p->freq == 0) {
		timer_del(p->timer);
		return;
	}

	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	/* a mid-TB read can run ahead of a later one (see rtc_sync) */
	uint64_t ticks = now > p->epoch_ns ?
		muldiv64(now - p->epoch_ns, p->freq, NANOSECONDS_PER_SECOND) : 0;
	if (ticks > p->ticks) {
		for (int n = 0; n < 2; n++) {
			if (capcom_t_run(p, n) && capcom_t_timer_mode(p, n))
				capcom_advance(p, n, ticks - p->ticks);
		}
		p->ticks = ticks;
	}

	uint64_t best = UINT64_MAX;
	for (int n = 0; n < 2; n++) {
		if (!capcom_t_run(p, n) || !capcom_t_timer_mode(p, n))
			continue;
		uint32_t cur = p->count[n];
		uint32_t rel = capcom_t_rel(p, n);
		if (capcom_src_wants_timer(&p->t_src[n]))
			best = MIN(best, (uint64_t) 0x10000 - cur);
		for (int m = 0; m < 8; m++) {
			int acc;
			if (!capcom_cc_compare(p, m, &acc) || acc != n ||
				!capcom_src_wants_timer(&p->cc_src[m]))
				continue;
			uint32_t ccv = p->cc[m] & 0xFFFF;
			/* after a wrap the counter restarts above rel */
			if (ccv > cur)
				best = MIN(best, (uint64_t) ccv - cur);
			else if (ccv > rel)
				best = MIN(best, (uint64_t) (0x10000 - cur) + (ccv - rel));
		}
	}
	if (best == UINT64_MAX) {
		timer_del(p->timer);
	} else {
		timer_mod(p->timer, p->epoch_ns +
			muldiv64_round_up(p->ticks + best, NANOSECONDS_PER_SECOND, p->freq));
	}
}

static void capcom_timer_cb(void *opaque) {
	capcom_sync(opaque);
}

static void capcom_update_state(pmb887x_capcom_t *p) {
	capcom_update_freq(p);
	capcom_sync(p);
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
	
	/* SRR and OVF bits of events nobody timed are only applied here */
	capcom_sync(p);
	
	switch (haddr) {
		case CAPCOM_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;
		
		case CAPCOM_ID:
			value = 0x00005000 | p->revision;
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
			value = p->count[0] | (p->ovf[0] ? CAPCOM_T0_OVF0 : 0);
			break;
		
		case CAPCOM_T0REL:
			value = p->t0rel;
			break;
		
		case CAPCOM_T1:
			value = p->count[1] | (p->ovf[1] ? CAPCOM_T1_OVF1 : 0);
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
		
		case CAPCOM_CC0:
		case CAPCOM_CC1:
		case CAPCOM_CC2:
		case CAPCOM_CC3:
		case CAPCOM_CC4:
		case CAPCOM_CC5:
		case CAPCOM_CC6:
		case CAPCOM_CC7:
			value = p->cc[(haddr - CAPCOM_CC0) / 4];
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
			IO_DUMP_READ(haddr + p->mmio.addr, size, 0xFFFFFFFF);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}
	
	IO_DUMP_READ(haddr + p->mmio.addr, size, value);
	
	return value;
}

static void capcom_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_capcom_t *p = opaque;
	
	/* advance with the old configuration first, so the write takes
	 * effect from now on and no tick in flight is lost or re-counted */
	capcom_sync(p);
	
	IO_DUMP_WRITE(haddr + p->mmio.addr, size, value);
	
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
			p->count[0] = value & 0xFFFF;
			p->ovf[0] = !!(value & CAPCOM_T0_OVF0);
			break;
		
		case CAPCOM_T0REL:
			p->t0rel = value;
			break;
		
		case CAPCOM_T1:
			p->count[1] = value & 0xFFFF;
			p->ovf[1] = !!(value & CAPCOM_T1_OVF1);
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
		
		case CAPCOM_CC0:
		case CAPCOM_CC1:
		case CAPCOM_CC2:
		case CAPCOM_CC3:
		case CAPCOM_CC4:
		case CAPCOM_CC5:
		case CAPCOM_CC6:
		case CAPCOM_CC7:
			p->cc[(haddr - CAPCOM_CC0) / 4] = value;
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

	/* a capture latches the accumulated counter into CCm on the edge,
	 * then raises the service request */
	if (capcom_get_mode(p, id) >= CAPCOM_CC_MODE_RISING_EDGE &&
		capcom_get_mode(p, id) <= CAPCOM_CC_MODE_BOTH_EDGES) {
		const pmb887x_capcom_cc_t *cc = &capcom_cc_list[id];
		uint32_t ccm = p->ccm[cc->ccm_index];
		int acc = (ccm & cc->acc_mask) ? 1 : 0;
		capcom_sync(p);
		p->cc[id] = p->count[acc];
	}

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

static void capcom_reset(DeviceState *dev) {
	pmb887x_capcom_t *p = PMB887X_CAPCOM(dev);

	pmb887x_clc_init(&p->clc);

	for (size_t i = 0; i < ARRAY_SIZE(p->t_src); i++)
		pmb887x_src_reset(&p->t_src[i]);
	for (size_t i = 0; i < ARRAY_SIZE(p->cc_src); i++)
		pmb887x_src_reset(&p->cc_src[i]);

	p->pisel = 0;
	p->t01con = 0;
	memset(p->ccm, 0, sizeof(p->ccm));
	p->out = 0;
	p->ioc = 0;
	p->sem = 0;
	p->see = 0;
	p->drm = 0;
	p->whbssee = 0;
	p->whbcsee = 0;
	p->t0rel = 0;
	p->t1rel = 0;
	p->t01ocr = 0;
	p->whbsout = 0;
	p->whbcout = 0;
	memset(p->cc, 0, sizeof(p->cc));
	p->count[0] = p->count[1] = 0;
	p->ovf[0] = p->ovf[1] = false;
	p->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	p->ticks = 0;

	capcom_update_state(p);
}

static void capcom_realize(DeviceState *dev, Error **errp) {
	pmb887x_capcom_t *p = PMB887X_CAPCOM(dev);
	
	pmb887x_clc_init(&p->clc);
	p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, capcom_timer_cb, p);
	p->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	
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

static const Property capcom_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_capcom_t, revision, 0),
	DEFINE_PROP_LINK("cgu", pmb887x_capcom_t, cgu, "pmb887x-cgu", struct pmb887x_cgu_t *),
};

static void capcom_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, capcom_properties);
	device_class_set_legacy_reset(dc, capcom_reset);
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
