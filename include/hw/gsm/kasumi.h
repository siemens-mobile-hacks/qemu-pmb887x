/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#define KASUMI_KEY_BYTES 16
#define KASUMI_ROUNDS 8

typedef struct KasumiKeys KasumiKeys;
typedef struct KGCore KGCore;

struct KasumiKeys {
    uint16_t kl1[KASUMI_ROUNDS];
    uint16_t kl2[KASUMI_ROUNDS];
    uint16_t ko1[KASUMI_ROUNDS];
    uint16_t ko2[KASUMI_ROUNDS];
    uint16_t ko3[KASUMI_ROUNDS];
    uint16_t ki1[KASUMI_ROUNDS];
    uint16_t ki2[KASUMI_ROUNDS];
    uint16_t ki3[KASUMI_ROUNDS];
};

/**
 * KGCore: the KASUMI keystream generator of 3GPP TS 55.216 used by A5/3 and GEA3.
 */
struct KGCore {
    KasumiKeys keys;
    uint64_t modifier;
    uint64_t block;
    uint64_t block_count;
    uint8_t byte_index;
};

/**
 * kgcore_start: start the keystream for the 64-bit input register
 * CC || CB || CD || 00 || CA || CE and the 128-bit key @key.
 */
void kgcore_start(KGCore *kgcore, uint64_t input, const uint8_t key[KASUMI_KEY_BYTES]);

/**
 * kgcore_next_byte: return the next keystream byte, the first bit in the MSB.
 */
uint8_t kgcore_next_byte(KGCore *kgcore);
