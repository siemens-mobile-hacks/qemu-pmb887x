/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "net/eth.h"
#include "hw/gsm/gsm-air.h"
#include "hw/gsm/gsm-bts.h"

#define PDCH_MULTIFRAME_FRAMES 52U
#define PDCH_BLOCKS_PER_MULTIFRAME 12U
#define PDCH_BLOCKS_PER_GROUP 3U
#define PDCH_GROUP_FRAMES 13U
#define PDCH_RRBP_N13_FRAMES 13U

#define RLC_SNS 128U
#define RLC_WINDOW 64U
#define RLC_DATA_BYTES 20U
#define RLC_HEADER_BYTES 3U
#define RLC_MAC_HEADER_OCTET 0
#define RLC_TFI_OCTET 1
#define RLC_BSN_OCTET 2
#define RLC_TLLI_BYTES 4U
#define RLC_MAX_CHUNKS (RLC_DATA_BYTES + 1)
#define RLC_DOWNLINK_POLL_BLOCKS 8U
#define RLC_UPLINK_ACK_BLOCKS 8U
#define RLC_POLL_ATTEMPTS 8U
#define RLC_IDLE_USF_BLOCKS 100U

#define RLC_PAYLOAD_TYPE_SHIFT 6
#define RLC_UL_COUNTDOWN_SHIFT 2
#define RLC_UL_COUNTDOWN_MASK 0x0FU
#define RLC_TFI_SHIFT 1
#define RLC_TFI_MASK 0x1FU
#define RLC_UL_TLLI_INDICATOR 0x01U
#define RLC_DL_FINAL_BLOCK 0x01U
#define RLC_BSN_SHIFT 1
#define RLC_EXTENSION 0x01U
#define RLC_LI_SHIFT 2
#define RLC_LI_MORE 0x02U
#define RLC_DL_POLL_SHIFT 3

#define GPRS_UPLINK_TFI 0U
#define GPRS_TFI_COUNT 32U
#define GPRS_USF 0U
#define GPRS_USF_FREE 7U
#define GPRS_T3192_FRAMES 325U
#define GPRS_ASSIGNMENT_REPEATS 6U
#define GPRS_PACCH_ASSIGNMENT_DELAY_FRAMES 26U
#define GPRS_CCCH_ASSIGNMENT_DELAY_FRAMES 120U
#define GPRS_SINGLE_BLOCK_DELAY_FRAMES 102U

#define RLCMAC_PAYLOAD_TYPE_BITS 2
#define RLCMAC_RRBP_BITS 2
#define RLCMAC_POLL_BITS 1
#define RLCMAC_USF_BITS 3
#define RLCMAC_UPLINK_HEADER_BITS 8
#define RLCMAC_MESSAGE_TYPE_BITS 6
#define RLCMAC_PAGE_MODE_BITS 2
#define RLCMAC_TFI_BITS 5
#define RLCMAC_TLLI_BITS 32
#define RLCMAC_SSN_BITS 7
#define RLCMAC_CHANNEL_CODING_BITS 2
#define RLCMAC_TIMING_ADVANCE_BITS 6
#define RLCMAC_TSC_BITS 3
#define RLCMAC_FREQUENCY_CHOICE_BITS 2
#define RLCMAC_ARFCN_BITS 10
#define RLCMAC_ALPHA_BITS 4
#define RLCMAC_GAMMA_BITS 5
#define RLCMAC_MAC_MODE_BITS 2
#define RLCMAC_TIMESLOT_ALLOCATION_BITS 8
#define RLCMAC_ID_TLLI_BITS 2
#define RLCMAC_ALLOCATION_BITS 2
#define RLCMAC_UPLINK_ACK_TFI_CHOICE_BITS 2
#define RLCMAC_ACCESS_TYPE_BITS 2

#define RLCMAC_PT_DATA 0U
#define RLCMAC_PT_CONTROL 1U
#define RLCMAC_RRBP_N13 0U
#define RLCMAC_PAGE_MODE_NORMAL 0U
#define RLCMAC_UL_PACKET_CONTROL_ACK 0x01U
#define RLCMAC_UL_PACKET_DOWNLINK_ACK_NACK 0x02U
#define RLCMAC_UL_PACKET_RESOURCE_REQUEST 0x05U
#define RLCMAC_DL_PACKET_DOWNLINK_ASSIGNMENT 0x02U
#define RLCMAC_DL_PACKET_UPLINK_ACK_NACK 0x09U
#define RLCMAC_DL_PACKET_UPLINK_ASSIGNMENT 0x0AU
#define RLCMAC_DL_PACKET_DUMMY_CONTROL_BLOCK 0x25U
#define RLCMAC_ID_GLOBAL_TFI 0U
#define RLCMAC_ID_TLLI 2U
#define RLCMAC_REQUEST_ID_TLLI 1U
#define RLCMAC_GLOBAL_TFI_UPLINK 0U
#define RLCMAC_GLOBAL_TFI_DOWNLINK 1U
#define RLCMAC_UPLINK_ACK_UPLINK_TFI 0U
#define RLCMAC_GPRS 0U
#define RLCMAC_NO_MESSAGE_ESCAPE 0U
#define RLCMAC_CHANNEL_CODING_CS1 0U
#define RLCMAC_TLLI_BLOCK_COMMANDED_CODING 1U
#define RLCMAC_MAC_MODE_DYNAMIC 0U
#define RLCMAC_RLC_MODE_ACKNOWLEDGED 0U
#define RLCMAC_FREQUENCY_ARFCN 0U
#define RLCMAC_ALLOCATION_DYNAMIC 1U
#define RLCMAC_NOT_EXTENDED_DYNAMIC 0U
#define RLCMAC_USF_GRANULARITY_ONE_BLOCK 0U
#define RLCMAC_TIMESLOT_ALLOCATION 0U
#define RLCMAC_TIMING_ADVANCE 0U
#define RLCMAC_ALPHA 0U
#define RLCMAC_GAMMA 0U
#define RLCMAC_POLLING_NONE 0U
#define RLCMAC_TBF_ESTABLISHMENT_ALLOWED 1U
#define RLCMAC_PDCH_TIMESLOT_BIT (0x80U >> GSM_BTS_PDCH_TIMESLOT)

#define RACH_PACKET_ACCESS_MASK 0xF8U
#define RACH_ONE_PHASE 0x70U
#define RACH_SINGLE_BLOCK 0x78U

#define IA_MODE_TBF 0x10U
#define IA_MODE_DOWNLINK 0x20U
#define IA_PACKET_CHANNEL_PDTCH 0x01U
#define IA_PACKET_CHANNEL_TYPE_BITS 5
#define IA_TIMESLOT_BITS 3
#define IA_NON_HOPPING 0U
#define IA_HOPPING_BITS 3
#define IA_PACKET_UPLINK_ASSIGNMENT 0U
#define IA_PACKET_DOWNLINK_ASSIGNMENT 1U
#define IA_ASSIGNMENT_BITS 2
#define IA_SINGLE_BLOCK_ALLOCATION 0U
#define IA_MULTI_BLOCK_ALLOCATION 1U
#define IA_STARTING_TIME_BITS 16
#define IA_DOWNLINK_REFERENCE_RA 0x7DU
#define IA_DOWNLINK_REFERENCE_AGE_FRAMES 21216U

#define LLC_ADDRESS_BYTES 1U
#define LLC_ADDRESS_COMMAND 0x40U
#define LLC_SAPI_MASK 0x0FU
#define LLC_SAPI_GMM 1U
#define LLC_SAPI_DATA_DEFAULT 3U
#define LLC_UI_MASK 0xE0U
#define LLC_UI 0xC0U
#define LLC_UI_NU_HIGH_SHIFT 6
#define LLC_UI_NU_LOW_SHIFT 2
#define LLC_UI_NU_LOW_MASK 0xFCU
#define LLC_U 0xE0U
#define LLC_U_PF 0x10U
#define LLC_U_COMMAND_MASK 0x0FU
#define LLC_U_XID 0x0BU
#define LLC_UI_PM 0x01U
#define LLC_UI_HEADER_BYTES 3U
#define LLC_U_HEADER_BYTES 2U
#define LLC_FCS_BYTES 3U
#define LLC_FCS_POLYNOMIAL 0xAD85DDU
#define LLC_FCS_MASK 0xFFFFFFU
#define LLC_N202 4U
#define LLC_NU_MASK 0x1FFU
#define LLC_SAPIS 16U
#define LLC_N201_U 500U
#define LLC_QUEUE_SIZE 64U
#define LLC_MAX_BYTES (LLC_N201_U + LLC_UI_HEADER_BYTES + LLC_FCS_BYTES)

