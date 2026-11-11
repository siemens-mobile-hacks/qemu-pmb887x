/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qapi/error.h"
#include "qom/object_interfaces.h"
#include "system/system.h"
#include "hw/gsm/gsm-bts.h"

#define GSM_CONTROL_MULTIFRAME_FRAMES 51U
#define GSM_TRAFFIC_MULTIFRAME_FRAMES 26U
#define GSM_STARTING_TIME_T1_MODULUS 32
#define GSM_STARTING_TIME_T1_SHIFT 11
#define GSM_STARTING_TIME_T3_SHIFT 5

#define GSM_SCH_DATA_BITS 25U
#define GSM_SCH_PARITY_BITS 10U
#define GSM_SCH_UNCODED_BITS (GSM_SCH_DATA_BITS + GSM_SCH_PARITY_BITS)
#define GSM_SCH_CONV_TAIL_BITS 4U
#define GSM_SCH_CODED_HALF_BITS (GSM_SCH_UNCODED_BITS + GSM_SCH_CONV_TAIL_BITS)
#define GSM_SCH_CONV_TAP_D1 1
#define GSM_SCH_CONV_TAP_D3 3
#define GSM_SCH_CONV_TAP_D4 4
#define GSM_BURST_TAIL_BITS 3U
#define GSM_SCH_DATA_FIRST_START GSM_BURST_TAIL_BITS
#define GSM_SCH_TRAINING_START 42U
#define GSM_SCH_DATA_SECOND_START 106U
#define GSM_SCH_TAIL_START (GSM_BURST_BITS - GSM_BURST_TAIL_BITS)
#define GSM_SCH_CRC_MASK 0x03FFU
#define GSM_SCH_CRC_POLYNOMIAL 0x0175U
#define GSM_SCH_FCCH_PERIOD 10U
#define GSM_SCH_BSIC_SHIFT 2
#define GSM_SCH_T3_PRIME_HIGH_SHIFT 16
#define GSM_SCH_T2_SHIFT 18
#define GSM_SCH_T3_PRIME_LOW_SHIFT 24

#define GSM_TS0_FIRST_BLOCK 2U
#define GSM_TS0_BCCH_START 2U
#define GSM_TS0_CCCH0_START 6U
#define GSM_TS0_CCCH1_START 12U
#define GSM_TS0_CCCH2_START 16U
#define GSM_TS0_SDCCH0_START 22U
#define GSM_TS0_SACCH0_START 42U
#define GSM_TS0_UPLINK_SDCCH0_START 37U
#define GSM_BCCH_TYPES 8U

#define GSMTAP_PORT 4729
#define GSMTAP_VERSION 2
#define GSMTAP_TYPE_UM 0x01
#define GSMTAP_ARFCN_UPLINK 0x4000
#define GSMTAP_HEADER_WORD_BYTES 4

#define GSM48_PD_MM 0x05
#define GSM48_PD_RR 0x06
#define GSM48_PD_MASK 0x0F
#define GSM48_MT_MASK 0x3F
#define GSM48_MT_RR_SYSINFO_13 0x00
#define GSM48_MT_RR_CHANNEL_RELEASE 0x0D
#define GSM48_MT_RR_SYSINFO_1 0x19
#define GSM48_MT_RR_SYSINFO_2 0x1A
#define GSM48_MT_RR_SYSINFO_3 0x1B
#define GSM48_MT_RR_SYSINFO_4 0x1C
#define GSM48_MT_RR_SYSINFO_5 0x1D
#define GSM48_MT_RR_SYSINFO_6 0x1E
#define GSM48_MT_RR_PAGING_REQUEST_1 0x21
#define GSM48_MT_RR_IMMEDIATE_ASSIGNMENT 0x3F
#define GSM48_MT_MM_IMSI_DETACH_INDICATION 0x01
#define GSM48_MT_MM_LOCATION_UPDATING_ACCEPT 0x02
#define GSM48_MT_MM_LOCATION_UPDATING_REQUEST 0x08
#define GSM48_MT_MM_CM_SERVICE_REJECT 0x22
#define GSM48_MT_MM_CM_SERVICE_REQUEST 0x24
#define GSM48_RR_CAUSE_NORMAL 0x00
#define GSM48_MM_CAUSE_SERVICE_OPTION_NOT_SUPPORTED 0x20
#define GSM48_MODE_DEDICATED 0x00
#define GSM48_PAGE_MODE_NORMAL 0x00
#define GSM48_MOBILE_IDENTITY_NONE 0xF0
#define GSM48_MOBILE_IDENTITY_NONE_BYTES 1
#define GSM48_CHANNEL_SDCCH4_SUBCHANNEL0 0x20
#define GSM48_CHANNEL_TSC_SHIFT 5
#define GSM48_CHANNEL_ARFCN_HIGH_SHIFT 8
#define GSM48_CHANNEL_LIST_BYTES 16
#define GSM48_BCD_FILLER 0x0F
#define GSM48_BCD_HIGH_SHIFT 4
#define GSM48_L2_PSEUDO_LENGTH_SHIFT 2
#define GSM48_L2_PSEUDO_LENGTH_SPARE 0x01

#define GSM48_RACH_MAX_RETRANS_7 3
#define GSM48_RACH_MAX_RETRANS_SHIFT 6
#define GSM48_RACH_TX_INTEGER_32_SLOTS 9
#define GSM48_RACH_TX_INTEGER_SHIFT 2
#define GSM48_RACH_REESTABLISHMENT_NOT_ALLOWED 1
#define GSM48_RACH_ALL_ACCESS_CLASSES 0x00
#define GSM48_MSCR_R99 1
#define GSM48_MSCR_SHIFT 7
#define GSM48_ATT_IMSI_ATTACH 1
#define GSM48_ATT_SHIFT 6
#define GSM48_BS_AG_BLKS_RES 1
#define GSM48_BS_AG_BLKS_RES_SHIFT 3
#define GSM48_CCCH_CONF_COMBINED 1
#define GSM48_BS_PA_MFRMS_2 0
#define GSM48_T3212_NO_PERIODIC_UPDATE 0
#define GSM48_DTX_NOT_USED 2
#define GSM48_DTX_SHIFT 4
#define GSM48_RADIO_LINK_TIMEOUT_64 15
#define GSM48_CELL_RESELECT_HYSTERESIS_4_DB 2
#define GSM48_CELL_RESELECT_HYSTERESIS_SHIFT 5
#define GSM48_MS_TXPWR_MAX_CCH 5
#define GSM48_NECI_SUPPORTED 1
#define GSM48_NECI_SHIFT 6
#define GSM48_RXLEV_ACCESS_MIN 0
#define GSM48_NCC_PERMITTED_ALL 0xFF

#define GSM_BTS_RA_COLOUR 0
#define GSM_BTS_RAC 0
#define GSM_BTS_LAC 1
#define GSM_BTS_CELL_IDENTITY 0
#define GSM_BTS_CELL_OPTIONS ((GSM48_DTX_NOT_USED << GSM48_DTX_SHIFT) | GSM48_RADIO_LINK_TIMEOUT_64)
#define SI3_ABSENT_FIELDS_BEFORE_GPRS 5
#define SI4_ABSENT_FIELDS_BEFORE_GPRS 2
#define SI13_L2_PSEUDO_LENGTH 0
#define SI13_POSITION_BCCH_NORM 0
#define SI13_BCCH_CHANGE_MARK 0
#define SI13_SI_CHANGE_FIELD 0
#define SI13_NO_PBCCH 0
#define SI13_NO_SPLIT_PAGING_CYCLE 0
#define SI13_PRIORITY_ACCESS_ALL 6
#define SI13_NETWORK_CONTROL_ORDER_NC0 0
#define SI13_SGSN_RELEASE_99 1
#define GPRS_NMO_II 1
#define GPRS_T3168_2000_MS 3
#define GPRS_T3192_1500_MS 2
#define GPRS_DRX_TIMER_MAX_4_S 3
#define GPRS_ACCESS_BURST_8_BIT 0
#define GPRS_CONTROL_ACK_RLCMAC_BLOCK 1
#define GPRS_BS_CV_MAX 15
#define GPRS_PAN_DEC 1
#define GPRS_PAN_INC 1
#define GPRS_PAN_MAX 7
#define GPRS_CELL_OPTIONS_EXTENSION_BITS 6
#define GPRS_ALPHA 0
#define GPRS_T_AVG_W 16
#define GPRS_T_AVG_T 16
#define GPRS_PC_MEAS_CHAN_BCCH 0
#define GPRS_N_AVG_I 8

