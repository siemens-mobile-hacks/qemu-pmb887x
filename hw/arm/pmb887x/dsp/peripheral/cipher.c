#define PMB887X_TRACE_ID		DSP_CIPHER
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-cipher"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "hw/arm/pmb887x/dsp/peripheral/cipher-kasumi.h"
#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define CIPHER_REGISTER_COUNT	(TEAK_CIPH_KDATA4 + 1)
#define CIPHER_STREAM_OFFSET	0x0020
#define CIPHER_GSM_BITS		114
#define CIPHER_EDGE_BITS		348
#define CIPHER_A512_GSM_CYCLES	4784
#define CIPHER_A512_EDGE_CYCLES	12480
#define CIPHER_A53_GSM_CYCLES	1872
#define CIPHER_A53_EDGE_CYCLES	3744
#define CIPHER_INTERRUPT_GROUP	2

#define A5_R1_LENGTH		19
#define A5_R2_LENGTH		22
#define A5_R3_LENGTH		23
#define A5_R4_LENGTH		17
#define A5_R1_MASK		((1U << A5_R1_LENGTH) - 1)
#define A5_R2_MASK		((1U << A5_R2_LENGTH) - 1)
#define A5_R3_MASK		((1U << A5_R3_LENGTH) - 1)
#define A5_R4_MASK		((1U << A5_R4_LENGTH) - 1)
#define A5_R1_TAPS		0x072000
#define A5_R2_TAPS		0x300000
#define A5_R3_TAPS		0x700080
#define A5_R4_TAPS		0x010800
#define A51_R1_CLOCK		0x000100
#define A51_R2_CLOCK		0x000400
#define A51_R3_CLOCK		0x000400
#define A52_R4_CLOCK0		0x000400
#define A52_R4_CLOCK1		0x000008
#define A52_R4_CLOCK2		0x000080

typedef struct cipher_state_t cipher_state_t;

struct cipher_state_t {
	uint16_t registers[CIPHER_REGISTER_COUNT];
	dsp_device_t *interrupt;
	dsp_host_t host;
	uint16_t ram_base;
	size_t cycles_remaining;
	bool active;
};

static uint32_t cipher_parity(uint32_t value) {
	value ^= value >> 16;
	value ^= value >> 8;
	value ^= value >> 4;
	return 0x6996U >> (value & 0x0F) & 1;
}

static bool cipher_majority(bool first, bool second, bool third) {
	return first + second + third >= 2;
}

static uint32_t cipher_clock_register(uint32_t value, uint32_t mask, uint32_t taps) {
	uint32_t shifted = value << 1 & mask;
	return shifted | cipher_parity(value & taps);
}

static void cipher_a51_clock(uint32_t registers[3], bool force) {
	bool clocks[3] = {
		(registers[0] & A51_R1_CLOCK) != 0,
		(registers[1] & A51_R2_CLOCK) != 0,
		(registers[2] & A51_R3_CLOCK) != 0,
	};
	bool majority = cipher_majority(clocks[0], clocks[1], clocks[2]);

	if (force || clocks[0] == majority)
		registers[0] = cipher_clock_register(registers[0], A5_R1_MASK, A5_R1_TAPS);
	if (force || clocks[1] == majority)
		registers[1] = cipher_clock_register(registers[1], A5_R2_MASK, A5_R2_TAPS);
	if (force || clocks[2] == majority)
		registers[2] = cipher_clock_register(registers[2], A5_R3_MASK, A5_R3_TAPS);
}

static uint8_t cipher_a51_output(const uint32_t registers[3]) {
	return registers[0] >> (A5_R1_LENGTH - 1) ^ registers[1] >> (A5_R2_LENGTH - 1) ^
		registers[2] >> (A5_R3_LENGTH - 1);
}

static void cipher_a52_clock(uint32_t registers[4], bool force) {
	bool clocks[3] = {
		(registers[3] & A52_R4_CLOCK0) != 0,
		(registers[3] & A52_R4_CLOCK1) != 0,
		(registers[3] & A52_R4_CLOCK2) != 0,
	};
	bool majority = cipher_majority(clocks[0], clocks[1], clocks[2]);

	if (force || clocks[0] == majority)
		registers[0] = cipher_clock_register(registers[0], A5_R1_MASK, A5_R1_TAPS);
	if (force || clocks[1] == majority)
		registers[1] = cipher_clock_register(registers[1], A5_R2_MASK, A5_R2_TAPS);
	if (force || clocks[2] == majority)
		registers[2] = cipher_clock_register(registers[2], A5_R3_MASK, A5_R3_TAPS);
	registers[3] = cipher_clock_register(registers[3], A5_R4_MASK, A5_R4_TAPS);
}

