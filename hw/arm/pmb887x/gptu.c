/*
 * GPTU (General Purpose Timer Unit)
 * */
#define PMB887X_TRACE_ID		GPTU
#define PMB887X_TRACE_PREFIX	"pmb887x-gptu"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "system/memory.h"
#include "cpu.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"

#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_GPTU	"pmb887x-gptu"
#define PMB887X_GPTU(obj)	OBJECT_CHECK(pmb887x_gptu_t, (obj), TYPE_PMB887X_GPTU)

#define GPTU_OVERFLOW	0x100
#define GPTU_OUT_MASK	0xFF

const char *TIMER_NAMES[] = {"T0A", "T0B", "T0C", "T0D", "T1A", "T1B", "T1C", "T1D"};

enum {
	INPUT_BYPASS = 0,
	INPUT_CNT0,
	INPUT_CNT1,
	INPUT_CONCAT
};

enum {
	EV_START_A		= 0,
	EV_STOP_A		= 1,
	EV_UPDOWN_A		= 2,
	EV_CLEAR_A		= 3,
	EV_RLCP0_A		= 4,
	EV_RLCP1_A		= 5,
	EV_OUV_T2A		= 6,
	EV_OUV_T2B		= 7,
	EV_START_B		= 8,
	EV_STOP_B		= 9,
	EV_RLCP0_B		= 10,
	EV_RLCP1_B		= 11,
	EV_SR00			= 12,
	EV_SR01			= 13,
	EV_SR10			= 14,
	EV_SR11			= 15,
};

typedef struct pmb887x_gptu_ev_t pmb887x_gptu_ev_t;
typedef struct pmb887x_gptu_timer_t pmb887x_gptu_timer_t;
typedef struct pmb887x_gptu_timer_t2_t pmb887x_gptu_timer_t2_t;
typedef struct pmb887x_gptu_t pmb887x_gptu_t;

struct pmb887x_gptu_ev_t {
	int id;
	uint32_t mask;
};

struct pmb887x_gptu_timer_t {
	int id;
	int prev;
	bool enabled;
	bool concat;
	bool ev_ssr[2];
	uint64_t start;
	uint64_t counter;
	uint64_t reload;
};

struct pmb887x_gptu_timer_t2_t {
	bool enabled;
	bool oneshot;
	bool stopped;
	uint64_t start;
	int64_t counter;
};

struct pmb887x_gptu_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;

	qemu_irq irq[8];
	QEMUTimer *timer;
	QEMUTimer *timer_t2;

	bool enabled;
	uint32_t freq;

	pmb887x_clc_reg_t clc;
	pmb887x_src_reg_t src[8];

	pmb887x_gptu_timer_t timers[8];
	pmb887x_gptu_timer_t2_t timers_t2[2];
	pmb887x_gptu_ev_t events[16];
	uint32_t events_ssr[2][2];

	struct pmb887x_pll_t *pll;

	int64_t next;
	int64_t next_t2;

	uint32_t t01irs;
	uint32_t t01ots;
	uint32_t t2con;
	uint32_t t2rccon;
	uint32_t t2ais;
	uint32_t t2bis;
	uint32_t t2es;
	uint32_t osel;
	uint32_t out;
	uint32_t t2rc0;
	uint32_t t2rc1;
	uint32_t t012run;
	uint32_t srsel;

	bool syncing_t01;
	bool syncing_t2;
};

static void gptu_sync_timer(pmb887x_gptu_t *p);
static void gptu_t2_sync_timer(pmb887x_gptu_t *p);
static void gptu_t2_external_trigger(pmb887x_gptu_t *p, int trigger_id, uint64_t count);
static void gptu_t01_external_count(pmb887x_gptu_t *p, int cnt_id, uint64_t count);

/*
 * Common
 * */
static void gptu_update_freq(pmb887x_gptu_t *p) {
	uint8_t rmc = pmb887x_clc_get_rmc(&p->clc);

	p->freq = rmc > 0 ? pmb887x_pll_get_fsys(p->pll) / rmc : 0;
	p->enabled = pmb887x_clc_is_enabled(&p->clc) && p->freq > 0;

	DPRINTF("fgptu=%d %s\n", p->freq, p->enabled ? "[ON]" : "[OFF]");
}

static int64_t gptu_ticks_to_ns(pmb887x_gptu_t *p, uint64_t ticks) {
	if (p->freq == 0)
		return INT64_MAX;
	return (int64_t) muldiv64(ticks, NANOSECONDS_PER_SECOND, p->freq);
}

static void gptu_trigger_ev_irq(pmb887x_gptu_t *p, int ev_id) {
	pmb887x_gptu_ev_t *ev = &p->events[ev_id];
	for (int i = 0; i < 8; i++) {
		if ((ev->mask) & (1 << i))
			pmb887x_src_update(&p->src[i], 0, MOD_SRC_SETR);
	}
}

static void gptu_toggle_output_source(pmb887x_gptu_t *p, int source, uint64_t count) {
	if ((count & 1) == 0)
		return;

	for (int i = 0; i < 8; i++) {
		if (((p->osel >> (i * 4)) & 0x7) == source)
			p->out ^= (1 << i);
	}
	p->out &= GPTU_OUT_MASK;
}

static int gptu_get_ssr_ev(int id, int n) {
	if (id == 0 && n == 0)
		return EV_SR00;
	if (id == 0 && n == 1)
		return EV_SR01;
	if (id == 1 && n == 0)
		return EV_SR10;
	if (id == 1 && n == 1)
		return EV_SR11;

	hw_error("gptu_get_ssr_ev(%d, %d)", id, n);
}