#define GSM_BTS_RA_COLOUR_BITS 3
#define GSM_BTS_RAC_BITS 8
#define SI13_BCCH_CHANGE_MARK_BITS 3
#define SI13_SI_CHANGE_FIELD_BITS 4
#define SI13_PRIORITY_ACCESS_BITS 3
#define SI13_NETWORK_CONTROL_ORDER_BITS 2
#define GPRS_NMO_BITS 2
#define GPRS_T3168_BITS 3
#define GPRS_T3192_BITS 3
#define GPRS_DRX_TIMER_MAX_BITS 3
#define GPRS_BS_CV_MAX_BITS 4
#define GPRS_PAN_BITS 3
#define GPRS_EXTENSION_LENGTH_BITS 6
#define GPRS_ALPHA_BITS 4
#define GPRS_T_AVG_BITS 5
#define GPRS_N_AVG_I_BITS 4

#define LAPDM_ADDRESS_EA 0x01
#define LAPDM_ADDRESS_CR 0x02
#define LAPDM_ADDRESS_SAPI_SHIFT 2
#define LAPDM_ADDRESS_SAPI_MASK 0x07
#define LAPDM_SAPI_SIGNALLING 0
#define LAPDM_CONTROL_PF 0x10
#define LAPDM_CONTROL_I_MASK 0x01
#define LAPDM_CONTROL_I 0x00
#define LAPDM_CONTROL_S_MASK 0x03
#define LAPDM_CONTROL_S 0x01
#define LAPDM_CONTROL_RR 0x01
#define LAPDM_CONTROL_U_MASK 0xEF
#define LAPDM_CONTROL_SABM 0x2F
#define LAPDM_CONTROL_UA 0x63
#define LAPDM_CONTROL_DISC 0x43
#define LAPDM_CONTROL_UI 0x03
#define LAPDM_LENGTH_EL 0x01
#define LAPDM_LENGTH_M 0x02
#define LAPDM_LENGTH_SHIFT 2
#define LAPDM_SEQUENCE_MASK 0x07
#define LAPDM_NR_SHIFT 5
#define LAPDM_NS_SHIFT 1
#define LAPDM_SDCCH_INFO_BYTES 20
#define LAPDM_N200 8

#define SACCH_MS_POWER_LEVEL 5
#define SACCH_TIMING_ADVANCE 0

typedef enum GSMBtsTs0Channel GSMBtsTs0Channel;
typedef struct GSMTapHeader GSMTapHeader;
typedef struct GSMSystemInformationHeader GSMSystemInformationHeader;
typedef struct GSMSystemInformation1 GSMSystemInformation1;
typedef struct GSMSystemInformation2 GSMSystemInformation2;
typedef struct GSMSystemInformation3 GSMSystemInformation3;
typedef struct GSMSystemInformation4 GSMSystemInformation4;
typedef struct GSMSystemInformation13 GSMSystemInformation13;
typedef struct GSMPagingRequest1 GSMPagingRequest1;
typedef struct GSMLapdmHeader GSMLapdmHeader;
typedef struct GSMSacchHeader GSMSacchHeader;
typedef struct GSMSacchSystemInformation5 GSMSacchSystemInformation5;
typedef struct GSMSacchSystemInformation6 GSMSacchSystemInformation6;
typedef struct GSMSchField GSMSchField;

enum GSMBtsTs0Channel {
    GSM_BTS_TS0_FCCH,
    GSM_BTS_TS0_SCH,
    GSM_BTS_TS0_BCCH,
    GSM_BTS_TS0_CCCH,
    GSM_BTS_TS0_SDCCH,
    GSM_BTS_TS0_SACCH,
    GSM_BTS_TS0_OTHER,
};

struct QEMU_PACKED GSMTapHeader {
    uint8_t version;
    uint8_t header_words;
    uint8_t type;
    uint8_t timeslot;
    uint16_t arfcn;
    int8_t signal_dbm;
    int8_t snr_db;
    uint32_t frame_number;
    uint8_t channel;
    uint8_t antenna;
    uint8_t subslot;
    uint8_t reserved;
};

struct QEMU_PACKED GSMSystemInformationHeader {
    uint8_t l2_pseudo_length;
    uint8_t protocol_discriminator;
    uint8_t message_type;
};

struct QEMU_PACKED GSMSystemInformation1 {
    GSMSystemInformationHeader header;
    uint8_t cell_channels[GSM48_CHANNEL_LIST_BYTES];
    uint8_t rach_control[3];
    uint8_t rest_octets[1];
};

struct QEMU_PACKED GSMSystemInformation2 {
    GSMSystemInformationHeader header;
    uint8_t neighbour_cells[GSM48_CHANNEL_LIST_BYTES];
    uint8_t ncc_permitted;
    uint8_t rach_control[3];
};

struct QEMU_PACKED GSMSystemInformation3 {
    GSMSystemInformationHeader header;
    uint16_t cell_identity;
    uint8_t lai[GSM48_LAI_BYTES];
    uint8_t control_channels[3];
    uint8_t cell_options;
    uint8_t cell_selection[2];
    uint8_t rach_control[3];
    uint8_t rest_octets[4];
};

struct QEMU_PACKED GSMSystemInformation4 {
    GSMSystemInformationHeader header;
    uint8_t lai[GSM48_LAI_BYTES];
    uint8_t cell_selection[2];
    uint8_t rach_control[3];
    uint8_t rest_octets[10];
};

struct QEMU_PACKED GSMSystemInformation13 {
    GSMSystemInformationHeader header;
    uint8_t rest_octets[20];
};

struct QEMU_PACKED GSMPagingRequest1 {
    GSMSystemInformationHeader header;
    uint8_t page_mode;
    uint8_t mobile_identity_length;
    uint8_t mobile_identity;
    uint8_t rest_octets[17];
};

struct QEMU_PACKED GSMLapdmHeader {
    uint8_t address;
    uint8_t control;
    uint8_t length;
};

struct QEMU_PACKED GSMSacchHeader {
    uint8_t ms_power_level;
    uint8_t timing_advance;
    GSMLapdmHeader lapdm;
};

struct QEMU_PACKED GSMSacchSystemInformation5 {
    uint8_t protocol_discriminator;
    uint8_t message_type;
    uint8_t neighbour_cells[GSM48_CHANNEL_LIST_BYTES];
};

struct QEMU_PACKED GSMSacchSystemInformation6 {
    uint8_t protocol_discriminator;
    uint8_t message_type;
    uint16_t cell_identity;
    uint8_t lai[GSM48_LAI_BYTES];
    uint8_t cell_options;
    uint8_t ncc_permitted;
};

struct GSMSchField {
    uint32_t value_shift;
    uint32_t bits;
    uint32_t information_shift;
};

