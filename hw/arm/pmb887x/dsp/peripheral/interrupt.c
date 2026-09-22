#define PMB887X_TRACE_ID		DSP_INTERRUPT
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-interrupt"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bitops.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define INTERRUPT_GROUP_STRIDE		(TEAK_INT_FINTB0 - TEAK_INT_FINTA0)
#define INTERRUPT_GROUP_COUNT		(TEAK_INT_TOMCU / INTERRUPT_GROUP_STRIDE)
#define INTERRUPT_FLAG_REGISTER		TEAK_INT_FINTA0
#define INTERRUPT_ENABLE_REGISTER	TEAK_INT_EINTA0
#define INTERRUPT_RESET_REGISTER		TEAK_INT_RINTA0
#define INTERRUPT_SET_REGISTER		TEAK_INT_SINTA0
#define MCU_INTERRUPT_MASK		(TEAK_INT_TOMCU_TOMCU0 | TEAK_INT_TOMCU_TOMCU1 | \
	TEAK_INT_TOMCU_TOMCU2 | TEAK_INT_TOMCU_TOMCU3)
#define MCU_REQUEST_COUNT	3

typedef struct dsp_int_state_t dsp_int_state_t;

struct dsp_int_state_t {
	uint16_t flags[INTERRUPT_GROUP_COUNT];
	uint16_t enable[INTERRUPT_GROUP_COUNT];
	uint16_t mcu_events;
	dsp_host_t host;
};

static const uint8_t INTERRUPT_GROUP_LINES[] = { 0, 0, 1, 2 };

static uint8_t dsp_int_compute_lines(const dsp_int_state_t *state) {
	uint8_t lines = 0;

	for (size_t group = 0; group < INTERRUPT_GROUP_COUNT; group++)
		if ((qatomic_read(&state->flags[group]) & qatomic_read(&state->enable[group])) != 0)
			lines |= BIT(INTERRUPT_GROUP_LINES[group]);
	return lines;
}

static void dsp_int_update_lines(dsp_int_state_t *state) {
	if (state->host.set_interrupt_lines != NULL)
		state->host.set_interrupt_lines(state->host.opaque, dsp_int_compute_lines(state));
}

static void dsp_int_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void dsp_int_reset(dsp_device_t *device) {
	dsp_int_state_t *state = device->state;
	memset(state->flags, 0, sizeof(state->flags));
	memset(state->enable, 0, sizeof(state->enable));
	qatomic_set(&state->mcu_events, 0);
	dsp_int_update_lines(state);
}

static bool dsp_int_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	dsp_int_state_t *state = device->state;
	*value = 0;

	switch (offset) {
		case TEAK_INT_TOMCU:
			break;

		default:
			if (offset >= INTERRUPT_GROUP_COUNT * INTERRUPT_GROUP_STRIDE)
				break;

			size_t group = offset / INTERRUPT_GROUP_STRIDE;
			size_t register_index = offset % INTERRUPT_GROUP_STRIDE;
			switch (register_index) {
				case INTERRUPT_FLAG_REGISTER:
					*value = qatomic_read(&state->flags[group]);
					break;

				case INTERRUPT_ENABLE_REGISTER:
					*value = qatomic_read(&state->enable[group]);
					break;

				case INTERRUPT_RESET_REGISTER:
				case INTERRUPT_SET_REGISTER:
					break;
			}
			break;
	}

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool dsp_int_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	dsp_int_state_t *state = device->state;

	switch (offset) {
		case TEAK_INT_TOMCU:
			qatomic_or(&state->mcu_events, value & MCU_INTERRUPT_MASK);
			break;

		default:
			if (offset >= INTERRUPT_GROUP_COUNT * INTERRUPT_GROUP_STRIDE)
				break;

			size_t group = offset / INTERRUPT_GROUP_STRIDE;
			size_t register_index = offset % INTERRUPT_GROUP_STRIDE;
			switch (register_index) {
				case INTERRUPT_ENABLE_REGISTER:
					qatomic_set(&state->enable[group], value);
					break;

				case INTERRUPT_RESET_REGISTER:
					qatomic_and(&state->flags[group], (uint16_t) ~value);
					break;

				case INTERRUPT_SET_REGISTER:
					qatomic_or(&state->flags[group], value);
					break;
			}
			dsp_int_update_lines(state);
			break;
	}

	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t dsp_int_ops = {
	.destroy = dsp_int_destroy,
	.reset = dsp_int_reset,
	.read = dsp_int_read,
	.write = dsp_int_write,
};

dsp_device_t *dsp_int_create(const pmb887x_dsp_peripheral_config_t *config, const dsp_host_t *host) {
	dsp_int_state_t *state = g_new0(dsp_int_state_t, 1);
	state->host = *host;
	return dsp_device_create(config, &dsp_int_ops, state);
}

uint8_t dsp_int_get_lines(dsp_device_t *device) {
	dsp_int_state_t *state = device->state;
	return dsp_int_compute_lines(state);
}

uint16_t dsp_int_get_flags(dsp_device_t *device, size_t group) {
	dsp_int_state_t *state = device->state;

	g_assert(group < INTERRUPT_GROUP_COUNT);
	return qatomic_read(&state->flags[group]);
}

uint16_t dsp_int_get_pending_flags(dsp_device_t *device, size_t group) {
	dsp_int_state_t *state = device->state;

	g_assert(group < INTERRUPT_GROUP_COUNT);
	return qatomic_read(&state->flags[group]) & qatomic_read(&state->enable[group]);
}

void dsp_int_set_request(dsp_device_t *device, size_t index, bool level) {
	dsp_int_state_t *state = device->state;
	uint16_t mask;

	g_assert(index < MCU_REQUEST_COUNT);
	if (!level)
		return;

	mask = (uint16_t) (1U << index);
	qatomic_or(&state->flags[0], mask);
	dsp_int_update_lines(state);
}

void dsp_int_set_flags(dsp_device_t *device, size_t group, uint16_t flags) {
	dsp_int_state_t *state = device->state;

	g_assert(group < INTERRUPT_GROUP_COUNT);

	qatomic_or(&state->flags[group], flags);
	dsp_int_update_lines(state);
}

uint16_t dsp_int_take_mcu_events(dsp_device_t *device) {
	dsp_int_state_t *state = device->state;
	return qatomic_xchg(&state->mcu_events, 0);
}
