/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/host-utils.h"
#include "hw/gsm/gsm-air.h"

#define GSM_FRAME_QUARTER_SYMBOLS 5000U
#define GSM_TIMESLOT_QUARTER_SYMBOLS 625U
#define GSM_QUARTER_SYMBOLS_PER_NS_NUMERATOR 13U
#define GSM_QUARTER_SYMBOLS_PER_NS_DENOMINATOR 12000U
#define GSM_GMSK_SAMPLES_PER_SYMBOL 4U
#define GSM_GMSK_COEFFICIENT_SYMBOLS (GSM_BURST_BITS + 2U)
#define GSM_UPLINK_DELAY_QUARTER_SYMBOLS (3U * GSM_TIMESLOT_QUARTER_SYMBOLS)
#define GSM_UPLINK_DELIVERY_QUARTER_SYMBOLS GSM_FRAME_QUARTER_SYMBOLS
#define GSM_PHASE_UNITS_PER_QUARTER_SYMBOL 64
/*
 * The received carrier is at -60 dBm and the receiver noise floor near -110 dBm.
 * A real ADC never outputs a run of exact zeros: firmware converts the logarithm
 * of an all-zero measurement window into the strongest possible level.
 */
#define GSM_AIR_SIGNAL_TO_NOISE_RATIO 316
#define GSM_AIR_MIN_NOISE_AMPLITUDE 1
#define GSM_AIR_NOISE_SEED 0x2545F491U

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

static uint64_t gsm_air_quarter_symbols(int64_t timestamp) {
    return muldiv64((uint64_t) timestamp, GSM_QUARTER_SYMBOLS_PER_NS_NUMERATOR, GSM_QUARTER_SYMBOLS_PER_NS_DENOMINATOR);
}

uint32_t gsm_air_frame_number(int64_t timestamp) {
    return gsm_air_quarter_symbols(timestamp) / GSM_FRAME_QUARTER_SYMBOLS % GSM_HYPERFRAME_FRAMES;
}

int64_t gsm_air_frames_to_ns(uint32_t frames) {
    return muldiv64((uint64_t) frames * GSM_FRAME_QUARTER_SYMBOLS, GSM_QUARTER_SYMBOLS_PER_NS_DENOMINATOR,
        GSM_QUARTER_SYMBOLS_PER_NS_NUMERATOR);
}

void gsm_air_tune(GSMAir *air, GSMAirPath path, uint32_t frequency_hz, int32_t amplitude, int64_t timestamp) {
    uint16_t arfcn = 0;
    bool on_raster = false;

    switch (path) {
        case GSM_AIR_PATH_OFF:
            break;

        case GSM_AIR_PATH_RECEIVE:
            on_raster = gsm_downlink_frequency_to_arfcn(frequency_hz, &arfcn);
            break;

        case GSM_AIR_PATH_TRANSMIT:
            on_raster = gsm_uplink_frequency_to_arfcn(frequency_hz, &arfcn);
            break;
    }

    air->tuning_last = (air->tuning_last + 1) % GSM_AIR_TUNING_HISTORY;
    air->tunings[air->tuning_last] = (GSMAirTuning) {
        .timestamp = timestamp,
        .path = on_raster ? path : GSM_AIR_PATH_OFF,
        .arfcn = arfcn,
        .amplitude = amplitude,
    };
    air->tuning_count = MIN(air->tuning_count + 1, GSM_AIR_TUNING_HISTORY);
}

static const GSMAirTuning *gsm_air_find_tuning(const GSMAir *air, GSMAirPath path, int64_t timestamp) {
    for (uint8_t age = 0; age < air->tuning_count; age++) {
        const GSMAirTuning *tuning =
            &air->tunings[(air->tuning_last + GSM_AIR_TUNING_HISTORY - age) % GSM_AIR_TUNING_HISTORY];

        if (tuning->timestamp <= timestamp)
            return tuning->path == path ? tuning : NULL;
    }
    return NULL;
}