static void gptu_update_events(pmb887x_gptu_t *p) {
	for (int i = 0; i < 16; i++) {
		p->events[i].id = i;
		p->events[i].mask = 0;
	}

	for (int i = 0; i < 8; i++) {
		int source_id = 8 - i - 1;
		uint32_t event_id = (p->srsel >> (4 * i)) & 0xF;
		p->events[event_id].mask |= (1 << source_id);
	}

	p->events_ssr[0][0] = (p->t01ots & GPTU_T01OTS_SSR00) >> GPTU_T01OTS_SSR00_SHIFT;
	p->events_ssr[0][1] = (p->t01ots & GPTU_T01OTS_SSR01) >> GPTU_T01OTS_SSR01_SHIFT;
	p->events_ssr[1][0] = ((p->t01ots & GPTU_T01OTS_SSR10) >> GPTU_T01OTS_SSR10_SHIFT) + 4;
	p->events_ssr[1][1] = ((p->t01ots & GPTU_T01OTS_SSR11) >> GPTU_T01OTS_SSR11_SHIFT) + 4;

	for (int i = 0; i < 8; i++) {
		for (int j = 0; j < 2; j++) {
			int timer_group = i / 4;
			int ev_id = gptu_get_ssr_ev(timer_group, j);
			p->timers[i].ev_ssr[j] = p->events[ev_id].mask && p->events_ssr[timer_group][j] == i;
		}
	}

	gptu_sync_timer(p);
	gptu_t2_sync_timer(p);
}

/*
 * T2
 * */
static bool gptu_t2_split(pmb887x_gptu_t *p) {
	return (p->t2con & GPTU_T2CON_T2SPLIT) != 0;
}

static int gptu_t2_logical_id(pmb887x_gptu_t *p, int timer_id) {
	return gptu_t2_split(p) ? timer_id : 0;
}

static int64_t gptu_t2_limit(pmb887x_gptu_t *p) {
	return gptu_t2_split(p) ? 0x10000LL : 0x100000000LL;
}

static uint32_t gptu_t2_ctrl_shift(pmb887x_gptu_t *p, int timer_id) {
	return gptu_t2_logical_id(p, timer_id) ? 16 : 0;
}

static uint32_t gptu_t2_csrc(pmb887x_gptu_t *p, int timer_id) {
	return (p->t2con >> (gptu_t2_ctrl_shift(p, timer_id) + GPTU_T2CON_T2ACSRC_SHIFT)) & 3;
}

static uint32_t gptu_t2_cdir(pmb887x_gptu_t *p, int timer_id) {
	return (p->t2con >> (gptu_t2_ctrl_shift(p, timer_id) + GPTU_T2CON_T2ACDIR_SHIFT)) & 3;
}

static uint32_t gptu_t2_cclr(pmb887x_gptu_t *p, int timer_id) {
	return (p->t2con >> (gptu_t2_ctrl_shift(p, timer_id) + GPTU_T2CON_T2ACCLR_SHIFT)) & 3;
}

static uint32_t gptu_t2_cov(pmb887x_gptu_t *p, int timer_id) {
	return (p->t2con >> (gptu_t2_ctrl_shift(p, timer_id) + GPTU_T2CON_T2ACOV_SHIFT)) & 3;
}

static bool gptu_t2_count_down(pmb887x_gptu_t *p, int timer_id) {
	uint32_t cdir = gptu_t2_cdir(p, timer_id);
	if (cdir == 1)
		return true;
	if (cdir == 2 || cdir == 3) {
		uint32_t dir_mask = gptu_t2_logical_id(p, timer_id) ?
			GPTU_T2CON_T2BDIR : GPTU_T2CON_T2ADIR;
		bool input_down = (p->t2con & dir_mask) != 0;
		return cdir == 2 ? input_down : !input_down;
	}
	return false;
}

static void gptu_t2_update_dir_status(pmb887x_gptu_t *p, int timer_id) {
	uint32_t dir_mask = gptu_t2_logical_id(p, timer_id) ?
		GPTU_T2CON_T2BDIR : GPTU_T2CON_T2ADIR;
	uint32_t cdir = gptu_t2_cdir(p, timer_id);

	if (cdir == 1) {
		p->t2con |= dir_mask;
	} else if (cdir == 0) {
		p->t2con &= ~dir_mask;
	}
}

static uint32_t gptu_t2_mrc(pmb887x_gptu_t *p, int timer_id, int rc_id) {
	int logical_id = gptu_t2_logical_id(p, timer_id);
	uint32_t shift = logical_id ?
		(rc_id ? GPTU_T2RCCON_T2BMRC1_SHIFT : GPTU_T2RCCON_T2BMRC0_SHIFT) :
		(rc_id ? GPTU_T2RCCON_T2AMRC1_SHIFT : GPTU_T2RCCON_T2AMRC0_SHIFT);
	return (p->t2rccon >> shift) & 7;
}

static uint32_t gptu_t2_rc_get(pmb887x_gptu_t *p, int timer_id, int rc_id) {
	uint32_t value = rc_id ? p->t2rc1 : p->t2rc0;
	if (!gptu_t2_split(p))
		return value;
	return timer_id == 0 ? (value & 0xFFFF) : (value >> 16);
}

static void gptu_t2_rc_set(pmb887x_gptu_t *p, int timer_id, int rc_id, uint32_t value) {
	uint32_t *reg = rc_id ? &p->t2rc1 : &p->t2rc0;
	if (!gptu_t2_split(p)) {
		*reg = value;
		return;
	}
	if (timer_id == 0) {
		*reg = (*reg & 0xFFFF0000) | (value & 0xFFFF);
	} else {
		*reg = (*reg & 0x0000FFFF) | ((value & 0xFFFF) << 16);
	}
}

