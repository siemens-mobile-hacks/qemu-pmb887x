/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "qom/object.h"
#include "net/net.h"
#include "hw/gsm/gsm-coding.h"
#include "hw/gsm/gsm-network.h"

#define TYPE_GSM_BTS "gsm-bts"
OBJECT_DECLARE_SIMPLE_TYPE(GSMBts, GSM_BTS)

#define GSM_BTS_BSIC 0
#define GSM_BTS_TSC 0
#define GSM_BTS_PDCH_TIMESLOT 7
#define GSM_MACBLOCK_PADDING 0x2B
#define GSM_CCCH_QUEUE_SIZE 8
#define GSM_LAPDM_QUEUE_SIZE 4
#define GSM_L3_MAX_BYTES 251
#define GSM_CHANNEL_DESCRIPTION_BYTES 3
#define GSM_IA_REST_OCTETS_BYTES 11
#define GSM48_LAI_BYTES 5
#define GSM48_L3_HEADER_BYTES 2
#define GSM48_MESSAGE_TYPE_OFFSET 1
#define CSN_ABSENT 0
#define CSN_PRESENT 1

#define GSMTAP_CHANNEL_BCCH 0x01
#define GSMTAP_CHANNEL_CCCH 0x02
#define GSMTAP_CHANNEL_RACH 0x03
#define GSMTAP_CHANNEL_SDCCH4 0x07
#define GSMTAP_CHANNEL_PACCH 0x0B
#define GSMTAP_CHANNEL_PDTCH 0x0D
#define GSMTAP_CHANNEL_ACCH 0x80

typedef struct GSMBitWriter GSMBitWriter;
typedef struct GSMImmediateAssignment GSMImmediateAssignment;
typedef struct GSMBtsDownlinkBlock GSMBtsDownlinkBlock;
typedef struct GSMBtsUplinkBlock GSMBtsUplinkBlock;
typedef struct GSMBtsMessage GSMBtsMessage;
typedef struct GSMBtsLapdm GSMBtsLapdm;
typedef struct GSMBtsGprs GSMBtsGprs;

struct GSMBitWriter {
    uint8_t *data;
    size_t position;
};

struct QEMU_PACKED GSMImmediateAssignment {
    uint8_t l2_pseudo_length;
    uint8_t protocol_discriminator;
    uint8_t message_type;
    uint8_t mode;
    uint8_t channel_description[GSM_CHANNEL_DESCRIPTION_BYTES];
    uint8_t request_reference_ra;
    uint8_t request_reference_time[2];
    uint8_t timing_advance;
    uint8_t mobile_allocation_length;
    uint8_t rest_octets[GSM_IA_REST_OCTETS_BYTES];
};

_Static_assert(sizeof(GSMImmediateAssignment) == GSM_XCCH_BLOCK_BYTES, "Immediate Assignment layout");

struct GSMBtsDownlinkBlock {
    uint32_t frame_number;
    bool valid;
    uint8_t data[GSM_XCCH_BLOCK_BURSTS][GSM_BURST_DATA_BITS];
};

struct GSMBtsUplinkBlock {
    uint32_t frame_number;
    uint8_t soft_bits[GSM_XCCH_BLOCK_BURSTS][GSM_BURST_DATA_BITS];
};

struct GSMBtsMessage {
    uint8_t data[GSM_L3_MAX_BYTES];
    size_t length;
};

struct GSMBtsLapdm {
    bool active;
    bool established;
    bool send_ua;
    bool send_ack;
    bool release;
    bool disconnected;
    uint8_t vs;
    uint8_t vr;
    bool unacknowledged;
    uint8_t retransmissions;
    size_t sent_offset;
    size_t sent_length;
    GSMBtsMessage ua_info;
    GSMBtsMessage queue[GSM_LAPDM_QUEUE_SIZE];
    size_t queue_head;
    size_t queue_count;
    GSMBtsMessage reassembly;
};

struct GSMBts {
    Object parent_obj;

    char *netdev;
    char *gsmtap;
    NICState *nic;
    NICConf nic_conf;
    MemReentrancyGuard reentrancy_guard;
    Notifier machine_done;
    int gsmtap_fd;
    uint16_t arfcn;
    bool pcs1900;
    uint16_t channel;

    GSMBtsDownlinkBlock ts0_block;
    GSMBtsUplinkBlock sdcch_uplink;
    uint8_t ccch_queue[GSM_CCCH_QUEUE_SIZE][GSM_XCCH_BLOCK_BYTES];
    size_t ccch_head;
    size_t ccch_count;
    GSMBtsLapdm lapdm;
    bool sacch_si6;

    GSMBtsGprs *gprs;
};

void gsm_bit_writer_put(GSMBitWriter *writer, uint32_t value, uint32_t count);
void gsm_bit_writer_put_lh(GSMBitWriter *writer, bool high);
void gsm_bit_writer_pad(GSMBitWriter *writer, size_t bytes);
uint32_t gsm_bts_frame_number_add(uint32_t frame_number, uint32_t frames);
uint16_t gsm_starting_time(uint32_t frame_number);
void gsm_bts_encode_lai(uint8_t lai[GSM48_LAI_BYTES]);
void gsm_bts_ccch_enqueue(GSMBts *bts, const uint8_t message[GSM_XCCH_BLOCK_BYTES]);
void gsm_bts_encode_immediate_assignment(
    GSMImmediateAssignment *ia,
    uint8_t mode,
    const uint8_t channel_description[GSM_CHANNEL_DESCRIPTION_BYTES],
    uint8_t ra,
    uint32_t frame_number
);
void gsm_bts_gsmtap(
    GSMBts *bts,
    bool uplink,
    uint8_t timeslot,
    uint32_t frame_number,
    uint8_t channel,
    const uint8_t *data,
    size_t length
);
void gsm_bts_send_ethernet(GSMBts *bts, const uint8_t *frame, size_t size);

void gsm_bts_gprs_init(GSMBts *bts);
void gsm_bts_gprs_cleanup(GSMBts *bts);
bool gsm_bts_gprs_handle_rach(GSMBts *bts, uint8_t ra, uint32_t frame_number);
void gsm_bts_gprs_downlink_burst(GSMBts *bts, uint32_t frame_number, GSMBurst *burst);
void gsm_bts_gprs_uplink_burst(GSMBts *bts, uint32_t frame_number, const GSMUplinkBurst *burst);
void gsm_bts_gprs_handle_ethernet(GSMBts *bts, const uint8_t *frame, size_t size);
