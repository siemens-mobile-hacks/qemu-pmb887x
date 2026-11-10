/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/host-utils.h"
#include "hw/gsm/gsm-air.h"

#define GSM_HYPERFRAME_FRAMES 2715648U
#define GSM_FRAME_QUARTER_SYMBOLS 5000U
#define GSM_TIMESLOT_QUARTER_SYMBOLS 625U
#define GSM_QUARTER_SYMBOLS_PER_NS_NUMERATOR 13U
#define GSM_QUARTER_SYMBOLS_PER_NS_DENOMINATOR 12000U
#define GSM_GMSK_SAMPLES_PER_SYMBOL 4U
#define GSM_GMSK_COEFFICIENT_SYMBOLS (GSM_BURST_BITS + 2U)
#define GSM900_DOWNLINK_ARFCN0_HZ 935000000U
#define GSM_CHANNEL_SPACING_HZ 200000U
#define GSM900_HIGHEST_ARFCN 124
#define EGSM900_LOWEST_ARFCN 975
#define GSM_ARFCN_COUNT 1024

typedef struct GSMComplex GSMComplex;

struct GSMComplex {
    double real;
    double imaginary;
};

/* Four-sample Laurent C0/C1 pulse decomposition for BT=0.3 GMSK. */
static const double GSM_GMSK_LAURENT_C0[] = {
    0.0, 4.46348606e-03, 2.84385729e-02, 1.03184855e-01,
    2.56065552e-01, 4.76375085e-01, 7.05961177e-01, 8.71291644e-01,
    9.29453645e-01, 8.71291644e-01, 7.05961177e-01, 4.76375085e-01,
    2.56065552e-01, 1.03184855e-01, 2.84385729e-02, 4.46348606e-03,
};
static const double GSM_GMSK_LAURENT_C1[] = {
    0.0, 8.16373112e-03, 2.84385729e-02, 5.64158904e-02,
    7.05463553e-02, 5.64158904e-02, 2.84385729e-02, 8.16373112e-03,
};

static bool gsm_air_frequency_to_arfcn(uint32_t frequency_hz, uint16_t *arfcn) {
    int64_t offset_hz = (int64_t) frequency_hz - GSM900_DOWNLINK_ARFCN0_HZ;

    if (offset_hz % GSM_CHANNEL_SPACING_HZ != 0)
        return false;

    int64_t channel = offset_hz / GSM_CHANNEL_SPACING_HZ;
    if (channel > GSM900_HIGHEST_ARFCN || channel < EGSM900_LOWEST_ARFCN - GSM_ARFCN_COUNT)
        return false;

    *arfcn = channel >= 0 ? channel : channel + GSM_ARFCN_COUNT;
    return true;
}

static bool gsm_air_load_burst(GSMAir *air, uint16_t arfcn, uint32_t frame_number, uint8_t timeslot) {
    bool same_burst = air->burst_cached && air->burst_arfcn == arfcn &&
        air->burst_frame_number == frame_number && air->burst_timeslot == timeslot;

    if (same_burst)
        return air->burst_present;

    GSMNetworkClass *network_class = GSM_NETWORK_GET_CLASS(air->network);
    air->burst_present = network_class->downlink_burst(air->network, arfcn, frame_number, timeslot, &air->burst);
    air->burst_arfcn = arfcn;
    air->burst_frame_number = frame_number;
    air->burst_timeslot = timeslot;
    air->burst_cached = true;
    return air->burst_present;
}

static bool gsm_gmsk_raw_bit(const GSMBurst *burst, uint32_t coefficient_symbol) {
    if (coefficient_symbol == 0U || coefficient_symbol == GSM_BURST_BITS + 1U)
        return false;

    return burst->bits[coefficient_symbol - 1U];
}

static GSMComplex gsm_gmsk_c0_coefficient(const GSMBurst *burst, uint32_t coefficient_symbol) {
    int32_t value = gsm_gmsk_raw_bit(burst, coefficient_symbol) ? 1 : -1;
    GSMComplex coefficient = { 0.0, 0.0 };

    switch (coefficient_symbol & 3U) {
        case 0:
            coefficient.real = value;
            break;

        case 1:
            coefficient.imaginary = value;
            break;

        case 2:
            coefficient.real = -value;
            break;

        case 3:
            coefficient.imaginary = -value;
            break;
    }

    return coefficient;
}

