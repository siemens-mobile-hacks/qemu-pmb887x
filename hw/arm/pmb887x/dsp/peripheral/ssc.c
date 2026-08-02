#define PMB887X_TRACE_ID		DSP_SSC
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-ssc"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"
#include "qemu/bitops.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/fifo.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define SSC_FIFO_SIZE		32
#define SSC_INTERRUPT_GROUP	1
#define SSC_OPERATING_CONTROL_MASK	(TEAK_SSC_CON_EN | TEAK_SSC_CON_MS)
#define SSC_ERROR_MASK		(TEAK_SSC_CON_TE | TEAK_SSC_CON_RE | TEAK_SSC_CON_PE | TEAK_SSC_CON_BE)

typedef enum ssc_fifo_t ssc_fifo_t;

enum ssc_fifo_t {
	SSC_FIFO_RX,
	SSC_FIFO_TX,
};

typedef struct ssc_state_t ssc_state_t;

struct ssc_state_t {
	uint16_t control;
	uint16_t status;
	uint16_t receive_fifo_control;
	uint16_t transmit_fifo_control;
	uint16_t baud_rate;
	uint16_t fractional_divider;
	uint16_t transmit_data;
	uint16_t transfer_data;
	uint8_t bits;
	uint8_t bit_count;
	uint16_t mask;
	size_t transfer_cycles;
	size_t transfer_cycles_total;
	size_t start_cycles;
	bool transfer_active;
	pmb887x_fifo16_t receive_fifo;
	pmb887x_fifo16_t transmit_fifo;
	dsp_device_t *interrupt;
	dsp_host_t host;
};

static bool ssc_running(const ssc_state_t *state) {
	uint16_t required = TEAK_SSC_CON_EN | TEAK_SSC_CON_CLKON | TEAK_SSC_CON_MS;
	return (state->control & required) == required;
}

static size_t ssc_fifo_limit(const ssc_state_t *state, ssc_fifo_t fifo) {
	uint16_t control = fifo == SSC_FIFO_RX ? state->receive_fifo_control : state->transmit_fifo_control;
	uint16_t enabled = fifo == SSC_FIFO_RX ? TEAK_SSC_RXFCON_EN : TEAK_SSC_TXFCON_EN;
	return (control & enabled) != 0 ? SSC_FIFO_SIZE : 1;
}

static void ssc_set_interrupts(ssc_state_t *state, uint16_t flags) {
	dsp_int_set_flags(state->interrupt, SSC_INTERRUPT_GROUP, flags);
}

static void ssc_update_receive_request(ssc_state_t *state) {
	size_t level = 1;

	if ((state->receive_fifo_control & TEAK_SSC_RXFCON_EN) != 0)
		level = (state->receive_fifo_control & TEAK_SSC_RXFCON_ITL) >> TEAK_SSC_RXFCON_ITL_SHIFT;
	if ((state->receive_fifo_control & TEAK_SSC_RXFCON_TMEN) != 0)
		level = 1;
	if (level == 0)
		level = 1;

	if (pmb887x_fifo_count(&state->receive_fifo) >= level)
		ssc_set_interrupts(state, TEAK_INT_FINTB0_SSC1RX);
}

static void ssc_update_transmit_request(ssc_state_t *state) {
	size_t level = 0;
	bool transparent;
	bool below_threshold;

	if ((state->transmit_fifo_control & TEAK_SSC_TXFCON_EN) != 0)
		level = (state->transmit_fifo_control & TEAK_SSC_TXFCON_ITL) >> TEAK_SSC_TXFCON_ITL_SHIFT;

	transparent = (state->transmit_fifo_control & TEAK_SSC_TXFCON_TMEN) != 0;
	below_threshold = pmb887x_fifo_count(&state->transmit_fifo) <= level;
	if (transparent || below_threshold)
		ssc_set_interrupts(state, TEAK_INT_FINTB0_SSC1TX);
}

static void ssc_reset_fifo(ssc_state_t *state, ssc_fifo_t fifo) {
	if (fifo == SSC_FIFO_RX) {
		pmb887x_fifo_reset(&state->receive_fifo);
	} else {
		pmb887x_fifo_reset(&state->transmit_fifo);
	}
}

static void ssc_push_receive(ssc_state_t *state, uint16_t value) {
	size_t limit = ssc_fifo_limit(state, SSC_FIFO_RX);

	if (pmb887x_fifo_count(&state->receive_fifo) >= limit) {
		if ((state->control & TEAK_SSC_CON_REN) != 0) {
			state->status |= TEAK_SSC_CON_RE;
			ssc_set_interrupts(state, TEAK_INT_FINTB0_SSC1ERR);
		}
		pmb887x_fifo16_pop(&state->receive_fifo);
	}

	pmb887x_fifo16_push(&state->receive_fifo, value & state->mask);
	ssc_update_receive_request(state);
}