static uint8_t cipher_a52_output(const uint32_t registers[4]) {
	bool r1 = cipher_majority((registers[0] & 0x08000) != 0, (~registers[0] & 0x04000) != 0,
		(registers[0] & 0x01000) != 0);
	bool r2 = cipher_majority((~registers[1] & 0x10000) != 0, (registers[1] & 0x02000) != 0,
		(registers[1] & 0x00200) != 0);
	bool r3 = cipher_majority((registers[2] & 0x40000) != 0, (registers[2] & 0x10000) != 0,
		(~registers[2] & 0x02000) != 0);
	return (registers[0] >> (A5_R1_LENGTH - 1)) ^ (registers[1] >> (A5_R2_LENGTH - 1)) ^
		(registers[2] >> (A5_R3_LENGTH - 1)) ^ r1 ^ r2 ^ r3;
}

static void cipher_get_a512_key(const cipher_state_t *state, uint8_t key[8]) {
	for (size_t i = 0; i < 4; i++) {
		uint16_t word = state->registers[TEAK_CIPH_KEY0 + 3 - i];
		key[i * 2] = word >> 8;
		key[i * 2 + 1] = word;
	}
}

static uint32_t cipher_get_frame_count(const cipher_state_t *state) {
	uint32_t t1 = state->registers[TEAK_CIPH_SFNUM] & 0x07FF;
	uint32_t t2 = state->registers[TEAK_CIPH_TMOD26] & 0x001F;
	uint32_t t3 = state->registers[TEAK_CIPH_TMOD51] & 0x003F;
	return t1 << 11 | t3 << 5 | t2;
}

static void cipher_write_bit(cipher_state_t *state, uint16_t base, size_t bit, uint8_t value) {
	uint16_t address = base + bit / 16;
	uint16_t word = state->host.data_read(state->host.opaque, address);
	uint16_t mask = (uint16_t) BIT(bit % 16);

	if (value != 0) {
		word |= mask;
	} else {
		word &= ~mask;
	}

	state->host.data_write(state->host.opaque, address, word);
}

static void cipher_a51_generate(cipher_state_t *state, size_t stream_bits) {
	uint8_t key[8];
	uint32_t registers[3] = { 0, 0, 0 };
	uint32_t frame_count = cipher_get_frame_count(state);

	cipher_get_a512_key(state, key);
	for (size_t i = 0; i < 64; i++) {
		uint8_t bit = key[7 - i / 8] >> (i % 8) & 1;
		cipher_a51_clock(registers, true);
		registers[0] ^= bit;
		registers[1] ^= bit;
		registers[2] ^= bit;
	}

	for (size_t i = 0; i < 22; i++) {
		uint8_t bit = frame_count >> i & 1;
		cipher_a51_clock(registers, true);
		registers[0] ^= bit;
		registers[1] ^= bit;
		registers[2] ^= bit;
	}

	for (size_t i = 0; i < 100; i++)
		cipher_a51_clock(registers, false);

	for (size_t stream = 0; stream < 2; stream++) {
		uint16_t base = state->ram_base + stream * CIPHER_STREAM_OFFSET;
		for (size_t bit = 0; bit < stream_bits; bit++) {
			cipher_a51_clock(registers, false);
			cipher_write_bit(state, base, bit, cipher_a51_output(registers));
		}
	}
}

static void cipher_a52_generate(cipher_state_t *state) {
	uint8_t key[8];
	uint32_t registers[4] = { 0, 0, 0, 0 };
	uint32_t frame_count = cipher_get_frame_count(state);

	cipher_get_a512_key(state, key);
	for (size_t i = 0; i < 64; i++) {
		uint8_t bit = key[7 - i / 8] >> (i % 8) & 1;
		cipher_a52_clock(registers, true);
		for (size_t j = 0; j < 4; j++)
			registers[j] ^= bit;
	}

	for (size_t i = 0; i < 22; i++) {
		uint8_t bit = frame_count >> i & 1;
		cipher_a52_clock(registers, true);
		for (size_t j = 0; j < 4; j++)
			registers[j] ^= bit;
	}

	registers[0] |= BIT(15);
	registers[1] |= BIT(16);
	registers[2] |= BIT(18);
	registers[3] |= BIT(10);

	for (size_t i = 0; i < 99; i++)
		cipher_a52_clock(registers, false);

	for (size_t stream = 0; stream < 2; stream++) {
		uint16_t base = state->ram_base + stream * CIPHER_STREAM_OFFSET;
		for (size_t bit = 0; bit < CIPHER_GSM_BITS; bit++) {
			cipher_a52_clock(registers, false);
			cipher_write_bit(state, base, bit, cipher_a52_output(registers));
		}
	}
}