static GSMComplex gsm_gmsk_c1_coefficient(const GSMBurst *burst, uint32_t coefficient_symbol) {
    GSMComplex coefficient = { 0.0, 0.0 };
    int32_t phase;

    if (coefficient_symbol < 2U)
        return coefficient;

    if (coefficient_symbol == 2U) {
        phase = -1;
    } else {
        bool current_bit = burst->bits[coefficient_symbol - 2U];
        bool previous_bit = burst->bits[coefficient_symbol - 3U];

        phase = current_bit != previous_bit ? 1 : -1;
    }

    GSMComplex c0 = gsm_gmsk_c0_coefficient(burst, coefficient_symbol);
    coefficient.real = -c0.imaginary * phase;
    coefficient.imaginary = c0.real * phase;
    return coefficient;
}

static GSMComplex gsm_gmsk_laurent_sample(const GSMBurst *burst, uint32_t quarter_symbol) {
    GSMComplex sample = { 0.0, 0.0 };
    uint32_t first_delay = quarter_symbol % GSM_GMSK_SAMPLES_PER_SYMBOL;
    uint32_t c0_last_delay = MIN(quarter_symbol, (uint32_t) ARRAY_SIZE(GSM_GMSK_LAURENT_C0) - 1U);

    for (uint32_t delay = first_delay; delay <= c0_last_delay; delay += GSM_GMSK_SAMPLES_PER_SYMBOL) {
        uint32_t coefficient_symbol = (quarter_symbol - delay) / GSM_GMSK_SAMPLES_PER_SYMBOL;

        if (coefficient_symbol >= GSM_GMSK_COEFFICIENT_SYMBOLS)
            continue;

        GSMComplex coefficient = gsm_gmsk_c0_coefficient(burst, coefficient_symbol);
        uint32_t pulse_sample = ARRAY_SIZE(GSM_GMSK_LAURENT_C0) - delay - 1U;
        double pulse = GSM_GMSK_LAURENT_C0[pulse_sample];

        sample.real += coefficient.real * pulse;
        sample.imaginary += coefficient.imaginary * pulse;
    }

    uint32_t c1_last_delay = MIN(quarter_symbol, (uint32_t) ARRAY_SIZE(GSM_GMSK_LAURENT_C1) - 1U);
    for (uint32_t delay = first_delay; delay <= c1_last_delay; delay += GSM_GMSK_SAMPLES_PER_SYMBOL) {
        uint32_t coefficient_symbol = (quarter_symbol - delay) / GSM_GMSK_SAMPLES_PER_SYMBOL;

        if (coefficient_symbol >= GSM_GMSK_COEFFICIENT_SYMBOLS)
            continue;

        GSMComplex coefficient = gsm_gmsk_c1_coefficient(burst, coefficient_symbol);
        uint32_t pulse_sample = ARRAY_SIZE(GSM_GMSK_LAURENT_C1) - delay - 1U;
        double pulse = GSM_GMSK_LAURENT_C1[pulse_sample];

        sample.real += coefficient.real * pulse;
        sample.imaginary += coefficient.imaginary * pulse;
    }

    return sample;
}

IQSample gsm_air_read_iq(GSMAir *air, uint32_t frequency_hz, int64_t timestamp, int32_t amplitude) {
    IQSample sample = { 0, 0 };
    uint16_t arfcn;

    if (air->network == NULL)
        return sample;
    if (timestamp < 0)
        return sample;
    if (!gsm_air_frequency_to_arfcn(frequency_hz, &arfcn))
        return sample;

    uint64_t quarter_symbols = muldiv64((uint64_t) timestamp, GSM_QUARTER_SYMBOLS_PER_NS_NUMERATOR,
        GSM_QUARTER_SYMBOLS_PER_NS_DENOMINATOR);
    uint32_t frame_number = quarter_symbols / GSM_FRAME_QUARTER_SYMBOLS % GSM_HYPERFRAME_FRAMES;
    uint32_t frame_quarter_symbol = quarter_symbols % GSM_FRAME_QUARTER_SYMBOLS;
    uint8_t timeslot = frame_quarter_symbol / GSM_TIMESLOT_QUARTER_SYMBOLS;
    uint32_t timeslot_quarter_symbol = frame_quarter_symbol % GSM_TIMESLOT_QUARTER_SYMBOLS;

    if (!gsm_air_load_burst(air, arfcn, frame_number, timeslot))
        return sample;

    GSMComplex reference = gsm_gmsk_laurent_sample(&air->burst, timeslot_quarter_symbol);

    /* Preserve the baseband model's existing complex orientation and initial phase. */
    sample.i = (int16_t) lround(-(double) amplitude * reference.real);
    sample.q = (int16_t) lround((double) amplitude * reference.imaginary);
    return sample;
}