#define GSM48_PD_GMM 0x08U
#define GSM48_PD_SM 0x0AU
#define GSM48_PD_MASK 0x0FU
#define GSM48_TI_FLAG 0x80U
#define GSM48_LV_LENGTH_BYTES 1U
#define GSM48_TLV_LENGTH_OFFSET 1
#define GSM48_TLV_HEADER_BYTES 2U
#define GMM_ATTACH_REQUEST 0x01U
#define GMM_ATTACH_ACCEPT 0x02U
#define GMM_DETACH_REQUEST 0x05U
#define GMM_DETACH_ACCEPT 0x06U
#define GMM_RAU_REQUEST 0x08U
#define GMM_RAU_ACCEPT 0x09U
#define GMM_DETACH_TYPE_OFFSET 2U
#define GMM_DETACH_POWER_OFF 0x08U
#define GMM_ATTACH_RESULT_GPRS 0x01U
#define GMM_RAU_RESULT_RA_UPDATED 0x00U
#define GMM_DETACH_NO_FORCE_TO_STANDBY 0x00U
#define GMM_TIMER_DEACTIVATED 0xE0U
#define GMM_RADIO_PRIORITY_LOWEST 0x44U
#define GMM_ACCEPT_HEADER_MAX_BYTES 5U
#define GMM_RAI_BYTES (GSM48_LAI_BYTES + 1U)
#define SM_ACTIVATE_PDP_REQUEST 0x41U
#define SM_ACTIVATE_PDP_ACCEPT 0x42U
#define SM_DEACTIVATE_PDP_REQUEST 0x46U
#define SM_DEACTIVATE_PDP_ACCEPT 0x47U
#define SM_REQUEST_NSAPI_OFFSET 2U
#define SM_REQUEST_LLC_SAPI_OFFSET 3U
#define SM_REQUEST_QOS_OFFSET 4U
#define SM_IEI_PDP_ADDRESS 0x2BU
#define SM_IEI_PCO 0x27U
#define SM_RADIO_PRIORITY_LOWEST 0x04U
#define SM_PDP_TYPE_IETF 0x01U
#define SM_PDP_TYPE_IPV4 0x21U
#define SM_PDP_TYPE_BYTES 2U
#define SM_ACCEPT_MAX_BYTES 64U
#define SM_QOS_DELAY_CLASS_4 4U
#define SM_QOS_DELAY_CLASS_SHIFT 3
#define SM_QOS_RELIABILITY_UNACKNOWLEDGED_LLC 3U
#define SM_QOS_PEAK_THROUGHPUT_256K 9U
#define SM_QOS_PEAK_THROUGHPUT_SHIFT 4
#define SM_QOS_PRECEDENCE_NORMAL 2U
#define SM_QOS_MEAN_THROUGHPUT_BEST_EFFORT 31U
#define PCO_CONFIGURATION_PROTOCOL_PPP 0x80U
#define PCO_ITEM_HEADER_BYTES 3U
#define PCO_ITEM_LENGTH_OFFSET 2
#define PCO_IPCP 0x8021U
#define PCO_DNS_SERVER_IPV4 0x000DU
#define IPCP_HEADER_BYTES 4U
#define IPCP_IDENTIFIER_OFFSET 1
#define IPCP_LENGTH_OFFSET 2
#define IPCP_OPTION_LENGTH_OFFSET 1
#define IPCP_OPTION_VALUE_OFFSET 2
#define IPCP_CONFIGURE_REQUEST 0x01U
#define IPCP_CONFIGURE_NAK 0x03U
#define IPCP_PRIMARY_DNS 0x81U
#define IPCP_SECONDARY_DNS 0x83U
#define IPCP_ADDRESS_OPTION_BYTES 6U
#define IPCP_DNS_NAK_BYTES (IPCP_HEADER_BYTES + (2 * IPCP_ADDRESS_OPTION_BYTES))

#define SNDCP_F 0x40U
#define SNDCP_T 0x20U
#define SNDCP_M 0x10U
#define SNDCP_NSAPI_MASK 0x0FU
#define SNDCP_NPDU_MASK 0x0FFFU
#define SNDCP_NPDU_HIGH_SHIFT 8
#define SNDCP_SEGMENT_SHIFT 4
#define SNDCP_NO_COMPRESSION 0x00U
#define SNDCP_FIRST_HEADER_BYTES 4U
#define SNDCP_NEXT_HEADER_BYTES 3U

#define IPV4_ADDRESS_BYTES 4
#define ARP_HARDWARE_ETHERNET 1U
#define ARP_REPLY 2U

typedef enum GSMBtsDownlinkState GSMBtsDownlinkState;
typedef enum GSMBtsBlockState GSMBtsBlockState;
typedef struct GSMBitReader GSMBitReader;
typedef struct GSMBtsArpPacket GSMBtsArpPacket;
typedef struct GSMBtsLlcFrame GSMBtsLlcFrame;
typedef struct GSMBtsRlcChunk GSMBtsRlcChunk;
typedef struct GSMBtsUplinkTbf GSMBtsUplinkTbf;
typedef struct GSMBtsDownlinkTbf GSMBtsDownlinkTbf;

enum GSMBtsDownlinkState {
    GSM_BTS_DL_IDLE,
    GSM_BTS_DL_ACTIVE,
};

enum GSMBtsBlockState {
    GSM_BTS_BLOCK_FREE,
    GSM_BTS_BLOCK_SENT,
    GSM_BTS_BLOCK_NACKED,
    GSM_BTS_BLOCK_ACKED,
};

struct GSMBitReader {
    const uint8_t *data;
    size_t position;
};

struct QEMU_PACKED GSMBtsArpPacket {
    uint16_t hardware_type;
    uint16_t protocol_type;
    uint8_t hardware_length;
    uint8_t protocol_length;
    uint16_t operation;
    uint8_t sender_hardware[ETH_ALEN];
    uint8_t sender_protocol[IPV4_ADDRESS_BYTES];
    uint8_t target_hardware[ETH_ALEN];
    uint8_t target_protocol[IPV4_ADDRESS_BYTES];
};

_Static_assert(sizeof(GSMBtsArpPacket) == 28, "ARP packet layout");

struct GSMBtsLlcFrame {
    uint8_t data[LLC_MAX_BYTES];
    size_t length;
};

struct GSMBtsRlcChunk {
    size_t length;
    bool to_end;
    bool complete;
};

struct GSMBtsUplinkTbf {
    bool active;
    uint32_t tlli;
    bool tlli_acknowledged;
    uint8_t vq;
    uint8_t vr;
    bool received[RLC_SNS];
    uint8_t blocks[RLC_SNS][GSM_XCCH_BLOCK_BYTES];
    bool final_received;
    uint8_t final_bsn;
    bool final_acknowledged;
    uint32_t unacknowledged_blocks;
    uint32_t idle_blocks;
    GSMBtsMessage llc;
};

struct GSMBtsDownlinkTbf {
    GSMBtsDownlinkState state;
    uint8_t tfi;
    bool t3192_running;
    uint32_t start_frame;
    uint8_t vs;
    uint8_t va;
    GSMBtsBlockState block_state[RLC_SNS];
    uint8_t blocks[RLC_SNS][GSM_XCCH_BLOCK_BYTES];
    bool final_sent;
    uint32_t blocks_since_poll;
    uint32_t unanswered_polls;
    bool released_recently;
    uint32_t release_frame;
};

struct GSMBtsGprs {
    GSMBtsDownlinkBlock downlink;
    GSMBtsUplinkBlock uplink;

    uint32_t tlli;
    bool tlli_valid;
    bool poll_pending;
    uint32_t poll_frame;
    bool poll_downlink;
    bool single_block_pending;
    uint32_t single_block_frame;

    bool uplink_ack_pending;
    bool downlink_assignment_pending;
    bool uplink_assignment_pending;
    GSMBtsUplinkTbf ul;
    GSMBtsDownlinkTbf dl;
    uint8_t next_downlink_tfi;

    GSMBtsLlcFrame llc_queue[LLC_QUEUE_SIZE];
    size_t llc_head;
    size_t llc_count;
    size_t llc_offset;
    uint16_t nu[LLC_SAPIS];

    bool pdp_active;
    uint8_t nsapi;
    uint8_t data_sapi;
    uint16_t npdu;
    uint8_t sndcp_buffer[ETH_MTU];
    size_t sndcp_length;
};

static const uint8_t GPRS_QOS[] = {
    (SM_QOS_DELAY_CLASS_4 << SM_QOS_DELAY_CLASS_SHIFT) | SM_QOS_RELIABILITY_UNACKNOWLEDGED_LLC,
    (SM_QOS_PEAK_THROUGHPUT_256K << SM_QOS_PEAK_THROUGHPUT_SHIFT) | SM_QOS_PRECEDENCE_NORMAL,
    SM_QOS_MEAN_THROUGHPUT_BEST_EFFORT,
};
static const uint8_t GPRS_MS_ADDRESS[IPV4_ADDRESS_BYTES] = { 10, 0, 2, 15 };
static const uint8_t GPRS_DNS_ADDRESS[IPV4_ADDRESS_BYTES] = { 10, 0, 2, 3 };
static const uint8_t SLIRP_GATEWAY_MAC[ETH_ALEN] = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 };

static uint32_t gsm_bit_reader_get(GSMBitReader *reader, uint32_t count) {
    uint32_t value = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t byte = reader->data[reader->position / 8];

        value = (value << 1) | ((byte >> (7 - (reader->position % 8))) & 1);
        reader->position++;
    }
    return value;
}

static bool pdch_block_position(uint32_t frame_number, uint32_t *block_start, uint32_t *burst_index) {
    uint32_t position = frame_number % PDCH_MULTIFRAME_FRAMES;
    uint32_t group = position / PDCH_GROUP_FRAMES;

    if (position % PDCH_GROUP_FRAMES == PDCH_GROUP_FRAMES - 1)
        return false;

    *burst_index = (position - group) % GSM_XCCH_BLOCK_BURSTS;
    *block_start = frame_number - *burst_index;
    return true;
}

static uint32_t pdch_next_block(uint32_t block_start) {
    uint32_t position = block_start % PDCH_MULTIFRAME_FRAMES;
    uint32_t block = (position - (position / PDCH_GROUP_FRAMES)) / GSM_XCCH_BLOCK_BURSTS;
    uint32_t next = (block + 1) % PDCH_BLOCKS_PER_MULTIFRAME;
    uint32_t next_position = (GSM_XCCH_BLOCK_BURSTS * next) + (next / PDCH_BLOCKS_PER_GROUP);
    uint32_t step = next == 0 ? PDCH_MULTIFRAME_FRAMES - position : next_position - position;

    return gsm_bts_frame_number_add(block_start, step);
}

static uint32_t pdch_block_after(uint32_t frame_number) {
    uint32_t block_start;
    uint32_t burst_index;

    while (!pdch_block_position(frame_number, &block_start, &burst_index) || burst_index != 0)
        frame_number = gsm_bts_frame_number_add(frame_number, 1);
    return frame_number;
}

static bool frame_number_after(uint32_t a, uint32_t b) {
    uint32_t difference = (a + GSM_HYPERFRAME_FRAMES - b) % GSM_HYPERFRAME_FRAMES;

    return difference != 0 && difference < GSM_HYPERFRAME_FRAMES / 2;
}

static uint8_t rlc_mod(int value) {
    return (value + RLC_SNS) % RLC_SNS;
}