_Static_assert(sizeof(GSMTapHeader) == 16, "GSMTAP header layout");
_Static_assert(sizeof(GSMSystemInformation1) == GSM_XCCH_BLOCK_BYTES, "SI1 layout");
_Static_assert(sizeof(GSMSystemInformation2) == GSM_XCCH_BLOCK_BYTES, "SI2 layout");
_Static_assert(sizeof(GSMSystemInformation3) == GSM_XCCH_BLOCK_BYTES, "SI3 layout");
_Static_assert(sizeof(GSMSystemInformation4) == GSM_XCCH_BLOCK_BYTES, "SI4 layout");
_Static_assert(sizeof(GSMSystemInformation13) == GSM_XCCH_BLOCK_BYTES, "SI13 layout");
_Static_assert(sizeof(GSMPagingRequest1) == GSM_XCCH_BLOCK_BYTES, "Paging Request Type 1 layout");
_Static_assert(sizeof(GSMSacchHeader) + sizeof(GSMSacchSystemInformation5) <= GSM_XCCH_BLOCK_BYTES, "SI5 layout");

static const uint8_t GSM_BTS_MCC[] = { 0, 0, 1 };
static const uint8_t GSM_BTS_MNC[] = { 0, 1 };
static const uint8_t GSM_BTS_RACH_CONTROL[] = {
    (GSM48_RACH_MAX_RETRANS_7 << GSM48_RACH_MAX_RETRANS_SHIFT) |
        (GSM48_RACH_TX_INTEGER_32_SLOTS << GSM48_RACH_TX_INTEGER_SHIFT) | GSM48_RACH_REESTABLISHMENT_NOT_ALLOWED,
    GSM48_RACH_ALL_ACCESS_CLASSES,
    GSM48_RACH_ALL_ACCESS_CLASSES,
};
static const uint8_t GSM_BTS_CONTROL_CHANNELS[] = {
    (GSM48_MSCR_R99 << GSM48_MSCR_SHIFT) | (GSM48_ATT_IMSI_ATTACH << GSM48_ATT_SHIFT) |
        (GSM48_BS_AG_BLKS_RES << GSM48_BS_AG_BLKS_RES_SHIFT) | GSM48_CCCH_CONF_COMBINED,
    GSM48_BS_PA_MFRMS_2,
    GSM48_T3212_NO_PERIODIC_UPDATE,
};
static const uint8_t GSM_BTS_CELL_SELECTION[] = {
    (GSM48_CELL_RESELECT_HYSTERESIS_4_DB << GSM48_CELL_RESELECT_HYSTERESIS_SHIFT) | GSM48_MS_TXPWR_MAX_CCH,
    (GSM48_NECI_SUPPORTED << GSM48_NECI_SHIFT) | GSM48_RXLEV_ACCESS_MIN,
};
static const uint8_t GSM_TS0_UPLINK_DEDICATED_BLOCKS[] = { 0, 6, 10, 37, 41, 47 };
static const GSMSchField GSM_SCH_T1_FIELDS[] = {
    { 9, 2, 0 },
    { 1, 8, 8 },
    { 0, 1, 23 },
};
static const char GSM_SCH_TRAINING_SEQUENCE[] =
    "1011100101100010000001000000111100101101010001010111011000011011";

void gsm_bit_writer_put(GSMBitWriter *writer, uint32_t value, uint32_t count) {
    for (uint32_t i = count; i-- > 0;) {
        size_t byte = writer->position / 8;
        uint8_t mask = 0x80 >> (writer->position % 8);

        if ((value >> i) & 1) {
            writer->data[byte] |= mask;
        } else {
            writer->data[byte] &= ~mask;
        }
        writer->position++;
    }
}

void gsm_bit_writer_put_lh(GSMBitWriter *writer, bool high) {
    bool low = (GSM_MACBLOCK_PADDING >> (7 - (writer->position % 8))) & 1;

    gsm_bit_writer_put(writer, high ? !low : low, 1);
}

void gsm_bit_writer_pad(GSMBitWriter *writer, size_t bytes) {
    while (writer->position < bytes * 8)
        gsm_bit_writer_put_lh(writer, false);
}

uint32_t gsm_bts_frame_number_add(uint32_t frame_number, uint32_t frames) {
    return (frame_number + frames) % GSM_HYPERFRAME_FRAMES;
}

uint16_t gsm_starting_time(uint32_t frame_number) {
    uint32_t t1 = (frame_number / (GSM_TRAFFIC_MULTIFRAME_FRAMES * GSM_CONTROL_MULTIFRAME_FRAMES)) %
        GSM_STARTING_TIME_T1_MODULUS;
    uint32_t t3 = frame_number % GSM_CONTROL_MULTIFRAME_FRAMES;
    uint32_t t2 = frame_number % GSM_TRAFFIC_MULTIFRAME_FRAMES;

    return (t1 << GSM_STARTING_TIME_T1_SHIFT) | (t3 << GSM_STARTING_TIME_T3_SHIFT) | t2;
}

void gsm_bts_encode_lai(uint8_t lai[GSM48_LAI_BYTES]) {
    lai[0] = (GSM_BTS_MCC[1] << GSM48_BCD_HIGH_SHIFT) | GSM_BTS_MCC[0];
    lai[1] = (GSM48_BCD_FILLER << GSM48_BCD_HIGH_SHIFT) | GSM_BTS_MCC[2];
    lai[2] = (GSM_BTS_MNC[1] << GSM48_BCD_HIGH_SHIFT) | GSM_BTS_MNC[0];
    stw_be_p(&lai[3], GSM_BTS_LAC);
}

static uint8_t gsm_l2_pseudo_length(size_t length) {
    return (length << GSM48_L2_PSEUDO_LENGTH_SHIFT) | GSM48_L2_PSEUDO_LENGTH_SPARE;
}

static void gsm_bts_encode_header(GSMSystemInformationHeader *header, uint8_t message_type, size_t rest_offset) {
    header->l2_pseudo_length = gsm_l2_pseudo_length(rest_offset - sizeof(header->l2_pseudo_length));
    header->protocol_discriminator = GSM48_PD_RR;
    header->message_type = message_type;
}

static void gsm_bts_encode_si1(uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    GSMSystemInformation1 *si = (GSMSystemInformation1 *) block;
    size_t arfcn_byte = GSM48_CHANNEL_LIST_BYTES - 1 - ((GSM_BTS_ARFCN - 1) / 8);

    memset(block, GSM_MACBLOCK_PADDING, GSM_XCCH_BLOCK_BYTES);
    gsm_bts_encode_header(&si->header, GSM48_MT_RR_SYSINFO_1, offsetof(GSMSystemInformation1, rest_octets));
    memset(si->cell_channels, 0, sizeof(si->cell_channels));
    si->cell_channels[arfcn_byte] = 1 << ((GSM_BTS_ARFCN - 1) % 8);
    memcpy(si->rach_control, GSM_BTS_RACH_CONTROL, sizeof(si->rach_control));
}

static void gsm_bts_encode_si2(uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    GSMSystemInformation2 *si = (GSMSystemInformation2 *) block;

    gsm_bts_encode_header(&si->header, GSM48_MT_RR_SYSINFO_2, sizeof(GSMSystemInformation2));
    memset(si->neighbour_cells, 0, sizeof(si->neighbour_cells));
    si->ncc_permitted = GSM48_NCC_PERMITTED_ALL;
    memcpy(si->rach_control, GSM_BTS_RACH_CONTROL, sizeof(si->rach_control));
}

static void gsm_bts_put_gprs_indicator(GSMBitWriter *writer) {
    gsm_bit_writer_put_lh(writer, true);
    gsm_bit_writer_put(writer, GSM_BTS_RA_COLOUR, GSM_BTS_RA_COLOUR_BITS);
    gsm_bit_writer_put(writer, SI13_POSITION_BCCH_NORM, 1);
}

