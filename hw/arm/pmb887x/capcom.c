/*
 * Capture/Compare
 */
#define PMB887X_TRACE_ID		CAPCOM
#define PMB887X_TRACE_PREFIX	"pmb887x-capcom"

#include "qemu/osdep.h"
#include "hw/core/hw-error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "system/memory.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_CAPCOM	"pmb887x-capcom"
#define PMB887X_CAPCOM(obj)	OBJECT_CHECK(pmb887x_capcom_t, (obj), TYPE_PMB887X_CAPCOM)

#define CAPCOM_TIMER_COUNT	2
#define CAPCOM_CHANNEL_COUNT	8

typedef struct pmb887x_capcom_t pmb887x_capcom_t;
typedef struct pmb887x_capcom_timer_t pmb887x_capcom_timer_t;
typedef struct pmb887x_capcom_pwm_t pmb887x_capcom_pwm_t;
typedef enum pmb887x_capcom_cc_mode_t pmb887x_capcom_cc_mode_t;

struct pmb887x_capcom_timer_t {
	uint16_t value;
	uint16_t reload;
	uint32_t frequency_hz;
	uint64_t fractional_tick;
	int64_t last_update_ns;
	bool is_running;
};

struct pmb887x_capcom_pwm_t {
	uint64_t frequency_millihz;
	uint16_t duty_per_mille;
	bool is_enabled;
};

struct pmb887x_capcom_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	uint32_t revision;
	QEMUTimer *event_timer;

	qemu_irq timer_irq[CAPCOM_TIMER_COUNT];
	qemu_irq channel_irq[CAPCOM_CHANNEL_COUNT];
	pmb887x_src_reg_t timer_src[CAPCOM_TIMER_COUNT];
	pmb887x_src_reg_t channel_src[CAPCOM_CHANNEL_COUNT];
	pmb887x_clc_reg_t clc;

	pmb887x_capcom_timer_t timers[CAPCOM_TIMER_COUNT];
	pmb887x_capcom_pwm_t pwm[CAPCOM_CHANNEL_COUNT];
	uint16_t compare[CAPCOM_CHANNEL_COUNT];
	bool period_fired[CAPCOM_CHANNEL_COUNT];
	bool input_level[CAPCOM_CHANNEL_COUNT];

	uint32_t pisel;
	uint32_t t01con;
	uint32_t ccm[2];
	uint32_t out;
	uint32_t ioc;
	uint32_t sem;
	uint32_t see;
	uint32_t drm;
	uint32_t t01ocr;
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

static pmb887x_capcom_cc_mode_t capcom_channel_mode(const pmb887x_capcom_t *p, unsigned channel) {
	unsigned shift = (channel % 4) * 4;
	return (p->ccm[channel / 4] >> shift) & 0x7;
}

static unsigned capcom_channel_timer(const pmb887x_capcom_t *p, unsigned channel) {
	unsigned shift = (channel % 4) * 4;
	return (p->ccm[channel / 4] >> (shift + 3)) & 0x1;
}

static bool capcom_compare_is_enabled(const pmb887x_capcom_t *p, unsigned channel) {
	pmb887x_capcom_cc_mode_t mode = capcom_channel_mode(p, channel);

	if (mode < CAPCOM_CC_MODE_0)
		return false;
	if (mode >= CAPCOM_CC_MODE_2 && p->period_fired[channel])
		return false;
	if ((p->sem & BIT(channel)) && !(p->see & BIT(channel)))
		return false;

	return true;
}

static bool capcom_double_compare_is_enabled(const pmb887x_capcom_t *p, unsigned pair) {
	uint32_t drm = (p->drm >> (pair * 2)) & 0x3;

	if (drm == CAPCOM_DRM_DR0M_EN)
		return true;
	if (drm != CAPCOM_DRM_DR0M_CON)
		return false;

	return capcom_channel_mode(p, pair) == CAPCOM_CC_MODE_1 &&
		capcom_channel_mode(p, pair + 4) == CAPCOM_CC_MODE_0;
}

static void capcom_output_set(pmb887x_capcom_t *p, uint32_t value) {
	value &= 0xFF;
	if (value == p->out)
		return;

	p->out = value;
}