static uint32_t llc_fcs(const uint8_t *data, size_t length) {
    uint32_t crc = LLC_FCS_MASK;

    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8; bit++)
            crc = (crc & 1) ? ((crc >> 1) ^ LLC_FCS_POLYNOMIAL) : (crc >> 1);
    }
    return ~crc & LLC_FCS_MASK;
}

static void rlc_downlink_assign(GSMBts *bts, uint32_t frame_number);

static void llc_enqueue(GSMBts *bts, const uint8_t *frame, size_t length) {
    GSMBtsGprs *gprs = bts->gprs;
    size_t tail = (gprs->llc_head + gprs->llc_count) % LLC_QUEUE_SIZE;

    if (gprs->llc_count == LLC_QUEUE_SIZE)
        return;

    memcpy(gprs->llc_queue[tail].data, frame, length);
    gprs->llc_queue[tail].length = length;
    gprs->llc_count++;
    if (gprs->dl.state == GSM_BTS_DL_IDLE)
        rlc_downlink_assign(bts, gsm_air_frame_number(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)));
}

static void llc_send_frame(
    GSMBts *bts,
    uint8_t address,
    const uint8_t *control,
    size_t control_length,
    const uint8_t *info,
    size_t length
) {
    uint8_t frame[LLC_MAX_BYTES];
    size_t header = LLC_ADDRESS_BYTES + control_length;
    uint32_t fcs;

    if (header + length + LLC_FCS_BYTES > sizeof(frame))
        return;

    frame[0] = address;
    memcpy(&frame[LLC_ADDRESS_BYTES], control, control_length);
    memcpy(&frame[header], info, length);
    fcs = llc_fcs(frame, header + length);
    for (size_t i = 0; i < LLC_FCS_BYTES; i++)
        frame[header + length + i] = (fcs >> (8 * i)) & 0xFF;
    llc_enqueue(bts, frame, header + length + LLC_FCS_BYTES);
}

static void llc_send_ui(GSMBts *bts, uint8_t sapi, const uint8_t *info, size_t length) {
    GSMBtsGprs *gprs = bts->gprs;
    uint16_t nu = gprs->nu[sapi];
    uint8_t control[] = {
        LLC_UI | (nu >> LLC_UI_NU_HIGH_SHIFT),
        ((nu << LLC_UI_NU_LOW_SHIFT) & LLC_UI_NU_LOW_MASK) | LLC_UI_PM,
    };

    gprs->nu[sapi] = (nu + 1) & LLC_NU_MASK;
    llc_send_frame(bts, LLC_ADDRESS_COMMAND | sapi, control, sizeof(control), info, length);
}

static void gmm_encode_rai(uint8_t rai[GMM_RAI_BYTES]) {
    gsm_bts_encode_lai(rai);
    rai[GSM48_LAI_BYTES] = 0;
}

static void gmm_handle(GSMBts *bts, const uint8_t *message, size_t length) {
    uint8_t reply[GMM_ACCEPT_HEADER_MAX_BYTES + GMM_RAI_BYTES];
    size_t position = 0;

    reply[position++] = GSM48_PD_GMM;
    switch (message[GSM48_MESSAGE_TYPE_OFFSET]) {
        case GMM_ATTACH_REQUEST:
            bts->gprs->pdp_active = false;
            reply[position++] = GMM_ATTACH_ACCEPT;
            reply[position++] = GMM_ATTACH_RESULT_GPRS;
            reply[position++] = GMM_TIMER_DEACTIVATED;
            reply[position++] = GMM_RADIO_PRIORITY_LOWEST;
            break;
        case GMM_RAU_REQUEST:
            reply[position++] = GMM_RAU_ACCEPT;
            reply[position++] = GMM_RAU_RESULT_RA_UPDATED;
            reply[position++] = GMM_TIMER_DEACTIVATED;
            break;
        case GMM_DETACH_REQUEST:
            bts->gprs->pdp_active = false;
            if (length <= GMM_DETACH_TYPE_OFFSET)
                return;
            if (message[GMM_DETACH_TYPE_OFFSET] & GMM_DETACH_POWER_OFF)
                return;
            reply[position++] = GMM_DETACH_ACCEPT;
            reply[position++] = GMM_DETACH_NO_FORCE_TO_STANDBY;
            llc_send_ui(bts, LLC_SAPI_GMM, reply, position);
            return;
        default:
            return;
    }
    gmm_encode_rai(&reply[position]);
    llc_send_ui(bts, LLC_SAPI_GMM, reply, position + GMM_RAI_BYTES);
}

static uint8_t pco_ipcp_identifier(const uint8_t *pco, size_t length) {
    size_t offset = 1;

    while (offset + PCO_ITEM_HEADER_BYTES <= length) {
        uint16_t protocol = lduw_be_p(&pco[offset]);
        uint8_t item_length = pco[offset + PCO_ITEM_LENGTH_OFFSET];
        const uint8_t *item = &pco[offset + PCO_ITEM_HEADER_BYTES];

        if (offset + PCO_ITEM_HEADER_BYTES + item_length > length)
            break;
        if (protocol == PCO_IPCP && item_length > IPCP_IDENTIFIER_OFFSET && item[0] == IPCP_CONFIGURE_REQUEST)
            return item[IPCP_IDENTIFIER_OFFSET];
        offset += PCO_ITEM_HEADER_BYTES + item_length;
    }
    return 0;
}

static size_t pco_encode_item(uint8_t *pco, uint16_t protocol, const uint8_t *item, size_t length) {
    stw_be_p(pco, protocol);
    pco[PCO_ITEM_LENGTH_OFFSET] = length;
    memcpy(&pco[PCO_ITEM_HEADER_BYTES], item, length);
    return PCO_ITEM_HEADER_BYTES + length;
}

static size_t ipcp_encode_dns_option(uint8_t *option, uint8_t type) {
    option[0] = type;
    option[IPCP_OPTION_LENGTH_OFFSET] = IPCP_ADDRESS_OPTION_BYTES;
    memcpy(&option[IPCP_OPTION_VALUE_OFFSET], GPRS_DNS_ADDRESS, sizeof(GPRS_DNS_ADDRESS));
    return IPCP_ADDRESS_OPTION_BYTES;
}

static size_t sm_encode_pco(uint8_t *pco, uint8_t ipcp_identifier) {
    uint8_t ipcp[IPCP_DNS_NAK_BYTES] = { IPCP_CONFIGURE_NAK };
    size_t option = IPCP_HEADER_BYTES;
    size_t length = 0;

    ipcp[IPCP_IDENTIFIER_OFFSET] = ipcp_identifier;
    stw_be_p(&ipcp[IPCP_LENGTH_OFFSET], IPCP_DNS_NAK_BYTES);
    option += ipcp_encode_dns_option(&ipcp[option], IPCP_PRIMARY_DNS);
    ipcp_encode_dns_option(&ipcp[option], IPCP_SECONDARY_DNS);

    pco[length++] = PCO_CONFIGURATION_PROTOCOL_PPP;
    length += pco_encode_item(&pco[length], PCO_IPCP, ipcp, sizeof(ipcp));
    length += pco_encode_item(&pco[length], PCO_DNS_SERVER_IPV4, GPRS_DNS_ADDRESS, sizeof(GPRS_DNS_ADDRESS));
    return length;
}

static uint8_t sm_request_ipcp_identifier(const uint8_t *message, size_t length) {
    size_t offset;

    if (length <= SM_REQUEST_QOS_OFFSET)
        return 0;
    offset = SM_REQUEST_QOS_OFFSET + GSM48_LV_LENGTH_BYTES + message[SM_REQUEST_QOS_OFFSET];
    if (offset >= length)
        return 0;
    offset += GSM48_LV_LENGTH_BYTES + message[offset];

    while (offset + GSM48_TLV_HEADER_BYTES <= length) {
        uint8_t iei = message[offset];
        uint8_t ie_length = message[offset + GSM48_TLV_LENGTH_OFFSET];
        const uint8_t *value = &message[offset + GSM48_TLV_HEADER_BYTES];

        if (offset + GSM48_TLV_HEADER_BYTES + ie_length > length)
            break;
        if (iei == SM_IEI_PCO)
            return pco_ipcp_identifier(value, ie_length);
        offset += GSM48_TLV_HEADER_BYTES + ie_length;
    }
    return 0;
}

static void sm_handle_activate(GSMBts *bts, const uint8_t *message, size_t length) {
    GSMBtsGprs *gprs = bts->gprs;
    uint8_t accept[SM_ACCEPT_MAX_BYTES];
    size_t position = 0;

    if (length <= SM_REQUEST_QOS_OFFSET)
        return;

    gprs->nsapi = message[SM_REQUEST_NSAPI_OFFSET] & SNDCP_NSAPI_MASK;
    gprs->data_sapi = message[SM_REQUEST_LLC_SAPI_OFFSET] & LLC_SAPI_MASK;
    gprs->pdp_active = true;
    gprs->npdu = 0;
    gprs->sndcp_length = 0;

    accept[position++] = message[0] ^ GSM48_TI_FLAG;
    accept[position++] = SM_ACTIVATE_PDP_ACCEPT;
    accept[position++] = gprs->data_sapi;
    accept[position++] = sizeof(GPRS_QOS);
    memcpy(&accept[position], GPRS_QOS, sizeof(GPRS_QOS));
    position += sizeof(GPRS_QOS);
    accept[position++] = SM_RADIO_PRIORITY_LOWEST;
    accept[position++] = SM_IEI_PDP_ADDRESS;
    accept[position++] = SM_PDP_TYPE_BYTES + sizeof(GPRS_MS_ADDRESS);
    accept[position++] = SM_PDP_TYPE_IETF;
    accept[position++] = SM_PDP_TYPE_IPV4;
    memcpy(&accept[position], GPRS_MS_ADDRESS, sizeof(GPRS_MS_ADDRESS));
    position += sizeof(GPRS_MS_ADDRESS);
    uint8_t ipcp_identifier = sm_request_ipcp_identifier(message, length);

    accept[position++] = SM_IEI_PCO;
    accept[position] = sm_encode_pco(&accept[position + GSM48_LV_LENGTH_BYTES], ipcp_identifier);
    position += GSM48_LV_LENGTH_BYTES + accept[position];
    llc_send_ui(bts, LLC_SAPI_GMM, accept, position);
}

