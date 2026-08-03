#define PMB887X_TRACE_ID		DSP_MCS
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-mcs"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bitops.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define MCS_SEMAPHORE_COUNT	16
#define MCS_SEMAPHORE_STATE_BITS	4
#define MCS_SEMAPHORE_OWNER_MASK	3U
#define MCS_SEMAPHORE_MCU_WAITING	BIT(2)
#define MCS_SEMAPHORE_DSP_WAITING	BIT(3)

typedef enum mcs_semaphore_owner_t mcs_semaphore_owner_t;

enum mcs_semaphore_owner_t {
	MCS_SEMAPHORE_FREE,
	MCS_SEMAPHORE_MCU,
	MCS_SEMAPHORE_DSP,
};

typedef struct mcs_state_t mcs_state_t;

struct mcs_state_t {
	dsp_host_t host;
	uint16_t comm_status;
	uint16_t comm_cleared;
	uint64_t semaphores;
};

static uint64_t mcs_waiting_mask(mcs_semaphore_owner_t owner) {
	return owner == MCS_SEMAPHORE_MCU ? MCS_SEMAPHORE_MCU_WAITING : MCS_SEMAPHORE_DSP_WAITING;
}

static uint16_t mcs_get_semaphore_status(mcs_state_t *state, mcs_semaphore_owner_t owner) {
	uint64_t semaphores = qatomic_read(&state->semaphores);
	uint16_t status = 0;

	for (size_t i = 0; i < MCS_SEMAPHORE_COUNT; i++) {
		uint64_t current_owner = semaphores >> (i * MCS_SEMAPHORE_STATE_BITS) & MCS_SEMAPHORE_OWNER_MASK;
		if (current_owner != owner)
			status |= (uint16_t) BIT(i);
	}
	return status;
}

static void mcs_request_semaphores(mcs_state_t *state, uint16_t value, mcs_semaphore_owner_t owner) {
	uint64_t semaphores;
	uint64_t updated;
	uint64_t waiting = mcs_waiting_mask(owner);

	do {
		semaphores = qatomic_read(&state->semaphores);
		updated = semaphores;
		for (size_t i = 0; i < MCS_SEMAPHORE_COUNT; i++) {
			uint32_t shift = i * MCS_SEMAPHORE_STATE_BITS;
			uint64_t owner_mask = (uint64_t) MCS_SEMAPHORE_OWNER_MASK << shift;
			uint64_t current_owner = semaphores & owner_mask;
			uint64_t requested_owner = (uint64_t) owner << shift;

			if ((value & BIT(i)) == 0)
				continue;

			if (current_owner == 0) {
				updated &= ~owner_mask;
				updated |= requested_owner;
			} else if (current_owner != requested_owner) {
				updated |= waiting << shift;
			}
		}
	} while (qatomic_cmpxchg(&state->semaphores, semaphores, updated) != semaphores);
}

static void mcs_release_semaphores(mcs_state_t *state, uint16_t value, mcs_semaphore_owner_t owner) {
	uint64_t semaphores;
	uint64_t updated;
	mcs_semaphore_owner_t next_owner = owner == MCS_SEMAPHORE_MCU ? MCS_SEMAPHORE_DSP : MCS_SEMAPHORE_MCU;
	uint64_t next_waiting = mcs_waiting_mask(next_owner);

	do {
		semaphores = qatomic_read(&state->semaphores);
		updated = semaphores;
		for (size_t i = 0; i < MCS_SEMAPHORE_COUNT; i++) {
			uint32_t shift = i * MCS_SEMAPHORE_STATE_BITS;
			uint64_t owner_mask = (uint64_t) MCS_SEMAPHORE_OWNER_MASK << shift;
			uint64_t current_owner = semaphores & owner_mask;
			uint64_t releasing_owner = (uint64_t) owner << shift;

			if ((value & BIT(i)) == 0)
				continue;
			if (current_owner != releasing_owner)
				continue;

			updated &= ~owner_mask;
			if ((semaphores & (next_waiting << shift)) != 0) {
				updated &= ~(next_waiting << shift);
				updated |= (uint64_t) next_owner << shift;
			}
		}
	} while (qatomic_cmpxchg(&state->semaphores, semaphores, updated) != semaphores);
}

