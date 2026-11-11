/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/gsm/gsm-coding.h"

#define GSM_SOFT_BIT_THRESHOLD 128
#define GSM_TAIL_BITS 3
#define GSM_BURST_HALF_DATA_BITS 58
#define GSM_STEALING_FLAG_FIRST (GSM_BURST_HALF_DATA_BITS - 1)
#define GSM_STEALING_FLAG_SECOND GSM_BURST_HALF_DATA_BITS
#define GSM_STEALING_FLAG_BITS 2
#define GSM_NORMAL_TSC_START 61
#define GSM_NORMAL_TSC_MIN_MATCHES 23
#define GSM_ACCESS_EXTENDED_TAIL_BITS 8
#define GSM_ACCESS_SYNC_MIN_MATCHES 37

#define GSM_CONV_MEMORY 4
#define GSM_CONV_STATES (1 << GSM_CONV_MEMORY)
#define GSM_CONV_STATE_MASK (GSM_CONV_STATES - 1)
#define GSM_CONV_OLDEST_SHIFT (GSM_CONV_MEMORY - 1)

#define GSM_XCCH_DATA_BITS 184
#define GSM_XCCH_PARITY_BITS 40
#define GSM_XCCH_UNCODED_BITS (GSM_XCCH_DATA_BITS + GSM_XCCH_PARITY_BITS)
#define GSM_XCCH_CODED_BITS 456
#define GSM_XCCH_INTERLEAVE_COLUMNS 57
#define GSM_XCCH_INTERLEAVE_STEP 49
#define GSM_XCCH_INTERLEAVE_HALF_GROUP 4
#define GSM_XCCH_INTERLEAVE_GROUP 8
#define GSM_FIRE_POLYNOMIAL 0x0004820009ULL

#define GSM_RACH_DATA_BITS 8
#define GSM_RACH_PARITY_BITS 6
#define GSM_RACH_UNCODED_BITS (GSM_RACH_DATA_BITS + GSM_RACH_PARITY_BITS)
#define GSM_RACH_POLYNOMIAL 0x2F
#define GSM_BSIC_BITS 6

#define GSM_CONV_MAX_STEPS (GSM_XCCH_UNCODED_BITS + GSM_CONV_MEMORY)
#define GSM_CONV_UNREACHABLE_METRIC (2 * GSM_SOFT_BIT_ONE * GSM_CONV_MAX_STEPS + 1)

static const char *const GSM_NORMAL_TSC[GSM_TSC_COUNT] = {
    "00100101110000100010010111",
    "00101101110111100010110111",
    "01000011101110100100001110",
    "01000111101101000100011110",
    "00011010111001000001101011",
    "01001110101100000100111010",
    "10100111110110001010011111",
    "11101111000100101110111100",
};
static const char GSM_ACCESS_SYNC[] = "01001011011111111001100110101010001111000";
static const char GSM_DUMMY_BURST[] =
    "0001111101101110110000010100100111000001001000100000001111100011100010111000101110001010111010010100"
    "011001100111001111010011111000100101111101010000";

void gsm_normal_burst(GSMBurst *burst, uint8_t tsc, const uint8_t data[GSM_BURST_DATA_BITS]) {
    memset(burst->bits, 0, sizeof(burst->bits));
    memcpy(&burst->bits[GSM_TAIL_BITS], data, GSM_BURST_HALF_DATA_BITS);
    for (size_t i = 0; GSM_NORMAL_TSC[tsc][i] != '\0'; i++)
        burst->bits[GSM_NORMAL_TSC_START + i] = GSM_NORMAL_TSC[tsc][i] == '1';
    memcpy(&burst->bits[GSM_BURST_BITS - GSM_TAIL_BITS - GSM_BURST_HALF_DATA_BITS],
        &data[GSM_BURST_HALF_DATA_BITS], GSM_BURST_HALF_DATA_BITS);
}

void gsm_dummy_burst(GSMBurst *burst) {
    for (size_t i = 0; i < GSM_BURST_BITS; i++)
        burst->bits[i] = GSM_DUMMY_BURST[i] == '1';
}