static void gsm_bts_encode_si3(uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    GSMSystemInformation3 *si = (GSMSystemInformation3 *) block;
    GSMBitWriter rest = { .data = si->rest_octets };

    memset(block, GSM_MACBLOCK_PADDING, GSM_XCCH_BLOCK_BYTES);
    gsm_bts_encode_header(&si->header, GSM48_MT_RR_SYSINFO_3, offsetof(GSMSystemInformation3, rest_octets));
    stw_be_p(&si->cell_identity, GSM_BTS_CELL_IDENTITY);
    gsm_bts_encode_lai(si->lai);
    memcpy(si->control_channels, GSM_BTS_CONTROL_CHANNELS, sizeof(si->control_channels));
    si->cell_options = GSM_BTS_CELL_OPTIONS;
    memcpy(si->cell_selection, GSM_BTS_CELL_SELECTION, sizeof(si->cell_selection));
    memcpy(si->rach_control, GSM_BTS_RACH_CONTROL, sizeof(si->rach_control));

    for (size_t i = 0; i < SI3_ABSENT_FIELDS_BEFORE_GPRS; i++)
        gsm_bit_writer_put_lh(&rest, false);
    gsm_bts_put_gprs_indicator(&rest);
    gsm_bit_writer_put_lh(&rest, false);
}

static void gsm_bts_encode_si4(uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    GSMSystemInformation4 *si = (GSMSystemInformation4 *) block;
    GSMBitWriter rest = { .data = si->rest_octets };

    memset(block, GSM_MACBLOCK_PADDING, GSM_XCCH_BLOCK_BYTES);
    gsm_bts_encode_header(&si->header, GSM48_MT_RR_SYSINFO_4, offsetof(GSMSystemInformation4, rest_octets));
    gsm_bts_encode_lai(si->lai);
    memcpy(si->cell_selection, GSM_BTS_CELL_SELECTION, sizeof(si->cell_selection));
    memcpy(si->rach_control, GSM_BTS_RACH_CONTROL, sizeof(si->rach_control));

    for (size_t i = 0; i < SI4_ABSENT_FIELDS_BEFORE_GPRS; i++)
        gsm_bit_writer_put_lh(&rest, false);
    gsm_bts_put_gprs_indicator(&rest);
}

static void gsm_bts_put_gprs_cell_options(GSMBitWriter *writer) {
    gsm_bit_writer_put(writer, GPRS_NMO_II, GPRS_NMO_BITS);
    gsm_bit_writer_put(writer, GPRS_T3168_2000_MS, GPRS_T3168_BITS);
    gsm_bit_writer_put(writer, GPRS_T3192_1500_MS, GPRS_T3192_BITS);
    gsm_bit_writer_put(writer, GPRS_DRX_TIMER_MAX_4_S, GPRS_DRX_TIMER_MAX_BITS);
    gsm_bit_writer_put(writer, GPRS_ACCESS_BURST_8_BIT, 1);
    gsm_bit_writer_put(writer, GPRS_CONTROL_ACK_RLCMAC_BLOCK, 1);
    gsm_bit_writer_put(writer, GPRS_BS_CV_MAX, GPRS_BS_CV_MAX_BITS);
    gsm_bit_writer_put(writer, CSN_PRESENT, 1);
    gsm_bit_writer_put(writer, GPRS_PAN_DEC, GPRS_PAN_BITS);
    gsm_bit_writer_put(writer, GPRS_PAN_INC, GPRS_PAN_BITS);
    gsm_bit_writer_put(writer, GPRS_PAN_MAX, GPRS_PAN_BITS);
    gsm_bit_writer_put(writer, CSN_PRESENT, 1);
    gsm_bit_writer_put(writer, GPRS_CELL_OPTIONS_EXTENSION_BITS - 1, GPRS_EXTENSION_LENGTH_BITS);
    gsm_bit_writer_put(writer, 0, GPRS_CELL_OPTIONS_EXTENSION_BITS);
}

static void gsm_bts_encode_si13(uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    GSMSystemInformation13 *si = (GSMSystemInformation13 *) block;
    GSMBitWriter rest = { .data = si->rest_octets };

    memset(block, GSM_MACBLOCK_PADDING, GSM_XCCH_BLOCK_BYTES);
    si->header.l2_pseudo_length = gsm_l2_pseudo_length(SI13_L2_PSEUDO_LENGTH);
    si->header.protocol_discriminator = GSM48_PD_RR;
    si->header.message_type = GSM48_MT_RR_SYSINFO_13;

    gsm_bit_writer_put_lh(&rest, true);
    gsm_bit_writer_put(&rest, SI13_BCCH_CHANGE_MARK, SI13_BCCH_CHANGE_MARK_BITS);
    gsm_bit_writer_put(&rest, SI13_SI_CHANGE_FIELD, SI13_SI_CHANGE_FIELD_BITS);
    gsm_bit_writer_put(&rest, CSN_ABSENT, 1);
    gsm_bit_writer_put(&rest, SI13_NO_PBCCH, 1);
    gsm_bit_writer_put(&rest, GSM_BTS_RAC, GSM_BTS_RAC_BITS);
    gsm_bit_writer_put(&rest, SI13_NO_SPLIT_PAGING_CYCLE, 1);
    gsm_bit_writer_put(&rest, SI13_PRIORITY_ACCESS_ALL, SI13_PRIORITY_ACCESS_BITS);
    gsm_bit_writer_put(&rest, SI13_NETWORK_CONTROL_ORDER_NC0, SI13_NETWORK_CONTROL_ORDER_BITS);
    gsm_bts_put_gprs_cell_options(&rest);
    gsm_bit_writer_put(&rest, GPRS_ALPHA, GPRS_ALPHA_BITS);
    gsm_bit_writer_put(&rest, GPRS_T_AVG_W, GPRS_T_AVG_BITS);
    gsm_bit_writer_put(&rest, GPRS_T_AVG_T, GPRS_T_AVG_BITS);
    gsm_bit_writer_put(&rest, GPRS_PC_MEAS_CHAN_BCCH, 1);
    gsm_bit_writer_put(&rest, GPRS_N_AVG_I, GPRS_N_AVG_I_BITS);
    gsm_bit_writer_put_lh(&rest, true);
    gsm_bit_writer_put(&rest, SI13_SGSN_RELEASE_99, 1);
}

void gsm_bts_encode_immediate_assignment(
    GSMImmediateAssignment *ia,
    uint8_t mode,
    const uint8_t channel_description[GSM_CHANNEL_DESCRIPTION_BYTES],
    uint8_t ra,
    uint32_t frame_number
) {
    size_t rest_offset = offsetof(GSMImmediateAssignment, rest_octets);

    memset(ia, GSM_MACBLOCK_PADDING, sizeof(*ia));
    ia->l2_pseudo_length = gsm_l2_pseudo_length(rest_offset - sizeof(ia->l2_pseudo_length));
    ia->protocol_discriminator = GSM48_PD_RR;
    ia->message_type = GSM48_MT_RR_IMMEDIATE_ASSIGNMENT;
    ia->mode = mode;
    memcpy(ia->channel_description, channel_description, GSM_CHANNEL_DESCRIPTION_BYTES);
    ia->request_reference_ra = ra;
    stw_be_p(ia->request_reference_time, gsm_starting_time(frame_number));
    ia->timing_advance = 0;
    ia->mobile_allocation_length = 0;
}

static void gsm_bts_encode_sdcch_assignment(uint8_t block[GSM_XCCH_BLOCK_BYTES], uint8_t ra, uint32_t frame_number) {
    const uint8_t channel_description[GSM_CHANNEL_DESCRIPTION_BYTES] = {
        GSM48_CHANNEL_SDCCH4_SUBCHANNEL0,
        (GSM_BTS_TSC << GSM48_CHANNEL_TSC_SHIFT) | (GSM_BTS_ARFCN >> GSM48_CHANNEL_ARFCN_HIGH_SHIFT),
        GSM_BTS_ARFCN & 0xFF,
    };

    gsm_bts_encode_immediate_assignment((GSMImmediateAssignment *) block, GSM48_MODE_DEDICATED,
        channel_description, ra, frame_number);
}

