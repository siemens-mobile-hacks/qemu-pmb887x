#define PMB887X_TRACE_ID		DSP_TIMER2
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-timer2"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"
#include "qemu/timer.h"
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
	uint64_t prescaler;
	uint64_t clock_remainder;
	int64_t last_update_ns;
	uint32_t clock_hz;
	dsp_device_t *interrupt;
	dsp_host_t host;
};

static void timer2_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void timer2_reset(dsp_device_t *device) {
	timer2_state_t *state = device->state;

	state->control = 0;
	state->counter = 0;
	state->maximum = TEAK_TMR2_MAX_T2MAX;
	state->prescaler = 0;
	state->clock_remainder = 0;
	state->last_update_ns = state->host.get_time_ns(state->host.opaque);
}

static bool timer2_count_clock_cycles(timer2_state_t *state, uint64_t cycles) {
	uint64_t ticks;
	bool interrupt = false;

	if (state->clock_hz == 0 || (state->control & TEAK_TMR2_CTRL_DT2ACT) == 0)
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

static bool timer2_update_state(timer2_state_t *state) {
	int64_t now = state->host.get_time_ns(state->host.opaque);
	uint64_t elapsed_ns = MAX(now - state->last_update_ns, 0);
	uint64_t scaled_cycles;
	uint64_t cycles;

	state->last_update_ns = now;
	scaled_cycles = elapsed_ns * state->clock_hz + state->clock_remainder;
	cycles = scaled_cycles / NANOSECONDS_PER_SECOND;
	state->clock_remainder = scaled_cycles % NANOSECONDS_PER_SECOND;
	return timer2_count_clock_cycles(state, cycles);
}

static void timer2_raise_interrupt(timer2_state_t *state) {
	dsp_int_set_flags(state->interrupt, TIMER_INTERRUPT_GROUP, TEAK_INT_FINT1_TMR2);
}

static bool timer2_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	timer2_state_t *state = device->state;
	bool interrupt = timer2_update_state(state);

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

	if (interrupt)
		timer2_raise_interrupt(state);

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool timer2_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	timer2_state_t *state = device->state;
	bool interrupt = timer2_update_state(state);

	switch (offset) {
		case TEAK_TMR2_CTRL:
			state->control = value & TEAK_TMR2_CTRL_DT2ACT;
			if (state->control != 0) {
				state->prescaler = 0;
				state->clock_remainder = 0;
			}
			break;

		case TEAK_TMR2_CNT:
			if ((state->control & TEAK_TMR2_CTRL_DT2ACT) == 0)
				state->counter = value;
			break;

		case TEAK_TMR2_MAX:
			state->maximum = value;
			break;
	}

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

dsp_device_t *timer2_create(
	const pmb887x_dsp_peripheral_config_t *config,
	dsp_device_t *interrupt,
	const dsp_host_t *host
) {
	timer2_state_t *state = g_new0(timer2_state_t, 1);
	state->interrupt = interrupt;
	state->host = *host;
	state->last_update_ns = state->host.get_time_ns(state->host.opaque);
	return dsp_device_create(config, &timer2_ops, state);
}

void timer2_set_clock_hz(dsp_device_t *device, uint32_t frequency_hz) {
	timer2_state_t *state = device->state;
	bool interrupt = timer2_update_state(state);

	if (interrupt)
		timer2_raise_interrupt(state);
	state->clock_hz = frequency_hz;
}

void timer2_update(dsp_device_t *device) {
	timer2_state_t *state = device->state;
	bool interrupt = timer2_update_state(state);

	if (interrupt)
		timer2_raise_interrupt(state);
}

bool timer2_is_active(dsp_device_t *device) {
	timer2_state_t *state = device->state;
	return (state->control & TEAK_TMR2_CTRL_DT2ACT) != 0;
}
