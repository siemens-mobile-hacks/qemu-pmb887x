/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "qemu/timer.h"
#include "hw/gsm/gsm-network.h"
#include "hw/gsm/iq-bus.h"

#define GSM_AIR_TUNING_HISTORY 64

typedef enum GSMAirPath GSMAirPath;
typedef struct GSMAirTuning GSMAirTuning;
typedef struct GSMAir GSMAir;

enum GSMAirPath {
    GSM_AIR_PATH_OFF,
    GSM_AIR_PATH_RECEIVE,
    GSM_AIR_PATH_TRANSMIT,
};

/**
 * GSMAirTuning: the transceiver setting that takes effect at virtual time @timestamp.
 * @amplitude: full scale of the received I/Q samples.
 */
struct GSMAirTuning {
    int64_t timestamp;
    GSMAirPath path;
    uint16_t arfcn;
    int32_t amplitude;
};

/**
 * GSMAir: radio channel between a transceiver and the GSM network it is linked to.
 * @network: the linked network; NULL means no signal.
 * @tunings: the latest transceiver settings, so that samples and symbols the
 * baseband produces late still see the setting of their own time.
 */
struct GSMAir {
    GSMNetwork *network;
    GSMAirTuning tunings[GSM_AIR_TUNING_HISTORY];
    uint8_t tuning_last;
    uint8_t tuning_count;
    uint32_t noise_state;
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
 * gsm_air_tune: from virtual time @timestamp on the transceiver uses @path on carrier
 * @frequency_hz, the downlink carrier when receiving and the uplink carrier when
 * transmitting, and receives samples of full scale @amplitude. A carrier off the GSM
 * channel raster turns the path off. Calls must come in @timestamp order.
 */
void gsm_air_tune(GSMAir *air, GSMAirPath path, uint32_t frequency_hz, int32_t amplitude, int64_t timestamp);

/**
 * gsm_air_read_iq: return the downlink I/Q sample received at virtual time @timestamp.
 */
IQSample gsm_air_read_iq(GSMAir *air, int64_t timestamp);

/**
 * gsm_air_init_uplink: prepare @air for gsm_air_transmit().
 */
void gsm_air_init_uplink(GSMAir *air);

/**
 * gsm_air_cleanup_uplink: release what gsm_air_init_uplink() allocated.
 */
void gsm_air_cleanup_uplink(GSMAir *air);

/**
 * gsm_air_reset: turn the transceiver off and drop the burst in progress without
 * delivering it.
 */
void gsm_air_reset(GSMAir *air);

/**
 * gsm_air_transmit: send the GMSK modulator input bit @symbol that starts at virtual
 * time @timestamp. The network receives each burst at the end of its timeslot.
 */
void gsm_air_transmit(GSMAir *air, int64_t timestamp, bool symbol);

/**
 * gsm_air_transmit_until: the transmitter will send no more symbols that start
 * before virtual time @timestamp. Delivers a burst of an earlier frame and reports
 * every uplink frame that is complete to the network.
 */
void gsm_air_transmit_until(GSMAir *air, int64_t timestamp);