static uint64_t gsm_crc(const uint8_t *bits, size_t count, uint64_t polynomial, uint32_t width) {
    uint64_t top = 1ULL << (width - 1);
    uint64_t mask = (top << 1) - 1;
    uint64_t crc = 0;

    for (size_t i = 0; i < count; i++) {
        crc ^= (uint64_t) bits[i] << (width - 1);
        crc = (crc & top) ? ((crc << 1) ^ polynomial) : (crc << 1);
        crc &= mask;
    }
    return crc ^ mask;
}

static void gsm_crc_to_bits(uint64_t crc, uint32_t width, uint8_t *bits) {
    for (uint32_t i = 0; i < width; i++)
        bits[i] = (crc >> (width - 1 - i)) & 1;
}

static uint8_t gsm_conv_output(uint32_t state, uint8_t input) {
    uint8_t d1 = state & 1;
    uint8_t d3 = (state >> 2) & 1;
    uint8_t d4 = (state >> 3) & 1;
    uint8_t g0 = input ^ d3 ^ d4;
    uint8_t g1 = input ^ d1 ^ d3 ^ d4;

    return (g0 << 1) | g1;
}

static uint32_t gsm_conv_next_state(uint32_t state, uint8_t input) {
    return ((state << 1) | input) & GSM_CONV_STATE_MASK;
}

static void gsm_conv_encode(const uint8_t *input, size_t count, uint8_t *coded) {
    uint32_t state = 0;

    for (size_t i = 0; i < count + GSM_CONV_MEMORY; i++) {
        uint8_t bit = i < count ? input[i] : 0;
        uint8_t output = gsm_conv_output(state, bit);

        coded[2 * i] = output >> 1;
        coded[2 * i + 1] = output & 1;
        state = gsm_conv_next_state(state, bit);
    }
}

static uint32_t gsm_soft_bit_cost(uint8_t soft_bit, uint8_t expected) {
    return expected ? GSM_SOFT_BIT_ONE - soft_bit : soft_bit;
}

static uint32_t gsm_conv_branch_metric(const uint8_t *soft_bits, uint32_t state, uint8_t input) {
    uint8_t expected = gsm_conv_output(state, input);

    return gsm_soft_bit_cost(soft_bits[0], expected >> 1) + gsm_soft_bit_cost(soft_bits[1], expected & 1);
}

static void gsm_conv_decode(const uint8_t *soft_bits, size_t count, uint8_t *output) {
    size_t steps = count + GSM_CONV_MEMORY;
    uint8_t decisions[GSM_CONV_MAX_STEPS][GSM_CONV_STATES];
    uint32_t metrics[GSM_CONV_STATES];
    uint32_t state = 0;

    for (uint32_t s = 0; s < GSM_CONV_STATES; s++)
        metrics[s] = s == 0 ? 0 : GSM_CONV_UNREACHABLE_METRIC;

    for (size_t i = 0; i < steps; i++) {
        uint32_t next[GSM_CONV_STATES];

        for (uint32_t to = 0; to < GSM_CONV_STATES; to++) {
            uint8_t input = to & 1;
            uint32_t from0 = to >> 1;
            uint32_t from1 = (to >> 1) | (1 << GSM_CONV_OLDEST_SHIFT);
            uint32_t metric0 = metrics[from0] + gsm_conv_branch_metric(&soft_bits[2 * i], from0, input);
            uint32_t metric1 = metrics[from1] + gsm_conv_branch_metric(&soft_bits[2 * i], from1, input);

            next[to] = MIN(metric0, metric1);
            decisions[i][to] = metric0 <= metric1 ? from0 : from1;
        }
        memcpy(metrics, next, sizeof(metrics));
    }

    for (size_t i = steps; i-- > 0;) {
        if (i < count)
            output[i] = state & 1;
        state = decisions[i][state];
    }
}