static bool gptu_t2_reload_for_ouv(pmb887x_gptu_t *p, int timer_id,
	bool overflow, uint32_t *reload) {
	uint32_t mrc1 = gptu_t2_mrc(p, timer_id, 1);
	uint32_t mrc0 = gptu_t2_mrc(p, timer_id, 0);

	if (mrc1 == 4 || (!overflow && mrc1 == 6)) {
		*reload = gptu_t2_rc_get(p, timer_id, 1);
		return true;
	}

	if (mrc0 == 4 || (overflow && mrc0 == 6)) {
		*reload = gptu_t2_rc_get(p, timer_id, 0);
		return true;
	}

	return false;
}

static void gptu_t2_set_counter_value(pmb887x_gptu_t *p, int timer_id, uint32_t value) {
	pmb887x_gptu_timer_t2_t *timer = &p->timers_t2[timer_id];
	int64_t limit = gptu_t2_limit(p);
	timer->counter = value % limit;
	timer->start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void gptu_t2_apply_ouv(pmb887x_gptu_t *p, int timer_id, bool overflow, uint64_t count) {
	pmb887x_gptu_timer_t2_t *timer = &p->timers_t2[timer_id];
	int logical_id = gptu_t2_logical_id(p, timer_id);
	int event_id = gptu_t2_split(p) ? timer_id : 1;
	int ev_id = event_id ? EV_OUV_T2B : EV_OUV_T2A;
	int out_source = event_id ? 5 : 4;
	uint32_t reload = 0;

	gptu_trigger_ev_irq(p, ev_id);
	gptu_toggle_output_source(p, out_source, count);
	gptu_t01_external_count(p, event_id, count);

	if (gptu_t2_reload_for_ouv(p, timer_id, overflow, &reload)) {
		gptu_t2_set_counter_value(p, timer_id, reload);
	} else {
		int64_t limit = gptu_t2_limit(p);
		if (overflow) {
			timer->counter %= limit;
		} else if (timer->counter < 0) {
			timer->counter = limit - 1;
		}
	}

	if (timer->oneshot) {
		timer->stopped = true;
		if (logical_id) {
			p->t012run &= ~GPTU_T012RUN_T2BRUN;
		} else {
			p->t012run &= ~GPTU_T012RUN_T2ARUN;
		}
	}
}

static uint64_t gptu_t2_ticks_to_ouv(pmb887x_gptu_t *p, int timer_id) {
	pmb887x_gptu_timer_t2_t *timer = &p->timers_t2[timer_id];
	int64_t limit = gptu_t2_limit(p);
	uint32_t cov = gptu_t2_cov(p, timer_id);

	if (gptu_t2_count_down(p, timer_id)) {
		int64_t threshold = (cov & 2) ? 0 : -1;
		int64_t distance = timer->counter - threshold;
		if (distance <= 0)
			distance += limit;
		return distance;
	}

	int64_t threshold = limit - ((cov & 1) ? 1 : 0);
	int64_t distance = threshold - timer->counter;
	if (distance <= 0)
		distance += limit;
	return distance;
}

static void gptu_t2_advance_ticks(pmb887x_gptu_t *p, int timer_id, uint64_t ticks) {
	pmb887x_gptu_timer_t2_t *timer = &p->timers_t2[timer_id];

	if (!timer->enabled || timer->stopped || ticks == 0)
		return;

	while (ticks > 0 && !timer->stopped) {
		uint64_t distance = gptu_t2_ticks_to_ouv(p, timer_id);
		bool down = gptu_t2_count_down(p, timer_id);

		if (ticks < distance) {
			timer->counter += down ? -(int64_t) ticks : (int64_t) ticks;
			break;
		}

		timer->counter += down ? -(int64_t) distance : (int64_t) distance;
		ticks -= distance;
		gptu_t2_apply_ouv(p, timer_id, !down, 1);

		if (timer->stopped || ticks == 0)
			break;

		distance = gptu_t2_ticks_to_ouv(p, timer_id);
		if (distance > 0 && ticks >= distance) {
			uint64_t repeats = ticks / distance;
			ticks %= distance;
			timer->counter += down ? -(int64_t) (distance * repeats) :
				(int64_t) (distance * repeats);
			gptu_t2_apply_ouv(p, timer_id, !down, repeats);
		}
	}
}

static void gptu_t2_sync_timer(pmb887x_gptu_t *p) {
	if (p->syncing_t2)
		return;

	p->syncing_t2 = true;

	if (!p->enabled) {
		timer_del(p->timer_t2);
		p->syncing_t2 = false;
		return;
	}

	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	p->next_t2 = INT64_MAX;
	bool has_enabled = false;

	for (int i = 0; i < 2; i++) {
		if (!gptu_t2_split(p) && i == 0)
			continue;

		pmb887x_gptu_timer_t2_t *timer = &p->timers_t2[i];
		gptu_t2_update_dir_status(p, i);

		if (!timer->enabled || timer->stopped) {
			timer->start = 0;
			continue;
		}

		if (!timer->start)
			timer->start = now;

		if (gptu_t2_csrc(p, i) == 0) {
			uint64_t elapsed = muldiv64(now - timer->start, p->freq, NANOSECONDS_PER_SECOND);
			if (elapsed > 0) {
				int64_t start = timer->start;
				gptu_t2_advance_ticks(p, i, elapsed);
				timer->start = start + gptu_ticks_to_ns(p, elapsed);
			}

			if (!timer->stopped) {
				uint64_t ticks = gptu_t2_ticks_to_ouv(p, i);
				p->next_t2 = MIN(p->next_t2, timer->start + gptu_ticks_to_ns(p, ticks));
				has_enabled = true;
			}
		}
	}

	if (has_enabled) {
		timer_mod(p->timer_t2, p->next_t2);
	} else {
		timer_del(p->timer_t2);
	}

	p->syncing_t2 = false;
}

static void gptu_t2_update_state(pmb887x_gptu_t *p) {
	bool split = gptu_t2_split(p);

	for (int i = 0; i < 2; i++) {
		pmb887x_gptu_timer_t2_t *timer = &p->timers_t2[i];
		int logical_id = split ? i : 0;

		if (!split && i == 0) {
			timer->enabled = false;
			timer->start = 0;
			continue;
		}

		timer->counter %= split ? 0x10000LL : 0x100000000LL;
		timer->oneshot = logical_id ?
			((p->t2con & GPTU_T2CON_T2BCOS) != 0) :
			((p->t2con & GPTU_T2CON_T2ACOS) != 0);
		timer->enabled = logical_id ?
			((p->t012run & GPTU_T012RUN_T2BRUN) != 0) :
			((p->t012run & GPTU_T012RUN_T2ARUN) != 0);
		if (!timer->oneshot && timer->enabled)
			timer->stopped = false;
	}

	gptu_t2_sync_timer(p);
}

static uint32_t gptu_t2_get_counter(pmb887x_gptu_t *p) {
	gptu_t2_sync_timer(p);
	if (gptu_t2_split(p)) {
		return (p->timers_t2[0].counter & 0xFFFF) |
			((p->timers_t2[1].counter & 0xFFFF) << 16);
	}
	return p->timers_t2[1].counter;
}

static void gptu_t2_set_counter(pmb887x_gptu_t *p, uint32_t value) {
	gptu_t2_sync_timer(p);
	if ((p->t2con & GPTU_T2CON_T2SPLIT)) {
		p->timers_t2[0].counter = value & 0xFFFF;
		p->timers_t2[0].start = 0;
		p->timers_t2[1].counter = (value >> 16) & 0xFFFF;
		p->timers_t2[1].start = 0;
	} else {
		p->timers_t2[0].counter = value & 0xFFFF;
		p->timers_t2[0].start = 0;
		p->timers_t2[1].counter = value;
		p->timers_t2[1].start = 0;
	}
	p->timers_t2[0].stopped = false;
	p->timers_t2[1].stopped = false;
	gptu_t2_sync_timer(p);
}

static bool gptu_t2_input_matches_trigger(pmb887x_gptu_t *p, int timer_id, int is_shift, int edge_shift, int trigger_id) {
	bool split = gptu_t2_split(p);
	uint32_t is = (!split || timer_id == 0) ? p->t2ais : p->t2bis;
	uint32_t es_shift = edge_shift + ((split && timer_id == 1) ? 16 : 0);
	uint32_t edge = (p->t2es >> es_shift) & 3;
	uint32_t source = (is >> is_shift) & 7;

	return edge == 0 && (source & 3) == trigger_id;
}

static void gptu_t2_handle_rc_event(pmb887x_gptu_t *p, int timer_id, int rc_id) {
	pmb887x_gptu_timer_t2_t *timer = &p->timers_t2[timer_id];
	uint32_t mode = gptu_t2_mrc(p, timer_id, rc_id);
	bool down = gptu_t2_count_down(p, timer_id);
	bool reload = mode == 5 || (mode == 7 && (rc_id ? down : !down));
	bool capture = mode == 3;

	if (capture)
		gptu_t2_rc_set(p, timer_id, rc_id, timer->counter);

	if (reload)
		gptu_t2_set_counter_value(p, timer_id, gptu_t2_rc_get(p, timer_id, rc_id));

	if (capture && gptu_t2_cclr(p, timer_id) == (rc_id ? 2 : 1))
		gptu_t2_set_counter_value(p, timer_id, 0);
}

static void gptu_t2_external_trigger(pmb887x_gptu_t *p, int trigger_id, uint64_t count) {
	if (count == 0)
		return;

	gptu_t2_sync_timer(p);

	for (int i = 0; i < 2; i++) {
		if (!gptu_t2_split(p) && i == 0)
			continue;

		pmb887x_gptu_timer_t2_t *timer = &p->timers_t2[i];
		int logical_id = gptu_t2_logical_id(p, i);

		if (gptu_t2_input_matches_trigger(p, i, GPTU_T2AIS_T2AISTR_SHIFT, GPTU_T2ES_T2AESTR_SHIFT, trigger_id)) {
			if (logical_id) {
				p->t012run |= GPTU_T012RUN_T2BRUN;
			} else {
				p->t012run |= GPTU_T012RUN_T2ARUN;
			}
			timer->enabled = true;
			timer->stopped = false;
			timer->start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
			gptu_trigger_ev_irq(p, logical_id ? EV_START_B : EV_START_A);
		}

		if (gptu_t2_input_matches_trigger(p, i, GPTU_T2AIS_T2AISTP_SHIFT, GPTU_T2ES_T2AESTP_SHIFT, trigger_id)) {
			if (logical_id) {
				p->t012run &= ~GPTU_T012RUN_T2BRUN;
			} else {
				p->t012run &= ~GPTU_T012RUN_T2ARUN;
			}
			timer->enabled = false;
			timer->stopped = false;
			gptu_trigger_ev_irq(p, logical_id ? EV_STOP_B : EV_STOP_A);
		}

		if (gptu_t2_input_matches_trigger(p, i, GPTU_T2AIS_T2AIUD_SHIFT, GPTU_T2ES_T2AEUD_SHIFT, trigger_id)) {
			gptu_trigger_ev_irq(p, EV_UPDOWN_A);
		}

		if (gptu_t2_input_matches_trigger(p, i, GPTU_T2AIS_T2AICLR_SHIFT, GPTU_T2ES_T2AECLR_SHIFT, trigger_id)) {
			if (gptu_t2_cclr(p, i) == 0)
				gptu_t2_set_counter_value(p, i, 0);
			gptu_trigger_ev_irq(p, EV_CLEAR_A);
		}

		if (gptu_t2_input_matches_trigger(p, i, GPTU_T2AIS_T2AIRC0_SHIFT, GPTU_T2ES_T2AERC0_SHIFT, trigger_id)) {
			gptu_t2_handle_rc_event(p, i, 0);
			gptu_trigger_ev_irq(p, logical_id ? EV_RLCP0_B : EV_RLCP0_A);
		}

		if (gptu_t2_input_matches_trigger(p, i, GPTU_T2AIS_T2AIRC1_SHIFT, GPTU_T2ES_T2AERC1_SHIFT, trigger_id)) {
			gptu_t2_handle_rc_event(p, i, 1);
			gptu_trigger_ev_irq(p, logical_id ? EV_RLCP1_B : EV_RLCP1_A);
		}

		if (timer->enabled && !timer->stopped &&
			gptu_t2_csrc(p, i) != 0 &&
			gptu_t2_input_matches_trigger(p, i, GPTU_T2AIS_T2AICNT_SHIFT, GPTU_T2ES_T2AECNT_SHIFT, trigger_id)) {
			gptu_t2_advance_ticks(p, i, count);
		}
	}

	gptu_t2_update_state(p);
}

static void gptu_t2_ptimer_reset(void *opaque) {
	pmb887x_gptu_t *p = opaque;
	gptu_t2_sync_timer(p);
}

/*
 * T0/T1
 * */
static uint32_t gptu_t01_input(pmb887x_gptu_t *p, int timer_id) {
	return (p->t01irs >> (timer_id * 2)) & 3;
}

static bool gptu_t01_reload_own(pmb887x_gptu_t *p, int timer_id) {
	return ((p->t01irs >> (16 + timer_id)) & 1) == 0;
}

static int gptu_t01_reload_source(pmb887x_gptu_t *p, int timer_id) {
	if (gptu_t01_reload_own(p, timer_id))
		return timer_id;

	switch (timer_id) {
		case 0: case 1: case 2:
		case 4: case 5: case 6:
			return timer_id + 1;
		case 3:
			return 4;
		case 7:
			return 0;
		default:
			g_assert_not_reached();
	}
}

static int gptu_t01_carry_source(pmb887x_gptu_t *p, int timer_id) {
	if ((timer_id % 4) != 0)
		return timer_id - 1;
	if (timer_id == 0)
		return (p->t01irs & GPTU_T01IRS_T0INC) ? 7 : 3;
	return (p->t01irs & GPTU_T01IRS_T1INC) ? 3 : 7;
}

static uint64_t gptu_t01_ticks_to_overflow(pmb887x_gptu_t *p, int timer_id) {
	pmb887x_gptu_timer_t *timer = &p->timers[timer_id];
	return GPTU_OVERFLOW - (timer->counter & 0xFF);
}

static void gptu_t01_reload_event(pmb887x_gptu_t *p, int source_id, int depth);
static void gptu_t01_add_ticks(pmb887x_gptu_t *p, int timer_id, uint64_t ticks, int depth);

static void gptu_t01_fire_outputs(pmb887x_gptu_t *p, int timer_id, uint64_t count) {
	int group = timer_id / 4;
	int part = timer_id % 4;

	if (group == 0) {
		if (((p->t01ots & GPTU_T01OTS_SOUT00) >> GPTU_T01OTS_SOUT00_SHIFT) == part)
			gptu_toggle_output_source(p, 0, count);
		if (((p->t01ots & GPTU_T01OTS_SOUT01) >> GPTU_T01OTS_SOUT01_SHIFT) == part)
			gptu_toggle_output_source(p, 1, count);
		if (((p->t01ots & GPTU_T01OTS_STRG00) >> GPTU_T01OTS_STRG00_SHIFT) == part)
			gptu_t2_external_trigger(p, 0, count);
		if (((p->t01ots & GPTU_T01OTS_STRG01) >> GPTU_T01OTS_STRG01_SHIFT) == part)
			gptu_t2_external_trigger(p, 1, count);
	} else {
		if (((p->t01ots & GPTU_T01OTS_SOUT10) >> GPTU_T01OTS_SOUT10_SHIFT) == part)
			gptu_toggle_output_source(p, 2, count);
		if (((p->t01ots & GPTU_T01OTS_SOUT11) >> GPTU_T01OTS_SOUT11_SHIFT) == part)
			gptu_toggle_output_source(p, 3, count);
		if (((p->t01ots & GPTU_T01OTS_STRG10) >> GPTU_T01OTS_STRG10_SHIFT) == part)
			gptu_t2_external_trigger(p, 2, count);
		if (((p->t01ots & GPTU_T01OTS_STRG11) >> GPTU_T01OTS_STRG11_SHIFT) == part)
			gptu_t2_external_trigger(p, 3, count);
	}
}

static void gptu_t01_overflow_event(pmb887x_gptu_t *p, int timer_id, uint64_t count, int depth) {
	int timer_group = timer_id / 4;

	if (depth > 16 || count == 0)
		return;

	for (int j = 0; j < 2; j++) {
		if (p->timers[timer_id].ev_ssr[j] &&
			p->events_ssr[timer_group][j] == timer_id &&
			p->timers[timer_id].enabled) {
			gptu_trigger_ev_irq(p, gptu_get_ssr_ev(timer_group, j));
		}
	}

	gptu_t01_fire_outputs(p, timer_id, count);

	for (int i = 0; i < 8; i++) {
		if (gptu_t01_input(p, i) == INPUT_CONCAT &&
			gptu_t01_carry_source(p, i) == timer_id) {
			gptu_t01_add_ticks(p, i, count, depth + 1);
		}
	}
}

static void gptu_t01_reload_event(pmb887x_gptu_t *p, int source_id, int depth) {
	if (depth > 16)
		return;

	for (int i = 0; i < 8; i++) {
		if (!gptu_t01_reload_own(p, i) && gptu_t01_reload_source(p, i) == source_id) {
			p->timers[i].counter = p->timers[i].reload & 0xFF;
			gptu_t01_reload_event(p, i, depth + 1);
		}
	}
}

static void gptu_t01_add_ticks(pmb887x_gptu_t *p, int timer_id, uint64_t ticks, int depth) {
	pmb887x_gptu_timer_t *timer = &p->timers[timer_id];
	uint64_t counter;
	uint64_t overflows;

	if (depth > 16 || ticks == 0 || !timer->enabled)
		return;

	timer->counter &= 0xFF;
	counter = timer->counter + ticks;

	if (gptu_t01_reload_own(p, timer_id)) {
		uint64_t reload = timer->reload & 0xFF;
		uint64_t period = GPTU_OVERFLOW - reload;

		if (counter < GPTU_OVERFLOW) {
			timer->counter = counter;
			return;
		}

		overflows = 1 + ((counter - GPTU_OVERFLOW) / period);
		timer->counter = reload + ((counter - GPTU_OVERFLOW) % period);
	} else {
		overflows = counter / GPTU_OVERFLOW;
		timer->counter = counter % GPTU_OVERFLOW;
	}

	if (overflows) {
		gptu_t01_overflow_event(p, timer_id, overflows, depth + 1);
		if (gptu_t01_reload_own(p, timer_id))
			gptu_t01_reload_event(p, timer_id, depth + 1);
	}
}

static void gptu_t01_external_count(pmb887x_gptu_t *p, int cnt_id, uint64_t count) {
	uint32_t shift = cnt_id ? GPTU_T01IRS_T01IN1_SHIFT :
		GPTU_T01IRS_T01IN0_SHIFT;
	uint32_t source = (p->t01irs >> shift) & 3;

	if (count == 0 || source != 0)
		return;

	if (!p->syncing_t01)
		gptu_sync_timer(p);

	for (int i = 0; i < 8; i++) {
		if (gptu_t01_input(p, i) == (cnt_id ? INPUT_CNT1 : INPUT_CNT0))
			gptu_t01_add_ticks(p, i, count, 0);
	}
}

static void gptu_sync_timer(pmb887x_gptu_t *p) {
	if (p->syncing_t01)
		return;

	p->syncing_t01 = true;

	if (!p->enabled) {
		timer_del(p->timer);
		p->syncing_t01 = false;
		return;
	}

	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	p->next = INT64_MAX;
	bool has_enabled = false;

	for (int i = 0; i < 8; i++) {
		pmb887x_gptu_timer_t *timer = &p->timers[i];
		uint32_t input = gptu_t01_input(p, i);

		if (!timer->enabled || input != INPUT_BYPASS) {
			timer->start = 0;
			continue;
		}

		if (!timer->start)
			timer->start = now;

		uint64_t elapsed = muldiv64(now - timer->start, p->freq, NANOSECONDS_PER_SECOND);
		if (elapsed > 0) {
			gptu_t01_add_ticks(p, i, elapsed, 0);
			timer->start += gptu_ticks_to_ns(p, elapsed);
		}

		uint64_t ticks = gptu_t01_ticks_to_overflow(p, i);
		p->next = MIN(p->next, timer->start + gptu_ticks_to_ns(p, ticks));
		has_enabled = true;
	}

	if (has_enabled) {
		timer_mod(p->timer, p->next);
	} else {
		timer_del(p->timer);
	}

	p->syncing_t01 = false;
}

static uint32_t gptu_get_counter(pmb887x_gptu_t *p, int id, int size) {
	gptu_sync_timer(p);
	uint32_t value = 0;
	for (int i = 0; i < size; i++) {
		pmb887x_gptu_timer_t *timer = &p->timers[id * 4 + i];
		value |= (timer->counter & 0xFF) << (i * 8);
	}
	return value;
}

static void gptu_set_counter(pmb887x_gptu_t *p, int id, uint32_t value, int size) {
	gptu_sync_timer(p);
	for (int i = 0; i < size; i++) {
		pmb887x_gptu_timer_t *timer = &p->timers[id * 4 + i];
		timer->counter = (value >> (i * 8)) & 0xFF;
		timer->start = 0;
	}
	gptu_sync_timer(p);
}

static uint32_t gptu_get_reload(pmb887x_gptu_t *p, int id, int size) {
	uint32_t value = 0;
	for (int i = 0; i < size; i++) {
		pmb887x_gptu_timer_t *timer = &p->timers[id * 4 + i];
		value |= timer->reload << (i * 8);
	}
	return value;
}

static void gptu_set_reload(pmb887x_gptu_t *p, int id, uint32_t value, int size) {
	gptu_sync_timer(p);
	for (int i = 0; i < size; i++) {
		pmb887x_gptu_timer_t *timer = &p->timers[id * 4 + i];
		timer->reload = (value >> (i * 8)) & 0xFF;
	}
}

static void gptu_rebuild_timers(pmb887x_gptu_t *p) {
	for (int i = 0; i < 8; i++) {
		p->timers[i].id = i;
		p->timers[i].enabled = p->enabled && (p->t012run & (1 << i)) != 0;
		p->timers[i].concat = gptu_t01_input(p, i) == INPUT_CONCAT;
		p->timers[i].prev = gptu_t01_carry_source(p, i);
	}

	gptu_sync_timer(p);
}

static void gptu_ptimer_reset(void *opaque) {
	pmb887x_gptu_t *p = opaque;
	gptu_sync_timer(p);
}

static uint64_t gptu_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_gptu_t *p = opaque;

	uint64_t value = 0;

	switch (haddr) {
		case GPTU_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case GPTU_ID:
			value = 0x0001C011;
			break;

		case GPTU_T01IRS:
			value = p->t01irs;
			break;

		case GPTU_T01OTS:
			value = p->t01ots;
			break;

		case GPTU_T2CON:
			gptu_t2_sync_timer(p);
			value = p->t2con;
			break;

		case GPTU_T2RCCON:
			value = p->t2rccon;
			break;

		case GPTU_T2AIS:
			value = p->t2ais;
			break;

		case GPTU_T2BIS:
			value = p->t2bis;
			break;

		case GPTU_T2ES:
			value = p->t2es;
			break;

		case GPTU_OSEL:
			value = p->osel;
			break;

		case GPTU_OUT:
			gptu_sync_timer(p);
			gptu_t2_sync_timer(p);
			value = p->out & GPTU_OUT_MASK;
			break;

		case GPTU_T0DCBA:
			value = gptu_get_counter(p, 0, 4);
			break;

		case GPTU_T0CBA:
			value = gptu_get_counter(p, 0, 3);
			break;

		case GPTU_T0RDCBA:
			value = gptu_get_reload(p, 0, 4);
			break;

		case GPTU_T0RCBA:
			value = gptu_get_reload(p, 0, 3);
			break;

		case GPTU_T1DCBA:
			value = gptu_get_counter(p, 1, 4);
			break;

		case GPTU_T1CBA:
			value = gptu_get_counter(p, 1, 3);
			break;

		case GPTU_T1RDCBA:
			value = gptu_get_reload(p, 1, 4);
			break;

		case GPTU_T1RCBA:
			value = gptu_get_reload(p, 1, 3);
			break;

		case GPTU_T2:
			value = gptu_t2_get_counter(p);
			break;

		case GPTU_T2RC0:
			value = p->t2rc0;
			break;

		case GPTU_T2RC1:
			value = p->t2rc1;
			break;

		case GPTU_T012RUN:
			gptu_sync_timer(p);
			gptu_t2_sync_timer(p);
			value = p->t012run;
			value &= ~(GPTU_T012RUN_T2ARUN | GPTU_T012RUN_T2BRUN);

			if ((p->t2con & GPTU_T2CON_T2SPLIT)) {
				value |= (p->timers_t2[0].enabled && !p->timers_t2[0].stopped ? GPTU_T012RUN_T2ARUN : 0);
				value |= (p->timers_t2[1].enabled && !p->timers_t2[1].stopped ? GPTU_T012RUN_T2BRUN : 0);
			} else {
				value |= (p->timers_t2[1].enabled && !p->timers_t2[1].stopped ? GPTU_T012RUN_T2ARUN : 0);
			}
			break;

		case GPTU_SRSEL:
			value = p->srsel;
			break;

		case GPTU_SRC0:
		case GPTU_SRC1:
		case GPTU_SRC2:
		case GPTU_SRC3:
		case GPTU_SRC4:
		case GPTU_SRC5:
		case GPTU_SRC6:
		case GPTU_SRC7:
			gptu_sync_timer(p);
			gptu_t2_sync_timer(p);
			value = pmb887x_src_get(&p->src[(haddr - GPTU_SRC0) / 4]);
			break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}

	IO_DUMP(haddr + p->mmio.addr, size, value, false);

	return value;
}