static void gsm_bts_encode_empty_paging(uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    GSMPagingRequest1 *paging = (GSMPagingRequest1 *) block;

    memset(block, GSM_MACBLOCK_PADDING, GSM_XCCH_BLOCK_BYTES);
    gsm_bts_encode_header(&paging->header, GSM48_MT_RR_PAGING_REQUEST_1, offsetof(GSMPagingRequest1, rest_octets));
    paging->page_mode = GSM48_PAGE_MODE_NORMAL;
    paging->mobile_identity_length = GSM48_MOBILE_IDENTITY_NONE_BYTES;
    paging->mobile_identity = GSM48_MOBILE_IDENTITY_NONE;
}

void gsm_bts_gsmtap(
    GSMBts *bts,
    bool uplink,
    uint8_t timeslot,
    uint32_t frame_number,
    uint8_t channel,
    const uint8_t *data,
    size_t length
) {
    uint8_t packet[sizeof(GSMTapHeader) + GSM_L3_MAX_BYTES];
    GSMTapHeader *header = (GSMTapHeader *) packet;

    if (bts->gsmtap_fd < 0)
        return;

    memset(header, 0, sizeof(*header));
    header->version = GSMTAP_VERSION;
    header->header_words = sizeof(*header) / GSMTAP_HEADER_WORD_BYTES;
    header->type = GSMTAP_TYPE_UM;
    header->timeslot = timeslot;
    stw_be_p(&header->arfcn, GSM_BTS_ARFCN | (uplink ? GSMTAP_ARFCN_UPLINK : 0));
    stl_be_p(&header->frame_number, frame_number);
    header->channel = channel;
    memcpy(&packet[sizeof(*header)], data, length);
    if (send(bts->gsmtap_fd, packet, sizeof(*header) + length, 0) < 0)
        warn_report("gsm-bts: GSMTAP send failed: %s", strerror(errno));
}

void gsm_bts_ccch_enqueue(GSMBts *bts, const uint8_t message[GSM_XCCH_BLOCK_BYTES]) {
    size_t tail = (bts->ccch_head + bts->ccch_count) % GSM_CCCH_QUEUE_SIZE;

    if (bts->ccch_count == GSM_CCCH_QUEUE_SIZE) {
        warn_report("gsm-bts: CCCH queue full, dropping a message");
        return;
    }
    memcpy(bts->ccch_queue[tail], message, GSM_XCCH_BLOCK_BYTES);
    bts->ccch_count++;
}

static void gsm_bts_ccch_dequeue(GSMBts *bts, uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    if (bts->ccch_count == 0) {
        gsm_bts_encode_empty_paging(block);
        return;
    }
    memcpy(block, bts->ccch_queue[bts->ccch_head], GSM_XCCH_BLOCK_BYTES);
    bts->ccch_head = (bts->ccch_head + 1) % GSM_CCCH_QUEUE_SIZE;
    bts->ccch_count--;
}

static void gsm_bts_encode_bcch(uint32_t frame_number, uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    static void (*const schedule[GSM_BCCH_TYPES])(uint8_t block[GSM_XCCH_BLOCK_BYTES]) = {
        gsm_bts_encode_si1,
        gsm_bts_encode_si2,
        gsm_bts_encode_si3,
        gsm_bts_encode_si4,
        gsm_bts_encode_si13,
        gsm_bts_encode_si2,
        gsm_bts_encode_si3,
        gsm_bts_encode_si4,
    };

    schedule[(frame_number / GSM_CONTROL_MULTIFRAME_FRAMES) % GSM_BCCH_TYPES](block);
}

static void gsm_lapdm_encode_frame(
    GSMLapdmHeader *header,
    uint8_t control,
    bool command,
    const uint8_t *info,
    size_t length,
    bool more
) {
    header->address = (command ? LAPDM_ADDRESS_CR : 0) | LAPDM_ADDRESS_EA;
    header->control = control;
    header->length = (length << LAPDM_LENGTH_SHIFT) | (more ? LAPDM_LENGTH_M : 0) | LAPDM_LENGTH_EL;
    if (length > 0)
        memcpy(header + 1, info, length);
}

static void gsm_bts_lapdm_enqueue(GSMBtsLapdm *lapdm, const uint8_t *message, size_t length) {
    size_t tail = (lapdm->queue_head + lapdm->queue_count) % GSM_LAPDM_QUEUE_SIZE;

    if (lapdm->queue_count == GSM_LAPDM_QUEUE_SIZE) {
        warn_report("gsm-bts: SDCCH queue full, dropping a message");
        return;
    }
    memcpy(lapdm->queue[tail].data, message, length);
    lapdm->queue[tail].length = length;
    lapdm->queue_count++;
}

static void gsm_bts_release_channel(GSMBtsLapdm *lapdm) {
    static const uint8_t channel_release[] = { GSM48_PD_RR, GSM48_MT_RR_CHANNEL_RELEASE, GSM48_RR_CAUSE_NORMAL };

    gsm_bts_lapdm_enqueue(lapdm, channel_release, sizeof(channel_release));
    lapdm->release = true;
}

static void gsm_bts_handle_mm(GSMBtsLapdm *lapdm, const uint8_t *message) {
    switch (message[GSM48_MESSAGE_TYPE_OFFSET] & GSM48_MT_MASK) {
        case GSM48_MT_MM_LOCATION_UPDATING_REQUEST: {
            uint8_t accept[GSM48_L3_HEADER_BYTES + GSM48_LAI_BYTES] = {
                GSM48_PD_MM, GSM48_MT_MM_LOCATION_UPDATING_ACCEPT
            };

            gsm_bts_encode_lai(&accept[GSM48_L3_HEADER_BYTES]);
            gsm_bts_lapdm_enqueue(lapdm, accept, sizeof(accept));
            gsm_bts_release_channel(lapdm);
            break;
        }
        case GSM48_MT_MM_CM_SERVICE_REQUEST: {
            static const uint8_t reject[] = {
                GSM48_PD_MM, GSM48_MT_MM_CM_SERVICE_REJECT, GSM48_MM_CAUSE_SERVICE_OPTION_NOT_SUPPORTED
            };

            gsm_bts_lapdm_enqueue(lapdm, reject, sizeof(reject));
            gsm_bts_release_channel(lapdm);
            break;
        }
        case GSM48_MT_MM_IMSI_DETACH_INDICATION:
            gsm_bts_release_channel(lapdm);
            break;
    }
}

static void gsm_bts_handle_l3(GSMBtsLapdm *lapdm, const uint8_t *message, size_t length) {
    if (length < GSM48_L3_HEADER_BYTES)
        return;
    if (lapdm->release)
        return;
    if ((message[0] & GSM48_PD_MASK) == GSM48_PD_MM)
        gsm_bts_handle_mm(lapdm, message);
}

static void gsm_bts_lapdm_handle_info(GSMBtsLapdm *lapdm, const uint8_t *info, size_t length, bool more) {
    GSMBtsMessage *reassembly = &lapdm->reassembly;

    if (reassembly->length + length > sizeof(reassembly->data)) {
        reassembly->length = 0;
        return;
    }
    memcpy(&reassembly->data[reassembly->length], info, length);
    reassembly->length += length;
    if (more)
        return;

    gsm_bts_handle_l3(lapdm, reassembly->data, reassembly->length);
    reassembly->length = 0;
}

static void gsm_bts_lapdm_acknowledge(GSMBtsLapdm *lapdm, uint8_t nr) {
    if (!lapdm->unacknowledged)
        return;
    if (nr != ((lapdm->vs + 1) & LAPDM_SEQUENCE_MASK))
        return;

    lapdm->vs = nr;
    lapdm->unacknowledged = false;
    lapdm->retransmissions = 0;
    lapdm->sent_offset += lapdm->sent_length;
    if (lapdm->sent_offset < lapdm->queue[lapdm->queue_head].length)
        return;

    lapdm->sent_offset = 0;
    lapdm->queue_head = (lapdm->queue_head + 1) % GSM_LAPDM_QUEUE_SIZE;
    lapdm->queue_count--;
}

