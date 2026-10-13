#define PMB887X_TRACE_ID		DSP_TIMER1
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-timer1"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"
#include "qemu/timer.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define TIMER1_DIVIDER		384U
#define TIMER1_RESTART_CYCLES	3U
#define TIMER_INTERRUPT_GROUP	2

typedef struct timer1_state_t timer1_state_t;

struct timer1_state_t {
	uint16_t control;
	uint16_t counter;
	uint16_t compare[2];
	size_t prescaler;
	size_t restart_cycles;
	uint64_t clock_remainder;
	int64_t last_update_ns;
	uint32_t clock_hz;
	bool restart_pending;
	dsp_device_t *interrupt;
	dsp_host_t host;
};

static void timer1_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void timer1_reset(dsp_device_t *device) {
	timer1_state_t *state = device->state;
	dsp_device_t *interrupt = state->interrupt;
	dsp_host_t host = state->host;
	uint32_t clock_hz = state->clock_hz;

	memset(state, 0, sizeof(*state));
	state->interrupt = interrupt;
	state->host = host;
	state->clock_hz = clock_hz;
	state->last_update_ns = state->host.get_time_ns(state->host.opaque);
	state->compare[0] = TEAK_TMR1_INT0_T1INT0;
	state->compare[1] = TEAK_TMR1_INT1_T1INT1;
}

static void timer1_update_state(timer1_state_t *state) {
	int64_t now = state->host.get_time_ns(state->host.opaque);
	uint64_t elapsed_ns = MAX(now - state->last_update_ns, 0);
	uint64_t scaled_cycles;
	uint64_t cycles;

	state->last_update_ns = now;
	bool active = state->restart_pending || (state->control & TEAK_TMR1_CTRL_DT1ACT) != 0;
	if (state->clock_hz == 0 || !active)
		return;

	scaled_cycles = elapsed_ns * state->clock_hz + state->clock_remainder;
	cycles = scaled_cycles / NANOSECONDS_PER_SECOND;
	state->clock_remainder = scaled_cycles % NANOSECONDS_PER_SECOND;

	if (state->restart_pending) {
		if (cycles < state->restart_cycles) {
			state->restart_cycles -= cycles;
			return;
		}
		cycles -= state->restart_cycles;
		state->counter = 0;
		state->prescaler = 0;
		state->restart_cycles = 0;
		state->control |= TEAK_TMR1_CTRL_DT1ACT;
		state->restart_pending = false;
	}

	if ((state->control & TEAK_TMR1_CTRL_DT1ACT) == 0)
		return;

	state->prescaler += cycles;
	while (state->prescaler >= TIMER1_DIVIDER) {
		uint16_t interrupts = 0;

		state->prescaler -= TIMER1_DIVIDER;
		state->counter++;

		if (state->counter == state->compare[0])
			interrupts |= TEAK_INT_FINT1_TMR10;
		if (state->counter == state->compare[1])
			interrupts |= TEAK_INT_FINT1_TMR11;
		if (interrupts != 0)
			dsp_int_set_flags(state->interrupt, TIMER_INTERRUPT_GROUP, interrupts);

		if (state->counter == TEAK_TMR1_CNT_T1CNT) {
			state->control &= (uint16_t) ~TEAK_TMR1_CTRL_DT1ACT;
			break;
		}
	}
}

static bool timer1_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	timer1_state_t *state = device->state;
	timer1_update_state(state);

	switch (offset) {
		case TEAK_TMR1_CTRL:
			*value = state->control;
			break;

		case TEAK_TMR1_CNT:
			*value = state->counter;
			break;

		case TEAK_TMR1_INT0:
			*value = state->compare[0];
			break;

		case TEAK_TMR1_INT1:
			*value = state->compare[1];
			break;

		default:
			*value = 0;
			break;
	}

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool timer1_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	timer1_state_t *state = device->state;
	timer1_update_state(state);

	switch (offset) {
		case TEAK_TMR1_CTRL:
			if ((value & TEAK_TMR1_CTRL_DT1ENA) == 0) {
				state->control = 0;
				state->restart_cycles = 0;
				state->restart_pending = false;
			} else if ((value & TEAK_TMR1_CTRL_RESTART) != 0) {
				state->control = TEAK_TMR1_CTRL_DT1ENA;
				state->clock_remainder = 0;
				state->restart_cycles = TIMER1_RESTART_CYCLES;
				state->restart_pending = true;
			} else {
				state->control |= TEAK_TMR1_CTRL_DT1ENA;
			}
			break;

		case TEAK_TMR1_INT0:
			state->compare[0] = value & TEAK_TMR1_INT0_T1INT0;
			break;

		case TEAK_TMR1_INT1:
			state->compare[1] = value & TEAK_TMR1_INT1_T1INT1;
			break;
	}

	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t timer1_ops = {
	.destroy = timer1_destroy,
	.reset = timer1_reset,
	.read = timer1_read,
	.write = timer1_write,
};

dsp_device_t *timer1_create(
	const pmb887x_dsp_peripheral_config_t *config,
	dsp_device_t *interrupt,
	const dsp_host_t *host
) {
	timer1_state_t *state = g_new0(timer1_state_t, 1);
	state->interrupt = interrupt;
	state->host = *host;
	state->last_update_ns = state->host.get_time_ns(state->host.opaque);
	return dsp_device_create(config, &timer1_ops, state);
}

void timer1_set_clock_hz(dsp_device_t *device, uint32_t frequency_hz) {
	timer1_state_t *state = device->state;
	timer1_update_state(state);
	state->clock_hz = frequency_hz;
}

void timer1_update(dsp_device_t *device) {
	timer1_update_state(device->state);
}

bool timer1_is_active(const dsp_device_t *device) {
	const timer1_state_t *state = device->state;
	return state->restart_pending || (state->control & TEAK_TMR1_CTRL_DT1ACT) != 0;
}