static void capcom_pwm_stub_configure(
	pmb887x_capcom_t *p,
	unsigned channel,
	bool is_enabled,
	uint64_t frequency_millihz,
	uint16_t duty_per_mille
) {
	pmb887x_capcom_pwm_t *pwm = &p->pwm[channel];
	bool unchanged = pwm->is_enabled == is_enabled &&
		pwm->frequency_millihz == frequency_millihz &&
		pwm->duty_per_mille == duty_per_mille;

	if (unchanged)
		return;

	pwm->is_enabled = is_enabled;
	pwm->frequency_millihz = frequency_millihz;
	pwm->duty_per_mille = duty_per_mille;

	if (is_enabled) {
		DPRINTF("%08" PRIX64 " CC%uIO PWM configured: %" PRIu64 ".%03" PRIu64 " Hz, %u.%u%% duty\n",
			(uint64_t) p->mmio.addr, channel, frequency_millihz / 1000, frequency_millihz % 1000,
			duty_per_mille / 10, duty_per_mille % 10);
	} else {
		DPRINTF("%08" PRIX64 " CC%uIO PWM disabled\n", (uint64_t) p->mmio.addr, channel);
	}
}

static void capcom_pwm_update(pmb887x_capcom_t *p, unsigned channel) {
	unsigned timer_index = capcom_channel_timer(p, channel);
	const pmb887x_capcom_timer_t *timer = &p->timers[timer_index];
	bool is_enabled = capcom_channel_mode(p, channel) == CAPCOM_CC_MODE_3 &&
		timer->is_running && !(p->sem & BIT(channel)) && p->compare[channel] > timer->reload;
	if (!is_enabled) {
		capcom_pwm_stub_configure(p, channel, false, 0, 0);
		return;
	}

	uint32_t period_ticks = (1U << 16) - timer->reload;
	uint32_t high_ticks = (1U << 16) - p->compare[channel];
	uint64_t scaled_frequency = (uint64_t) timer->frequency_hz * 1000;
	uint64_t frequency_millihz = (scaled_frequency + period_ticks / 2) / period_ticks;
	uint16_t duty_per_mille = (high_ticks * 1000 + period_ticks / 2) / period_ticks;
	capcom_pwm_stub_configure(p, channel, true, frequency_millihz, duty_per_mille);
}

static void capcom_pwm_outputs_update(pmb887x_capcom_t *p) {
	for (unsigned channel = 0; channel < CAPCOM_CHANNEL_COUNT; channel++)
		capcom_pwm_update(p, channel);
}

static void capcom_timer_period_start(pmb887x_capcom_t *p, unsigned timer_index) {
	for (unsigned channel = 0; channel < CAPCOM_CHANNEL_COUNT; channel++) {
		if (capcom_channel_timer(p, channel) == timer_index)
			p->period_fired[channel] = false;
	}
}

static uint32_t capcom_matching_channels(const pmb887x_capcom_t *p, unsigned timer_index) {
	uint32_t matches = 0;
	uint16_t timer_value = p->timers[timer_index].value;

	for (unsigned channel = 0; channel < CAPCOM_CHANNEL_COUNT; channel++) {
		if (capcom_channel_timer(p, channel) != timer_index)
			continue;
		if (p->compare[channel] != timer_value)
			continue;
		if (!capcom_compare_is_enabled(p, channel))
			continue;

		matches |= BIT(channel);
	}

	return matches;
}