static void gsm_bts_lapdm_handle_frame(GSMBtsLapdm *lapdm, const uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    const GSMLapdmHeader *header = (const GSMLapdmHeader *) block;
    const uint8_t *info = (const uint8_t *) (header + 1);
    uint8_t sapi = (header->address >> LAPDM_ADDRESS_SAPI_SHIFT) & LAPDM_ADDRESS_SAPI_MASK;
    uint8_t control = header->control;
    uint8_t nr = control >> LAPDM_NR_SHIFT;
    size_t length = MIN(header->length >> LAPDM_LENGTH_SHIFT, LAPDM_SDCCH_INFO_BYTES);
    bool more = header->length & LAPDM_LENGTH_M;

    if (!lapdm->active)
        return;
    if (sapi != LAPDM_SAPI_SIGNALLING)
        return;

    if ((control & LAPDM_CONTROL_I_MASK) == LAPDM_CONTROL_I) {
        uint8_t ns = (control >> LAPDM_NS_SHIFT) & LAPDM_SEQUENCE_MASK;

        gsm_bts_lapdm_acknowledge(lapdm, nr);
        lapdm->send_ack = true;
        if (!lapdm->established)
            return;
        if (ns != lapdm->vr)
            return;
        lapdm->vr = (lapdm->vr + 1) & LAPDM_SEQUENCE_MASK;
        gsm_bts_lapdm_handle_info(lapdm, info, length, more);
        return;
    }

    if ((control & LAPDM_CONTROL_S_MASK) == LAPDM_CONTROL_S) {
        gsm_bts_lapdm_acknowledge(lapdm, nr);
        return;
    }

    switch (control & LAPDM_CONTROL_U_MASK) {
        case LAPDM_CONTROL_SABM:
            if (lapdm->established)
                return;
            lapdm->established = true;
            lapdm->send_ua = true;
            memcpy(lapdm->ua_info.data, info, length);
            lapdm->ua_info.length = length;
            gsm_bts_handle_l3(lapdm, info, length);
            break;
        case LAPDM_CONTROL_DISC:
            lapdm->send_ua = true;
            lapdm->ua_info.length = 0;
            lapdm->disconnected = true;
            break;
    }
}

static void gsm_bts_lapdm_transmit(GSMBtsLapdm *lapdm, uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    GSMLapdmHeader *header = (GSMLapdmHeader *) block;

    memset(block, GSM_MACBLOCK_PADDING, GSM_XCCH_BLOCK_BYTES);
    if (lapdm->send_ua) {
        lapdm->send_ua = false;
        gsm_lapdm_encode_frame(header, LAPDM_CONTROL_UA | LAPDM_CONTROL_PF, false, lapdm->ua_info.data,
            lapdm->ua_info.length, false);
        if (lapdm->disconnected)
            memset(lapdm, 0, sizeof(*lapdm));
        return;
    }

    if (lapdm->established && lapdm->queue_count > 0) {
        const GSMBtsMessage *message = &lapdm->queue[lapdm->queue_head];
        size_t remaining = message->length - lapdm->sent_offset;
        size_t length = MIN(remaining, LAPDM_SDCCH_INFO_BYTES);
        uint8_t control = (lapdm->vr << LAPDM_NR_SHIFT) | (lapdm->vs << LAPDM_NS_SHIFT);

        if (lapdm->unacknowledged && ++lapdm->retransmissions > LAPDM_N200) {
            memset(lapdm, 0, sizeof(*lapdm));
            gsm_lapdm_encode_frame(header, LAPDM_CONTROL_UI, false, NULL, 0, false);
            return;
        }
        lapdm->unacknowledged = true;
        lapdm->sent_length = length;
        lapdm->send_ack = false;
        gsm_lapdm_encode_frame(header, control, true, &message->data[lapdm->sent_offset], length, length < remaining);
        return;
    }

    if (lapdm->send_ack) {
        lapdm->send_ack = false;
        gsm_lapdm_encode_frame(header, (lapdm->vr << LAPDM_NR_SHIFT) | LAPDM_CONTROL_RR, false, NULL, 0, false);
        return;
    }

    if (lapdm->release && lapdm->queue_count == 0 && ++lapdm->retransmissions > LAPDM_N200)
        memset(lapdm, 0, sizeof(*lapdm));
    gsm_lapdm_encode_frame(header, LAPDM_CONTROL_UI, false, NULL, 0, false);
}

static void gsm_bts_sacch_transmit(GSMBts *bts, uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    GSMSacchHeader *header = (GSMSacchHeader *) block;

    memset(block, GSM_MACBLOCK_PADDING, GSM_XCCH_BLOCK_BYTES);
    header->ms_power_level = SACCH_MS_POWER_LEVEL;
    header->timing_advance = SACCH_TIMING_ADVANCE;
    if (bts->sacch_si6) {
        GSMSacchSystemInformation6 si = {
            .protocol_discriminator = GSM48_PD_RR,
            .message_type = GSM48_MT_RR_SYSINFO_6,
            .cell_options = GSM_BTS_CELL_OPTIONS,
            .ncc_permitted = GSM48_NCC_PERMITTED_ALL,
        };

        stw_be_p(&si.cell_identity, GSM_BTS_CELL_IDENTITY);
        gsm_bts_encode_lai(si.lai);
        gsm_lapdm_encode_frame(&header->lapdm, LAPDM_CONTROL_UI, true, (uint8_t *) &si, sizeof(si), false);
    } else {
        GSMSacchSystemInformation5 si = {
            .protocol_discriminator = GSM48_PD_RR,
            .message_type = GSM48_MT_RR_SYSINFO_5,
        };

        gsm_lapdm_encode_frame(&header->lapdm, LAPDM_CONTROL_UI, true, (uint8_t *) &si, sizeof(si), false);
    }
    bts->sacch_si6 = !bts->sacch_si6;
}

static GSMBtsTs0Channel gsm_bts_ts0_downlink_channel(uint32_t frame_number, uint32_t *block_start) {
    uint32_t position = frame_number % GSM_CONTROL_MULTIFRAME_FRAMES;
    bool even_multiframe = ((frame_number / GSM_CONTROL_MULTIFRAME_FRAMES) % 2) == 0;
    uint32_t offset;

    if (position == GSM_CONTROL_MULTIFRAME_FRAMES - 1)
        return GSM_BTS_TS0_OTHER;
    if (position % GSM_SCH_FCCH_PERIOD == 0)
        return GSM_BTS_TS0_FCCH;
    if (position % GSM_SCH_FCCH_PERIOD == 1)
        return GSM_BTS_TS0_SCH;

    offset = ((position % GSM_SCH_FCCH_PERIOD) - GSM_TS0_FIRST_BLOCK) % GSM_XCCH_BLOCK_BURSTS;
    *block_start = frame_number - offset;
    switch (position - offset) {
        case GSM_TS0_BCCH_START:
            return GSM_BTS_TS0_BCCH;
        case GSM_TS0_CCCH0_START:
        case GSM_TS0_CCCH1_START:
        case GSM_TS0_CCCH2_START:
            return GSM_BTS_TS0_CCCH;
        case GSM_TS0_SDCCH0_START:
            return GSM_BTS_TS0_SDCCH;
        case GSM_TS0_SACCH0_START:
            return even_multiframe ? GSM_BTS_TS0_SACCH : GSM_BTS_TS0_OTHER;
        default:
            return GSM_BTS_TS0_OTHER;
    }
}