static void mcs_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void mcs_reset(dsp_device_t *device) {
	mcs_state_t *state = device->state;
	qatomic_set(&state->comm_status, 0);
	qatomic_set(&state->comm_cleared, 0);
	qatomic_set(&state->semaphores, 0);
}

static bool mcs_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	mcs_state_t *state = device->state;

	switch (offset) {
		case TEAK_MCS_CFSTA:
			*value = qatomic_read(&state->comm_status);
			break;

		case TEAK_MCS_MCU_SEM:
			*value = mcs_get_semaphore_status(state, MCS_SEMAPHORE_DSP);
			break;

		default:
			*value = 0;
			break;
	}

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool mcs_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	mcs_state_t *state = device->state;

	switch (offset) {
		case TEAK_MCS_CFSET: {
			uint16_t previous = qatomic_fetch_or(&state->comm_status, value);
			uint16_t set = value & ~previous;

			if (set != 0 && state->host.comm_changed != NULL)
				state->host.comm_changed(state->host.opaque, set, true);
			break;
		}

		case TEAK_MCS_CFR: {
			uint16_t cleared = qatomic_read(&state->comm_status) & value;

			qatomic_and(&state->comm_status, (uint16_t) ~value);
			qatomic_or(&state->comm_cleared, cleared);

			if (cleared != 0 && state->host.comm_changed != NULL)
				state->host.comm_changed(state->host.opaque, cleared, false);
			break;
		}

		case TEAK_MCS_MCU_SEMS:
			mcs_request_semaphores(state, value, MCS_SEMAPHORE_DSP);
			break;

		case TEAK_MCS_MCU_SEMR:
			mcs_release_semaphores(state, value, MCS_SEMAPHORE_DSP);
			break;
	}

	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t mcs_ops = {
	.destroy = mcs_destroy,
	.reset = mcs_reset,
	.read = mcs_read,
	.write = mcs_write,
};

dsp_device_t *mcs_create(const pmb887x_dsp_peripheral_config_t *config, const dsp_host_t *host) {
	mcs_state_t *state = g_new0(mcs_state_t, 1);
	state->host = *host;
	return dsp_device_create(config, &mcs_ops, state);
}

uint16_t mcs_get_flags(dsp_device_t *device) {
	mcs_state_t *state = device->state;
	return qatomic_read(&state->comm_status);
}

void mcs_set_flags(dsp_device_t *device, uint16_t value) {
	mcs_state_t *state = device->state;
	qatomic_or(&state->comm_status, value);
}

void mcs_clear_flags(dsp_device_t *device, uint16_t value) {
	mcs_state_t *state = device->state;
	qatomic_and(&state->comm_status, (uint16_t) ~value);
}

uint16_t mcs_take_clear(dsp_device_t *device) {
	mcs_state_t *state = device->state;
	return qatomic_xchg(&state->comm_cleared, 0);
}

uint16_t mcs_get_mcu_semaphore_status(dsp_device_t *device) {
	mcs_state_t *state = device->state;
	return mcs_get_semaphore_status(state, MCS_SEMAPHORE_MCU);
}

void mcs_request_mcu_semaphores(dsp_device_t *device, uint16_t value) {
	mcs_state_t *state = device->state;
	mcs_request_semaphores(state, value, MCS_SEMAPHORE_MCU);
}

void mcs_release_mcu_semaphores(dsp_device_t *device, uint16_t value) {
	mcs_state_t *state = device->state;
	mcs_release_semaphores(state, value, MCS_SEMAPHORE_MCU);
}
