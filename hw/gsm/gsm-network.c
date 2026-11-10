/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/gsm/gsm-network.h"

#define GSM900_DOWNLINK_ARFCN0_HZ 935000000U
#define GSM_CHANNEL_SPACING_HZ 200000U
#define GSM900_HIGHEST_ARFCN 124
#define EGSM900_LOWEST_ARFCN 975
#define GSM_ARFCN_COUNT 1024

bool gsm_downlink_frequency_to_arfcn(uint32_t frequency_hz, uint16_t *arfcn) {
    int64_t offset_hz = (int64_t) frequency_hz - GSM900_DOWNLINK_ARFCN0_HZ;

    if (offset_hz % GSM_CHANNEL_SPACING_HZ != 0)
        return false;

    int64_t channel = offset_hz / GSM_CHANNEL_SPACING_HZ;
    if (channel > GSM900_HIGHEST_ARFCN || channel < EGSM900_LOWEST_ARFCN - GSM_ARFCN_COUNT)
        return false;

    *arfcn = channel >= 0 ? channel : channel + GSM_ARFCN_COUNT;
    return true;
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
