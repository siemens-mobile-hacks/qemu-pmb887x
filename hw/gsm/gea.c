/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "hw/gsm/gea.h"

/*
 * C. Beierle et al., "Cryptanalysis of the GPRS Encryption Algorithms GEA-1 and GEA-2",
 * EUROCRYPT 2021, https://eprint.iacr.org/2021/819.
 */
#define GEA_FILTER_INPUTS 7
#define GEA_INPUT_BITS 32
#define GEA_KEY_BITS 64
#define GEA_INITIALIZER_MAX_BITS 97

typedef struct GEARegister GEARegister;
typedef struct GEAInitializer GEAInitializer;
typedef struct GEAInitRegister GEAInitRegister;

struct GEARegister {
    uint8_t length;
    uint64_t taps;
    uint8_t filter_inputs[GEA_FILTER_INPUTS];
};

struct GEAInitializer {
    uint8_t length;
    uint8_t filter_inputs[GEA_FILTER_INPUTS];
    uint16_t mixing_clocks;
    uint8_t register_count;
    uint8_t load_rotations[GEA_MAX_REGISTERS];
};

/* Bit index (head + n) % length of bits holds register bit n. */
struct GEAInitRegister {
    uint8_t bits[GEA_INITIALIZER_MAX_BITS];
    uint8_t length;
    uint8_t head;
};

/* Truth table of the filter function f: bit x is f(x0, ..., x6) with xi = bit i of x. */
static const uint64_t GEA_FILTER[2] = { 0x09E2DE35F5B4DD9CULL, 0xFC1D2B0681ACA948ULL };

static const GEARegister GEA_REGISTERS[GEA_MAX_REGISTERS] = {
    { 31, 0x2C7646EE, { 8, 30, 17, 9, 5, 28, 23 } },
    { 32, 0x510781C7, { 19, 4, 31, 30, 2, 10, 26 } },
    { 33, 0x245F670A, { 22, 2, 0, 29, 13, 32, 28 } },
    { 29, 0x09FD59A5, { 16, 5, 25, 28, 18, 1, 11 } },
};

static const GEAInitializer GEA_INITIALIZERS[] = {
    [GEA_ALGORITHM_GEA1] = { 64, { 60, 51, 41, 25, 21, 8, 0 }, 128, 3, { 0, 16, 32 } },
    [GEA_ALGORITHM_GEA2] = { 97, { 92, 78, 63, 39, 33, 13, 0 }, 194, 4, { 16, 33, 51, 0 } },
};

static bool gea_filter(unsigned inputs) {
    return (GEA_FILTER[inputs / 64] >> (inputs % 64)) & 1;
}

static bool gea_init_bit(const GEAInitRegister *s, unsigned index) {
    return s->bits[(s->head + index) % s->length];
}

static void gea_clock_initializer(GEAInitRegister *s, const GEAInitializer *initializer, bool input) {
    unsigned filter_inputs = 0;

    for (unsigned i = 0; i < GEA_FILTER_INPUTS; i++)
        filter_inputs |= gea_init_bit(s, initializer->filter_inputs[i]) << i;
    bool feedback = gea_filter(filter_inputs) ^ gea_init_bit(s, s->length - 1) ^ input;

    s->head = (s->head + s->length - 1) % s->length;
    s->bits[s->head] = feedback;
}

static void gea_clock_register(uint64_t *state, const GEARegister *lfsr, bool input) {
    bool feedback = ((*state >> (lfsr->length - 1)) & 1) ^ input;

    if (feedback)
        *state ^= lfsr->taps;
    *state = ((*state << 1) | feedback) & MAKE_64BIT_MASK(0, lfsr->length);
}

static bool gea_register_output(uint64_t state, const GEARegister *lfsr) {
    unsigned filter_inputs = 0;

    for (unsigned i = 0; i < GEA_FILTER_INPUTS; i++)
        filter_inputs |= ((state >> lfsr->filter_inputs[i]) & 1) << i;
    return gea_filter(filter_inputs);
}

void gea_start(GEAKeystream *keystream, GEAAlgorithm algorithm, uint32_t input, bool direction, uint64_t kc) {
    const GEAInitializer *initializer = &GEA_INITIALIZERS[algorithm];
    GEAInitRegister s = { .length = initializer->length };

    for (unsigned i = 0; i < GEA_INPUT_BITS; i++)
        gea_clock_initializer(&s, initializer, (input >> i) & 1);
    gea_clock_initializer(&s, initializer, direction);
    for (unsigned i = 0; i < GEA_KEY_BITS; i++)
        gea_clock_initializer(&s, initializer, (kc >> i) & 1);
    for (unsigned i = 0; i < initializer->mixing_clocks; i++)
        gea_clock_initializer(&s, initializer, false);

    keystream->register_count = initializer->register_count;
    for (unsigned r = 0; r < keystream->register_count; r++) {
        const GEARegister *lfsr = &GEA_REGISTERS[r];
        unsigned rotation = initializer->load_rotations[r];
        uint64_t *state = &keystream->registers[r];

        *state = 0;
        for (int bit = s.length - 1; bit >= 0; bit--)
            gea_clock_register(state, lfsr, gea_init_bit(&s, (bit + s.length - rotation) % s.length));
        if (*state == 0)
            *state = BIT_ULL(lfsr->length - 1);
    }
}

uint8_t gea_next_byte(GEAKeystream *keystream) {
    uint8_t byte = 0;

    for (unsigned bit = 0; bit < 8; bit++) {
        bool output = false;

        for (unsigned r = 0; r < keystream->register_count; r++)
            output ^= gea_register_output(keystream->registers[r], &GEA_REGISTERS[r]);
        byte |= output << bit;
        for (unsigned r = 0; r < keystream->register_count; r++)
            gea_clock_register(&keystream->registers[r], &GEA_REGISTERS[r], false);
    }
    return byte;
}
