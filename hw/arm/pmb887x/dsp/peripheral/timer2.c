#define PMB887X_TRACE_ID		DSP_TIMER2
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-timer2"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/timer.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define TIMER2_DIVIDER		96U
#define TIMER2_CLOCK_NUMERATOR	13U
#define TIMER2_CLOCK_DENOMINATOR_NS	125U
#define TIMER_INTERRUPT_GROUP	2

typedef struct timer2_state_t timer2_state_t;

struct timer2_state_t {
	uint16_t control;
	uint16_t counter;
	uint16_t maximum;
	uint64_t prescaler;
	uint64_t clock_remainder;
	int64_t last_update;
	QEMUTimer *timer;
	QemuMutex mutex;
	bool clock_enabled;
	bool core_idle;
	dsp_device_t *interrupt;
};

static void timer2_destroy(dsp_device_t *device) {
	timer2_state_t *state = device->state;
	timer_free(state->timer);
	qemu_mutex_destroy(&state->mutex);
	g_free(state);
}

static void timer2_reset(dsp_device_t *device) {
	timer2_state_t *state = device->state;

	qemu_mutex_lock(&state->mutex);
	timer_del(state->timer);
	state->control = 0;
	state->counter = 0;
	state->maximum = TEAK_TMR2_MAX_T2MAX;
	state->prescaler = 0;
	state->clock_remainder = 0;
	state->last_update = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	qemu_mutex_unlock(&state->mutex);
}

static bool timer2_advance_cycles_locked(timer2_state_t *state, uint64_t cycles) {
	uint64_t ticks;
	bool interrupt = false;

	if (!state->clock_enabled || (state->control & TEAK_TMR2_CTRL_DT2ACT) == 0)
		return false;

	ticks = (state->prescaler + cycles) / TIMER2_DIVIDER;
	state->prescaler = (state->prescaler + cycles) % TIMER2_DIVIDER;

	while (ticks != 0) {
		ticks--;
		if (state->counter == state->maximum) {
			state->counter = 0;
			continue;
		}
		state->counter++;
		if (state->counter == state->maximum)
			interrupt = true;
	}
	return interrupt;
}

static bool timer2_update_idle_locked(timer2_state_t *state, int64_t now) {
	int64_t elapsed = MAX(now - state->last_update, 0);
	uint64_t scaled_cycles;
	uint64_t cycles;

	state->last_update = now;

	if (!state->core_idle)
		return false;

	scaled_cycles = (uint64_t) elapsed * TIMER2_CLOCK_NUMERATOR + state->clock_remainder;
	cycles = scaled_cycles / TIMER2_CLOCK_DENOMINATOR_NS;
	state->clock_remainder = scaled_cycles % TIMER2_CLOCK_DENOMINATOR_NS;
	return timer2_advance_cycles_locked(state, cycles);
}

static uint64_t timer2_ticks_until_interrupt(const timer2_state_t *state) {
	uint16_t distance = state->maximum - state->counter;

	if (distance != 0)
		return distance;
	return (uint64_t) state->maximum + 1;
}

static void timer2_schedule_locked(timer2_state_t *state, int64_t now) {
	uint64_t ticks;
	uint64_t cycles;
	uint64_t scaled_time;
	uint64_t delay;
	bool stopped;

	stopped = !state->clock_enabled || !state->core_idle ||
		(state->control & TEAK_TMR2_CTRL_DT2ACT) == 0 || state->maximum == 0;
	if (stopped) {
		timer_del(state->timer);
		return;
	}

	ticks = timer2_ticks_until_interrupt(state);
	cycles = ticks * TIMER2_DIVIDER - state->prescaler;
	scaled_time = cycles * TIMER2_CLOCK_DENOMINATOR_NS - state->clock_remainder;
	delay = DIV_ROUND_UP(scaled_time, TIMER2_CLOCK_NUMERATOR);
	timer_mod(state->timer, now + delay);
}

static void timer2_raise_interrupt(timer2_state_t *state) {
	dsp_int_set_flags(state->interrupt, TIMER_INTERRUPT_GROUP, TEAK_INT_FINT1_TMR2);
}

