/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/module.h"
#include "hw/gsm/gsm-network.h"

#define GSM900_DOWNLINK_ARFCN0_HZ 935000000U
#define GSM_CHANNEL_SPACING_HZ 200000U
#define GSM900_HIGHEST_ARFCN 124
#define EGSM900_LOWEST_ARFCN 975
#define GSM_ARFCN_COUNT 1024
#define GSM900_DUPLEX_SPACING_HZ 45000000U
/* 3GPP TS 45.005 4.1: carrier frequencies are accurate to 0.1 ppm, about 100 Hz at 900 MHz. */
#define GSM_CARRIER_TOLERANCE_HZ 100

bool gsm_downlink_frequency_to_arfcn(uint32_t frequency_hz, uint16_t *arfcn) {
    int64_t offset_hz = (int64_t) frequency_hz - GSM900_DOWNLINK_ARFCN0_HZ;
    int64_t channel = llround((double) offset_hz / GSM_CHANNEL_SPACING_HZ);

    if (llabs(offset_hz - channel * (int64_t) GSM_CHANNEL_SPACING_HZ) > GSM_CARRIER_TOLERANCE_HZ)
        return false;
    if (channel > GSM900_HIGHEST_ARFCN || channel < EGSM900_LOWEST_ARFCN - GSM_ARFCN_COUNT)
        return false;

    *arfcn = channel >= 0 ? channel : channel + GSM_ARFCN_COUNT;
    return true;
}

bool gsm_uplink_frequency_to_arfcn(uint32_t frequency_hz, uint16_t *arfcn) {
    return gsm_downlink_frequency_to_arfcn(frequency_hz + GSM900_DUPLEX_SPACING_HZ, arfcn);
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