static uint32_t gsm_frame_number_to_sch_information(uint32_t frame_number) {
    uint32_t t1 = frame_number / (GSM_TRAFFIC_MULTIFRAME_FRAMES * GSM_CONTROL_MULTIFRAME_FRAMES);
    uint32_t t2 = frame_number % GSM_TRAFFIC_MULTIFRAME_FRAMES;
    uint32_t t3_prime = ((frame_number % GSM_CONTROL_MULTIFRAME_FRAMES) - 1) / GSM_SCH_FCCH_PERIOD;
    uint32_t t3_prime_high = t3_prime >> 1;
    uint32_t t3_prime_low = t3_prime & 1;
    uint32_t information = (GSM_BTS_BSIC << GSM_SCH_BSIC_SHIFT) | (t3_prime_high << GSM_SCH_T3_PRIME_HIGH_SHIFT) |
        (t2 << GSM_SCH_T2_SHIFT) | (t3_prime_low << GSM_SCH_T3_PRIME_LOW_SHIFT);

    for (size_t i = 0; i < ARRAY_SIZE(GSM_SCH_T1_FIELDS); i++) {
        const GSMSchField *field = &GSM_SCH_T1_FIELDS[i];
        uint32_t value = (t1 >> field->value_shift) & ((1U << field->bits) - 1);

        information |= value << field->information_shift;
    }
    return information;
}

static uint16_t gsm_sch_parity(uint32_t information) {
    uint16_t remainder = 0;

    for (size_t bit = 0; bit < GSM_SCH_DATA_BITS; bit++) {
        bool input = (information >> bit) & 1;
        bool feedback = ((remainder >> (GSM_SCH_PARITY_BITS - 1)) & 1) != input;

        remainder = (remainder << 1) & GSM_SCH_CRC_MASK;
        if (feedback)
            remainder ^= GSM_SCH_CRC_POLYNOMIAL;
    }
    return remainder ^ GSM_SCH_CRC_MASK;
}

static bool gsm_sch_uncoded_bit(uint32_t information, uint16_t parity, int bit) {
    if (bit < 0 || bit >= (int) GSM_SCH_UNCODED_BITS)
        return false;
    if (bit < (int) GSM_SCH_DATA_BITS)
        return (information >> bit) & 1;
    return (parity >> (GSM_SCH_PARITY_BITS - 1 - (bit - GSM_SCH_DATA_BITS))) & 1;
}

static bool gsm_sch_encoded_bit(uint32_t information, uint16_t parity, size_t bit) {
    int input = bit / 2;
    bool value = gsm_sch_uncoded_bit(information, parity, input);

    value ^= gsm_sch_uncoded_bit(information, parity, input - GSM_SCH_CONV_TAP_D3);
    value ^= gsm_sch_uncoded_bit(information, parity, input - GSM_SCH_CONV_TAP_D4);
    if (bit & 1)
        value ^= gsm_sch_uncoded_bit(information, parity, input - GSM_SCH_CONV_TAP_D1);
    return value;
}

static bool gsm_sch_burst_bit(uint32_t information, uint16_t parity, size_t symbol) {
    if (symbol >= GSM_SCH_DATA_FIRST_START && symbol < GSM_SCH_TRAINING_START)
        return gsm_sch_encoded_bit(information, parity, symbol - GSM_SCH_DATA_FIRST_START);
    if (symbol >= GSM_SCH_TRAINING_START && symbol < GSM_SCH_DATA_SECOND_START)
        return GSM_SCH_TRAINING_SEQUENCE[symbol - GSM_SCH_TRAINING_START] == '1';
    if (symbol >= GSM_SCH_DATA_SECOND_START && symbol < GSM_SCH_TAIL_START)
        return gsm_sch_encoded_bit(information, parity, symbol - GSM_SCH_DATA_SECOND_START + GSM_SCH_CODED_HALF_BITS);
    return false;
}

static void gsm_bts_encode_sch(uint32_t frame_number, GSMBurst *burst) {
    uint32_t information = gsm_frame_number_to_sch_information(frame_number);
    uint16_t parity = gsm_sch_parity(information);

    for (size_t symbol = 0; symbol < GSM_BURST_BITS; symbol++)
        burst->bits[symbol] = gsm_sch_burst_bit(information, parity, symbol);
}

static uint8_t gsm_bts_ts0_transmit_block(
    GSMBts *bts,
    GSMBtsTs0Channel channel,
    uint32_t block_start,
    uint8_t block[GSM_XCCH_BLOCK_BYTES]
) {
    switch (channel) {
        case GSM_BTS_TS0_BCCH:
            gsm_bts_encode_bcch(block_start, block);
            return GSMTAP_CHANNEL_BCCH;
        case GSM_BTS_TS0_CCCH:
            gsm_bts_ccch_dequeue(bts, block);
            return GSMTAP_CHANNEL_CCCH;
        case GSM_BTS_TS0_SDCCH:
            gsm_bts_lapdm_transmit(&bts->lapdm, block);
            return GSMTAP_CHANNEL_SDCCH4;
        default:
            gsm_bts_sacch_transmit(bts, block);
            return GSMTAP_CHANNEL_SDCCH4 | GSMTAP_CHANNEL_ACCH;
    }
}

static void gsm_bts_ts0_burst(GSMBts *bts, uint32_t frame_number, GSMBurst *burst) {
    GSMBtsDownlinkBlock *cache = &bts->ts0_block;
    uint32_t block_start = 0;
    GSMBtsTs0Channel channel = gsm_bts_ts0_downlink_channel(frame_number, &block_start);

    switch (channel) {
        case GSM_BTS_TS0_FCCH:
            memset(burst->bits, 0, sizeof(burst->bits));
            return;
        case GSM_BTS_TS0_SCH:
            gsm_bts_encode_sch(frame_number, burst);
            return;
        case GSM_BTS_TS0_OTHER:
            gsm_dummy_burst(burst);
            return;
        default:
            break;
    }

    if (!cache->valid || cache->frame_number != block_start) {
        uint8_t block[GSM_XCCH_BLOCK_BYTES];
        uint8_t gsmtap_channel = gsm_bts_ts0_transmit_block(bts, channel, block_start, block);

        gsm_xcch_encode(block, cache->data);
        cache->frame_number = block_start;
        cache->valid = true;
        gsm_bts_gsmtap(bts, false, 0, block_start, gsmtap_channel, block, sizeof(block));
    }
    gsm_normal_burst(burst, GSM_BTS_TSC, cache->data[frame_number - block_start]);
}

static void gsm_bts_handle_rach(GSMBts *bts, uint8_t ra, uint32_t frame_number) {
    uint8_t message[GSM_XCCH_BLOCK_BYTES];

    gsm_bts_gsmtap(bts, true, 0, frame_number, GSMTAP_CHANNEL_RACH, &ra, sizeof(ra));
    if (gsm_bts_gprs_handle_rach(bts, ra, frame_number))
        return;

    memset(&bts->lapdm, 0, sizeof(bts->lapdm));
    bts->lapdm.active = true;
    gsm_bts_encode_sdcch_assignment(message, ra, frame_number);
    gsm_bts_ccch_enqueue(bts, message);
}

static bool gsm_bts_ts0_uplink_is_rach(uint32_t position) {
    for (size_t i = 0; i < ARRAY_SIZE(GSM_TS0_UPLINK_DEDICATED_BLOCKS); i++) {
        uint32_t start = GSM_TS0_UPLINK_DEDICATED_BLOCKS[i];

        if (position >= start && position < start + GSM_XCCH_BLOCK_BURSTS)
            return false;
    }
    return true;
}

