/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "qom/object.h"

#define TYPE_GSM_NETWORK "gsm-network"
#define GSM_BURST_BITS 148
#define GSM_TIMESLOTS 8
#define GSM_HYPERFRAME_FRAMES 2715648U

typedef struct GSMBurst GSMBurst;
typedef struct GSMNetwork GSMNetwork;
typedef struct GSMNetworkClass GSMNetworkClass;

#define GSM_NETWORK(obj) INTERFACE_CHECK(GSMNetwork, (obj), TYPE_GSM_NETWORK)
DECLARE_CLASS_CHECKERS(GSMNetworkClass, GSM_NETWORK, TYPE_GSM_NETWORK)

struct GSMBurst {
    uint8_t bits[GSM_BURST_BITS];
};

/**
 * GSMNetworkClass:
 * @downlink_burst: fill @burst with the bits the network transmits on @arfcn in
 * frame @frame_number, timeslot @timeslot; return false if it transmits nothing.
 */
struct GSMNetworkClass {
    InterfaceClass parent_class;
    bool (*downlink_burst)(GSMNetwork *network, uint16_t arfcn, uint32_t frame_number, uint8_t timeslot,
        GSMBurst *burst);
};

/**
 * gsm_downlink_frequency_to_arfcn: store the GSM900 or E-GSM900 channel of the downlink
 * carrier @frequency_hz in @arfcn; return false for a frequency off the channel raster.
 */
bool gsm_downlink_frequency_to_arfcn(uint32_t frequency_hz, uint16_t *arfcn);