static void gptu_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_gptu_t *p = opaque;

	IO_DUMP(haddr + p->mmio.addr, size, value, true);

	switch (haddr) {
		case GPTU_CLC:
			pmb887x_clc_set(&p->clc, value);
			gptu_sync_timer(p);
			gptu_update_freq(p);
			gptu_rebuild_timers(p);
			gptu_t2_sync_timer(p);
			gptu_t2_update_state(p);
			break;

		case GPTU_T01IRS:
			p->t01irs = value;
			gptu_sync_timer(p);
			gptu_rebuild_timers(p);
			break;

		case GPTU_T01OTS:
			p->t01ots = value;
			gptu_update_events(p);
			break;

		case GPTU_T2CON:
			gptu_t2_sync_timer(p);
			p->t2con = value;
			gptu_t2_update_state(p);
			break;

		case GPTU_T2RCCON:
			gptu_t2_sync_timer(p);
			p->t2rccon = value;
			gptu_t2_update_state(p);
			break;

		case GPTU_T2AIS:
			gptu_t2_sync_timer(p);
			p->t2ais = value;
			break;

		case GPTU_T2BIS:
			gptu_t2_sync_timer(p);
			p->t2bis = value;
			break;

		case GPTU_T2ES:
			gptu_t2_sync_timer(p);
			p->t2es = value;
			break;

		case GPTU_OSEL:
			p->osel = value;
			break;

		case GPTU_OUT:
			for (int i = 0; i < 8; i++) {
				bool set = (value & (GPTU_OUT_SETO0 << i)) != 0;
				bool clear = (value & (GPTU_OUT_CLRO0 << i)) != 0;
				if (set && !clear) {
					p->out |= (1 << i);
				} else if (clear && !set) {
					p->out &= ~(1 << i);
				}
			}
			p->out &= GPTU_OUT_MASK;
			break;

		case GPTU_T0DCBA:
			gptu_set_counter(p, 0, value, 4);
			break;

		case GPTU_T0CBA:
			gptu_set_counter(p, 0, value, 3);
			break;

		case GPTU_T0RDCBA:
			gptu_set_reload(p, 0, value, 4);
			break;

		case GPTU_T0RCBA:
			gptu_set_reload(p, 0, value, 3);
			break;

		case GPTU_T1DCBA:
			gptu_set_counter(p, 1, value, 4);
			break;

		case GPTU_T1CBA:
			gptu_set_counter(p, 1, value, 3);
			break;

		case GPTU_T1RDCBA:
			gptu_set_reload(p, 1, value, 4);
			break;

		case GPTU_T1RCBA:
			gptu_set_reload(p, 1, value, 3);
			break;

		case GPTU_T2:
			gptu_t2_set_counter(p, value);
			break;

		case GPTU_T2RC0:
			p->t2rc0 = value;
			break;

		case GPTU_T2RC1:
			p->t2rc1 = value;
			break;

		case GPTU_T012RUN:
			gptu_sync_timer(p);
			gptu_t2_sync_timer(p);

			uint32_t t012run = (p->t012run &
				(GPTU_T012RUN_T2ARUN | GPTU_T012RUN_T2BRUN)) |
				(value & 0xFF);

			bool t2a_set = (value & GPTU_T012RUN_T2ASETR) != 0;
			bool t2a_clear = (value & GPTU_T012RUN_T2ACLRR) != 0;
			bool t2b_set = (value & GPTU_T012RUN_T2BSETR) != 0;
			bool t2b_clear = (value & GPTU_T012RUN_T2BCLRR) != 0;

			if (t2a_set && !t2a_clear) {
				t012run |= GPTU_T012RUN_T2ARUN;
				p->timers_t2[gptu_t2_split(p) ? 0 : 1].stopped = false;
			} else if (t2a_clear && !t2a_set) {
				t012run &= ~GPTU_T012RUN_T2ARUN;
			}

			if (t2b_set && !t2b_clear) {
				t012run |= GPTU_T012RUN_T2BRUN;
				p->timers_t2[1].stopped = false;
			} else if (t2b_clear && !t2b_set) {
				t012run &= ~GPTU_T012RUN_T2BRUN;
			}

			p->t012run = t012run;
			gptu_rebuild_timers(p);
			gptu_t2_update_state(p);
			break;

		case GPTU_SRSEL:
			p->srsel = value;
			gptu_update_events(p);
			break;

		case GPTU_SRC0:
		case GPTU_SRC1:
		case GPTU_SRC2:
		case GPTU_SRC3:
		case GPTU_SRC4:
		case GPTU_SRC5:
		case GPTU_SRC6:
		case GPTU_SRC7:
			pmb887x_src_update(&p->src[(haddr - GPTU_SRC0) / 4], 0, value);
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
}

