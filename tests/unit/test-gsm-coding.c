/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/gsm/gsm-coding.h"

#define XCCH_BURST_CODED_BITS 114
#define XCCH_STEALING_FLAG_BIT 57
#define XCCH_STEALING_FLAG_BITS 2
#define XCCH_ERASED_BITS 60

typedef struct XCCHVector XCCHVector;
typedef struct RACHVector RACHVector;

struct XCCHVector {
    uint8_t block[GSM_XCCH_BLOCK_BYTES];
    const char *bursts[GSM_XCCH_BLOCK_BURSTS];
};

struct RACHVector {
    uint8_t ra;
    uint8_t bsic;
    const char *coded;
};

/* libosmocore tests/coding/coding_test.ok: the coded bits of each burst without the stealing flags. */
static const XCCHVector XCCH_VECTORS[] = {
    {
        .block = { 0x03, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        .bursts = {
            "100000010000100000000100000000000000000000101000010000001"
            "010000100000010000001000000101000010100000000000001000000",
            "000000010010000000010000000010000001000010100001000000000"
            "000000100000010000101000000000000000000001000000000001000",
            "000000000010000000010000100000000000001010000101000010100"
            "001000000101000000001001010000000000000100000010100001000",
            "010100000010000000000000100000010000000000000000000010000"
            "000010010000000010000001000000001000000000001000000000000",
        },
    },
    {
        .block = { 0xa3, 0xaf, 0x5f, 0xc6, 0x36, 0x43, 0x44, 0xab, 0xd9, 0x6d, 0x7d, 0x62, 0x24, 0xc9, 0xd2, 0x92, 0xfa, 0x27, 0x5d, 0x71, 0x7a, 0x59, 0xa8 },
        .bursts = {
            "100101111001010011001000001110111100100110011000010001010"
            "001010000001111000110110001010011010011101101010100000000",
            "101010010101101011000100001001011101101110100001100010101"
            "000000001101011111000011000010110011110101111111101010010",
            "001001110100011001111111100011010111101010110100100000010"
            "110111000100010000101000011001100101001000000011111111000",
            "100100110110111001001001101000011110001111000101111101011"
            "000101111010101001011000100010000011000011010010100001011",
        },
    },
    {
        .block = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17 },
        .bursts = {
            "111011100001111000010010111001000101001111000001010011101"
            "110111110010100101110100100110001010110111110101111000100",
            "001110111011000100010011110000011010111001101111100101001"
            "011000101000100010001101111101001010100111011101100010110",
            "000000001111110110011100001111111010110111011100100101101"
            "010101010010111011101110000010101000011111101110010011100",
            "110100100101000000000101110110000111001011111010101101110"
            "110110010111110101010011111011010010110000100000011111101",
        },
    },
};

/* libosmocoding gsm0503_rach_ext_encode() output for an 8-bit RA. */
static const RACHVector RACH_VECTORS[] = {
    { 0x00, 0x00, "000000000000000011101001101001000011" },
    { 0x7A, 0x00, "001101110101010111011111001001110011" },
    { 0x7A, 0x3F, "001101110101010100110110100000110000" },
    { 0xE5, 0x1A, "110111101100011001011110100001111111" },
};

static size_t coded_bit_position(size_t bit) {
    return bit < XCCH_STEALING_FLAG_BIT ? bit : bit + XCCH_STEALING_FLAG_BITS;
}

static void test_xcch_encode(void) {
    for (size_t v = 0; v < ARRAY_SIZE(XCCH_VECTORS); v++) {
        uint8_t data[GSM_XCCH_BLOCK_BURSTS][GSM_BURST_DATA_BITS];

        gsm_xcch_encode(XCCH_VECTORS[v].block, data);
        for (size_t burst = 0; burst < GSM_XCCH_BLOCK_BURSTS; burst++) {
            for (size_t bit = 0; bit < XCCH_BURST_CODED_BITS; bit++)
                g_assert_cmpuint(data[burst][coded_bit_position(bit)], ==, XCCH_VECTORS[v].bursts[burst][bit] == '1');
            for (size_t flag = 0; flag < XCCH_STEALING_FLAG_BITS; flag++)
                g_assert_cmpuint(data[burst][XCCH_STEALING_FLAG_BIT + flag], ==, 1);
        }
    }
}

