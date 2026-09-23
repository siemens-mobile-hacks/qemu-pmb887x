#define PMB887X_TRACE_ID		DSP_TIMER2
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-timer2"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define TIMER2_DIVIDER		96U
#define TIMER_INTERRUPT_GROUP	2

typedef struct timer2_state_t timer2_state_t;

struct timer2_state_t {
	uint16_t control;
	uint16_t counter;
	uint16_t maximum;
	size_t prescaler;
	bool clock_enabled;
	dsp_device_t *interrupt;
};

static bool timer2_running(const timer2_state_t *state) {
	return state->clock_enabled && (state->control & TEAK_TMR2_CTRL_DT2ACT) != 0;
}

static void timer2_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void timer2_reset(dsp_device_t *device) {
	timer2_state_t *state = device->state;

	state->control = 0;
	state->counter = 0;
	state->maximum = TEAK_TMR2_MAX_T2MAX;
	state->prescaler = 0;
}

static bool timer2_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	timer2_state_t *state = device->state;

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

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool timer2_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	timer2_state_t *state = device->state;

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
	state->maximum = TEAK_TMR2_MAX_T2MAX;
	return dsp_device_create(config, &timer2_ops, state);
}

void timer2_set_clock_enabled(dsp_device_t *device, bool enabled) {
	timer2_state_t *state = device->state;
	state->clock_enabled = enabled;
}

/* The counter runs up to the maximum, interrupts there and restarts from 0 on the next tick. */
void timer2_advance(dsp_device_t *device, size_t cycles) {
	timer2_state_t *state = device->state;
	size_t ticks;

	if (!timer2_running(state))
		return;

	ticks = (state->prescaler + cycles) / TIMER2_DIVIDER;
	state->prescaler = (state->prescaler + cycles) % TIMER2_DIVIDER;

	while (ticks != 0) {
		size_t period = (size_t) state->maximum + 1;
		uint16_t distance;

		if (state->counter == state->maximum) {
			state->counter = 0;
			ticks--;
			continue;
		}

		distance = state->maximum - state->counter;
		if (ticks < distance) {
			state->counter += ticks;
			break;
		}

		ticks -= distance;
		state->counter = state->maximum;
		dsp_int_set_flags(state->interrupt, TIMER_INTERRUPT_GROUP, TEAK_INT_FINT1_TMR2);
		/* Whole periods only raise the same flag again. */
		ticks %= period;
	}
}

size_t timer2_next_event(dsp_device_t *device) {
	timer2_state_t *state = device->state;
	size_t ticks;

	if (!timer2_running(state))
		return SIZE_MAX;

	if (state->counter != state->maximum)
		ticks = (uint16_t) (state->maximum - state->counter);
	else if (state->maximum != 0)
		ticks = (size_t) state->maximum + 1;
	else
		return SIZE_MAX;
	return ticks * TIMER2_DIVIDER - state->prescaler;
}

bool timer2_is_active(const dsp_device_t *device) {
	const timer2_state_t *state = device->state;
	return timer2_running(state);
}