static void timer2_timer(void *opaque) {
	timer2_state_t *state = opaque;
	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	bool interrupt;

	qemu_mutex_lock(&state->mutex);
	interrupt = timer2_update_idle_locked(state, now);
	timer2_schedule_locked(state, now);
	qemu_mutex_unlock(&state->mutex);

	if (interrupt)
		timer2_raise_interrupt(state);
}

static bool timer2_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	timer2_state_t *state = device->state;
	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	bool interrupt;

	qemu_mutex_lock(&state->mutex);
	interrupt = timer2_update_idle_locked(state, now);

	switch (offset) {
		case TEAK_TMR2_CTRL:
			*value = state->control;
			break;

		case TEAK_TMR2_CNT:
			*value = state->counter;
			break;

		case TEAK_TMR2_MAX:
			*value = state->maximum;
			break;

		default:
			*value = 0;
			break;
	}

	timer2_schedule_locked(state, now);
	qemu_mutex_unlock(&state->mutex);

	if (interrupt)
		timer2_raise_interrupt(state);

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool timer2_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	timer2_state_t *state = device->state;
	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	bool interrupt;

	qemu_mutex_lock(&state->mutex);
	interrupt = timer2_update_idle_locked(state, now);

	switch (offset) {
		case TEAK_TMR2_CTRL:
			state->control = value & TEAK_TMR2_CTRL_DT2ACT;
			break;

		case TEAK_TMR2_CNT:
			if ((state->control & TEAK_TMR2_CTRL_DT2ACT) == 0)
				state->counter = value;
			break;

		case TEAK_TMR2_MAX:
			state->maximum = value;
			break;
	}

	timer2_schedule_locked(state, now);
	qemu_mutex_unlock(&state->mutex);

	if (interrupt)
		timer2_raise_interrupt(state);

	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t timer2_ops = {
	.destroy = timer2_destroy,
	.reset = timer2_reset,
	.read = timer2_read,
	.write = timer2_write,
};

dsp_device_t *timer2_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt) {
	timer2_state_t *state = g_new0(timer2_state_t, 1);
	state->interrupt = interrupt;
	state->last_update = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	qemu_mutex_init(&state->mutex);
	state->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, timer2_timer, state);
	return dsp_device_create(config, &timer2_ops, state);
}

void timer2_set_clock_enabled(dsp_device_t *device, bool enabled) {
	timer2_state_t *state = device->state;
	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	bool interrupt;

	qemu_mutex_lock(&state->mutex);
	interrupt = timer2_update_idle_locked(state, now);
	state->clock_enabled = enabled;
	state->last_update = now;
	timer2_schedule_locked(state, now);
	qemu_mutex_unlock(&state->mutex);

	if (interrupt)
		timer2_raise_interrupt(state);
}

void timer2_set_core_idle(dsp_device_t *device, bool idle) {
	timer2_state_t *state = device->state;
	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	bool interrupt;

	qemu_mutex_lock(&state->mutex);
	interrupt = timer2_update_idle_locked(state, now);
	state->core_idle = idle;
	state->last_update = now;
	timer2_schedule_locked(state, now);
	qemu_mutex_unlock(&state->mutex);

	if (interrupt)
		timer2_raise_interrupt(state);
}

void timer2_advance(dsp_device_t *device, size_t cycles) {
	timer2_state_t *state = device->state;
	bool interrupt = false;

	qemu_mutex_lock(&state->mutex);
	if (!state->core_idle)
		interrupt = timer2_advance_cycles_locked(state, cycles);
	qemu_mutex_unlock(&state->mutex);

	if (interrupt)
		timer2_raise_interrupt(state);
}

bool timer2_is_active(dsp_device_t *device) {
	timer2_state_t *state = device->state;
	bool active;

	qemu_mutex_lock(&state->mutex);
	active = (state->control & TEAK_TMR2_CTRL_DT2ACT) != 0;
	qemu_mutex_unlock(&state->mutex);

	return active;
}