static void gsm_bytes_to_bits(const uint8_t *bytes, size_t bit_count, uint8_t *bits) {
    for (size_t i = 0; i < bit_count; i++)
        bits[i] = (bytes[i / 8] >> (i % 8)) & 1;
}

static void gsm_bits_to_bytes(const uint8_t *bits, size_t bit_count, uint8_t *bytes) {
    memset(bytes, 0, DIV_ROUND_UP(bit_count, 8));
    for (size_t i = 0; i < bit_count; i++)
        bytes[i / 8] |= bits[i] << (i % 8);
}

static size_t gsm_xcch_interleaved_position(size_t k, size_t *burst) {
    size_t bit = (2 * ((GSM_XCCH_INTERLEAVE_STEP * k) % GSM_XCCH_INTERLEAVE_COLUMNS)) +
        ((k % GSM_XCCH_INTERLEAVE_GROUP) / GSM_XCCH_INTERLEAVE_HALF_GROUP);

    *burst = k % GSM_XCCH_BLOCK_BURSTS;
    return bit < GSM_STEALING_FLAG_FIRST ? bit : bit + GSM_STEALING_FLAG_BITS;
}

void gsm_xcch_encode(
    const uint8_t block[GSM_XCCH_BLOCK_BYTES],
    uint8_t data[GSM_XCCH_BLOCK_BURSTS][GSM_BURST_DATA_BITS]
) {
    uint8_t uncoded[GSM_XCCH_UNCODED_BITS];
    uint8_t coded[GSM_XCCH_CODED_BITS];
    uint64_t parity;

    gsm_bytes_to_bits(block, GSM_XCCH_DATA_BITS, uncoded);
    parity = gsm_crc(uncoded, GSM_XCCH_DATA_BITS, GSM_FIRE_POLYNOMIAL, GSM_XCCH_PARITY_BITS);
    gsm_crc_to_bits(parity, GSM_XCCH_PARITY_BITS, &uncoded[GSM_XCCH_DATA_BITS]);
    gsm_conv_encode(uncoded, GSM_XCCH_UNCODED_BITS, coded);

    for (size_t k = 0; k < GSM_XCCH_CODED_BITS; k++) {
        size_t burst;
        size_t bit = gsm_xcch_interleaved_position(k, &burst);

        data[burst][bit] = coded[k];
    }
    for (size_t burst = 0; burst < GSM_XCCH_BLOCK_BURSTS; burst++) {
        data[burst][GSM_STEALING_FLAG_FIRST] = 1;
        data[burst][GSM_STEALING_FLAG_SECOND] = 1;
    }
}

bool gsm_xcch_decode(
    const uint8_t soft_bits[GSM_XCCH_BLOCK_BURSTS][GSM_BURST_DATA_BITS],
    uint8_t block[GSM_XCCH_BLOCK_BYTES]
) {
    uint8_t coded[GSM_XCCH_CODED_BITS];
    uint8_t uncoded[GSM_XCCH_UNCODED_BITS];
    uint8_t parity[GSM_XCCH_PARITY_BITS];

    for (size_t k = 0; k < GSM_XCCH_CODED_BITS; k++) {
        size_t burst;
        size_t bit = gsm_xcch_interleaved_position(k, &burst);

        coded[k] = soft_bits[burst][bit];
    }
    gsm_conv_decode(coded, GSM_XCCH_UNCODED_BITS, uncoded);
    gsm_crc_to_bits(gsm_crc(uncoded, GSM_XCCH_DATA_BITS, GSM_FIRE_POLYNOMIAL, GSM_XCCH_PARITY_BITS),
        GSM_XCCH_PARITY_BITS, parity);
    if (memcmp(parity, &uncoded[GSM_XCCH_DATA_BITS], sizeof(parity)) != 0)
        return false;

    gsm_bits_to_bytes(uncoded, GSM_XCCH_DATA_BITS, block);
    return true;
}

