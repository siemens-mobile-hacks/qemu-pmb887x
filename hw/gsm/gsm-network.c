/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/module.h"
#include "hw/gsm/gsm-network.h"

#define GSM_CHANNEL_SPACING_HZ 200000U
/* 3GPP TS 45.005 4.1: carrier frequencies are accurate to 0.1 ppm, about 100 Hz at 900 MHz. */
#define GSM_CARRIER_TOLERANCE_HZ 100

typedef struct GSMBand GSMBand;

struct GSMBand {
    uint16_t first_arfcn;
    uint16_t last_arfcn;
    uint32_t first_downlink_hz;
    uint32_t duplex_spacing_hz;
    uint16_t arfcn_flags;
};

/* 3GPP TS 45.005 2: channel numbers and carrier frequencies of each band. */
static const GSMBand gsm_bands[] = {
    { 0, 124, 935000000U, 45000000U, 0 },
    { 975, 1023, 925200000U, 45000000U, 0 },
    { 128, 251, 869200000U, 45000000U, 0 },
    { 512, 885, 1805200000U, 95000000U, 0 },
    { 512, 810, 1930200000U, 80000000U, GSM_ARFCN_PCS1900 },
};

static bool gsm_band_downlink_to_arfcn(const GSMBand *band, uint32_t downlink_hz, uint16_t *arfcn) {
    int64_t offset_hz = (int64_t) downlink_hz - band->first_downlink_hz;
    int64_t channel = llround((double) offset_hz / GSM_CHANNEL_SPACING_HZ);

    if (llabs(offset_hz - channel * (int64_t) GSM_CHANNEL_SPACING_HZ) > GSM_CARRIER_TOLERANCE_HZ)
        return false;
    if (channel < 0 || channel > band->last_arfcn - band->first_arfcn)
        return false;

    *arfcn = (band->first_arfcn + channel) | band->arfcn_flags;
    return true;
}

bool gsm_downlink_frequency_to_arfcn(uint32_t frequency_hz, uint16_t *arfcn) {
    for (size_t i = 0; i < ARRAY_SIZE(gsm_bands); i++) {
        if (gsm_band_downlink_to_arfcn(&gsm_bands[i], frequency_hz, arfcn))
            return true;
    }
    return false;
}

bool gsm_uplink_frequency_to_arfcn(uint32_t frequency_hz, uint16_t *arfcn) {
    for (size_t i = 0; i < ARRAY_SIZE(gsm_bands); i++) {
        const GSMBand *band = &gsm_bands[i];

        if (gsm_band_downlink_to_arfcn(band, frequency_hz + band->duplex_spacing_hz, arfcn))
            return true;
    }
    return false;
}

bool gsm_arfcn_is_valid(uint16_t arfcn) {
    uint16_t number = arfcn & GSM_ARFCN_NUMBER;

    for (size_t i = 0; i < ARRAY_SIZE(gsm_bands); i++) {
        const GSMBand *band = &gsm_bands[i];
        bool in_band = number >= band->first_arfcn && number <= band->last_arfcn;

        if ((arfcn & ~GSM_ARFCN_NUMBER) == band->arfcn_flags && in_band)
            return true;
    }
    return false;
}

static const TypeInfo gsm_network_info = {
    .name           = TYPE_GSM_NETWORK,
    .parent         = TYPE_INTERFACE,
    .class_size     = sizeof(GSMNetworkClass),
};

static void gsm_network_register_types(void) {
    type_register_static(&gsm_network_info);
}

type_init(gsm_network_register_types)
