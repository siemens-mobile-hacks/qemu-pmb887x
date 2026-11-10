/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "hw/gsm/gsm-network.h"
#include "hw/gsm/iq-bus.h"

typedef struct GSMAir GSMAir;

/**
 * GSMAir: radio channel between a transceiver and the GSM network it is linked to.
 * @network: the linked network; NULL means no signal.
 */
struct GSMAir {
    GSMNetwork *network;
    GSMBurst burst;
    uint32_t burst_frame_number;
    uint16_t burst_arfcn;
    uint8_t burst_timeslot;
    bool burst_cached;
    bool burst_present;
};

/**
 * gsm_air_read_iq: return the downlink I/Q sample received at @frequency_hz at
 * virtual time @timestamp, scaled to @amplitude.
 */
IQSample gsm_air_read_iq(GSMAir *air, uint32_t frequency_hz, int64_t timestamp, int32_t amplitude);