static void sm_handle(GSMBts *bts, const uint8_t *message, size_t length) {
    switch (message[GSM48_MESSAGE_TYPE_OFFSET]) {
        case SM_ACTIVATE_PDP_REQUEST:
            sm_handle_activate(bts, message, length);
            break;
        case SM_DEACTIVATE_PDP_REQUEST: {
            uint8_t accept[] = { message[0] ^ GSM48_TI_FLAG, SM_DEACTIVATE_PDP_ACCEPT };

            bts->gprs->pdp_active = false;
            llc_send_ui(bts, LLC_SAPI_GMM, accept, sizeof(accept));
            break;
        }
    }
}

static void sndcp_deliver(GSMBts *bts, const uint8_t *packet, size_t length) {
    uint8_t frame[sizeof(struct eth_header) + ETH_MTU];
    struct eth_header *header = (struct eth_header *) frame;

    memcpy(header->h_dest, SLIRP_GATEWAY_MAC, ETH_ALEN);
    memcpy(header->h_source, bts->nic_conf.macaddr.a, ETH_ALEN);
    stw_be_p(&header->h_proto, ETH_P_IP);
    memcpy(&frame[sizeof(*header)], packet, length);
    gsm_bts_send_ethernet(bts, frame, sizeof(*header) + length);
}

static void sndcp_handle_unitdata(GSMBts *bts, const uint8_t *pdu, size_t length) {
    GSMBtsGprs *gprs = bts->gprs;
    bool first = pdu[0] & SNDCP_F;
    bool more = pdu[0] & SNDCP_M;
    size_t header = first ? SNDCP_FIRST_HEADER_BYTES : SNDCP_NEXT_HEADER_BYTES;

    if (!gprs->pdp_active)
        return;
    if (!(pdu[0] & SNDCP_T))
        return;
    if (length < header)
        return;

    if (first)
        gprs->sndcp_length = 0;
    if (gprs->sndcp_length + length - header > sizeof(gprs->sndcp_buffer)) {
        gprs->sndcp_length = 0;
        return;
    }
    memcpy(&gprs->sndcp_buffer[gprs->sndcp_length], &pdu[header], length - header);
    gprs->sndcp_length += length - header;
    if (more)
        return;

    sndcp_deliver(bts, gprs->sndcp_buffer, gprs->sndcp_length);
    gprs->sndcp_length = 0;
}

static void sndcp_send(GSMBts *bts, const uint8_t *packet, size_t length) {
    GSMBtsGprs *gprs = bts->gprs;
    uint16_t npdu = gprs->npdu;
    uint8_t segment = 0;
    size_t offset = 0;

    gprs->npdu = (npdu + 1) & SNDCP_NPDU_MASK;
    while (offset < length) {
        uint8_t pdu[LLC_N201_U];
        bool first = offset == 0;
        size_t header = first ? SNDCP_FIRST_HEADER_BYTES : SNDCP_NEXT_HEADER_BYTES;
        size_t chunk = MIN(length - offset, LLC_N201_U - header);
        bool more = offset + chunk < length;
        size_t position = 0;

        pdu[position++] = (first ? SNDCP_F : 0) | SNDCP_T | (more ? SNDCP_M : 0) | gprs->nsapi;
        if (first)
            pdu[position++] = SNDCP_NO_COMPRESSION;
        pdu[position++] = (segment << SNDCP_SEGMENT_SHIFT) | (npdu >> SNDCP_NPDU_HIGH_SHIFT);
        pdu[position++] = npdu & 0xFF;
        memcpy(&pdu[position], &packet[offset], chunk);
        llc_send_ui(bts, gprs->data_sapi, pdu, position + chunk);
        offset += chunk;
        segment++;
    }
}

static bool llc_fcs_valid(const uint8_t *frame, size_t length, bool protected_mode) {
    size_t covered = length - LLC_FCS_BYTES;
    uint32_t fcs;

    if (!protected_mode)
        covered = MIN(covered, LLC_UI_HEADER_BYTES + LLC_N202);
    fcs = llc_fcs(frame, covered);
    for (size_t i = 0; i < LLC_FCS_BYTES; i++) {
        if (frame[length - LLC_FCS_BYTES + i] != ((fcs >> (8 * i)) & 0xFF))
            return false;
    }
    return true;
}

static void llc_handle_ui(GSMBts *bts, const uint8_t *frame, size_t length) {
    uint8_t sapi = frame[0] & LLC_SAPI_MASK;
    const uint8_t *info = &frame[LLC_UI_HEADER_BYTES];
    size_t info_length;

    if (length < LLC_UI_HEADER_BYTES + LLC_FCS_BYTES)
        return;
    if (!llc_fcs_valid(frame, length, frame[2] & LLC_UI_PM))
        return;

    info_length = length - LLC_UI_HEADER_BYTES - LLC_FCS_BYTES;
    if (sapi == bts->gprs->data_sapi && info_length > 0) {
        sndcp_handle_unitdata(bts, info, info_length);
        return;
    }
    if (sapi != LLC_SAPI_GMM || info_length < GSM48_L3_HEADER_BYTES)
        return;

    switch (info[0] & GSM48_PD_MASK) {
        case GSM48_PD_GMM:
            gmm_handle(bts, info, info_length);
            break;
        case GSM48_PD_SM:
            sm_handle(bts, info, info_length);
            break;
    }
}

static void llc_handle_xid(GSMBts *bts, const uint8_t *frame, size_t length) {
    uint8_t control = LLC_U | (frame[1] & LLC_U_PF) | LLC_U_XID;

    if (!llc_fcs_valid(frame, length, true))
        return;

    llc_send_frame(bts, frame[0] & LLC_SAPI_MASK, &control, sizeof(control), &frame[LLC_U_HEADER_BYTES],
        length - LLC_U_HEADER_BYTES - LLC_FCS_BYTES);
}

static void llc_handle_frame(GSMBts *bts, const uint8_t *frame, size_t length) {
    if (length < LLC_U_HEADER_BYTES + LLC_FCS_BYTES)
        return;

    bool ui = (frame[1] & LLC_UI_MASK) == LLC_UI;
    bool xid = (frame[1] & LLC_UI_MASK) == LLC_U && (frame[1] & LLC_U_COMMAND_MASK) == LLC_U_XID;

    if (ui) {
        llc_handle_ui(bts, frame, length);
    } else if (xid) {
        llc_handle_xid(bts, frame, length);
    }
}

static void gsm_bts_reply_arp(GSMBts *bts, const struct eth_header *request_header, const GSMBtsArpPacket *request) {
    uint8_t frame[sizeof(struct eth_header) + sizeof(GSMBtsArpPacket)];
    struct eth_header *header = (struct eth_header *) frame;
    GSMBtsArpPacket *reply = (GSMBtsArpPacket *) &frame[sizeof(*header)];

    memcpy(header->h_dest, request_header->h_source, ETH_ALEN);
    memcpy(header->h_source, bts->nic_conf.macaddr.a, ETH_ALEN);
    stw_be_p(&header->h_proto, ETH_P_ARP);
    *reply = *request;
    stw_be_p(&reply->operation, ARP_REPLY);
    memcpy(reply->sender_hardware, bts->nic_conf.macaddr.a, ETH_ALEN);
    memcpy(reply->sender_protocol, GPRS_MS_ADDRESS, sizeof(GPRS_MS_ADDRESS));
    memcpy(reply->target_hardware, request->sender_hardware, ETH_ALEN);
    memcpy(reply->target_protocol, request->sender_protocol, sizeof(reply->target_protocol));
    gsm_bts_send_ethernet(bts, frame, sizeof(frame));
}

static void gsm_bts_handle_arp(GSMBts *bts, const struct eth_header *header, const uint8_t *payload, size_t size) {
    const GSMBtsArpPacket *arp = (const GSMBtsArpPacket *) payload;

    if (size < sizeof(*arp))
        return;
    if (lduw_be_p(&arp->hardware_type) != ARP_HARDWARE_ETHERNET)
        return;
    if (memcmp(arp->target_protocol, GPRS_MS_ADDRESS, sizeof(GPRS_MS_ADDRESS)) != 0)
        return;

    gsm_bts_reply_arp(bts, header, arp);
}

static void gsm_bts_handle_ipv4(GSMBts *bts, const uint8_t *packet, size_t size) {
    const struct ip_header *ip = (const struct ip_header *) packet;
    size_t ip_length;

    if (!bts->gprs->pdp_active)
        return;
    if (size < sizeof(*ip))
        return;
    if (memcmp(&ip->ip_dst, GPRS_MS_ADDRESS, sizeof(GPRS_MS_ADDRESS)) != 0)
        return;
    ip_length = lduw_be_p(&ip->ip_len);
    if (ip_length > size)
        return;

    sndcp_send(bts, packet, ip_length);
}

void gsm_bts_gprs_handle_ethernet(GSMBts *bts, const uint8_t *frame, size_t size) {
    const struct eth_header *header = (const struct eth_header *) frame;
    const uint8_t *payload = &frame[sizeof(*header)];

    if (size < sizeof(*header))
        return;

    switch (lduw_be_p(&header->h_proto)) {
        case ETH_P_ARP:
            gsm_bts_handle_arp(bts, header, payload, size - sizeof(*header));
            break;
        case ETH_P_IP:
            gsm_bts_handle_ipv4(bts, payload, size - sizeof(*header));
            break;
    }
}