static void test_xcch_decode_erased_bits(void) {
    for (size_t v = 0; v < ARRAY_SIZE(XCCH_VECTORS); v++) {
        uint8_t soft_bits[GSM_XCCH_BLOCK_BURSTS][GSM_BURST_DATA_BITS];
        uint8_t block[GSM_XCCH_BLOCK_BYTES];

        for (size_t burst = 0; burst < GSM_XCCH_BLOCK_BURSTS; burst++) {
            memset(soft_bits[burst], GSM_SOFT_BIT_ONE, GSM_BURST_DATA_BITS);
            for (size_t bit = 0; bit < XCCH_BURST_CODED_BITS; bit++) {
                bool one = XCCH_VECTORS[v].bursts[burst][bit] == '1';

                soft_bits[burst][coded_bit_position(bit)] = one ? GSM_SOFT_BIT_ONE : GSM_SOFT_BIT_ZERO;
            }
        }
        memset(soft_bits[0], GSM_SOFT_BIT_UNKNOWN, XCCH_ERASED_BITS);

        g_assert_true(gsm_xcch_decode(soft_bits, block));
        g_assert_cmpmem(block, sizeof(block), XCCH_VECTORS[v].block, sizeof(XCCH_VECTORS[v].block));
    }
}

static void test_xcch_decode_rejects_parity_error(void) {
    uint8_t data[GSM_XCCH_BLOCK_BURSTS][GSM_BURST_DATA_BITS];
    uint8_t soft_bits[GSM_XCCH_BLOCK_BURSTS][GSM_BURST_DATA_BITS];
    uint8_t block[GSM_XCCH_BLOCK_BYTES] = { 0 };
    uint8_t other[GSM_XCCH_BLOCK_BYTES] = { 0 };

    other[0] = 0x01;
    gsm_xcch_encode(other, data);
    gsm_xcch_encode(XCCH_VECTORS[0].block, soft_bits);
    for (size_t burst = 0; burst < GSM_XCCH_BLOCK_BURSTS; burst++) {
        for (size_t bit = 0; bit < GSM_BURST_DATA_BITS; bit++) {
            uint8_t hard = burst < GSM_XCCH_BLOCK_BURSTS / 2 ? soft_bits[burst][bit] : data[burst][bit];

            soft_bits[burst][bit] = hard ? GSM_SOFT_BIT_ONE : GSM_SOFT_BIT_ZERO;
        }
    }

    g_assert_false(gsm_xcch_decode(soft_bits, block));
}

static void test_rach_decode(void) {
    for (size_t v = 0; v < ARRAY_SIZE(RACH_VECTORS); v++) {
        uint8_t soft_bits[GSM_RACH_CODED_BITS];
        uint8_t ra = 0;

        for (size_t bit = 0; bit < GSM_RACH_CODED_BITS; bit++)
            soft_bits[bit] = RACH_VECTORS[v].coded[bit] == '1' ? GSM_SOFT_BIT_ONE : GSM_SOFT_BIT_ZERO;

        g_assert_true(gsm_rach_decode(soft_bits, RACH_VECTORS[v].bsic, &ra));
        g_assert_cmphex(ra, ==, RACH_VECTORS[v].ra);
        g_assert_false(gsm_rach_decode(soft_bits, RACH_VECTORS[v].bsic ^ 1, &ra));
    }
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/gsm-coding/xcch-encode", test_xcch_encode);
    g_test_add_func("/gsm-coding/xcch-decode-erased-bits", test_xcch_decode_erased_bits);
    g_test_add_func("/gsm-coding/xcch-decode-rejects-parity-error", test_xcch_decode_rejects_parity_error);
    g_test_add_func("/gsm-coding/rach-decode", test_rach_decode);
    return g_test_run();
}