static int16_t gsm_air_noise(GSMAir *air, int32_t noise_amplitude) {
    air->noise_state ^= air->noise_state << 13;
    air->noise_state ^= air->noise_state >> 17;
    air->noise_state ^= air->noise_state << 5;
    return (int32_t) (air->noise_state % (2 * noise_amplitude + 1)) - noise_amplitude;
}

IQSample gsm_air_read_iq(GSMAir *air, int64_t timestamp) {
    IQSample sample = { 0, 0 };

    if (air->network == NULL)
        return sample;
    if (timestamp < 0)
        return sample;

    const GSMAirTuning *tuning = gsm_air_find_tuning(air, GSM_AIR_PATH_RECEIVE, timestamp);
    if (tuning == NULL)
        return sample;

    uint64_t quarter_symbols = gsm_air_quarter_symbols(timestamp);
    uint32_t frame_number = quarter_symbols / GSM_FRAME_QUARTER_SYMBOLS % GSM_HYPERFRAME_FRAMES;
    uint32_t frame_quarter_symbol = quarter_symbols % GSM_FRAME_QUARTER_SYMBOLS;
    uint8_t timeslot = frame_quarter_symbol / GSM_TIMESLOT_QUARTER_SYMBOLS;
    uint32_t timeslot_quarter_symbol = frame_quarter_symbol % GSM_TIMESLOT_QUARTER_SYMBOLS;

    int32_t noise_amplitude = MAX(tuning->amplitude / GSM_AIR_SIGNAL_TO_NOISE_RATIO, GSM_AIR_MIN_NOISE_AMPLITUDE);
    sample.i = gsm_air_noise(air, noise_amplitude);
    sample.q = gsm_air_noise(air, noise_amplitude);
    if (!gsm_air_load_burst(air, tuning->arfcn, frame_number, timeslot))
        return sample;

    GSMComplex reference = gsm_gmsk_laurent_sample(&air->burst, timeslot_quarter_symbol);

    /* Preserve the baseband model's existing complex orientation and initial phase. */
    sample.i += (int16_t) lround(-(double) tuning->amplitude * reference.real);
    sample.q += (int16_t) lround((double) tuning->amplitude * reference.imaginary);
    return sample;
}

static uint64_t gsm_air_uplink_window(uint64_t quarter_symbols) {
    return quarter_symbols - GSM_UPLINK_DELAY_QUARTER_SYMBOLS + GSM_UPLINK_LEAD_SYMBOLS * GSM_GMSK_SAMPLES_PER_SYMBOL;
}

static void gsm_air_deliver_uplink(GSMAir *air) {
    if (!air->uplink_active)
        return;

    air->uplink_active = false;
    timer_del(air->uplink_timer);
    if (air->uplink_symbols <= GSM_UPLINK_WINDOW_SYMBOLS / 2)
        return;

    GSMNetworkClass *network_class = GSM_NETWORK_GET_CLASS(air->network);
    if (network_class->uplink_burst != NULL) {
        network_class->uplink_burst(air->network, air->uplink_arfcn, air->uplink_frame_number,
            air->uplink_timeslot, &air->uplink);
    }
}

static void gsm_air_uplink_timer(void *opaque) {
    gsm_air_deliver_uplink(opaque);
}

void gsm_air_init_uplink(GSMAir *air) {
    air->uplink_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gsm_air_uplink_timer, air);
}

void gsm_air_cleanup_uplink(GSMAir *air) {
    timer_free(air->uplink_timer);
}

void gsm_air_reset(GSMAir *air) {
    air->tuning_count = 0;
    air->noise_state = GSM_AIR_NOISE_SEED;
    timer_del(air->uplink_timer);
    air->uplink_active = false;
}