static const MemoryRegionOps io_ops = {
	.read			= gptu_io_read,
	.write			= gptu_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 4,
		.max_access_size	= 4
	}
};

static void gptu_init(Object *obj) {
	pmb887x_gptu_t *p = PMB887X_GPTU(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-gptu", GPTU_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[7]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[6]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[5]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[4]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[3]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[2]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[1]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[0]);
}

static void gptu_realize(DeviceState *dev, Error **errp) {
	pmb887x_gptu_t *p = PMB887X_GPTU(dev);

	if (!p->pll)
		hw_error("PLL not found...");

	pmb887x_clc_init(&p->clc);

	for (int i = 0; i < ARRAY_SIZE(p->src); i++) {
		if (!p->irq[i])
			hw_error("pmb887x-gptu: irq %d not set", i);
		pmb887x_src_init(&p->src[i], p->irq[i]);
	}

	p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gptu_ptimer_reset, p);
	p->timer_t2 = timer_new_ns(QEMU_CLOCK_VIRTUAL, gptu_t2_ptimer_reset, p);

	gptu_update_freq(p);
	gptu_update_events(p);
	gptu_rebuild_timers(p);
	gptu_sync_timer(p);
	gptu_t2_update_state(p);
	gptu_t2_sync_timer(p);
}

static const Property gptu_properties[] = {
	DEFINE_PROP_LINK("pll", pmb887x_gptu_t, pll, "pmb887x-pll", struct pmb887x_pll_t *),
};

static void gptu_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, gptu_properties);
	dc->realize = gptu_realize;
}

static const TypeInfo gptu_info = {
	.name          	= TYPE_PMB887X_GPTU,
	.parent        	= TYPE_SYS_BUS_DEVICE,
	.instance_size 	= sizeof(pmb887x_gptu_t),
	.instance_init 	= gptu_init,
	.class_init    	= gptu_class_init,
};

static void gptu_register_types(void) {
	type_register_static(&gptu_info);
}
type_init(gptu_register_types)