static uint16_t ssc_transfer(ssc_state_t *state, uint16_t transmitted) {
	uint16_t received = 0;

	if ((state->control & TEAK_SSC_CON_LB) != 0)
		return transmitted;
	if (state->host.ssc_transfer == NULL)
		return 0;

	if ((state->control & TEAK_SSC_CON_HB) != 0) {
		for (int shift = state->bits - 8; shift >= 0; shift -= 8) {
			uint32_t byte = transmitted >> shift & UINT8_MAX;
			received |= (state->host.ssc_transfer(state->host.ssc_opaque, byte) & UINT8_MAX) << shift;
		}
	} else {
		for (size_t shift = 0; shift < state->bits; shift += 8) {
			uint32_t byte = transmitted >> shift & UINT8_MAX;
			received |= (state->host.ssc_transfer(state->host.ssc_opaque, byte) & UINT8_MAX) << shift;
		}
	}
	return received;
}

static void ssc_start_transfer(ssc_state_t *state) {
	if (state->transfer_active || !ssc_running(state))
		return;
	if (pmb887x_fifo_is_empty(&state->transmit_fifo))
		return;

	state->transfer_data = pmb887x_fifo16_pop(&state->transmit_fifo);
	state->transfer_cycles_total = state->bits * 2U * ((size_t) state->baud_rate + 1U);
	state->transfer_cycles = state->transfer_cycles_total;
	state->bit_count = 0;
	state->transfer_active = true;
	state->status |= TEAK_SSC_CON_BSY;

	ssc_update_transmit_request(state);
}

static void ssc_complete_transfer(ssc_state_t *state) {
	uint16_t received = ssc_transfer(state, state->transfer_data);

	state->transfer_active = false;
	state->status &= (uint16_t) ~TEAK_SSC_CON_BSY;
	state->bit_count = 0;

	ssc_push_receive(state, received);
	ssc_update_transmit_request(state);
}

static uint16_t ssc_read_receive(ssc_state_t *state) {
	uint16_t value;

	if (pmb887x_fifo_is_empty(&state->receive_fifo)) {
		if ((state->control & TEAK_SSC_CON_REN) != 0) {
			state->status |= TEAK_SSC_CON_RE;
			ssc_set_interrupts(state, TEAK_INT_FINTB0_SSC1ERR);
		}
		return 0;
	}

	value = pmb887x_fifo16_pop(&state->receive_fifo);
	ssc_update_receive_request(state);
	return value;
}

static void ssc_write_transmit(ssc_state_t *state, uint16_t value) {
	size_t limit = ssc_fifo_limit(state, SSC_FIFO_TX);
	bool idle = !state->transfer_active && pmb887x_fifo_is_empty(&state->transmit_fifo);

	state->transmit_data = value & state->mask;

	if (pmb887x_fifo_count(&state->transmit_fifo) >= limit) {
		if ((state->control & TEAK_SSC_CON_TEN) != 0) {
			state->status |= TEAK_SSC_CON_TE;
			ssc_set_interrupts(state, TEAK_INT_FINTB0_SSC1ERR);
		}
		pmb887x_fifo16_pop(&state->transmit_fifo);
	}

	pmb887x_fifo16_push(&state->transmit_fifo, state->transmit_data);

	if (idle)
		state->start_cycles = 2U * ((size_t) state->baud_rate + 1U);
}

static void ssc_destroy(dsp_device_t *device) {
	ssc_state_t *state = device->state;
	pmb887x_fifo16_free(&state->receive_fifo);
	pmb887x_fifo16_free(&state->transmit_fifo);
	g_free(state);
}

static void ssc_reset(dsp_device_t *device) {
	ssc_state_t *state = device->state;
	dsp_device_t *interrupt = state->interrupt;
	dsp_host_t host = state->host;
	pmb887x_fifo16_t receive_fifo = state->receive_fifo;
	pmb887x_fifo16_t transmit_fifo = state->transmit_fifo;

	memset(state, 0, sizeof(*state));
	state->interrupt = interrupt;
	state->host = host;
	state->receive_fifo = receive_fifo;
	state->transmit_fifo = transmit_fifo;
	state->receive_fifo_control = 0x0100;
	state->transmit_fifo_control = 0x0100;
	pmb887x_fifo_reset(&state->receive_fifo);
	pmb887x_fifo_reset(&state->transmit_fifo);
}