static void cipher_a53_generate(cipher_state_t *state) {
	static const uint8_t key_registers[] = {
		TEAK_CIPH_KEY6, TEAK_CIPH_KEY7, TEAK_CIPH_KEY4, TEAK_CIPH_KEY5,
		TEAK_CIPH_KEY2, TEAK_CIPH_KEY3, TEAK_CIPH_KEY0, TEAK_CIPH_KEY1,
	};
	uint8_t key[16];
	uint8_t output[DIV_ROUND_UP(CIPHER_GSM_BITS * 2, 8)];
	uint16_t kdata2 = state->registers[TEAK_CIPH_KDATA2];
	uint16_t kdata3 = state->registers[TEAK_CIPH_KDATA3];
	uint16_t kdata4 = state->registers[TEAK_CIPH_KDATA4];
	uint8_t ca = state->registers[TEAK_CIPH_KDATA1] >> 8;
	uint8_t cb = kdata2 >> 11;
	uint8_t cd = kdata2 >> 10 & 1;
	uint16_t ce = 0;
	uint32_t frame_count = ((uint32_t) kdata3 & 0xFF00) << 8 | kdata4;

	for (size_t group = 0; group < 4; group++) {
		size_t key_offset = group * 4;
		uint16_t even = state->registers[key_registers[group * 2]];
		uint16_t odd = state->registers[key_registers[group * 2 + 1]];
		key[key_offset] = even;
		key[key_offset + 1] = even >> 8;
		key[key_offset + 2] = odd >> 8;
		key[key_offset + 3] = odd;
	}

	cipher_kgcore(ca, cb, frame_count, cd, ce, key, output, CIPHER_GSM_BITS * 2);

	for (size_t stream = 0; stream < 2; stream++) {
		uint16_t base = state->ram_base + stream * CIPHER_STREAM_OFFSET;
		for (size_t bit = 0; bit < CIPHER_GSM_BITS; bit++) {
			size_t output_bit = stream * CIPHER_GSM_BITS + CIPHER_GSM_BITS - 1 - bit;
			uint8_t value = output[output_bit / 8] >> (7 - output_bit % 8) & 1;
			cipher_write_bit(state, base, bit, value);
		}
	}
}

static size_t cipher_operation_cycles(uint16_t control) {
	bool edge = (control & TEAK_CIPH_CSTAT_EDGE) != 0;

	if ((control & TEAK_CIPH_CSTAT_A53) != 0)
		return edge ? CIPHER_A53_EDGE_CYCLES : CIPHER_A53_GSM_CYCLES;
	return edge ? CIPHER_A512_EDGE_CYCLES : CIPHER_A512_GSM_CYCLES;
}

static void cipher_run(cipher_state_t *state) {
	uint16_t control = state->registers[TEAK_CIPH_CSTAT];

	if ((control & TEAK_CIPH_CSTAT_A53) != 0) {
		cipher_a53_generate(state);
	} else if ((control & TEAK_CIPH_CSTAT_A52) != 0) {
		cipher_a52_generate(state);
	} else {
		size_t stream_bits = (control & TEAK_CIPH_CSTAT_EDGE) != 0 ? CIPHER_EDGE_BITS : CIPHER_GSM_BITS;
		cipher_a51_generate(state, stream_bits);
	}

	state->registers[TEAK_CIPH_CSTAT] &= ~(TEAK_CIPH_CSTAT_CACT | TEAK_CIPH_CSTAT_INIT);
	state->cycles_remaining = 0;
	state->active = false;

	dsp_int_set_flags(state->interrupt, CIPHER_INTERRUPT_GROUP, TEAK_INT_FINT1_CIPH);
}

static void cipher_destroy(dsp_device_t *device) {
	g_free(device->state);
}

static void cipher_reset(dsp_device_t *device) {
	cipher_state_t *state = device->state;
	memset(state->registers, 0, sizeof(state->registers));
	state->cycles_remaining = 0;
	state->active = false;
}

static bool cipher_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	cipher_state_t *state = device->state;

	*value = offset < CIPHER_REGISTER_COUNT ? state->registers[offset] : 0;

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool cipher_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	cipher_state_t *state = device->state;

	switch (offset) {
		case TEAK_CIPH_CSTAT: {
			uint16_t a53_start_mask = TEAK_CIPH_CSTAT_A53 | TEAK_CIPH_CSTAT_INIT;
			bool a53_start = (value & a53_start_mask) == a53_start_mask;
			bool a512_start = (value & TEAK_CIPH_CSTAT_A53) == 0 && (value & TEAK_CIPH_CSTAT_CACT) != 0;

			state->registers[offset] = value;
			state->active = a53_start || a512_start;
			if (state->active)
				state->cycles_remaining = cipher_operation_cycles(value);
			break;
		}

		default:
			if (offset < CIPHER_REGISTER_COUNT)
				state->registers[offset] = value;
			break;
	}

	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t cipher_ops = {
	.destroy = cipher_destroy,
	.reset = cipher_reset,
	.read = cipher_read,
	.write = cipher_write,
};

dsp_device_t *cipher_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt, const dsp_host_t *host) {
	cipher_state_t *state = g_new0(cipher_state_t, 1);
	state->interrupt = interrupt;
	state->host = *host;
	state->ram_base = config->ram_base;
	return dsp_device_create(config, &cipher_ops, state);
}

void cipher_advance(dsp_device_t *device, size_t cycles) {
	cipher_state_t *state = device->state;

	if (!state->active)
		return;

	if (cycles < state->cycles_remaining) {
		state->cycles_remaining -= cycles;
		return;
	}

	cipher_run(state);
}

bool cipher_is_active(const dsp_device_t *device) {
	cipher_state_t *state = device->state;
	return state->active;
}