static void capcom_timer_event(pmb887x_capcom_t *p, unsigned timer_index, bool overflow) {
	if (overflow) {
		pmb887x_src_update(&p->timer_src[timer_index], 0, MOD_SRC_SETR);
		capcom_timer_period_start(p, timer_index);
	}

	uint32_t matches = capcom_matching_channels(p, timer_index);
	for (unsigned channel = 0; channel < CAPCOM_CHANNEL_COUNT; channel++) {
		if (!(matches & BIT(channel)))
			continue;

		pmb887x_src_update(&p->channel_src[channel], 0, MOD_SRC_SETR);
		if (capcom_channel_mode(p, channel) >= CAPCOM_CC_MODE_2)
			p->period_fired[channel] = true;
		if (p->sem & BIT(channel))
			p->see &= ~BIT(channel);
	}

	uint32_t output = p->out;
	if (overflow) {
		for (unsigned channel = 0; channel < CAPCOM_CHANNEL_COUNT; channel++) {
			bool reload_match = (matches & BIT(channel)) != 0;
			if (capcom_channel_timer(p, channel) == timer_index &&
				capcom_channel_mode(p, channel) == CAPCOM_CC_MODE_3 && !reload_match)
				output &= ~BIT(channel);
		}
	}

	uint32_t double_channels = 0;
	for (unsigned pair = 0; pair < CAPCOM_CHANNEL_COUNT / 2; pair++) {
		uint32_t pair_channels = BIT(pair) | BIT(pair + 4);
		if (!(matches & pair_channels))
			continue;
		if (!capcom_double_compare_is_enabled(p, pair))
			continue;

		output ^= BIT(pair);
		double_channels |= pair_channels;
	}

	for (unsigned channel = 0; channel < CAPCOM_CHANNEL_COUNT; channel++) {
		if (!(matches & BIT(channel)) || (double_channels & BIT(channel)))
			continue;

		switch (capcom_channel_mode(p, channel)) {
			case CAPCOM_CC_MODE_1:
				output ^= BIT(channel);
				break;
			case CAPCOM_CC_MODE_3:
				if (!overflow)
					output |= BIT(channel);
				break;
			default:
				break;
		}
	}

	capcom_output_set(p, output);
}

static uint32_t capcom_timer_next_event_ticks(const pmb887x_capcom_t *p, unsigned timer_index) {
	const pmb887x_capcom_timer_t *timer = &p->timers[timer_index];
	uint32_t ticks = (1U << 16) - timer->value;

	for (unsigned channel = 0; channel < CAPCOM_CHANNEL_COUNT; channel++) {
		if (capcom_channel_timer(p, channel) != timer_index)
			continue;
		if (!capcom_compare_is_enabled(p, channel))
			continue;
		if (p->compare[channel] <= timer->value)
			continue;

		ticks = MIN(ticks, p->compare[channel] - timer->value);
	}

	return ticks;
}

static void capcom_timer_advance(pmb887x_capcom_t *p, unsigned timer_index, uint64_t ticks) {
	pmb887x_capcom_timer_t *timer = &p->timers[timer_index];

	while (ticks) {
		uint32_t overflow_ticks = (1U << 16) - timer->value;
		uint32_t event_ticks = capcom_timer_next_event_ticks(p, timer_index);
		if (ticks < event_ticks) {
			timer->value += ticks;
			return;
		}

		bool overflow = event_ticks == overflow_ticks;
		if (overflow) {
			timer->value = timer->reload;
		} else {
			timer->value += event_ticks;
		}
		ticks -= event_ticks;
		capcom_timer_event(p, timer_index, overflow);
	}
}

static void capcom_timer_sync(pmb887x_capcom_t *p, unsigned timer_index, int64_t now_ns) {
	pmb887x_capcom_timer_t *timer = &p->timers[timer_index];
	if (!timer->is_running) {
		timer->last_update_ns = now_ns;
		return;
	}

	uint64_t elapsed_ns = now_ns - timer->last_update_ns;
	uint64_t elapsed_seconds = elapsed_ns / NANOSECONDS_PER_SECOND;
	uint64_t remaining_ns = elapsed_ns % NANOSECONDS_PER_SECOND;
	uint64_t partial_ticks = remaining_ns * timer->frequency_hz + timer->fractional_tick;
	uint64_t ticks = elapsed_seconds * timer->frequency_hz + partial_ticks / NANOSECONDS_PER_SECOND;
	timer->fractional_tick = partial_ticks % NANOSECONDS_PER_SECOND;
	timer->last_update_ns = now_ns;

	capcom_timer_advance(p, timer_index, ticks);
}

static void capcom_sync(pmb887x_capcom_t *p) {
	int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

	for (unsigned timer_index = 0; timer_index < CAPCOM_TIMER_COUNT; timer_index++)
		capcom_timer_sync(p, timer_index, now_ns);
}