bool gsm_rach_decode(const uint8_t soft_bits[GSM_RACH_CODED_BITS], uint8_t bsic, uint8_t *ra) {
    uint8_t uncoded[GSM_RACH_UNCODED_BITS];
    uint8_t parity[GSM_RACH_PARITY_BITS];

    gsm_conv_decode(soft_bits, GSM_RACH_UNCODED_BITS, uncoded);
    gsm_crc_to_bits(gsm_crc(uncoded, GSM_RACH_DATA_BITS, GSM_RACH_POLYNOMIAL, GSM_RACH_PARITY_BITS),
        GSM_RACH_PARITY_BITS, parity);
    for (size_t i = 0; i < GSM_RACH_PARITY_BITS; i++) {
        uint8_t bsic_bit = (bsic >> (GSM_BSIC_BITS - 1 - i)) & 1;

        if ((parity[i] ^ bsic_bit) != uncoded[GSM_RACH_DATA_BITS + i])
            return false;
    }

    gsm_bits_to_bytes(uncoded, GSM_RACH_DATA_BITS, ra);
    return true;
}

static size_t gsm_count_matches(const uint8_t *soft_bits, const char *sequence) {
    size_t matches = 0;

    for (size_t i = 0; sequence[i] != '\0'; i++) {
        if (soft_bits[i] == GSM_SOFT_BIT_UNKNOWN)
            continue;
        if ((soft_bits[i] >= GSM_SOFT_BIT_THRESHOLD) == (sequence[i] == '1'))
            matches++;
    }
    return matches;
}

static bool gsm_uplink_find_sequence(
    const GSMUplinkBurst *burst,
    const char *sequence,
    size_t sequence_offset,
    size_t min_matches,
    size_t *start
) {
    size_t length = strlen(sequence);
    size_t best_matches = 0;

    for (size_t position = sequence_offset; position + length <= GSM_UPLINK_WINDOW_SYMBOLS; position++) {
        size_t matches = gsm_count_matches(&burst->soft_bits[position], sequence);

        if (matches > best_matches) {
            best_matches = matches;
            *start = position - sequence_offset;
        }
    }
    return best_matches >= min_matches;
}

bool gsm_uplink_find_normal_burst(const GSMUplinkBurst *burst, uint8_t tsc, size_t *start) {
    return gsm_uplink_find_sequence(burst, GSM_NORMAL_TSC[tsc], GSM_NORMAL_TSC_START, GSM_NORMAL_TSC_MIN_MATCHES,
        start);
}

bool gsm_uplink_find_access_burst(const GSMUplinkBurst *burst, size_t *start) {
    return gsm_uplink_find_sequence(burst, GSM_ACCESS_SYNC, GSM_ACCESS_EXTENDED_TAIL_BITS,
        GSM_ACCESS_SYNC_MIN_MATCHES, start);
}

static uint8_t gsm_uplink_soft_bit(const GSMUplinkBurst *burst, size_t position) {
    return position < GSM_UPLINK_WINDOW_SYMBOLS ? burst->soft_bits[position] : GSM_SOFT_BIT_UNKNOWN;
}

void gsm_uplink_normal_burst_data(const GSMUplinkBurst *burst, size_t start, uint8_t data[GSM_BURST_DATA_BITS]) {
    size_t second_half = GSM_BURST_BITS - GSM_TAIL_BITS - GSM_BURST_HALF_DATA_BITS;

    for (size_t i = 0; i < GSM_BURST_HALF_DATA_BITS; i++) {
        data[i] = gsm_uplink_soft_bit(burst, start + GSM_TAIL_BITS + i);
        data[GSM_BURST_HALF_DATA_BITS + i] = gsm_uplink_soft_bit(burst, start + second_half + i);
    }
}

void gsm_uplink_access_burst_data(const GSMUplinkBurst *burst, size_t start, uint8_t data[GSM_RACH_CODED_BITS]) {
    size_t data_start = start + GSM_ACCESS_EXTENDED_TAIL_BITS + strlen(GSM_ACCESS_SYNC);

    for (size_t i = 0; i < GSM_RACH_CODED_BITS; i++)
        data[i] = gsm_uplink_soft_bit(burst, data_start + i);
}
