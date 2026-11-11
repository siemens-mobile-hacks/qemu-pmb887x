/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "qemu/timer.h"
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
    QEMUTimer *uplink_timer;
    GSMUplinkBurst uplink;
    uint32_t uplink_frame_number;
    uint16_t uplink_arfcn;
    uint16_t uplink_symbols;
    uint8_t uplink_timeslot;
    bool uplink_active;
    uint32_t uplink_complete_frame;
    bool uplink_complete_valid;
};

/**
 * gsm_air_frame_number: return the TDMA frame number on air at virtual time @timestamp.
 */
uint32_t gsm_air_frame_number(int64_t timestamp);

/**
 * gsm_air_frames_to_ns: return the duration of @frames TDMA frames in nanoseconds.
 */
int64_t gsm_air_frames_to_ns(uint32_t frames);

/**
 * gsm_air_read_iq: return the downlink I/Q sample received at @frequency_hz at
 * virtual time @timestamp, scaled to @amplitude.
 */
IQSample gsm_air_read_iq(GSMAir *air, uint32_t frequency_hz, int64_t timestamp, int32_t amplitude);

/**
 * gsm_air_init_uplink: prepare @air for gsm_air_transmit().
 */
void gsm_air_init_uplink(GSMAir *air);

/**
 * gsm_air_cleanup_uplink: release what gsm_air_init_uplink() allocated.
 */
void gsm_air_cleanup_uplink(GSMAir *air);

/**
 * gsm_air_reset_uplink: drop the burst in progress without delivering it.
 */
void gsm_air_reset_uplink(GSMAir *air);

/**
 * gsm_air_transmit: send the GMSK modulator input bit @symbol that starts at virtual
 * time @timestamp on uplink channel @arfcn. The network receives each burst at the
 * end of its timeslot.
 */
void gsm_air_transmit(GSMAir *air, uint16_t arfcn, int64_t timestamp, bool symbol);

/**
 * gsm_air_transmit_until: the transmitter will send no more symbols that start
 * before virtual time @timestamp. Delivers a burst of an earlier frame and reports
 * every uplink frame that is complete to the network.
 */
void gsm_air_transmit_until(GSMAir *air, int64_t timestamp);
