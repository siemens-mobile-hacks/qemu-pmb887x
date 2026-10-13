#define PMB887X_TRACE_ID		DSP_AFE
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-afe"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"

#include "hw/arm/pmb887x/dsp/peripheral/afe-audio.h"
#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define AFE_REGISTER_COUNT	(TEAK_AFE_RINGCTRL + 1)
#define AFE_INTERRUPT_GROUP	1

static const uint16_t AFE_CONTROL_MASK = TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART |
	TEAK_AFE_BCON_RXRATE | TEAK_AFE_BCON_TXSTART | TEAK_AFE_BCON_TXRATE;

/*
 * The 0x80-word AFE RAM is split into two 0x40-word rings addressed by the
 * 6-bit RD/WR pointers. The DSP transmits its decoded playback samples to the
 * DAC through the upper ring (ram_base + 0x40), paced by the RX sample clock and
 * RXINTPTR; the lower ring (ram_base + 0) is the mic-ADC capture buffer. Host
 * audio-out must therefore read the upper (DAC) ring, not the lower one.
 */
#define AFE_DAC_RING_OFFSET	0x40U

#define AFE_DEFAULT_RATE_HZ	8000

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
	afe_audio_t *audio;
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
	afe_state_t *state = device->state;

	afe_audio_destroy(state->audio);
	g_free(device->state);
}

static void afe_reset(dsp_device_t *device) {
	afe_state_t *state = device->state;
	dsp_device_t *interrupt = state->interrupt;
	dsp_host_t host = state->host;
	uint16_t ram_base = state->ram_base;
	afe_audio_t *audio = state->audio;

	afe_audio_reset(audio);
	memset(state, 0, sizeof(*state));
	state->audio = audio;
	state->interrupt = interrupt;
	state->host = host;
	state->ram_base = ram_base;
	afe_audio_set_rate(audio, AFE_DEFAULT_RATE_HZ);
}