static void capcom_timer_state_update(pmb887x_capcom_t *p, unsigned timer_index, int64_t now_ns) {
	pmb887x_capcom_timer_t *timer = &p->timers[timer_index];
	unsigned control_shift = timer_index * 8;
	unsigned input_shift = CAPCOM_T01CON_T0I_SHIFT + control_shift;
	uint32_t input = (p->t01con >> input_shift) & 0x7;
	bool timer_mode = !(p->t01con & (CAPCOM_T01CON_T0M << control_shift));
	bool run_requested = (p->t01con & (CAPCOM_T01CON_T0R << control_shift)) != 0;
	unsigned divider_shift = input + ((p->ioc & CAPCOM_IOC_STAG) ? 0 : 3);
	uint32_t frequency_hz = pmb887x_clc_get_hz(&p->clc) >> divider_shift;
	bool is_running = timer_mode && run_requested && frequency_hz != 0;

	if (timer->frequency_hz == frequency_hz && timer->is_running == is_running)
		return;

	timer->frequency_hz = frequency_hz;
	timer->fractional_tick = 0;
	timer->last_update_ns = now_ns;
	timer->is_running = is_running;
}

static bool capcom_timer_needs_wakeup(const pmb887x_capcom_t *p, unsigned timer_index) {
	uint32_t timer_src = p->timer_src[timer_index].value;
	if (!(timer_src & MOD_SRC_SRR))
		return true;

	for (unsigned channel = 0; channel < CAPCOM_CHANNEL_COUNT; channel++) {
		if (capcom_channel_timer(p, channel) != timer_index)
			continue;
		if (capcom_channel_mode(p, channel) < CAPCOM_CC_MODE_0)
			continue;
		if ((p->sem & BIT(channel)) && !(p->see & BIT(channel)))
			continue;

		uint32_t channel_src = p->channel_src[channel].value;
		if (!(channel_src & MOD_SRC_SRR))
			return true;
	}

	return false;
}

static uint64_t capcom_timer_ticks_to_ns(const pmb887x_capcom_timer_t *timer, uint32_t ticks) {
	uint64_t scaled_ticks = (uint64_t) ticks * NANOSECONDS_PER_SECOND;
	uint64_t remaining = scaled_ticks - timer->fractional_tick;

	return DIV_ROUND_UP(remaining, timer->frequency_hz);
}

static void capcom_schedule(pmb887x_capcom_t *p) {
	int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	int64_t deadline_ns = -1;

	timer_del(p->event_timer);
	for (unsigned timer_index = 0; timer_index < CAPCOM_TIMER_COUNT; timer_index++) {
		pmb887x_capcom_timer_t *timer = &p->timers[timer_index];
		if (!timer->is_running)
			continue;
		if (!capcom_timer_needs_wakeup(p, timer_index))
			continue;

		uint32_t ticks = capcom_timer_next_event_ticks(p, timer_index);
		int64_t timer_deadline_ns = now_ns + MAX(1ULL, capcom_timer_ticks_to_ns(timer, ticks));
		if (deadline_ns < 0 || timer_deadline_ns < deadline_ns)
			deadline_ns = timer_deadline_ns;
	}

	if (deadline_ns >= 0)
		timer_mod_ns(p->event_timer, deadline_ns);
}

static void capcom_state_update(pmb887x_capcom_t *p) {
	int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

	for (unsigned timer_index = 0; timer_index < CAPCOM_TIMER_COUNT; timer_index++)
		capcom_timer_state_update(p, timer_index, now_ns);
	capcom_pwm_outputs_update(p);
	capcom_schedule(p);
}

static void capcom_event_timer_expired(void *opaque) {
	pmb887x_capcom_t *p = opaque;
	capcom_sync(p);
	capcom_schedule(p);
}

static void capcom_clock_update(void *opaque) {
	pmb887x_capcom_t *p = opaque;
	capcom_sync(p);
	capcom_state_update(p);
}

static int capcom_channel_from_src(hwaddr offset) {
	if (offset < CAPCOM_CC7_SRC || offset > CAPCOM_CC0_SRC)
		return -1;
	if ((offset - CAPCOM_CC7_SRC) % sizeof(uint32_t))
		return -1;

	return (CAPCOM_CC0_SRC - offset) / sizeof(uint32_t);
}