static size_t rlc_uplink_decode_chunks(
    const uint8_t block[GSM_XCCH_BLOCK_BYTES],
    GSMBtsRlcChunk chunks[RLC_MAX_CHUNKS],
    size_t *chunk_count
) {
    bool last_block = ((block[RLC_MAC_HEADER_OCTET] >> RLC_UL_COUNTDOWN_SHIFT) & RLC_UL_COUNTDOWN_MASK) == 0;
    bool extension = block[RLC_BSN_OCTET] & RLC_EXTENSION;
    size_t offset = RLC_HEADER_BYTES;

    *chunk_count = 0;
    if (extension)
        chunks[(*chunk_count)++] = (GSMBtsRlcChunk) { .to_end = true, .complete = last_block };

    while (!extension && offset < GSM_XCCH_BLOCK_BYTES) {
        uint8_t li = block[offset] >> RLC_LI_SHIFT;
        bool more = block[offset] & RLC_LI_MORE;

        extension = block[offset] & RLC_EXTENSION;
        offset++;
        if (li == 0) {
            chunks[(*chunk_count)++] = (GSMBtsRlcChunk) { .to_end = true, .complete = last_block };
            break;
        }
        chunks[(*chunk_count)++] = (GSMBtsRlcChunk) { .length = li, .complete = true };
        if (extension && more)
            chunks[(*chunk_count)++] = (GSMBtsRlcChunk) { .to_end = true, .complete = last_block };
    }
    return offset;
}

static void rlc_uplink_deliver_block(GSMBts *bts, const uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    GSMBtsMessage *llc = &bts->gprs->ul.llc;
    GSMBtsRlcChunk chunks[RLC_MAX_CHUNKS];
    size_t chunk_count;
    size_t offset = rlc_uplink_decode_chunks(block, chunks, &chunk_count);

    if (block[RLC_TFI_OCTET] & RLC_UL_TLLI_INDICATOR)
        offset += RLC_TLLI_BYTES;

    for (size_t i = 0; i < chunk_count && offset <= GSM_XCCH_BLOCK_BYTES; i++) {
        size_t available = GSM_XCCH_BLOCK_BYTES - offset;
        size_t length = chunks[i].to_end ? available : MIN(chunks[i].length, available);

        if (llc->length + length > sizeof(llc->data)) {
            llc->length = 0;
            return;
        }
        memcpy(&llc->data[llc->length], &block[offset], length);
        llc->length += length;
        offset += length;
        if (chunks[i].complete) {
            llc_handle_frame(bts, llc->data, llc->length);
            llc->length = 0;
        }
    }
}

static void rlc_uplink_release(GSMBtsGprs *gprs) {
    memset(&gprs->ul, 0, sizeof(gprs->ul));
    gprs->uplink_ack_pending = false;
}

static void rlc_uplink_start(GSMBtsGprs *gprs) {
    rlc_uplink_release(gprs);
    gprs->ul.active = true;
}

static bool rlc_uplink_is_complete(const GSMBtsUplinkTbf *ul) {
    return ul->final_received && ul->vq == rlc_mod(ul->final_bsn + 1);
}

static void rlc_uplink_learn_tlli(GSMBtsGprs *gprs, const uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    GSMBtsRlcChunk chunks[RLC_MAX_CHUNKS];
    size_t chunk_count;
    size_t offset = rlc_uplink_decode_chunks(block, chunks, &chunk_count);

    if (offset + RLC_TLLI_BYTES > GSM_XCCH_BLOCK_BYTES)
        return;

    gprs->ul.tlli = ldl_be_p(&block[offset]);
    gprs->tlli = gprs->ul.tlli;
    gprs->tlli_valid = true;
    gprs->uplink_ack_pending = true;
}

static void rlc_uplink_handle_data(GSMBts *bts, const uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    GSMBtsGprs *gprs = bts->gprs;
    GSMBtsUplinkTbf *ul = &gprs->ul;
    uint8_t tfi = (block[RLC_TFI_OCTET] >> RLC_TFI_SHIFT) & RLC_TFI_MASK;
    uint8_t bsn = block[RLC_BSN_OCTET] >> RLC_BSN_SHIFT;
    uint8_t countdown = (block[RLC_MAC_HEADER_OCTET] >> RLC_UL_COUNTDOWN_SHIFT) & RLC_UL_COUNTDOWN_MASK;

    if (!ul->active)
        return;
    if (tfi != GPRS_UPLINK_TFI)
        return;

    ul->idle_blocks = 0;
    if ((block[RLC_TFI_OCTET] & RLC_UL_TLLI_INDICATOR) && !ul->tlli_acknowledged)
        rlc_uplink_learn_tlli(gprs, block);
    if (countdown == 0) {
        ul->final_received = true;
        ul->final_bsn = bsn;
    }
    if (rlc_mod(bsn - ul->vq) >= RLC_WINDOW)
        return;
    if (ul->received[bsn])
        return;

    ul->received[bsn] = true;
    memcpy(ul->blocks[bsn], block, GSM_XCCH_BLOCK_BYTES);
    if (rlc_mod(bsn - ul->vr) < RLC_WINDOW)
        ul->vr = rlc_mod(bsn + 1);
    while (ul->received[ul->vq] && ul->vq != ul->vr) {
        rlc_uplink_deliver_block(bts, ul->blocks[ul->vq]);
        ul->received[rlc_mod(ul->vq - RLC_WINDOW)] = false;
        ul->vq = rlc_mod(ul->vq + 1);
    }

    if (++ul->unacknowledged_blocks >= RLC_UPLINK_ACK_BLOCKS || rlc_uplink_is_complete(ul))
        gprs->uplink_ack_pending = true;
}

static void rlcmac_put_uplink_ack_nack_description(GSMBitWriter *writer, const GSMBtsUplinkTbf *ul, bool final) {
    gsm_bit_writer_put(writer, final, 1);
    gsm_bit_writer_put(writer, ul->vr, RLCMAC_SSN_BITS);
    for (uint32_t i = 0; i < RLC_WINDOW; i++) {
        uint8_t bsn = rlc_mod(ul->vr - RLC_WINDOW + i);
        uint8_t behind_vq = rlc_mod(ul->vq - bsn);
        bool delivered = behind_vq != 0 && behind_vq <= RLC_WINDOW;

        gsm_bit_writer_put(writer, ul->received[bsn] || delivered, 1);
    }
}

static void rlcmac_put_control_header(GSMBitWriter *writer, bool poll, uint8_t usf) {
    gsm_bit_writer_put(writer, RLCMAC_PT_CONTROL, RLCMAC_PAYLOAD_TYPE_BITS);
    gsm_bit_writer_put(writer, RLCMAC_RRBP_N13, RLCMAC_RRBP_BITS);
    gsm_bit_writer_put(writer, poll, RLCMAC_POLL_BITS);
    gsm_bit_writer_put(writer, usf, RLCMAC_USF_BITS);
}

static void rlcmac_put_message_type(GSMBitWriter *writer, uint8_t message_type) {
    gsm_bit_writer_put(writer, message_type, RLCMAC_MESSAGE_TYPE_BITS);
    gsm_bit_writer_put(writer, RLCMAC_PAGE_MODE_NORMAL, RLCMAC_PAGE_MODE_BITS);
}

static void rlcmac_put_timing_advance(GSMBitWriter *writer) {
    gsm_bit_writer_put(writer, CSN_PRESENT, 1);
    gsm_bit_writer_put(writer, RLCMAC_TIMING_ADVANCE, RLCMAC_TIMING_ADVANCE_BITS);
    gsm_bit_writer_put(writer, CSN_ABSENT, 1);
}

static void rlcmac_put_frequency_parameters(GSMBitWriter *writer, uint16_t arfcn) {
    gsm_bit_writer_put(writer, GSM_BTS_TSC, RLCMAC_TSC_BITS);
    gsm_bit_writer_put(writer, RLCMAC_FREQUENCY_ARFCN, RLCMAC_FREQUENCY_CHOICE_BITS);
    gsm_bit_writer_put(writer, arfcn, RLCMAC_ARFCN_BITS);
}

static void rlcmac_put_tlli(GSMBitWriter *writer, uint32_t tlli) {
    gsm_bit_writer_put(writer, RLCMAC_ID_TLLI, RLCMAC_ID_TLLI_BITS);
    gsm_bit_writer_put(writer, tlli, RLCMAC_TLLI_BITS);
}

static void rlcmac_put_global_tfi(GSMBitWriter *writer, uint8_t direction, uint8_t tfi) {
    gsm_bit_writer_put(writer, RLCMAC_ID_GLOBAL_TFI, 1);
    gsm_bit_writer_put(writer, direction, 1);
    gsm_bit_writer_put(writer, tfi, RLCMAC_TFI_BITS);
}

static void rlcmac_encode_uplink_ack(
    const GSMBtsGprs *gprs,
    bool final,
    bool poll,
    uint8_t usf,
    uint8_t block[GSM_XCCH_BLOCK_BYTES]
) {
    GSMBitWriter writer = { .data = block };

    rlcmac_put_control_header(&writer, poll, usf);
    rlcmac_put_message_type(&writer, RLCMAC_DL_PACKET_UPLINK_ACK_NACK);
    gsm_bit_writer_put(&writer, RLCMAC_UPLINK_ACK_UPLINK_TFI, RLCMAC_UPLINK_ACK_TFI_CHOICE_BITS);
    gsm_bit_writer_put(&writer, GPRS_UPLINK_TFI, RLCMAC_TFI_BITS);
    gsm_bit_writer_put(&writer, RLCMAC_GPRS, 1);
    gsm_bit_writer_put(&writer, RLCMAC_CHANNEL_CODING_CS1, RLCMAC_CHANNEL_CODING_BITS);
    rlcmac_put_uplink_ack_nack_description(&writer, &gprs->ul, final);
    gsm_bit_writer_put(&writer, gprs->tlli_valid, 1);
    if (gprs->tlli_valid)
        gsm_bit_writer_put(&writer, gprs->ul.tlli, RLCMAC_TLLI_BITS);
    gsm_bit_writer_put(&writer, CSN_PRESENT, 1);
    rlcmac_put_timing_advance(&writer);
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    gsm_bit_writer_put(&writer, CSN_PRESENT, 1);
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    gsm_bit_writer_put(&writer, RLCMAC_TBF_ESTABLISHMENT_ALLOWED, 1);
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    gsm_bit_writer_pad(&writer, GSM_XCCH_BLOCK_BYTES);
}

