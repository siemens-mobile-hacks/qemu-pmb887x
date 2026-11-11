/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "hw/gsm/gsm-network.h"

#define GSM_TSC_COUNT 8
#define GSM_BURST_DATA_BITS 116
#define GSM_XCCH_BLOCK_BYTES 23
#define GSM_XCCH_BLOCK_BURSTS 4
#define GSM_RACH_CODED_BITS 36

/**
 * gsm_normal_burst: fill @burst with a normal burst of training sequence @tsc
 * carrying the @data bits of 45.003 burst mapping: 57 bits, the two stealing
 * flags, 57 bits.
 */
void gsm_normal_burst(GSMBurst *burst, uint8_t tsc, const uint8_t data[GSM_BURST_DATA_BITS]);

/**
 * gsm_dummy_burst: fill @burst with the 45.002 dummy burst.
 */
void gsm_dummy_burst(GSMBurst *burst);

/**
 * gsm_xcch_encode: code the 23-byte L2 block @block like an xCCH or CS-1 PDTCH
 * block (45.003 4.1) into the data bits of four bursts.
 */
void gsm_xcch_encode(
    const uint8_t block[GSM_XCCH_BLOCK_BYTES],
    uint8_t data[GSM_XCCH_BLOCK_BURSTS][GSM_BURST_DATA_BITS]
);

/**
 * gsm_xcch_decode: decode the soft data bits of four bursts of an xCCH or CS-1
 * PDTCH block into @block; return false when the parity check fails.
 */
bool gsm_xcch_decode(
    const uint8_t soft_bits[GSM_XCCH_BLOCK_BURSTS][GSM_BURST_DATA_BITS],
    uint8_t block[GSM_XCCH_BLOCK_BYTES]
);

/**
 * gsm_rach_decode: decode the 36 soft coded bits of an access burst sent to a
 * cell with @bsic into the 8-bit random access reference @ra; return false
 * when the parity check fails.
 */
bool gsm_rach_decode(const uint8_t soft_bits[GSM_RACH_CODED_BITS], uint8_t bsic, uint8_t *ra);

/**
 * gsm_uplink_find_normal_burst: store in @start the position in @burst where a
 * normal burst of training sequence @tsc begins; return false if there is none.
 */
bool gsm_uplink_find_normal_burst(const GSMUplinkBurst *burst, uint8_t tsc, size_t *start);

/**
 * gsm_uplink_find_access_burst: store in @start the position in @burst where an
 * access burst begins; return false if there is none.
 */
bool gsm_uplink_find_access_burst(const GSMUplinkBurst *burst, size_t *start);

/**
 * gsm_uplink_normal_burst_data: store the soft data bits of the normal burst
 * that begins at @start in @burst.
 */
void gsm_uplink_normal_burst_data(const GSMUplinkBurst *burst, size_t start, uint8_t data[GSM_BURST_DATA_BITS]);

/**
 * gsm_uplink_access_burst_data: store the soft coded bits of the access burst
 * that begins at @start in @burst.
 */
void gsm_uplink_access_burst_data(const GSMUplinkBurst *burst, size_t start, uint8_t data[GSM_RACH_CODED_BITS]);