static int capcom_channel_from_cc(hwaddr offset) {
	if (offset < CAPCOM_CC0 || offset > CAPCOM_CC7)
		return -1;
	if ((offset - CAPCOM_CC0) % sizeof(uint32_t))
		return -1;

	return (offset - CAPCOM_CC0) / sizeof(uint32_t);
}

static uint32_t capcom_register_read(pmb887x_capcom_t *p, hwaddr offset) {
	int channel = capcom_channel_from_cc(offset);
	if (channel >= 0)
		return p->compare[channel];

	channel = capcom_channel_from_src(offset);
	if (channel >= 0)
		return pmb887x_src_get(&p->channel_src[channel]);

	switch (offset) {
		case CAPCOM_CLC:
			return pmb887x_clc_get(&p->clc);
		case CAPCOM_PISEL:
			return p->pisel;
		case CAPCOM_ID:
			return 0x00005000 | p->revision;
		case CAPCOM_T01CON:
			return p->t01con;
		case CAPCOM_CCM0:
			return p->ccm[0];
		case CAPCOM_CCM1:
			return p->ccm[1];
		case CAPCOM_OUT:
			return p->out;
		case CAPCOM_IOC:
			return p->ioc;
		case CAPCOM_SEM:
			return p->sem;
		case CAPCOM_SEE:
			return p->see;
		case CAPCOM_DRM:
			return p->drm;
		case CAPCOM_WHBSSEE:
		case CAPCOM_WHBCSEE:
		case CAPCOM_WHBSOUT:
		case CAPCOM_WHBCOUT:
			return 0;
		case CAPCOM_T0:
			return p->timers[0].value;
		case CAPCOM_T0REL:
			return p->timers[0].reload;
		case CAPCOM_T1:
			return p->timers[1].value;
		case CAPCOM_T1REL:
			return p->timers[1].reload;
		case CAPCOM_T01OCR:
			return p->t01ocr;
		case CAPCOM_T1_SRC:
			return pmb887x_src_get(&p->timer_src[1]);
		case CAPCOM_T0_SRC:
			return pmb887x_src_get(&p->timer_src[0]);
		default:
			EPRINTF("unknown reg read: %02" PRIX64 "\n", offset);
			return 0;
	}
}

static uint64_t capcom_io_read(void *opaque, hwaddr offset, unsigned size) {
	pmb887x_capcom_t *p = opaque;

	capcom_sync(p);
	uint32_t value = capcom_register_read(p, offset);

	capcom_schedule(p);
	IO_DUMP_READ(offset + p->mmio.addr, size, value);
	return value;
}

static void capcom_ccm_set(pmb887x_capcom_t *p, unsigned index, uint32_t value) {
	uint32_t old_value = p->ccm[index];
	p->ccm[index] = value & 0xFFFF;

	for (unsigned local_channel = 0; local_channel < 4; local_channel++) {
		unsigned shift = local_channel * 4;
		if (((old_value ^ p->ccm[index]) & (0xF << shift)) != 0)
			p->period_fired[index * 4 + local_channel] = false;
	}
}