static void rlcmac_encode_downlink_assignment(
    const GSMBtsGprs *gprs,
    uint16_t arfcn,
    uint8_t usf,
    uint8_t block[GSM_XCCH_BLOCK_BYTES]
) {
    GSMBitWriter writer = { .data = block };

    rlcmac_put_control_header(&writer, false, usf);
    rlcmac_put_message_type(&writer, RLCMAC_DL_PACKET_DOWNLINK_ASSIGNMENT);
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    if (gprs->ul.active) {
        rlcmac_put_global_tfi(&writer, RLCMAC_GLOBAL_TFI_UPLINK, GPRS_UPLINK_TFI);
    } else {
        rlcmac_put_tlli(&writer, gprs->tlli);
    }
    gsm_bit_writer_put(&writer, RLCMAC_NO_MESSAGE_ESCAPE, 1);
    gsm_bit_writer_put(&writer, RLCMAC_MAC_MODE_DYNAMIC, RLCMAC_MAC_MODE_BITS);
    gsm_bit_writer_put(&writer, RLCMAC_RLC_MODE_ACKNOWLEDGED, 1);
    gsm_bit_writer_put(&writer, gprs->dl.t3192_running, 1);
    gsm_bit_writer_put(&writer, RLCMAC_PDCH_TIMESLOT_BIT, RLCMAC_TIMESLOT_ALLOCATION_BITS);
    rlcmac_put_timing_advance(&writer);
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    gsm_bit_writer_put(&writer, CSN_PRESENT, 1);
    rlcmac_put_frequency_parameters(&writer, arfcn);
    gsm_bit_writer_put(&writer, CSN_PRESENT, 1);
    gsm_bit_writer_put(&writer, gprs->dl.tfi, RLCMAC_TFI_BITS);
    gsm_bit_writer_put(&writer, CSN_PRESENT, 1);
    gsm_bit_writer_put(&writer, RLCMAC_ALPHA, RLCMAC_ALPHA_BITS);
    for (uint32_t tn = 0; tn < GSM_TIMESLOTS; tn++) {
        gsm_bit_writer_put(&writer, tn == GSM_BTS_PDCH_TIMESLOT, 1);
        if (tn == GSM_BTS_PDCH_TIMESLOT)
            gsm_bit_writer_put(&writer, RLCMAC_GAMMA, RLCMAC_GAMMA_BITS);
    }
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    gsm_bit_writer_pad(&writer, GSM_XCCH_BLOCK_BYTES);
}

static void rlcmac_encode_uplink_assignment(
    const GSMBtsGprs *gprs,
    uint16_t arfcn,
    uint8_t usf,
    uint8_t block[GSM_XCCH_BLOCK_BYTES]
) {
    GSMBitWriter writer = { .data = block };

    rlcmac_put_control_header(&writer, false, usf);
    rlcmac_put_message_type(&writer, RLCMAC_DL_PACKET_UPLINK_ASSIGNMENT);
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    if (gprs->dl.state == GSM_BTS_DL_ACTIVE) {
        rlcmac_put_global_tfi(&writer, RLCMAC_GLOBAL_TFI_DOWNLINK, gprs->dl.tfi);
    } else {
        rlcmac_put_tlli(&writer, gprs->tlli);
    }
    gsm_bit_writer_put(&writer, RLCMAC_GPRS, 1);
    gsm_bit_writer_put(&writer, RLCMAC_CHANNEL_CODING_CS1, RLCMAC_CHANNEL_CODING_BITS);
    gsm_bit_writer_put(&writer, RLCMAC_TLLI_BLOCK_COMMANDED_CODING, 1);
    rlcmac_put_timing_advance(&writer);
    gsm_bit_writer_put(&writer, CSN_PRESENT, 1);
    rlcmac_put_frequency_parameters(&writer, arfcn);
    gsm_bit_writer_put(&writer, RLCMAC_ALLOCATION_DYNAMIC, RLCMAC_ALLOCATION_BITS);
    gsm_bit_writer_put(&writer, RLCMAC_NOT_EXTENDED_DYNAMIC, 1);
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    gsm_bit_writer_put(&writer, RLCMAC_USF_GRANULARITY_ONE_BLOCK, 1);
    gsm_bit_writer_put(&writer, CSN_PRESENT, 1);
    gsm_bit_writer_put(&writer, GPRS_UPLINK_TFI, RLCMAC_TFI_BITS);
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    gsm_bit_writer_put(&writer, RLCMAC_TIMESLOT_ALLOCATION, 1);
    for (uint32_t tn = 0; tn < GSM_TIMESLOTS; tn++) {
        gsm_bit_writer_put(&writer, tn == GSM_BTS_PDCH_TIMESLOT, 1);
        if (tn == GSM_BTS_PDCH_TIMESLOT)
            gsm_bit_writer_put(&writer, GPRS_USF, RLCMAC_USF_BITS);
    }
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    gsm_bit_writer_pad(&writer, GSM_XCCH_BLOCK_BYTES);
}

static void rlcmac_encode_dummy(uint8_t usf, uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    GSMBitWriter writer = { .data = block };

    rlcmac_put_control_header(&writer, false, usf);
    rlcmac_put_message_type(&writer, RLCMAC_DL_PACKET_DUMMY_CONTROL_BLOCK);
    gsm_bit_writer_put(&writer, CSN_ABSENT, 1);
    gsm_bit_writer_pad(&writer, GSM_XCCH_BLOCK_BYTES);
}

static GSMBitWriter gsm_bts_encode_packet_assignment(
    uint8_t block[GSM_XCCH_BLOCK_BYTES],
    uint16_t arfcn,
    bool downlink,
    uint8_t ra,
    uint32_t reference_frame
) {
    GSMImmediateAssignment *ia = (GSMImmediateAssignment *) block;
    uint8_t channel_description[GSM_CHANNEL_DESCRIPTION_BYTES];
    GSMBitWriter description = { .data = channel_description };
    GSMBitWriter rest = { .data = ia->rest_octets };
    uint8_t mode = IA_MODE_TBF | (downlink ? IA_MODE_DOWNLINK : 0);

    gsm_bit_writer_put(&description, IA_PACKET_CHANNEL_PDTCH, IA_PACKET_CHANNEL_TYPE_BITS);
    gsm_bit_writer_put(&description, GSM_BTS_PDCH_TIMESLOT, IA_TIMESLOT_BITS);
    gsm_bit_writer_put(&description, GSM_BTS_TSC, RLCMAC_TSC_BITS);
    gsm_bit_writer_put(&description, IA_NON_HOPPING, IA_HOPPING_BITS);
    gsm_bit_writer_put(&description, arfcn, RLCMAC_ARFCN_BITS);
    gsm_bts_encode_immediate_assignment(ia, mode, channel_description, ra, reference_frame);

    gsm_bit_writer_put_lh(&rest, true);
    gsm_bit_writer_put_lh(&rest, true);
    gsm_bit_writer_put(&rest, downlink ? IA_PACKET_DOWNLINK_ASSIGNMENT : IA_PACKET_UPLINK_ASSIGNMENT,
        IA_ASSIGNMENT_BITS);
    return rest;
}

static void gsm_bts_encode_uplink_assignment(
    uint8_t block[GSM_XCCH_BLOCK_BYTES],
    uint16_t arfcn,
    uint8_t ra,
    uint32_t frame_number
) {
    GSMBitWriter rest = gsm_bts_encode_packet_assignment(block, arfcn, false, ra, frame_number);

    gsm_bit_writer_put(&rest, IA_MULTI_BLOCK_ALLOCATION, 1);
    gsm_bit_writer_put(&rest, GPRS_UPLINK_TFI, RLCMAC_TFI_BITS);
    gsm_bit_writer_put(&rest, RLCMAC_POLLING_NONE, 1);
    gsm_bit_writer_put(&rest, CSN_ABSENT, 1);
    gsm_bit_writer_put(&rest, GPRS_USF, RLCMAC_USF_BITS);
    gsm_bit_writer_put(&rest, RLCMAC_USF_GRANULARITY_ONE_BLOCK, 1);
    gsm_bit_writer_put(&rest, CSN_ABSENT, 1);
    gsm_bit_writer_put(&rest, RLCMAC_CHANNEL_CODING_CS1, RLCMAC_CHANNEL_CODING_BITS);
    gsm_bit_writer_put(&rest, RLCMAC_TLLI_BLOCK_COMMANDED_CODING, 1);
    gsm_bit_writer_put(&rest, CSN_ABSENT, 1);
    gsm_bit_writer_put(&rest, RLCMAC_GAMMA, RLCMAC_GAMMA_BITS);
    gsm_bit_writer_put(&rest, CSN_ABSENT, 1);
    gsm_bit_writer_put(&rest, CSN_ABSENT, 1);
}

static void gsm_bts_encode_single_block_assignment(
    uint8_t block[GSM_XCCH_BLOCK_BYTES],
    uint16_t arfcn,
    uint8_t ra,
    uint32_t frame_number,
    uint32_t single_block_frame
) {
    GSMBitWriter rest = gsm_bts_encode_packet_assignment(block, arfcn, false, ra, frame_number);

    gsm_bit_writer_put(&rest, IA_SINGLE_BLOCK_ALLOCATION, 1);
    gsm_bit_writer_put(&rest, CSN_ABSENT, 1);
    gsm_bit_writer_put(&rest, RLCMAC_GAMMA, RLCMAC_GAMMA_BITS);
    gsm_bit_writer_put(&rest, CSN_ABSENT, 1);
    gsm_bit_writer_put(&rest, CSN_PRESENT, 1);
    gsm_bit_writer_put(&rest, gsm_starting_time(single_block_frame), IA_STARTING_TIME_BITS);
    gsm_bit_writer_put_lh(&rest, false);
    gsm_bit_writer_put_lh(&rest, false);
    gsm_bit_writer_put_lh(&rest, false);
}

