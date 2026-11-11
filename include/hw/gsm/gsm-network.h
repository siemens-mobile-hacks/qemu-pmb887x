/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "qom/object.h"

#define TYPE_GSM_NETWORK "gsm-network"
#define GSM_BURST_BITS 148
#define GSM_TIMESLOTS 8
#define GSM_HYPERFRAME_FRAMES 2715648U
#define GSM_UPLINK_LEAD_SYMBOLS 8
#define GSM_UPLINK_WINDOW_SYMBOLS (GSM_UPLINK_LEAD_SYMBOLS + GSM_BURST_BITS)
#define GSM_SOFT_BIT_ZERO 0
#define GSM_SOFT_BIT_ONE 255
#define GSM_SOFT_BIT_UNKNOWN 127

typedef struct GSMBurst GSMBurst;
typedef struct GSMUplinkBurst GSMUplinkBurst;
typedef struct GSMNetwork GSMNetwork;
typedef struct GSMNetworkClass GSMNetworkClass;

#define GSM_NETWORK(obj) INTERFACE_CHECK(GSMNetwork, (obj), TYPE_GSM_NETWORK)
DECLARE_CLASS_CHECKERS(GSMNetworkClass, GSM_NETWORK, TYPE_GSM_NETWORK)

struct GSMBurst {
    uint8_t bits[GSM_BURST_BITS];
};

/**
 * GSMUplinkBurst: the symbols a mobile sent in one uplink timeslot.
 * @soft_bits: symbols from GSM_UPLINK_LEAD_SYMBOLS before the timeslot start,
 * from GSM_SOFT_BIT_ZERO to GSM_SOFT_BIT_ONE, or GSM_SOFT_BIT_UNKNOWN.
 * @phase_256: arrival time of each symbol after its nominal position, in 1/256 symbol.
 */
struct GSMUplinkBurst {
    uint8_t soft_bits[GSM_UPLINK_WINDOW_SYMBOLS];
    int16_t phase_256;
};

/**
 * GSMNetworkClass:
 * @downlink_burst: fill @burst with the bits the network transmits on @arfcn in
 * frame @frame_number, timeslot @timeslot; return false if it transmits nothing.
 * @uplink_burst: receive @burst a mobile sent on uplink channel @arfcn in frame
 * @frame_number, timeslot @timeslot; NULL when the network ignores the uplink.
 * @uplink_complete: the mobile delivered every uplink burst of frame @frame_number
 * and earlier; NULL when the network does not need it.
 */
struct GSMNetworkClass {
    InterfaceClass parent_class;
    bool (*downlink_burst)(GSMNetwork *network, uint16_t arfcn, uint32_t frame_number, uint8_t timeslot,
        GSMBurst *burst);
    void (*uplink_burst)(GSMNetwork *network, uint16_t arfcn, uint32_t frame_number, uint8_t timeslot,
        const GSMUplinkBurst *burst);
    void (*uplink_complete)(GSMNetwork *network, uint32_t frame_number);
};

/**
 * gsm_downlink_frequency_to_arfcn: store the GSM900 or E-GSM900 channel of the downlink
 * carrier @frequency_hz in @arfcn; return false for a frequency off the channel raster.
 */
bool gsm_downlink_frequency_to_arfcn(uint32_t frequency_hz, uint16_t *arfcn);

/**
 * gsm_uplink_frequency_to_arfcn: store the GSM900 or E-GSM900 channel of the uplink
 * carrier @frequency_hz in @arfcn; return false for a frequency off the channel raster.
 */
bool gsm_uplink_frequency_to_arfcn(uint32_t frequency_hz, uint16_t *arfcn);