static void capcom_timer_value_set(pmb887x_capcom_t *p, unsigned timer_index, uint32_t value) {
	pmb887x_capcom_timer_t *timer = &p->timers[timer_index];

	timer->value = value & 0xFFFF;
	timer->fractional_tick = 0;
	timer->last_update_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void capcom_register_write(pmb887x_capcom_t *p, hwaddr offset, uint32_t value) {
	int channel = capcom_channel_from_cc(offset);
	if (channel >= 0) {
		p->compare[channel] = value & 0xFFFF;
		return;
	}

	channel = capcom_channel_from_src(offset);
	if (channel >= 0) {
		pmb887x_src_set(&p->channel_src[channel], value);
		return;
	}

	switch (offset) {
		case CAPCOM_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;
		case CAPCOM_PISEL:
			p->pisel = value & 0x3F;
			break;
		case CAPCOM_ID:
			break;
		case CAPCOM_T01CON:
			p->t01con = value & (CAPCOM_T01CON_T0I | CAPCOM_T01CON_T0M | CAPCOM_T01CON_T0R |
				CAPCOM_T01CON_T1I | CAPCOM_T01CON_T1M | CAPCOM_T01CON_T1R);
			break;
		case CAPCOM_CCM0:
			capcom_ccm_set(p, 0, value);
			break;
		case CAPCOM_CCM1:
			capcom_ccm_set(p, 1, value);
			break;
		case CAPCOM_OUT:
			capcom_output_set(p, value);
			break;
		case CAPCOM_IOC:
			p->ioc = BIT(0) | (value & (CAPCOM_IOC_PL | CAPCOM_IOC_STAG | CAPCOM_IOC_PDS));
			break;
		case CAPCOM_SEM:
			p->sem = value & 0xFF;
			break;
		case CAPCOM_SEE:
			p->see = value & 0xFF;
			break;
		case CAPCOM_DRM:
			p->drm = value & 0xFF;
			break;
		case CAPCOM_WHBSSEE:
			p->see |= (value & 0xFF);
			break;
		case CAPCOM_WHBCSEE:
			p->see &= ~(value & 0xFF);
			break;
		case CAPCOM_T0:
			capcom_timer_value_set(p, 0, value);
			break;
		case CAPCOM_T0REL:
			p->timers[0].reload = value & 0xFFFF;
			break;
		case CAPCOM_T1:
			capcom_timer_value_set(p, 1, value);
			break;
		case CAPCOM_T1REL:
			p->timers[1].reload = value & 0xFFFF;
			break;
		case CAPCOM_T01OCR:
			p->t01ocr = value & (CAPCOM_T01OCR_CT0 | CAPCOM_T01OCR_CT1);
			break;
		case CAPCOM_WHBSOUT:
			capcom_output_set(p, p->out | value);
			break;
		case CAPCOM_WHBCOUT:
			capcom_output_set(p, p->out & ~value);
			break;
		case CAPCOM_T1_SRC:
			pmb887x_src_set(&p->timer_src[1], value);
			break;
		case CAPCOM_T0_SRC:
			pmb887x_src_set(&p->timer_src[0], value);
			break;
		default:
			EPRINTF("unknown reg write: %02" PRIX64 " = %08" PRIX32 "\n", offset, value);
			break;
	}
}

static void capcom_io_write(void *opaque, hwaddr offset, uint64_t value, unsigned size) {
	pmb887x_capcom_t *p = opaque;

	IO_DUMP_WRITE(offset + p->mmio.addr, size, value);
	capcom_sync(p);
	capcom_register_write(p, offset, (uint32_t) value);
	capcom_state_update(p);
}

static void capcom_handle_input_change(pmb887x_capcom_t *p, unsigned channel, int level) {
	bool input_level = level != 0;
	bool previous_level = p->input_level[channel];
	p->input_level[channel] = input_level;
	if (input_level == previous_level)
		return;

	pmb887x_capcom_cc_mode_t mode = capcom_channel_mode(p, channel);
	bool rising_edge = !previous_level && input_level;
	bool falling_edge = previous_level && !input_level;
	bool capture = (mode == CAPCOM_CC_MODE_RISING_EDGE && rising_edge) ||
		(mode == CAPCOM_CC_MODE_FALLING_EDGE && falling_edge) ||
		mode == CAPCOM_CC_MODE_BOTH_EDGES;
	if (!capture)
		return;

	capcom_sync(p);
	unsigned timer_index = capcom_channel_timer(p, channel);
	p->compare[channel] = p->timers[timer_index].value;
	pmb887x_src_update(&p->channel_src[channel], 0, MOD_SRC_SETR);
	DPRINTF("CC%u captured T%u=%04X on %s edge\n", channel, timer_index, p->compare[channel],
		rising_edge ? "rising" : "falling");
	capcom_schedule(p);
}

static void capcom_input_cc0_handler(void *opaque, int _id, int level) {
	capcom_handle_input_change(opaque, 0, level);
}

static void capcom_input_cc1_handler(void *opaque, int _id, int level) {
	capcom_handle_input_change(opaque, 1, level);
}

static void capcom_input_cc2_handler(void *opaque, int _id, int level) {
	capcom_handle_input_change(opaque, 2, level);
}

static void capcom_input_cc3_handler(void *opaque, int _id, int level) {
	capcom_handle_input_change(opaque, 3, level);
}

static void capcom_input_cc4_handler(void *opaque, int _id, int level) {
	capcom_handle_input_change(opaque, 4, level);
}

static void capcom_input_cc5_handler(void *opaque, int _id, int level) {
	capcom_handle_input_change(opaque, 5, level);
}

static void capcom_input_cc6_handler(void *opaque, int _id, int level) {
	capcom_handle_input_change(opaque, 6, level);
}

static void capcom_input_cc7_handler(void *opaque, int _id, int level) {
	capcom_handle_input_change(opaque, 7, level);
}

static const MemoryRegionOps capcom_io_ops = {
	.read = capcom_io_read,
	.write = capcom_io_write,
	.endianness = DEVICE_NATIVE_ENDIAN,
	.valid = {
		.min_access_size = 4,
		.max_access_size = 4,
	},
};

static void capcom_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_capcom_t *p = PMB887X_CAPCOM(obj);
	pmb887x_clc_init(&p->clc, dev);
	pmb887x_clc_set_callback(&p->clc, capcom_clock_update, p);
	p->event_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, capcom_event_timer_expired, p);
	memory_region_init_io(&p->mmio, obj, &capcom_io_ops, p, "pmb887x-capcom", CAPCOM_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	for (unsigned index = 0; index < CAPCOM_TIMER_COUNT; index++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->timer_irq[index]);
	for (unsigned index = 0; index < CAPCOM_CHANNEL_COUNT; index++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->channel_irq[index]);

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
	int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

	timer_del(p->event_timer);
	p->t01con = 0;
	memset(p->timers, 0, sizeof(p->timers));
	for (unsigned index = 0; index < CAPCOM_TIMER_COUNT; index++)
		p->timers[index].last_update_ns = now_ns;
	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	for (unsigned index = 0; index < CAPCOM_TIMER_COUNT; index++)
		pmb887x_src_reset(&p->timer_src[index]);
	for (unsigned index = 0; index < CAPCOM_CHANNEL_COUNT; index++)
		pmb887x_src_reset(&p->channel_src[index]);

	memset(p->compare, 0, sizeof(p->compare));
	memset(p->period_fired, 0, sizeof(p->period_fired));
	p->pisel = 0;
	memset(p->ccm, 0, sizeof(p->ccm));
	p->out = 0;
	p->ioc = BIT(0);
	p->sem = 0;
	p->see = 0;
	p->drm = 0;
	p->t01ocr = 0;
	capcom_pwm_outputs_update(p);
}

