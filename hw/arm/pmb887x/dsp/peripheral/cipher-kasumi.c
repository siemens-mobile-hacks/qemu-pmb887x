/*
 * KASUMI and KGCORE implementation based on 3GPP TS 35.201 and TS 35.202.
 *
 * Copyright (C) 2013 Max <Max.Suraev@fairwaves.ru>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/arm/pmb887x/dsp/peripheral/cipher-kasumi.h"

static const uint16_t KASUMI_S7[] = {
	54, 50, 62, 56, 22, 34, 94, 96, 38, 6, 63, 93, 2, 18, 123, 33,
	55, 113, 39, 114, 21, 67, 65, 12, 47, 73, 46, 27, 25, 111, 124, 81,
	53, 9, 121, 79, 52, 60, 58, 48, 101, 127, 40, 120, 104, 70, 71, 43,
	20, 122, 72, 61, 23, 109, 13, 100, 77, 1, 16, 7, 82, 10, 105, 98,
	117, 116, 76, 11, 89, 106, 0, 125, 118, 99, 86, 69, 30, 57, 126, 87,
	112, 51, 17, 5, 95, 14, 90, 84, 91, 8, 35, 103, 32, 97, 28, 66,
	102, 31, 26, 45, 75, 4, 85, 92, 37, 74, 80, 49, 68, 29, 115, 44,
	64, 107, 108, 24, 110, 83, 36, 78, 42, 19, 15, 41, 88, 119, 59, 3,
};

static const uint16_t KASUMI_S9[] = {
	167, 239, 161, 379, 391, 334, 9, 338, 38, 226, 48, 358, 452, 385, 90, 397,
	183, 253, 147, 331, 415, 340, 51, 362, 306, 500, 262, 82, 216, 159, 356, 177,
	175, 241, 489, 37, 206, 17, 0, 333, 44, 254, 378, 58, 143, 220, 81, 400,
	95, 3, 315, 245, 54, 235, 218, 405, 472, 264, 172, 494, 371, 290, 399, 76,
	165, 197, 395, 121, 257, 480, 423, 212, 240, 28, 462, 176, 406, 507, 288, 223,
	501, 407, 249, 265, 89, 186, 221, 428, 164, 74, 440, 196, 458, 421, 350, 163,
	232, 158, 134, 354, 13, 250, 491, 142, 191, 69, 193, 425, 152, 227, 366, 135,
	344, 300, 276, 242, 437, 320, 113, 278, 11, 243, 87, 317, 36, 93, 496, 27,
	487, 446, 482, 41, 68, 156, 457, 131, 326, 403, 339, 20, 39, 115, 442, 124,
	475, 384, 508, 53, 112, 170, 479, 151, 126, 169, 73, 268, 279, 321, 168, 364,
	363, 292, 46, 499, 393, 327, 324, 24, 456, 267, 157, 460, 488, 426, 309, 229,
	439, 506, 208, 271, 349, 401, 434, 236, 16, 209, 359, 52, 56, 120, 199, 277,
	465, 416, 252, 287, 246, 6, 83, 305, 420, 345, 153, 502, 65, 61, 244, 282,
	173, 222, 418, 67, 386, 368, 261, 101, 476, 291, 195, 430, 49, 79, 166, 330,
	280, 383, 373, 128, 382, 408, 155, 495, 367, 388, 274, 107, 459, 417, 62, 454,
	132, 225, 203, 316, 234, 14, 301, 91, 503, 286, 424, 211, 347, 307, 140, 374,
	35, 103, 125, 427, 19, 214, 453, 146, 498, 314, 444, 230, 256, 329, 198, 285,
	50, 116, 78, 410, 10, 205, 510, 171, 231, 45, 139, 467, 29, 86, 505, 32,
	72, 26, 342, 150, 313, 490, 431, 238, 411, 325, 149, 473, 40, 119, 174, 355,
	185, 233, 389, 71, 448, 273, 372, 55, 110, 178, 322, 12, 469, 392, 369, 190,
	1, 109, 375, 137, 181, 88, 75, 308, 260, 484, 98, 272, 370, 275, 412, 111,
	336, 318, 4, 504, 492, 259, 304, 77, 337, 435, 21, 357, 303, 332, 483, 18,
	47, 85, 25, 497, 474, 289, 100, 269, 296, 478, 270, 106, 31, 104, 433, 84,
	414, 486, 394, 96, 99, 154, 511, 148, 413, 361, 409, 255, 162, 215, 302, 201,
	266, 351, 343, 144, 441, 365, 108, 298, 251, 34, 182, 509, 138, 210, 335, 133,
	311, 352, 328, 141, 396, 346, 123, 319, 450, 281, 429, 228, 443, 481, 92, 404,
	485, 422, 248, 297, 23, 213, 130, 466, 22, 217, 283, 70, 294, 360, 419, 127,
	312, 377, 7, 468, 194, 2, 117, 295, 463, 258, 224, 447, 247, 187, 80, 398,
	284, 353, 105, 390, 299, 471, 470, 184, 57, 200, 348, 63, 204, 188, 33, 451,
	97, 30, 310, 219, 94, 160, 129, 493, 64, 179, 263, 102, 189, 207, 114, 402,
	438, 477, 387, 122, 192, 42, 381, 5, 145, 118, 180, 449, 293, 323, 136, 380,
	43, 66, 60, 455, 341, 445, 202, 432, 8, 237, 15, 376, 436, 464, 59, 461,
};

typedef struct kasumi_keys_t kasumi_keys_t;

struct kasumi_keys_t {
	uint16_t kl1[8];
	uint16_t kl2[8];
	uint16_t ko1[8];
	uint16_t ko2[8];
	uint16_t ko3[8];
	uint16_t ki1[8];
	uint16_t ki2[8];
	uint16_t ki3[8];
};

static uint16_t kasumi_load_be16(const uint8_t *data) {
	return (uint16_t) data[0] << 8 | data[1];
}

static uint16_t kasumi_rotate16(uint16_t value, unsigned shift) {
	return value << shift | value >> (16 - shift);
}

static uint16_t kasumi_fi(uint16_t input, uint16_t key) {
	uint16_t left = input >> 7;
	uint16_t right = input & 0x7F;

	left = KASUMI_S9[left] ^ right;
	right = KASUMI_S7[right] ^ (left & 0x7F);
	left ^= key & 0x01FF;
	right ^= key >> 9;
	left = KASUMI_S9[left] ^ right;
	right = KASUMI_S7[right] ^ (left & 0x7F);
	return right << 9 | left;
}

static uint32_t kasumi_fo(uint32_t input, const kasumi_keys_t *keys, size_t round) {
	uint16_t left = input >> 16;
	uint16_t right = input;

	left = kasumi_fi(left ^ keys->ko1[round], keys->ki1[round]) ^ right;
	right = kasumi_fi(right ^ keys->ko2[round], keys->ki2[round]) ^ left;
	left = kasumi_fi(left ^ keys->ko3[round], keys->ki3[round]) ^ right;
	return (uint32_t) right << 16 | left;
}

static uint32_t kasumi_fl(uint32_t input, const kasumi_keys_t *keys, size_t round) {
	uint16_t left = input >> 16;
	uint16_t right = input;

	right ^= kasumi_rotate16(left & keys->kl1[round], 1);
	left ^= kasumi_rotate16(right | keys->kl2[round], 1);
	return (uint32_t) left << 16 | right;
}

static uint64_t kasumi_encrypt(uint64_t input, const kasumi_keys_t *keys) {
	uint32_t left = input >> 32;
	uint32_t right = input;

	for (size_t round = 0; round < 8; round += 2) {
		right ^= kasumi_fo(kasumi_fl(left, keys, round), keys, round);
		left ^= kasumi_fl(kasumi_fo(right, keys, round + 1), keys, round + 1);
	}
	return (uint64_t) left << 32 | right;
}

static void kasumi_expand_key(const uint8_t key[16], kasumi_keys_t *keys) {
	static const uint16_t CONSTANTS[8] = { 0x0123, 0x4567, 0x89AB, 0xCDEF, 0xFEDC, 0xBA98, 0x7654, 0x3210 };
	uint16_t prime[8];

	for (size_t i = 0; i < 8; i++)
		prime[i] = kasumi_load_be16(key + i * 2) ^ CONSTANTS[i];

	for (size_t i = 0; i < 8; i++) {
		keys->kl1[i] = kasumi_rotate16(kasumi_load_be16(key + i * 2), 1);
		keys->kl2[i] = prime[(i + 2) & 7];
		keys->ko1[i] = kasumi_rotate16(kasumi_load_be16(key + ((2 * (i + 1)) & 0x0E)), 5);
		keys->ko2[i] = kasumi_rotate16(kasumi_load_be16(key + ((2 * (i + 5)) & 0x0E)), 8);
		keys->ko3[i] = kasumi_rotate16(kasumi_load_be16(key + ((2 * (i + 6)) & 0x0E)), 13);
		keys->ki1[i] = prime[(i + 4) & 7];
		keys->ki2[i] = prime[(i + 3) & 7];
		keys->ki3[i] = prime[(i + 7) & 7];
	}
}

static void kasumi_store_be64(uint8_t *data, uint64_t value) {
	for (size_t i = 0; i < 8; i++)
		data[i] = value >> (56 - i * 8);
}

void cipher_kgcore(
	uint8_t ca, uint8_t cb, uint32_t cc, uint8_t cd, uint16_t ce,
	const uint8_t key[16], uint8_t *output, size_t bits
) {
	kasumi_keys_t keys;
	uint8_t modified_key[16];
	uint64_t input = (uint64_t) cc << 32 | (uint64_t) ((cb << 3) | (cd << 2)) << 24 |
		(uint64_t) ca << 16 | ce;
	uint64_t block = 0;

	for (size_t i = 0; i < 16; i++)
		modified_key[i] = key[i] ^ 0x55;

	kasumi_expand_key(modified_key, &keys);
	input = kasumi_encrypt(input, &keys);

	kasumi_expand_key(key, &keys);

	for (size_t offset = 0; offset < bits; offset += 64) {
		size_t block_index = offset / 64;
		size_t remaining = MIN(bits - offset, 64);
		uint8_t encoded[8];

		block = kasumi_encrypt(input ^ block_index ^ block, &keys);
		kasumi_store_be64(encoded, block);
		memcpy(output + offset / 8, encoded, DIV_ROUND_UP(remaining, 8));
	}
}