static void gsm_bts_encode_downlink_assignment(
    uint8_t block[GSM_XCCH_BLOCK_BYTES],
    uint16_t arfcn,
    uint32_t reference_frame,
    uint32_t tlli,
    uint8_t tfi
) {
    GSMBitWriter rest = gsm_bts_encode_packet_assignment(block, arfcn, true, IA_DOWNLINK_REFERENCE_RA, reference_frame);

    gsm_bit_writer_put(&rest, tlli, RLCMAC_TLLI_BITS);
    gsm_bit_writer_put(&rest, CSN_PRESENT, 1);
    gsm_bit_writer_put(&rest, tfi, RLCMAC_TFI_BITS);
    gsm_bit_writer_put(&rest, RLCMAC_RLC_MODE_ACKNOWLEDGED, 1);
    gsm_bit_writer_put(&rest, CSN_ABSENT, 1);
    gsm_bit_writer_put(&rest, RLCMAC_GAMMA, RLCMAC_GAMMA_BITS);
    gsm_bit_writer_put(&rest, RLCMAC_POLLING_NONE, 1);
    gsm_bit_writer_put(&rest, CSN_PRESENT, 1);
    gsm_bit_writer_put(&rest, CSN_ABSENT, 1);
    gsm_bit_writer_put(&rest, CSN_ABSENT, 1);
    gsm_bit_writer_put(&rest, CSN_ABSENT, 1);
    gsm_bit_writer_put_lh(&rest, false);
}

bool gsm_bts_gprs_handle_rach(GSMBts *bts, uint8_t ra, uint32_t frame_number) {
    GSMBtsGprs *gprs = bts->gprs;
    uint8_t message[GSM_XCCH_BLOCK_BYTES];

    switch (ra & RACH_PACKET_ACCESS_MASK) {
        case RACH_ONE_PHASE:
            rlc_uplink_start(gprs);
            gsm_bts_encode_uplink_assignment(message, bts->arfcn, ra, frame_number);
            break;
        case RACH_SINGLE_BLOCK: {
            uint32_t earliest_frame = gsm_bts_frame_number_add(frame_number, GPRS_SINGLE_BLOCK_DELAY_FRAMES);

            gprs->single_block_pending = true;
            gprs->single_block_frame = pdch_block_after(earliest_frame);
            gsm_bts_encode_single_block_assignment(message, bts->arfcn, ra, frame_number, gprs->single_block_frame);
            break;
        }
        default:
            return false;
    }
    gsm_bts_ccch_enqueue(bts, message);
    return true;
}

static void rlc_downlink_reset(GSMBtsDownlinkTbf *dl) {
    bool released_recently = dl->released_recently;
    uint32_t release_frame = dl->release_frame;

    memset(dl, 0, sizeof(*dl));
    dl->released_recently = released_recently;
    dl->release_frame = release_frame;
}

static bool rlc_downlink_is_t3192_running(const GSMBtsDownlinkTbf *dl, uint32_t frame_number) {
    uint32_t expiry = gsm_bts_frame_number_add(dl->release_frame, GPRS_T3192_FRAMES);

    return dl->released_recently && !frame_number_after(frame_number, expiry);
}

static void rlc_downlink_assign(GSMBts *bts, uint32_t frame_number) {
    GSMBtsGprs *gprs = bts->gprs;
    GSMBtsDownlinkTbf *dl = &gprs->dl;
    bool t3192_running = rlc_downlink_is_t3192_running(dl, frame_number);

    if (!gprs->tlli_valid)
        return;

    rlc_downlink_reset(dl);
    dl->state = GSM_BTS_DL_ACTIVE;
    dl->tfi = gprs->next_downlink_tfi;
    dl->t3192_running = t3192_running;
    gprs->next_downlink_tfi = (gprs->next_downlink_tfi + 1) % GPRS_TFI_COUNT;
    if (gprs->ul.active || t3192_running) {
        dl->start_frame = gsm_bts_frame_number_add(frame_number, GPRS_PACCH_ASSIGNMENT_DELAY_FRAMES);
        gprs->downlink_assignment_pending = true;
        return;
    }

    dl->start_frame = gsm_bts_frame_number_add(frame_number, GPRS_CCCH_ASSIGNMENT_DELAY_FRAMES);
    uint32_t reference = gsm_bts_frame_number_add(frame_number,
        GSM_HYPERFRAME_FRAMES - IA_DOWNLINK_REFERENCE_AGE_FRAMES);
    for (uint32_t i = 0; i < GPRS_ASSIGNMENT_REPEATS; i++) {
        uint8_t message[GSM_XCCH_BLOCK_BYTES];

        gsm_bts_encode_downlink_assignment(message, bts->arfcn, reference, gprs->tlli, dl->tfi);
        gsm_bts_ccch_enqueue(bts, message);
    }
}

static bool rlc_downlink_take_new_block(GSMBtsGprs *gprs, uint8_t block[GSM_XCCH_BLOCK_BYTES], bool *final) {
    if (gprs->llc_count == 0)
        return false;

    const GSMBtsLlcFrame *frame = &gprs->llc_queue[gprs->llc_head];
    size_t remaining = frame->length - gprs->llc_offset;
    size_t offset = RLC_HEADER_BYTES;
    size_t chunk;
    bool extension = true;

    memset(block, GSM_MACBLOCK_PADDING, GSM_XCCH_BLOCK_BYTES);
    if (remaining < RLC_DATA_BYTES) {
        block[offset++] = (remaining << RLC_LI_SHIFT) | RLC_EXTENSION;
        extension = false;
        chunk = remaining;
    } else if (remaining == RLC_DATA_BYTES && gprs->llc_count == 1) {
        chunk = remaining;
    } else if (remaining == RLC_DATA_BYTES) {
        block[offset++] = RLC_EXTENSION;
        extension = false;
        chunk = RLC_DATA_BYTES - 1;
    } else {
        chunk = RLC_DATA_BYTES;
    }

    memcpy(&block[offset], &frame->data[gprs->llc_offset], chunk);
    gprs->llc_offset += chunk;
    *final = false;
    if (gprs->llc_offset == frame->length) {
        gprs->llc_offset = 0;
        gprs->llc_head = (gprs->llc_head + 1) % LLC_QUEUE_SIZE;
        gprs->llc_count--;
        *final = gprs->llc_count == 0;
    }
    block[RLC_TFI_OCTET] = (gprs->dl.tfi << RLC_TFI_SHIFT) | (*final ? RLC_DL_FINAL_BLOCK : 0);
    block[RLC_BSN_OCTET] = (gprs->dl.vs << RLC_BSN_SHIFT) | (extension ? RLC_EXTENSION : 0);
    return true;
}

static bool rlc_downlink_next_block(
    GSMBtsGprs *gprs,
    uint32_t block_start,
    uint8_t block[GSM_XCCH_BLOCK_BYTES],
    bool *want_poll
) {
    GSMBtsDownlinkTbf *dl = &gprs->dl;
    bool final = false;

    if (dl->state != GSM_BTS_DL_ACTIVE)
        return false;
    if (frame_number_after(dl->start_frame, block_start))
        return false;

    for (uint8_t bsn = dl->va; bsn != dl->vs; bsn = rlc_mod(bsn + 1)) {
        if (dl->block_state[bsn] == GSM_BTS_BLOCK_NACKED) {
            dl->block_state[bsn] = GSM_BTS_BLOCK_SENT;
            bool final_block = dl->blocks[bsn][RLC_TFI_OCTET] & RLC_DL_FINAL_BLOCK;

            memcpy(block, dl->blocks[bsn], GSM_XCCH_BLOCK_BYTES);
            *want_poll = final_block || ++dl->blocks_since_poll >= RLC_DOWNLINK_POLL_BLOCKS;
            return true;
        }
    }

    if (!dl->final_sent && rlc_mod(dl->vs - dl->va) < RLC_WINDOW && rlc_downlink_take_new_block(gprs, block, &final)) {
        memcpy(dl->blocks[dl->vs], block, GSM_XCCH_BLOCK_BYTES);
        dl->block_state[dl->vs] = GSM_BTS_BLOCK_SENT;
        dl->vs = rlc_mod(dl->vs + 1);
        dl->final_sent = final;
        *want_poll = final || ++dl->blocks_since_poll >= RLC_DOWNLINK_POLL_BLOCKS;
        return true;
    }

    if (dl->va == dl->vs)
        return false;
    memcpy(block, dl->blocks[dl->va], GSM_XCCH_BLOCK_BYTES);
    *want_poll = true;
    return true;
}

static void rlc_downlink_finish(GSMBts *bts, uint32_t frame_number) {
    GSMBtsDownlinkTbf *dl = &bts->gprs->dl;

    rlc_downlink_reset(dl);
    dl->released_recently = true;
    dl->release_frame = frame_number;
    if (bts->gprs->llc_count > 0)
        rlc_downlink_assign(bts, frame_number);
}

static void rlcmac_handle_downlink_ack(GSMBts *bts, GSMBitReader *reader, uint32_t frame_number) {
    GSMBtsGprs *gprs = bts->gprs;
    GSMBtsDownlinkTbf *dl = &gprs->dl;
    uint8_t tfi = gsm_bit_reader_get(reader, RLCMAC_TFI_BITS);
    bool final = gsm_bit_reader_get(reader, 1);
    uint8_t ssn = gsm_bit_reader_get(reader, RLCMAC_SSN_BITS);
    bool rbb[RLC_WINDOW];
    bool channel_request;

    for (uint32_t i = 0; i < RLC_WINDOW; i++)
        rbb[i] = gsm_bit_reader_get(reader, 1);
    channel_request = gsm_bit_reader_get(reader, 1);

    if (channel_request && !gprs->ul.active) {
        rlc_uplink_start(gprs);
        gprs->uplink_assignment_pending = true;
    }
    if (dl->state != GSM_BTS_DL_ACTIVE)
        return;
    if (tfi != dl->tfi)
        return;

    dl->unanswered_polls = 0;
    if (final && dl->final_sent) {
        rlc_downlink_finish(bts, frame_number);
        return;
    }
    for (uint32_t i = 0; i < RLC_WINDOW; i++) {
        uint8_t bsn = rlc_mod(ssn - RLC_WINDOW + i);
        bool outstanding = rlc_mod(bsn - dl->va) < rlc_mod(dl->vs - dl->va);
        bool before_ssn = rlc_mod(bsn - dl->va) < rlc_mod(ssn - dl->va);

        if (!outstanding)
            continue;
        if (rbb[i]) {
            dl->block_state[bsn] = GSM_BTS_BLOCK_ACKED;
        } else if (before_ssn) {
            dl->block_state[bsn] = GSM_BTS_BLOCK_NACKED;
        }
    }
    while (dl->va != dl->vs && dl->block_state[dl->va] == GSM_BTS_BLOCK_ACKED) {
        dl->block_state[dl->va] = GSM_BTS_BLOCK_FREE;
        dl->va = rlc_mod(dl->va + 1);
    }
}