static void capcom_realize(DeviceState *dev, Error **errp) {
	pmb887x_capcom_t *p = PMB887X_CAPCOM(dev);

	if (!clock_has_source(p->clc.clock)) {
		error_setg(errp, "CAPCOM 'clk' input is not connected");
		return;
	}

	for (unsigned index = 0; index < CAPCOM_TIMER_COUNT; index++) {
		if (!p->timer_irq[index])
			hw_error("pmb887x-capcom: timer IRQ %u is not connected", index);
		pmb887x_src_init(&p->timer_src[index], p->timer_irq[index]);
	}
	for (unsigned index = 0; index < CAPCOM_CHANNEL_COUNT; index++) {
		if (!p->channel_irq[index])
			hw_error("pmb887x-capcom: channel IRQ %u is not connected", index);
		pmb887x_src_init(&p->channel_src[index], p->channel_irq[index]);
	}
}

static void capcom_unrealize(DeviceState *dev) {
	pmb887x_capcom_t *p = PMB887X_CAPCOM(dev);
	timer_free(p->event_timer);
}

static const Property capcom_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_capcom_t, revision, 0),
};

static void capcom_class_init(ObjectClass *klass, const void *_data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, capcom_properties);
	device_class_set_legacy_reset(dc, capcom_reset);
	dc->realize = capcom_realize;
	dc->unrealize = capcom_unrealize;
}

static const TypeInfo capcom_info = {
	.name = TYPE_PMB887X_CAPCOM,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(pmb887x_capcom_t),
	.instance_init = capcom_init,
	.class_init = capcom_class_init,
};

static void capcom_register_types(void) {
	type_register_static(&capcom_info);
}

type_init(capcom_register_types)
