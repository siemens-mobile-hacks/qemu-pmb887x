/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qom/object_interfaces.h"
#include "hw/gsm/gsm-bts.h"
#include "hw/gsm/gsm-network.h"

#define GSM_BTS_ARFCN 5U
#define GSM_BTS_BSIC 0U
#define GSM_CONTROL_MULTIFRAME_FRAMES 51U
#define GSM_TRAFFIC_MULTIFRAME_FRAMES 26U
#define GSM_SCH_DATA_BITS 25U
#define GSM_SCH_PARITY_BITS 10U
#define GSM_SCH_TAIL_BITS 4U
#define GSM_SCH_UNCODED_BITS (GSM_SCH_DATA_BITS + GSM_SCH_PARITY_BITS + GSM_SCH_TAIL_BITS)
#define GSM_SCH_ENCODED_BITS (GSM_SCH_UNCODED_BITS * 2U)
#define GSM_SCH_DATA_FIRST_START 3U
#define GSM_SCH_TRAINING_START 42U
#define GSM_SCH_DATA_SECOND_START 106U
#define GSM_SCH_CRC_MASK 0x03FFU
#define GSM_SCH_CRC_POLYNOMIAL 0x0175U

OBJECT_DECLARE_SIMPLE_TYPE(GSMBts, GSM_BTS)

struct GSMBts {
    Object parent_obj;
};

static const char GSM_SCH_TRAINING_SEQUENCE[] =
    "1011100101100010000001000000111100101101010001010111011000011011";
static const char GSM_DUMMY_BURST[] =
    "0001111101101110110000010100100111000001001000100000001111100011100010111000101110001010111010010100"
    "011001100111001111010011111000100101111101010000";

static bool gsm_fcch_frame(uint32_t frame) {
    return frame <= 40U && frame % 10U == 0;
}

static bool gsm_sch_frame(uint32_t frame) {
    return frame <= 41U && frame % 10U == 1U;
}

static uint32_t gsm_frame_number_to_sch_information(uint32_t frame_number) {
    uint32_t t1 = frame_number / (GSM_TRAFFIC_MULTIFRAME_FRAMES * GSM_CONTROL_MULTIFRAME_FRAMES);
    uint32_t t2 = frame_number % GSM_TRAFFIC_MULTIFRAME_FRAMES;
    uint32_t t3 = frame_number % GSM_CONTROL_MULTIFRAME_FRAMES;
    uint32_t t3_prime = (t3 - 1U) / 10U;

    return ((t1 >> 9U) & 0x03U) | (GSM_BTS_BSIC << 2U) | (((t1 >> 1U) & 0xFFU) << 8U) |
        (((t3_prime >> 1U) & 0x03U) << 16U) | ((t2 & 0x1FU) << 18U) | ((t1 & 1U) << 23U) |
        ((t3_prime & 1U) << 24U);
}

static bool gsm_sch_information_bit(uint32_t information, size_t bit) {
    return ((information >> bit) & 1U) != 0;
}

static uint16_t gsm_sch_parity(uint32_t information) {
    uint16_t remainder = 0;

    for (size_t bit = 0; bit < GSM_SCH_DATA_BITS; bit++) {
        bool input = gsm_sch_information_bit(information, bit);
        bool feedback = ((remainder >> 9) & 1U) != input;

        remainder = (remainder << 1) & GSM_SCH_CRC_MASK;
        if (feedback)
            remainder ^= GSM_SCH_CRC_POLYNOMIAL;
    }

    return remainder ^ GSM_SCH_CRC_MASK;
}

static bool gsm_sch_uncoded_bit(uint32_t information, uint16_t parity, int bit) {
    if (bit < 0 || bit >= (int) (GSM_SCH_DATA_BITS + GSM_SCH_PARITY_BITS))
        return false;
    if (bit < (int) GSM_SCH_DATA_BITS)
        return gsm_sch_information_bit(information, bit);

    return ((parity >> (GSM_SCH_PARITY_BITS - 1U - (bit - GSM_SCH_DATA_BITS))) & 1U) != 0;
}

static bool gsm_sch_encoded_bit(uint32_t information, uint16_t parity, size_t bit) {
    int input = bit / 2;
    bool value = gsm_sch_uncoded_bit(information, parity, input);
    value ^= gsm_sch_uncoded_bit(information, parity, input - 3);
    value ^= gsm_sch_uncoded_bit(information, parity, input - 4);

    if ((bit & 1U))
        value ^= gsm_sch_uncoded_bit(information, parity, input - 1);

    return value;
}

static bool gsm_sch_burst_bit(uint32_t information, uint16_t parity, size_t symbol) {
    if (symbol >= GSM_SCH_DATA_FIRST_START && symbol < GSM_SCH_TRAINING_START)
        return gsm_sch_encoded_bit(information, parity, symbol - GSM_SCH_DATA_FIRST_START);
    if (symbol >= GSM_SCH_TRAINING_START && symbol < GSM_SCH_DATA_SECOND_START)
        return GSM_SCH_TRAINING_SEQUENCE[symbol - GSM_SCH_TRAINING_START] == '1';
    if (symbol >= GSM_SCH_DATA_SECOND_START && symbol < GSM_BURST_BITS - 3U)
        return gsm_sch_encoded_bit(information, parity,
            symbol - GSM_SCH_DATA_SECOND_START + GSM_SCH_ENCODED_BITS / 2U);
    return false;
}

static void gsm_bts_encode_sch(uint32_t frame_number, GSMBurst *burst) {
    uint32_t information = gsm_frame_number_to_sch_information(frame_number);
    uint16_t parity = gsm_sch_parity(information);

    for (size_t symbol = 0; symbol < GSM_BURST_BITS; symbol++)
        burst->bits[symbol] = gsm_sch_burst_bit(information, parity, symbol);
}

static bool gsm_bts_downlink_burst(
    GSMNetwork *_network,
    uint16_t arfcn,
    uint32_t frame_number,
    uint8_t timeslot,
    GSMBurst *burst
) {
    uint32_t multiframe_frame = frame_number % GSM_CONTROL_MULTIFRAME_FRAMES;

    if (arfcn != GSM_BTS_ARFCN)
        return false;

    if (timeslot == 0 && gsm_fcch_frame(multiframe_frame)) {
        memset(burst->bits, 0, sizeof(burst->bits));
    } else if (timeslot == 0 && gsm_sch_frame(multiframe_frame)) {
        gsm_bts_encode_sch(frame_number, burst);
    } else {
        for (size_t symbol = 0; symbol < GSM_BURST_BITS; symbol++)
            burst->bits[symbol] = GSM_DUMMY_BURST[symbol] == '1';
    }
    return true;
}

static void gsm_bts_class_init(ObjectClass *klass, const void *_data) {
    GSMNetworkClass *network_class = GSM_NETWORK_CLASS(klass);
    network_class->downlink_burst = gsm_bts_downlink_burst;
}

static const TypeInfo gsm_bts_info = {
    .name           = TYPE_GSM_BTS,
    .parent         = TYPE_OBJECT,
    .instance_size  = sizeof(GSMBts),
    .class_init     = gsm_bts_class_init,
    .interfaces     = (const InterfaceInfo[]) {
        { TYPE_GSM_NETWORK },
        { TYPE_USER_CREATABLE },
        { NULL },
    },
};

static void gsm_bts_register_types(void) {
    type_register_static(&gsm_bts_info);
}

type_init(gsm_bts_register_types)