static void rlcmac_handle_resource_request(GSMBtsGprs *gprs, GSMBitReader *reader) {
    if (gsm_bit_reader_get(reader, 1) == CSN_PRESENT)
        gsm_bit_reader_get(reader, RLCMAC_ACCESS_TYPE_BITS);
    if (gsm_bit_reader_get(reader, 1) != RLCMAC_REQUEST_ID_TLLI)
        return;

    gprs->tlli = gsm_bit_reader_get(reader, RLCMAC_TLLI_BITS);
    gprs->tlli_valid = true;
    rlc_uplink_start(gprs);
    gprs->ul.tlli = gprs->tlli;
    gprs->ul.tlli_acknowledged = true;
    gprs->uplink_assignment_pending = true;
}

static void rlcmac_handle_control(GSMBts *bts, const uint8_t block[GSM_XCCH_BLOCK_BYTES], uint32_t frame_number) {
    GSMBtsGprs *gprs = bts->gprs;
    GSMBitReader reader = { .data = block, .position = RLCMAC_UPLINK_HEADER_BITS };

    switch (gsm_bit_reader_get(&reader, RLCMAC_MESSAGE_TYPE_BITS)) {
        case RLCMAC_UL_PACKET_CONTROL_ACK:
            if (gprs->ul.active && gprs->ul.final_acknowledged)
                rlc_uplink_release(gprs);
            break;
        case RLCMAC_UL_PACKET_DOWNLINK_ACK_NACK:
            rlcmac_handle_downlink_ack(bts, &reader, frame_number);
            break;
        case RLCMAC_UL_PACKET_RESOURCE_REQUEST:
            rlcmac_handle_resource_request(gprs, &reader);
            break;
    }
}

static void gsm_bts_gprs_handle_uplink_block(
    GSMBts *bts,
    const uint8_t block[GSM_XCCH_BLOCK_BYTES],
    uint32_t block_start
) {
    GSMBtsGprs *gprs = bts->gprs;
    uint8_t payload_type = block[RLC_MAC_HEADER_OCTET] >> RLC_PAYLOAD_TYPE_SHIFT;
    uint8_t channel = payload_type == RLCMAC_PT_DATA ? GSMTAP_CHANNEL_PDTCH : GSMTAP_CHANNEL_PACCH;

    gsm_bts_gsmtap(bts, true, GSM_BTS_PDCH_TIMESLOT, block_start, channel, block, GSM_XCCH_BLOCK_BYTES);
    if (gprs->poll_pending && block_start == gprs->poll_frame)
        gprs->poll_pending = false;

    switch (payload_type) {
        case RLCMAC_PT_DATA:
            rlc_uplink_handle_data(bts, block);
            break;
        case RLCMAC_PT_CONTROL:
            rlcmac_handle_control(bts, block, block_start);
            break;
    }
}

static void gsm_bts_gprs_expire_poll(GSMBtsGprs *gprs, uint32_t block_start) {
    if (!gprs->poll_pending)
        return;
    if (!frame_number_after(block_start, gprs->poll_frame))
        return;

    gprs->poll_pending = false;
    if (gprs->poll_downlink && gprs->dl.state == GSM_BTS_DL_ACTIVE &&
        ++gprs->dl.unanswered_polls > RLC_POLL_ATTEMPTS) {
        rlc_downlink_reset(&gprs->dl);
        gprs->llc_offset = 0;
    }
    if (!gprs->poll_downlink && gprs->ul.final_acknowledged)
        rlc_uplink_release(gprs);
}

static void gsm_bts_gprs_update_timers(GSMBts *bts, uint32_t block_start, uint8_t usf) {
    GSMBtsGprs *gprs = bts->gprs;

    gsm_bts_gprs_expire_poll(gprs, block_start);
    if (gprs->single_block_pending && frame_number_after(block_start, gprs->single_block_frame))
        gprs->single_block_pending = false;
    if (gprs->ul.active && usf == GPRS_USF && ++gprs->ul.idle_blocks > RLC_IDLE_USF_BLOCKS)
        rlc_uplink_release(gprs);
    if (gprs->dl.state == GSM_BTS_DL_IDLE && gprs->llc_count > 0)
        rlc_downlink_assign(bts, block_start);

    bool pacch_unavailable = !gprs->ul.active && !rlc_downlink_is_t3192_running(&gprs->dl, block_start);

    if (gprs->downlink_assignment_pending && pacch_unavailable) {
        gprs->downlink_assignment_pending = false;
        rlc_downlink_assign(bts, block_start);
    }
}

static uint8_t gsm_bts_gprs_transmit_block(GSMBts *bts, uint32_t block_start, uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    GSMBtsGprs *gprs = bts->gprs;
    uint32_t next_uplink = pdch_next_block(block_start);
    bool uplink_reserved = (gprs->poll_pending && gprs->poll_frame == next_uplink) ||
        (gprs->single_block_pending && gprs->single_block_frame == next_uplink);
    uint8_t usf = gprs->ul.active && !uplink_reserved ? GPRS_USF : GPRS_USF_FREE;

    gsm_bts_gprs_update_timers(bts, block_start, usf);

    bool can_poll = !gprs->poll_pending;
    bool ul_final = rlc_uplink_is_complete(&gprs->ul);
    bool downlink_assignment_ready = gprs->downlink_assignment_pending &&
        (!gprs->ul.active || gprs->ul.tlli_acknowledged);
    uint8_t channel = GSMTAP_CHANNEL_PACCH;
    bool want_poll = false;
    bool poll = false;

    if (gprs->uplink_assignment_pending) {
        gprs->uplink_assignment_pending = false;
        rlcmac_encode_uplink_assignment(gprs, bts->arfcn, usf, block);
    } else if (gprs->uplink_ack_pending && gprs->ul.active) {
        poll = ul_final && can_poll;
        rlcmac_encode_uplink_ack(gprs, ul_final, poll, usf, block);
        gprs->uplink_ack_pending = false;
        gprs->poll_downlink = false;
        gprs->ul.unacknowledged_blocks = 0;
        gprs->ul.tlli_acknowledged = gprs->tlli_valid;
        gprs->ul.final_acknowledged = ul_final;
        if (ul_final && !poll)
            rlc_uplink_release(gprs);
    } else if (downlink_assignment_ready) {
        gprs->downlink_assignment_pending = false;
        rlcmac_encode_downlink_assignment(gprs, bts->arfcn, usf, block);
    } else if (rlc_downlink_next_block(gprs, block_start, block, &want_poll)) {
        channel = GSMTAP_CHANNEL_PDTCH;
        poll = want_poll && can_poll;
        if (poll)
            gprs->dl.blocks_since_poll = 0;
        gprs->poll_downlink = true;
        block[RLC_MAC_HEADER_OCTET] = (RLCMAC_PT_DATA << RLC_PAYLOAD_TYPE_SHIFT) | (poll << RLC_DL_POLL_SHIFT) | usf;
    } else {
        rlcmac_encode_dummy(usf, block);
    }

    if (poll) {
        gprs->poll_pending = true;
        gprs->poll_frame = gsm_bts_frame_number_add(block_start, PDCH_RRBP_N13_FRAMES);
    }
    return channel;
}

void gsm_bts_gprs_downlink_burst(GSMBts *bts, uint32_t frame_number, GSMBurst *burst) {
    GSMBtsDownlinkBlock *cache = &bts->gprs->downlink;
    uint32_t block_start;
    uint32_t burst_index;

    if (!pdch_block_position(frame_number, &block_start, &burst_index)) {
        gsm_dummy_burst(burst);
        return;
    }

    if (!cache->valid || cache->frame_number != block_start) {
        uint8_t block[GSM_XCCH_BLOCK_BYTES];
        uint8_t channel = gsm_bts_gprs_transmit_block(bts, block_start, block);

        gsm_xcch_encode(block, cache->data);
        cache->frame_number = block_start;
        cache->valid = true;
        gsm_bts_gsmtap(bts, false, GSM_BTS_PDCH_TIMESLOT, block_start, channel, block, sizeof(block));
    }
    gsm_normal_burst(burst, GSM_BTS_TSC, cache->data[burst_index]);
}

void gsm_bts_gprs_uplink_burst(GSMBts *bts, uint32_t frame_number, const GSMUplinkBurst *burst) {
    GSMBtsUplinkBlock *uplink = &bts->gprs->uplink;
    uint8_t block[GSM_XCCH_BLOCK_BYTES];
    uint32_t block_start;
    uint32_t burst_index;
    size_t start;

    if (!pdch_block_position(frame_number, &block_start, &burst_index))
        return;
    if (!gsm_uplink_find_normal_burst(burst, GSM_BTS_TSC, &start))
        return;

    if (uplink->frame_number != block_start) {
        uplink->frame_number = block_start;
        memset(uplink->soft_bits, GSM_SOFT_BIT_UNKNOWN, sizeof(uplink->soft_bits));
    }
    gsm_uplink_normal_burst_data(burst, start, uplink->soft_bits[burst_index]);
    if (burst_index != GSM_XCCH_BLOCK_BURSTS - 1)
        return;
    if (!gsm_xcch_decode(uplink->soft_bits, block))
        return;

    gsm_bts_gprs_handle_uplink_block(bts, block, block_start);
}

void gsm_bts_gprs_init(GSMBts *bts) {
    bts->gprs = g_new0(GSMBtsGprs, 1);
    bts->gprs->data_sapi = LLC_SAPI_DATA_DEFAULT;
}

void gsm_bts_gprs_cleanup(GSMBts *bts) {
    g_free(bts->gprs);
    bts->gprs = NULL;
}