static void gsm_bts_ts0_uplink_rach(GSMBts *bts, uint32_t frame_number, const GSMUplinkBurst *burst) {
    uint8_t coded[GSM_RACH_CODED_BITS];
    uint8_t ra;
    size_t start;

    if (!gsm_uplink_find_access_burst(burst, &start))
        return;

    gsm_uplink_access_burst_data(burst, start, coded);
    if (gsm_rach_decode(coded, GSM_BTS_BSIC, &ra))
        gsm_bts_handle_rach(bts, ra, frame_number);
}

static void gsm_bts_ts0_uplink_sdcch(
    GSMBts *bts,
    uint32_t burst_index,
    uint32_t frame_number,
    const GSMUplinkBurst *burst
) {
    GSMBtsUplinkBlock *uplink = &bts->sdcch_uplink;
    uint32_t block_start = frame_number - burst_index;
    uint8_t block[GSM_XCCH_BLOCK_BYTES];
    size_t start;

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

    gsm_bts_gsmtap(bts, true, 0, block_start, GSMTAP_CHANNEL_SDCCH4, block, sizeof(block));
    gsm_bts_lapdm_handle_frame(&bts->lapdm, block);
}

static void gsm_bts_ts0_uplink(GSMBts *bts, uint32_t frame_number, const GSMUplinkBurst *burst) {
    uint32_t position = frame_number % GSM_CONTROL_MULTIFRAME_FRAMES;
    bool sdcch0 = position >= GSM_TS0_UPLINK_SDCCH0_START &&
        position < GSM_TS0_UPLINK_SDCCH0_START + GSM_XCCH_BLOCK_BURSTS;

    if (gsm_bts_ts0_uplink_is_rach(position)) {
        gsm_bts_ts0_uplink_rach(bts, frame_number, burst);
    } else if (sdcch0) {
        gsm_bts_ts0_uplink_sdcch(bts, position - GSM_TS0_UPLINK_SDCCH0_START, frame_number, burst);
    }
}

static bool gsm_bts_downlink_burst(
    GSMNetwork *network,
    uint16_t arfcn,
    uint32_t frame_number,
    uint8_t timeslot,
    GSMBurst *burst
) {
    GSMBts *bts = GSM_BTS(network);

    if (arfcn != GSM_BTS_ARFCN)
        return false;

    if (timeslot == 0) {
        gsm_bts_ts0_burst(bts, frame_number, burst);
    } else if (timeslot == GSM_BTS_PDCH_TIMESLOT) {
        gsm_bts_gprs_downlink_burst(bts, frame_number, burst);
    } else {
        gsm_dummy_burst(burst);
    }
    return true;
}

static void gsm_bts_uplink_burst(
    GSMNetwork *network,
    uint16_t arfcn,
    uint32_t frame_number,
    uint8_t timeslot,
    const GSMUplinkBurst *burst
) {
    GSMBts *bts = GSM_BTS(network);

    if (arfcn != GSM_BTS_ARFCN)
        return;

    if (timeslot == 0) {
        gsm_bts_ts0_uplink(bts, frame_number, burst);
    } else if (timeslot == GSM_BTS_PDCH_TIMESLOT) {
        gsm_bts_gprs_uplink_burst(bts, frame_number, burst);
    }
}

static ssize_t gsm_bts_receive_ethernet(NetClientState *nc, const uint8_t *frame, size_t size) {
    gsm_bts_gprs_handle_ethernet(qemu_get_nic_opaque(nc), frame, size);
    return size;
}

static NetClientInfo gsm_bts_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = gsm_bts_receive_ethernet,
};

void gsm_bts_send_ethernet(GSMBts *bts, const uint8_t *frame, size_t size) {
    if (bts->nic)
        qemu_send_packet(qemu_get_queue(bts->nic), frame, size);
}

static bool gsm_bts_open_gsmtap(GSMBts *bts, Error **errp) {
    g_autofree char *address = g_strdup_printf("%s:%d", bts->gsmtap, GSMTAP_PORT);
    SocketAddress *remote = socket_parse(address, errp);

    if (!remote)
        return false;

    bts->gsmtap_fd = socket_dgram(remote, NULL, errp);
    qapi_free_SocketAddress(remote);
    return bts->gsmtap_fd >= 0;
}

static void gsm_bts_connect_netdev(Notifier *notifier, void *_data) {
    GSMBts *bts = container_of(notifier, GSMBts, machine_done);
    NetClientState *peer = qemu_find_netdev(bts->netdev);

    if (!peer) {
        error_report("gsm-bts: no netdev '%s'", bts->netdev);
        exit(1);
    }
    bts->nic_conf.peers.ncs[0] = peer;
    bts->nic_conf.peers.queues = 1;
    qemu_macaddr_default_if_unset(&bts->nic_conf.macaddr);
    bts->nic = qemu_new_nic(&gsm_bts_net_info, &bts->nic_conf, TYPE_GSM_BTS, NULL, &bts->reentrancy_guard, bts);
}

static void gsm_bts_complete(UserCreatable *uc, Error **errp) {
    GSMBts *bts = GSM_BTS(uc);

    if (bts->gsmtap && !gsm_bts_open_gsmtap(bts, errp))
        return;

    if (bts->netdev) {
        bts->machine_done.notify = gsm_bts_connect_netdev;
        qemu_add_machine_init_done_notifier(&bts->machine_done);
    }
    gsm_bts_gprs_init(bts);
}

static char *gsm_bts_get_netdev(Object *obj, Error **_errp) {
    return g_strdup(GSM_BTS(obj)->netdev);
}

static void gsm_bts_set_netdev(Object *obj, const char *value, Error **_errp) {
    GSMBts *bts = GSM_BTS(obj);

    g_free(bts->netdev);
    bts->netdev = g_strdup(value);
}

static char *gsm_bts_get_gsmtap(Object *obj, Error **_errp) {
    return g_strdup(GSM_BTS(obj)->gsmtap);
}

static void gsm_bts_set_gsmtap(Object *obj, const char *value, Error **_errp) {
    GSMBts *bts = GSM_BTS(obj);

    g_free(bts->gsmtap);
    bts->gsmtap = g_strdup(value);
}

static void gsm_bts_instance_init(Object *obj) {
    GSM_BTS(obj)->gsmtap_fd = -1;
}

static void gsm_bts_instance_finalize(Object *obj) {
    GSMBts *bts = GSM_BTS(obj);

    gsm_bts_gprs_cleanup(bts);
    if (bts->machine_done.notify)
        qemu_remove_machine_init_done_notifier(&bts->machine_done);
    if (bts->nic)
        qemu_del_nic(bts->nic);
    if (bts->gsmtap_fd >= 0)
        close(bts->gsmtap_fd);
    g_free(bts->netdev);
    g_free(bts->gsmtap);
}

static void gsm_bts_class_init(ObjectClass *klass, const void *_data) {
    GSMNetworkClass *network_class = GSM_NETWORK_CLASS(klass);
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(klass);

    network_class->downlink_burst = gsm_bts_downlink_burst;
    network_class->uplink_burst = gsm_bts_uplink_burst;
    ucc->complete = gsm_bts_complete;
    object_class_property_add_str(klass, "netdev", gsm_bts_get_netdev, gsm_bts_set_netdev);
    object_class_property_add_str(klass, "gsmtap", gsm_bts_get_gsmtap, gsm_bts_set_gsmtap);
}

static const TypeInfo gsm_bts_info = {
    .name              = TYPE_GSM_BTS,
    .parent            = TYPE_OBJECT,
    .instance_size     = sizeof(GSMBts),
    .instance_init     = gsm_bts_instance_init,
    .instance_finalize = gsm_bts_instance_finalize,
    .class_init        = gsm_bts_class_init,
    .interfaces        = (const InterfaceInfo[]) {
        { TYPE_GSM_NETWORK },
        { TYPE_USER_CREATABLE },
        { NULL },
    },
};

static void gsm_bts_register_types(void) {
    type_register_static(&gsm_bts_info);
}

type_init(gsm_bts_register_types)