void gsm_air_transmit(GSMAir *air, int64_t timestamp, bool symbol) {
    if (air->network == NULL || timestamp < 0)
        return;

    const GSMAirTuning *tuning = gsm_air_find_tuning(air, GSM_AIR_PATH_TRANSMIT, timestamp);
    if (tuning == NULL)
        return;

    uint64_t quarter_symbols = gsm_air_quarter_symbols(timestamp);
    if (quarter_symbols < GSM_UPLINK_DELAY_QUARTER_SYMBOLS)
        return;

    uint64_t window_quarter_symbols = gsm_air_uplink_window(quarter_symbols);
    uint32_t frame_number = window_quarter_symbols / GSM_FRAME_QUARTER_SYMBOLS % GSM_HYPERFRAME_FRAMES;
    uint32_t frame_quarter_symbol = window_quarter_symbols % GSM_FRAME_QUARTER_SYMBOLS;
    uint8_t timeslot = frame_quarter_symbol / GSM_TIMESLOT_QUARTER_SYMBOLS;
    uint32_t window_quarter_symbol = frame_quarter_symbol % GSM_TIMESLOT_QUARTER_SYMBOLS;
    uint32_t index = (window_quarter_symbol + GSM_GMSK_SAMPLES_PER_SYMBOL / 2) / GSM_GMSK_SAMPLES_PER_SYMBOL;
    bool same_burst = air->uplink_active && air->uplink_arfcn == tuning->arfcn &&
        air->uplink_frame_number == frame_number && air->uplink_timeslot == timeslot;

    if (!same_burst) {
        gsm_air_deliver_uplink(air);

        int32_t phase = (int32_t) window_quarter_symbol - (int32_t) index * GSM_GMSK_SAMPLES_PER_SYMBOL;
        uint64_t delivery = quarter_symbols - window_quarter_symbol + GSM_TIMESLOT_QUARTER_SYMBOLS +
            GSM_UPLINK_DELIVERY_QUARTER_SYMBOLS;

        memset(air->uplink.soft_bits, GSM_SOFT_BIT_UNKNOWN, sizeof(air->uplink.soft_bits));
        air->uplink.phase_256 = phase * GSM_PHASE_UNITS_PER_QUARTER_SYMBOL;
        air->uplink_arfcn = tuning->arfcn;
        air->uplink_frame_number = frame_number;
        air->uplink_timeslot = timeslot;
        air->uplink_symbols = 0;
        air->uplink_active = true;
        timer_mod(air->uplink_timer, muldiv64(delivery, GSM_QUARTER_SYMBOLS_PER_NS_DENOMINATOR,
            GSM_QUARTER_SYMBOLS_PER_NS_NUMERATOR));
    }

    if (index >= GSM_UPLINK_WINDOW_SYMBOLS)
        return;

    air->uplink.soft_bits[index] = symbol ? GSM_SOFT_BIT_ONE : GSM_SOFT_BIT_ZERO;
    air->uplink_symbols++;
}

void gsm_air_transmit_until(GSMAir *air, int64_t timestamp) {
    if (air->network == NULL || timestamp < 0)
        return;

    uint64_t quarter_symbols = gsm_air_quarter_symbols(timestamp);
    if (quarter_symbols < GSM_UPLINK_DELAY_QUARTER_SYMBOLS + GSM_FRAME_QUARTER_SYMBOLS)
        return;

    uint64_t window_frame = gsm_air_uplink_window(quarter_symbols) / GSM_FRAME_QUARTER_SYMBOLS;
    if (air->uplink_active && air->uplink_frame_number != window_frame % GSM_HYPERFRAME_FRAMES)
        gsm_air_deliver_uplink(air);

    uint32_t complete_frame = (window_frame - 1) % GSM_HYPERFRAME_FRAMES;
    uint32_t advance = (complete_frame + GSM_HYPERFRAME_FRAMES - air->uplink_complete_frame) % GSM_HYPERFRAME_FRAMES;
    if (air->uplink_complete_valid && (advance == 0 || advance > GSM_HYPERFRAME_FRAMES / 2))
        return;

    air->uplink_complete_frame = complete_frame;
    air->uplink_complete_valid = true;
    GSMNetworkClass *network_class = GSM_NETWORK_GET_CLASS(air->network);
    if (network_class->uplink_complete != NULL)
        network_class->uplink_complete(air->network, complete_frame);
}