static bool ssc_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	ssc_state_t *state = device->state;

	switch (offset) {
		case TEAK_SSC_CON:
			if ((state->control & TEAK_SSC_CON_EN) != 0) {
				*value = state->control & SSC_OPERATING_CONTROL_MASK;
				*value |= state->status | state->bit_count;
			} else {
				*value = state->control;
			}
			break;

		case TEAK_SSC_WHBCON:
			*value = 0;
			break;

		case TEAK_SSC_TXB:
			*value = state->transmit_data;
			break;

		case TEAK_SSC_RXB:
			*value = ssc_read_receive(state);
			break;

		case TEAK_SSC_RXFCON:
			*value = state->receive_fifo_control;
			break;

		case TEAK_SSC_TXFCON:
			*value = state->transmit_fifo_control;
			break;

		case TEAK_SSC_FSTAT:
			*value = 0;
			if ((state->receive_fifo_control & TEAK_SSC_RXFCON_EN) != 0)
				*value |= pmb887x_fifo_count(&state->receive_fifo) << TEAK_SSC_FSTAT_RXFFL_SHIFT;
			if ((state->transmit_fifo_control & TEAK_SSC_TXFCON_EN) != 0)
				*value |= pmb887x_fifo_count(&state->transmit_fifo) << TEAK_SSC_FSTAT_TXFFL_SHIFT;
			break;

		case TEAK_SSC_BR:
			*value = state->baud_rate;
			break;

		case TEAK_SSC_FDV:
			*value = state->fractional_divider;
			break;

		default:
			*value = 0;
			break;
	}

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool ssc_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	ssc_state_t *state = device->state;

	switch (offset) {
		case TEAK_SSC_CON:
			state->control = value;
			state->bits = (value & TEAK_SSC_CON_BM) + 1;
			state->mask = state->bits == 16 ? UINT16_MAX : BIT(state->bits) - 1;

			if ((value & TEAK_SSC_CON_EN) == 0) {
				state->transfer_active = false;
				state->start_cycles = 0;
				state->status = 0;
				state->bit_count = 0;
			}
			break;

		case TEAK_SSC_WHBCON: {
			uint16_t set_errors = value >> 4 & SSC_ERROR_MASK;

			state->status &= (uint16_t) ~(value & SSC_ERROR_MASK);
			state->status |= set_errors;

			if (set_errors != 0)
				ssc_set_interrupts(state, TEAK_INT_FINTB0_SSC1ERR);
			break;
		}

		case TEAK_SSC_TXB:
			ssc_write_transmit(state, value);
			break;

		case TEAK_SSC_RXFCON:
			if ((value & TEAK_SSC_RXFCON_EN) != (state->receive_fifo_control & TEAK_SSC_RXFCON_EN))
				ssc_reset_fifo(state, SSC_FIFO_RX);

			if ((value & TEAK_SSC_RXFCON_FLU) != 0) {
				ssc_reset_fifo(state, SSC_FIFO_RX);
				value &= (uint16_t) ~TEAK_SSC_RXFCON_FLU;
			}

			state->receive_fifo_control = value;
			ssc_update_receive_request(state);
			break;

		case TEAK_SSC_TXFCON:
			if ((value & TEAK_SSC_TXFCON_EN) != (state->transmit_fifo_control & TEAK_SSC_TXFCON_EN))
				ssc_reset_fifo(state, SSC_FIFO_TX);

			if ((value & TEAK_SSC_TXFCON_FLU) != 0) {
				ssc_reset_fifo(state, SSC_FIFO_TX);
				value &= (uint16_t) ~TEAK_SSC_TXFCON_FLU;
			}

			state->transmit_fifo_control = value;
			ssc_update_transmit_request(state);
			break;

		case TEAK_SSC_BR:
			state->baud_rate = value;
			break;

		case TEAK_SSC_FDV:
			state->fractional_divider = value & TEAK_SSC_FDV_VALUE;
			break;
	}

	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t ssc_ops = {
	.destroy = ssc_destroy,
	.reset = ssc_reset,
	.read = ssc_read,
	.write = ssc_write,
};

dsp_device_t *ssc_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt, const dsp_host_t *host) {
	ssc_state_t *state = g_new0(ssc_state_t, 1);
	state->interrupt = interrupt;
	state->host = *host;
	pmb887x_fifo16_init(&state->receive_fifo, SSC_FIFO_SIZE);
	pmb887x_fifo16_init(&state->transmit_fifo, SSC_FIFO_SIZE);
	return dsp_device_create(config, &ssc_ops, state);
}

void ssc_advance(dsp_device_t *device, size_t cycles) {
	ssc_state_t *state = device->state;

	while (cycles != 0 && ssc_running(state)) {
		if (!state->transfer_active && state->start_cycles != 0) {
			if (cycles < state->start_cycles) {
				state->start_cycles -= cycles;
				return;
			}
			cycles -= state->start_cycles;
			state->start_cycles = 0;
		}

		ssc_start_transfer(state);

		if (!state->transfer_active)
			return;

		if (cycles < state->transfer_cycles) {
			state->transfer_cycles -= cycles;
			state->bit_count = (state->transfer_cycles_total - state->transfer_cycles) /
				(2U * ((size_t) state->baud_rate + 1U));
			return;
		}

		cycles -= state->transfer_cycles;
		ssc_complete_transfer(state);
	}
}

bool ssc_is_active(const dsp_device_t *device) {
	const ssc_state_t *state = device->state;
	return ssc_running(state) && (state->transfer_active || state->transmit_fifo.base.count != 0);
}
