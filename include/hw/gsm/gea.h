/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#define GEA_MAX_REGISTERS 4

typedef enum GEAAlgorithm GEAAlgorithm;
typedef struct GEAKeystream GEAKeystream;

enum GEAAlgorithm {
    GEA_ALGORITHM_GEA1,
    GEA_ALGORITHM_GEA2,
};

/**
 * GEAKeystream: GEA-1 or GEA-2 keystream generator.
 * @registers: the LFSRs A, B, C and, for GEA-2, D.
 */
struct GEAKeystream {
    uint64_t registers[GEA_MAX_REGISTERS];
    uint8_t register_count;
};

/**
 * gea_start: start the @algorithm keystream for the 32-bit cipher input @input,
 * the transfer @direction and the 64-bit key @kc whose first key byte is the MSB.
 */
void gea_start(GEAKeystream *keystream, GEAAlgorithm algorithm, uint32_t input, bool direction, uint64_t kc);

/**
 * gea_next_byte: return the next keystream byte, the first bit in the LSB.
 */
uint8_t gea_next_byte(GEAKeystream *keystream);
