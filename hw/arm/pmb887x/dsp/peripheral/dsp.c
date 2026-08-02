#define PMB887X_TRACE_ID		DSP_CONTROL
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-control"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bitops.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define DSP_OUTPUT_MASK	(TEAK_DSP_DSPOUT_DSPOUT0 | TEAK_DSP_DSPOUT_DSPOUT1 | TEAK_DSP_DSPOUT_DSPOUT2)
#define DSP_INPUT_COUNT	2
#define DSP_HARDWARE_VERSION	0xE101U

typedef struct control_state_t control_state_t;

struct control_state_t {
	uint16_t page;
	uint16_t inputs;
	uint16_t outputs;
	uint16_t output_events;
	dsp_host_t host;
};

static void control_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void control_reset(dsp_device_t *device) {
	control_state_t *state = device->state;
	state->page = 0;
	qatomic_set(&state->outputs, 0);
	qatomic_set(&state->output_events, 0);
	state->host.set_page(state->host.opaque, 0);
	state->host.set_core_disabled(state->host.opaque, false);
}

static bool control_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	control_state_t *state = device->state;

	switch (offset) {
		case TEAK_DSP_ID:
			*value = DSP_HARDWARE_VERSION;
			break;

		case TEAK_DSP_DEBUG:
			*value = 0;
			break;

		case TEAK_DSP_PAGE:
			*value = state->page;
			break;

		case TEAK_DSP_DSPOUT:
			*value = qatomic_read(&state->outputs) |
				qatomic_read(&state->inputs) << TEAK_DSP_DSPOUT_DSPIN0_SHIFT;
			break;

		default:
			*value = 0;
			break;
	}

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool control_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	control_state_t *state = device->state;

	switch (offset) {
		case TEAK_DSP_CTRL:
			state->host.set_core_disabled(state->host.opaque, true);
			break;

		case TEAK_DSP_DSPOUT: {
			uint16_t outputs = value & DSP_OUTPUT_MASK;
			uint16_t changed = outputs ^ qatomic_read(&state->outputs);

			qatomic_set(&state->outputs, outputs);
			qatomic_or(&state->output_events, changed);
			break;
		}

		case TEAK_DSP_PAGE:
			state->page = value;
			state->host.set_page(state->host.opaque, value);
			break;
	}

	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t control_ops = {
	.destroy = control_destroy,
	.reset = control_reset,
	.read = control_read,
	.write = control_write,
};

dsp_device_t *control_create(const pmb887x_dsp_peripheral_config_t *config, const dsp_host_t *host) {
	control_state_t *state = g_new0(control_state_t, 1);
	state->host = *host;
	return dsp_device_create(config, &control_ops, state);
}

int control_set_input(dsp_device_t *device, size_t index, bool level) {
	control_state_t *state = device->state;
	uint16_t mask;
	uint16_t old_inputs;

	g_assert(index < DSP_INPUT_COUNT);
	mask = (uint16_t) BIT(index);
	old_inputs = qatomic_read(&state->inputs);

	if (((old_inputs & mask) != 0) == level)
		return 0;

	if (level) {
		qatomic_or(&state->inputs, mask);
		return 1;
	}
	qatomic_and(&state->inputs, (uint16_t) ~mask);
	return -1;
}

uint16_t control_get_outputs(dsp_device_t *device) {
	control_state_t *state = device->state;
	return qatomic_read(&state->outputs);
}

uint16_t control_take_output_events(dsp_device_t *device) {
	control_state_t *state = device->state;
	return qatomic_xchg(&state->output_events, 0);
}
