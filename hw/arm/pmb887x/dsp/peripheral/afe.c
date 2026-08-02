#define PMB887X_TRACE_ID		DSP_AFE
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-afe"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define AFE_REGISTER_COUNT	(TEAK_AFE_RINGCTRL + 1)
#define AFE_CONTROL_MASK	(TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART | TEAK_AFE_BCON_RXRATE | \
	TEAK_AFE_BCON_TXSTART | TEAK_AFE_BCON_TXRATE)
#define AFE_SAMPLE_CYCLES	16U
#define AFE_INTERRUPT_GROUP	1

static const uint16_t AFE_POWER_DOWN_SAMPLES[] = {
	0x85EA, 0x85F3, 0xB12F, 0x8000, 0x9048, 0x8A3B, 0x81C2, 0x8BCF,
	0x8000, 0x8B30, 0x8290, 0x87AF, 0x8556, 0x85B5, 0x86BA, 0x84BF,
	0x875B, 0x8465, 0x8773, 0x848D, 0x870E, 0x8524, 0x8659, 0x85DF,
	0x85AE, 0x8669, 0x854D, 0x869E, 0x8544, 0x8682, 0x857A, 0x863A,
	0x85C9, 0x85ED, 0x860D, 0x85B8, 0x8632, 0x85A6, 0x8633, 0x85B3,
	0x861B, 0x85D1, 0x85FA, 0x85F1, 0x85DE, 0x8606, 0x85D0, 0x860D,
	0x85D1, 0x8606, 0x85DC, 0x85F9, 0x85EA, 0x85EC, 0x85F5, 0x85E3,
	0x85FB, 0x85E1, 0x85FA, 0x85E4, 0x85F5, 0x85EA, 0x85EF, 0x85F0,
};

typedef struct afe_state_t afe_state_t;

struct afe_state_t {
	uint16_t registers[AFE_REGISTER_COUNT];
	dsp_device_t *interrupt;
	dsp_host_t host;
	uint16_t ram_base;
	uint16_t receive_position;
	uint16_t transmit_position;
	size_t receive_cycles;
	size_t transmit_cycles;
};

static bool afe_receive_active(const afe_state_t *state) {
	uint16_t control = state->registers[TEAK_AFE_BCON];
	uint16_t active = TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART;
	return (control & active) == active;
}

static bool afe_transmit_active(const afe_state_t *state) {
	uint16_t control = state->registers[TEAK_AFE_BCON];
	uint16_t active = TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_TXSTART;
	return (control & active) == active;
}

static void afe_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void afe_reset(dsp_device_t *device) {
	afe_state_t *state = device->state;
	dsp_device_t *interrupt = state->interrupt;
	dsp_host_t host = state->host;
	uint16_t ram_base = state->ram_base;

	memset(state, 0, sizeof(*state));
	state->interrupt = interrupt;
	state->host = host;
	state->ram_base = ram_base;
}

static bool afe_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	afe_state_t *state = device->state;

	switch (offset) {
		case TEAK_AFE_INTPTR:
			*value = 0;
			break;

		case TEAK_AFE_RWADDR:
			*value = state->receive_position | state->transmit_position << TEAK_AFE_RWADDR_WRADDR_SHIFT;
			break;

		default:
			*value = offset < AFE_REGISTER_COUNT ? state->registers[offset] : 0;
			break;
	}

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool afe_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	afe_state_t *state = device->state;

	switch (offset) {
		case TEAK_AFE_INTPTR:
			state->registers[offset] = value & (TEAK_AFE_INTPTR_RXINTPTR | TEAK_AFE_INTPTR_TXINTPTR);
			break;

		case TEAK_AFE_BCON:
			state->registers[offset] = value & AFE_CONTROL_MASK;
			if (!afe_receive_active(state)) {
				state->receive_position = 0;
				state->receive_cycles = 0;
			}
			if (!afe_transmit_active(state)) {
				state->transmit_position = 0;
				state->transmit_cycles = 0;
			}
			break;

		case TEAK_AFE_RWADDR:
			break;

		default:
			if (offset < AFE_REGISTER_COUNT)
				state->registers[offset] = value;
			break;
	}

	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t afe_ops = {
	.destroy = afe_destroy,
	.reset = afe_reset,
	.read = afe_read,
	.write = afe_write,
};

dsp_device_t *afe_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt, const dsp_host_t *host) {
	afe_state_t *state = g_new0(afe_state_t, 1);
	state->interrupt = interrupt;
	state->host = *host;
	state->ram_base = config->ram_base;
	return dsp_device_create(config, &afe_ops, state);
}

void afe_advance(dsp_device_t *device, size_t cycles) {
	afe_state_t *state = device->state;

	if (afe_receive_active(state)) {
		uint16_t interrupt_position = state->registers[TEAK_AFE_INTPTR] & TEAK_AFE_INTPTR_RXINTPTR;

		state->receive_cycles += cycles;
		while (state->receive_cycles >= AFE_SAMPLE_CYCLES) {
			state->receive_cycles -= AFE_SAMPLE_CYCLES;
			state->receive_position++;
			state->receive_position &= TEAK_AFE_RWADDR_RDADDR;

			if (state->receive_position == interrupt_position) {
				dsp_int_set_flags(state->interrupt, AFE_INTERRUPT_GROUP, TEAK_INT_FINTB0_VBRX);
				state->receive_cycles = 0;
				break;
			}
		}
	}

	if (afe_transmit_active(state)) {
		uint16_t interrupt_position = state->registers[TEAK_AFE_INTPTR] >> TEAK_AFE_INTPTR_TXINTPTR_SHIFT;

		state->transmit_cycles += cycles;
		while (state->transmit_cycles >= AFE_SAMPLE_CYCLES) {
			bool power_down = (state->registers[TEAK_AFE_VTXCTRL] & TEAK_AFE_VTXCTRL_TXMODE) ==
				TEAK_AFE_VTXCTRL_TXMODE_POWER_DOWN;

			state->transmit_cycles -= AFE_SAMPLE_CYCLES;
			if (power_down) {
				state->host.data_write(state->host.opaque, state->ram_base + state->transmit_position,
					AFE_POWER_DOWN_SAMPLES[state->transmit_position]);
			} else {
				state->host.data_write(state->host.opaque, state->ram_base + state->transmit_position, 0);
			}

			state->transmit_position++;
			state->transmit_position &= TEAK_AFE_RWADDR_WRADDR >> TEAK_AFE_RWADDR_WRADDR_SHIFT;

			if (state->transmit_position == interrupt_position) {
				dsp_int_set_flags(state->interrupt, AFE_INTERRUPT_GROUP, TEAK_INT_FINTB0_VBTX);
				state->transmit_cycles = 0;
				break;
			}
		}
	}
}

bool afe_is_active(const dsp_device_t *device) {
	const afe_state_t *state = device->state;
	return afe_receive_active(state) || afe_transmit_active(state);
}