static bool afe_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	afe_state_t *state = device->state;

	switch (offset) {
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

static size_t afe_receive_sample_cycles(const afe_state_t *state) {
	switch (state->registers[TEAK_AFE_BCON] & TEAK_AFE_BCON_RXRATE) {
		case TEAK_AFE_BCON_RXRATE_KHZ_8:
			return 1000;
		case TEAK_AFE_BCON_RXRATE_KHZ_16:
			return 500;
		case TEAK_AFE_BCON_RXRATE_KHZ_31_746:
			return 252;
		case TEAK_AFE_BCON_RXRATE_KHZ_44_444:
			return 180;
		case TEAK_AFE_BCON_RXRATE_KHZ_47_619:
			return 168;
	}
	return 1000;
}

static unsigned afe_receive_sample_rate_hz(const afe_state_t *state) {
	switch (state->registers[TEAK_AFE_BCON] & TEAK_AFE_BCON_RXRATE) {
		case TEAK_AFE_BCON_RXRATE_KHZ_8:
			return 8000;
		case TEAK_AFE_BCON_RXRATE_KHZ_16:
			return 16000;
		case TEAK_AFE_BCON_RXRATE_KHZ_31_746:
			return 31746;
		case TEAK_AFE_BCON_RXRATE_KHZ_44_444:
			return 44444;
		case TEAK_AFE_BCON_RXRATE_KHZ_47_619:
			return 47619;
	}
	return AFE_DEFAULT_RATE_HZ;
}

static size_t afe_transmit_sample_cycles(const afe_state_t *state) {
	bool rate_16_khz = (state->registers[TEAK_AFE_BCON] & TEAK_AFE_BCON_TXRATE) ==
		TEAK_AFE_BCON_TXRATE_KHZ_16;
	return rate_16_khz ? 500 : 1000;
}

static bool afe_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	afe_state_t *state = device->state;

	switch (offset) {
		case TEAK_AFE_INTPTR:
			state->registers[offset] = value & (TEAK_AFE_INTPTR_RXINTPTR | TEAK_AFE_INTPTR_TXINTPTR);
			break;

		case TEAK_AFE_BCON: {
			uint16_t previous_control = state->registers[offset];

			state->registers[offset] = value & AFE_CONTROL_MASK;
			if (((previous_control ^ state->registers[offset]) & TEAK_AFE_BCON_RXRATE) != 0)
				afe_audio_set_rate(state->audio, afe_receive_sample_rate_hz(state));
			if (!afe_receive_active(state)) {
				state->receive_position = 0;
				state->receive_cycles = 0;
			}
			if (!afe_transmit_active(state)) {
				state->transmit_position = 0;
				state->transmit_cycles = 0;
			}
			break;
		}

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
	state->audio = afe_audio_create();
	return dsp_device_create(config, &afe_ops, state);
}

static bool afe_receive_sample(afe_state_t *state) {
	uint16_t interrupt_position = state->registers[TEAK_AFE_INTPTR] & TEAK_AFE_INTPTR_RXINTPTR;

	afe_audio_produce(state->audio, state->host.data_read(state->host.opaque,
		state->ram_base + AFE_DAC_RING_OFFSET + state->receive_position));

	state->receive_position++;
	state->receive_position &= TEAK_AFE_RWADDR_RDADDR;
	if (state->receive_position != interrupt_position)
		return false;

	dsp_int_set_flags(state->interrupt, AFE_INTERRUPT_GROUP, TEAK_INT_FINTB0_VBRX);
	return true;
}

static bool afe_transmit_sample(afe_state_t *state) {
	uint16_t interrupt_position = state->registers[TEAK_AFE_INTPTR] >> TEAK_AFE_INTPTR_TXINTPTR_SHIFT;
	bool power_down = (state->registers[TEAK_AFE_VTXCTRL] & TEAK_AFE_VTXCTRL_TXMODE) ==
		TEAK_AFE_VTXCTRL_TXMODE_POWER_DOWN;

	if (power_down) {
		state->host.data_write(state->host.opaque, state->ram_base + state->transmit_position,
			AFE_POWER_DOWN_SAMPLES[state->transmit_position]);
	} else {
		state->host.data_write(state->host.opaque, state->ram_base + state->transmit_position, 0);
	}

	state->transmit_position++;
	state->transmit_position &= TEAK_AFE_RWADDR_WRADDR >> TEAK_AFE_RWADDR_WRADDR_SHIFT;
	if (state->transmit_position != interrupt_position)
		return false;

	dsp_int_set_flags(state->interrupt, AFE_INTERRUPT_GROUP, TEAK_INT_FINTB0_VBTX);
	return true;
}

size_t afe_advance(dsp_device_t *device, size_t cycles) {
	afe_state_t *state = device->state;
	size_t remaining_cycles = cycles;

	while (remaining_cycles != 0) {
		bool receive_active = afe_receive_active(state);
		bool transmit_active = afe_transmit_active(state);

		if (!receive_active && !transmit_active)
			return cycles;

		size_t receive_period = afe_receive_sample_cycles(state);
		size_t transmit_period = afe_transmit_sample_cycles(state);
		bool interrupt = false;

		if (receive_active && state->receive_cycles >= receive_period) {
			state->receive_cycles -= receive_period;
			interrupt |= afe_receive_sample(state);
		}
		if (transmit_active && state->transmit_cycles >= transmit_period) {
			state->transmit_cycles -= transmit_period;
			interrupt |= afe_transmit_sample(state);
		}
		if (interrupt)
			break;

		size_t receive_step = receive_active ? receive_period - state->receive_cycles : SIZE_MAX;
		size_t transmit_step = transmit_active ? transmit_period - state->transmit_cycles : SIZE_MAX;
		size_t step = MIN(remaining_cycles, MIN(receive_step, transmit_step));

		if (receive_active)
			state->receive_cycles += step;
		if (transmit_active)
			state->transmit_cycles += step;
		remaining_cycles -= step;

		if (receive_active && state->receive_cycles == receive_period) {
			state->receive_cycles = 0;
			interrupt |= afe_receive_sample(state);
		}
		if (transmit_active && state->transmit_cycles == transmit_period) {
			state->transmit_cycles = 0;
			interrupt |= afe_transmit_sample(state);
		}
		if (interrupt)
			break;
	}
	return cycles - remaining_cycles;
}

bool afe_is_active(const dsp_device_t *device) {
	const afe_state_t *state = device->state;
	return afe_receive_active(state) || afe_transmit_active(state);
}
