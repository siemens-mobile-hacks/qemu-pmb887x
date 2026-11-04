/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"

#include "hw/smartcard/gsm11.h"
#include "hw/smartcard/iso7816.h"
#include "gsm-sim-fs.h"

#define TYPE_GSM_SIM "gsm-sim"
OBJECT_DECLARE_TYPE(GSMSim, GSMSimClass, GSM_SIM)

#define GSM_SIM_SELECT_DATA_SIZE 2
#define GSM_SIM_IMSI_DIGIT_COUNT 15
#define GSM_SIM_IMSI_BODY_SIZE 8
#define GSM_SIM_IMSI_SIZE (1 + GSM_SIM_IMSI_BODY_SIZE)
#define GSM_SIM_IMSI_IDENTITY_NIBBLE 0x09
#define GSM_SIM_BCD_HIGH_NIBBLE_SHIFT 4
#define GSM_SIM_MCC_LENGTH 3
#define GSM_SIM_OPERATOR_MIN_LENGTH (GSM_SIM_MCC_LENGTH + 2)
#define GSM_SIM_OPERATOR_MAX_LENGTH (GSM_SIM_MCC_LENGTH + 3)
#define GSM_SIM_AD_SIZE 4
#define GSM_SIM_AD_WITHOUT_MNC_LENGTH_SIZE 3
#define GSM_SIM_AD_MNC_LENGTH_OFFSET 3
#define GSM_SIM_DIALING_RECORD_SIZE 28
#define GSM_SIM_EXT_RECORD_SIZE 13
#define GSM_SIM_ACM_RECORD_SIZE 3
#define GSM_SIM_CCP_RECORD_SIZE 14
#define GSM_SIM_SMSP_RECORD_SIZE 42
#define GSM_SIM_SMS_RECORD_SIZE 176
#define GSM_SIM_SMS_STATUS_FREE 0x00
#define GSM_SIM_MSISDN_NUMBER_LENGTH 7
#define GSM_SIM_MSISDN_TON_NPI 0x91

#define GSM_SIM_ACCESS_READ_ALWAYS_UPDATE_CHV1 { 0x01, 0xF0, 0x44 }
#define GSM_SIM_ACCESS_READ_ALWAYS_UPDATE_ADM { 0x04, 0xF0, 0x44 }
#define GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 { 0x11, 0xF0, 0x44 }
#define GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV2 { 0x12, 0xF0, 0x44 }
#define GSM_SIM_ACCESS_READ_CHV1_UPDATE_ADM { 0x14, 0xF0, 0x44 }
#define GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV2_INCREASE_CHV1 { 0x12, 0x10, 0x44 }

#define GSM_SIM_DEFAULT_OPERATOR "00101"
#define GSM_SIM_DEFAULT_SUBSCRIBER "0123456789"

#define GSM_SIM_ATR_DELAY_CLOCKS 11960
#define GSM_SIM_SELECT_CLOCKS 10199
#define GSM_SIM_STATUS_CLOCKS 12253
#define GSM_SIM_READ_BINARY_CLOCKS 959
#define GSM_SIM_READ_RECORD_CLOCKS 1017
#define GSM_SIM_GET_RESPONSE_CLOCKS 296

struct GSMSim {
    ISO7816Card parent_obj;

    char *imsi;
    char *operator_code;

    GSMSimFS fs;
};

struct GSMSimClass {
    ISO7816CardClass parent_class;

    DeviceRealize parent_realize;
    DeviceUnrealize parent_unrealize;
};

enum {
    GSM_SIM_EF_ICCID = 0x2FE2,
    GSM_SIM_DF_TELECOM = 0x7F10,
    GSM_SIM_DF_GSM = 0x7F20,
    GSM_SIM_EF_ADN = 0x6F3A,
    GSM_SIM_EF_FDN = 0x6F3B,
    GSM_SIM_EF_SMS = 0x6F3C,
    GSM_SIM_EF_CCP = 0x6F3D,
    GSM_SIM_EF_MSISDN = 0x6F40,
    GSM_SIM_EF_SMSP = 0x6F42,
    GSM_SIM_EF_SMSS = 0x6F43,
    GSM_SIM_EF_LND = 0x6F44,
    GSM_SIM_EF_EXT1 = 0x6F4A,
    GSM_SIM_EF_EXT2 = 0x6F4B,
    GSM_SIM_EF_LP = 0x6F05,
    GSM_SIM_EF_IMSI = 0x6F07,
    GSM_SIM_EF_KC = 0x6F20,
    GSM_SIM_EF_PLMNSEL = 0x6F30,
    GSM_SIM_EF_HPLMN = 0x6F31,
    GSM_SIM_EF_ACMMAX = 0x6F37,
    GSM_SIM_EF_SST = 0x6F38,
    GSM_SIM_EF_ACM = 0x6F39,
    GSM_SIM_EF_PUCT = 0x6F41,
    GSM_SIM_EF_CBMI = 0x6F45,
    GSM_SIM_EF_KCGPRS = 0x6F52,
    GSM_SIM_EF_LOCIGPRS = 0x6F53,
    GSM_SIM_EF_BCCH = 0x6F74,
    GSM_SIM_EF_ACC = 0x6F78,
    GSM_SIM_EF_FPLMN = 0x6F7B,
    GSM_SIM_EF_LOCI = 0x6F7E,
    GSM_SIM_EF_AD = 0x6FAD,
    GSM_SIM_EF_PHASE = 0x6FAE,
};

static const uint8_t GSM_SIM_ATR[] = { 0x3B, 0x10, 0x95 };
static const uint8_t GSM_SIM_ICCID[] = {
    0x98, 0x88, 0x12, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20
};
static const uint8_t GSM_SIM_KC[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07
};
static const uint8_t GSM_SIM_ACMMAX[] = { 0x00, 0x00, 0x00 };
/*
 * GSM 11.11 services: CHV1 disable, ADN, FDN, SMS, AoC, CCP, PLMNsel,
 * MSISDN, EXT1, EXT2, SMSP, LND, CBMI, GPRS.
 */
static const uint8_t GSM_SIM_SST[] = {
    0xFF, 0x3F, 0xFF, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C
};
static const uint8_t GSM_SIM_ACC[] = { 0x00, 0x00 };
static const uint8_t GSM_SIM_LOCI[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00
};
static const uint8_t GSM_SIM_LOCIGPRS[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0x01
};
static const uint8_t GSM_SIM_PHASE[] = { 0x02 };
static const uint8_t GSM_SIM_MSISDN[GSM_SIM_DIALING_RECORD_SIZE] = {
    'V', 'i', 'k', 't', 'o', 'r', '8', '9',
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    GSM_SIM_MSISDN_NUMBER_LENGTH, GSM_SIM_MSISDN_TON_NPI,
    0x99, 0x09, 0x00, 0x00, 0x00, 0x98,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static const GSMSimFileDefinition GSM_SIM_FILES[] = {
    { .id = GSM_SIM_FS_MF_ID, .structure = GSM_SIM_FILE_MF },
    { .id = GSM_SIM_EF_ICCID, .parent_id = GSM_SIM_FS_MF_ID,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = sizeof(GSM_SIM_ICCID), .content = GSM_SIM_ICCID,
      .access = GSM_SIM_ACCESS_READ_ALWAYS_UPDATE_ADM },
    { .id = GSM_SIM_DF_GSM, .parent_id = GSM_SIM_FS_MF_ID,
      .structure = GSM_SIM_FILE_DF },
    { .id = GSM_SIM_DF_TELECOM, .parent_id = GSM_SIM_FS_MF_ID,
      .structure = GSM_SIM_FILE_DF },

    { .id = GSM_SIM_EF_LP, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = 7, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_ALWAYS_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_IMSI, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = GSM_SIM_IMSI_SIZE, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_ADM },
    { .id = GSM_SIM_EF_KC, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = sizeof(GSM_SIM_KC), .content = GSM_SIM_KC,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_PLMNSEL, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = 48, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_ALWAYS_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_HPLMN, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = 1, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_ADM },
    { .id = GSM_SIM_EF_ACMMAX, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = sizeof(GSM_SIM_ACMMAX), .content = GSM_SIM_ACMMAX,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV2 },
    { .id = GSM_SIM_EF_SST, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = sizeof(GSM_SIM_SST), .content = GSM_SIM_SST,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_ADM },
    { .id = GSM_SIM_EF_ACM, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_CYCLIC,
      .size = 10 * GSM_SIM_ACM_RECORD_SIZE,
      .record_size = GSM_SIM_ACM_RECORD_SIZE,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV2_INCREASE_CHV1 },
    { .id = GSM_SIM_EF_PUCT, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = 5, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV2 },
    { .id = GSM_SIM_EF_CBMI, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = 20, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_KCGPRS, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = sizeof(GSM_SIM_KC), .content = GSM_SIM_KC,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_LOCIGPRS, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = sizeof(GSM_SIM_LOCIGPRS), .content = GSM_SIM_LOCIGPRS,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_BCCH, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = 16, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_ACC, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = sizeof(GSM_SIM_ACC), .content = GSM_SIM_ACC,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_ADM },
    { .id = GSM_SIM_EF_FPLMN, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = 12, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_LOCI, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = sizeof(GSM_SIM_LOCI), .content = GSM_SIM_LOCI,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_AD, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = GSM_SIM_AD_SIZE,
      .access = GSM_SIM_ACCESS_READ_ALWAYS_UPDATE_ADM },
    { .id = GSM_SIM_EF_PHASE, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = sizeof(GSM_SIM_PHASE), .content = GSM_SIM_PHASE,
      .access = GSM_SIM_ACCESS_READ_ALWAYS_UPDATE_ADM },

    { .id = GSM_SIM_EF_ADN, .parent_id = GSM_SIM_DF_TELECOM,
      .structure = GSM_SIM_FILE_LINEAR_FIXED,
      .size = 250 * GSM_SIM_DIALING_RECORD_SIZE,
      .record_size = GSM_SIM_DIALING_RECORD_SIZE, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_FDN, .parent_id = GSM_SIM_DF_TELECOM,
      .structure = GSM_SIM_FILE_LINEAR_FIXED,
      .size = 10 * GSM_SIM_DIALING_RECORD_SIZE,
      .record_size = GSM_SIM_DIALING_RECORD_SIZE, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV2 },
    { .id = GSM_SIM_EF_SMS, .parent_id = GSM_SIM_DF_TELECOM,
      .structure = GSM_SIM_FILE_LINEAR_FIXED,
      .size = 20 * GSM_SIM_SMS_RECORD_SIZE,
      .record_size = GSM_SIM_SMS_RECORD_SIZE, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_CCP, .parent_id = GSM_SIM_DF_TELECOM,
      .structure = GSM_SIM_FILE_LINEAR_FIXED,
      .size = 10 * GSM_SIM_CCP_RECORD_SIZE,
      .record_size = GSM_SIM_CCP_RECORD_SIZE, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_MSISDN, .parent_id = GSM_SIM_DF_TELECOM,
      .structure = GSM_SIM_FILE_LINEAR_FIXED,
      .size = sizeof(GSM_SIM_MSISDN),
      .record_size = GSM_SIM_DIALING_RECORD_SIZE, .content = GSM_SIM_MSISDN,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_SMSP, .parent_id = GSM_SIM_DF_TELECOM,
      .structure = GSM_SIM_FILE_LINEAR_FIXED,
      .size = 5 * GSM_SIM_SMSP_RECORD_SIZE,
      .record_size = GSM_SIM_SMSP_RECORD_SIZE, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_SMSS, .parent_id = GSM_SIM_DF_TELECOM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = 2, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_LND, .parent_id = GSM_SIM_DF_TELECOM,
      .structure = GSM_SIM_FILE_CYCLIC,
      .size = 10 * GSM_SIM_DIALING_RECORD_SIZE,
      .record_size = GSM_SIM_DIALING_RECORD_SIZE, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_EXT1, .parent_id = GSM_SIM_DF_TELECOM,
      .structure = GSM_SIM_FILE_LINEAR_FIXED,
      .size = 10 * GSM_SIM_EXT_RECORD_SIZE,
      .record_size = GSM_SIM_EXT_RECORD_SIZE, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 },
    { .id = GSM_SIM_EF_EXT2, .parent_id = GSM_SIM_DF_TELECOM,
      .structure = GSM_SIM_FILE_LINEAR_FIXED,
      .size = 5 * GSM_SIM_EXT_RECORD_SIZE,
      .record_size = GSM_SIM_EXT_RECORD_SIZE, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV2 },
};

static const uint8_t *gsm_sim_get_atr(ISO7816Card *card, size_t *size)
{
    *size = sizeof(GSM_SIM_ATR);
    return GSM_SIM_ATR;
}

static void gsm_sim_cold_reset(ISO7816Card *card)
{
    GSMSimFS *fs = &GSM_SIM(card)->fs;

    gsm_sim_fs_reset(fs);
    fs->chv1_fulfilled = true;
    fs->chv2_fulfilled = false;
}

static bool gsm_sim_command_has_data(ISO7816Card *card, const uint8_t *header)
{
    return gsm11_command_has_data(header[ISO7816_OFFSET_INS]);
}

static uint32_t gsm_sim_processing_clocks(ISO7816Card *card,
                                          const uint8_t *header)
{
    switch (header[ISO7816_OFFSET_INS]) {
    case GSM11_INS_SELECT:
        return GSM_SIM_SELECT_CLOCKS;
    case GSM11_INS_STATUS:
        return GSM_SIM_STATUS_CLOCKS;
    case GSM11_INS_READ_BINARY:
        return GSM_SIM_READ_BINARY_CLOCKS;
    case GSM11_INS_READ_RECORD:
        return GSM_SIM_READ_RECORD_CLOCKS;
    case GSM11_INS_GET_RESPONSE:
        return GSM_SIM_GET_RESPONSE_CLOCKS;
    default:
        return 0;
    }
}

static uint16_t gsm_sim_data_size(uint8_t p3)
{
    return p3 ? p3 : ISO7816_MAX_DATA_SIZE;
}

static void gsm_sim_select(GSMSim *sim, const uint8_t *command,
                           ISO7816Response *response)
{
    const uint8_t *data = command + ISO7816_HEADER_SIZE;

    if (command[ISO7816_OFFSET_P3] != GSM_SIM_SELECT_DATA_SIZE) {
        iso7816_response_set_status(response, GSM11_STATUS_WRONG_LENGTH);
        return;
    }
    gsm_sim_fs_select(&sim->fs, (data[0] << 8) | data[1], response);
}

static void gsm_sim_execute(GSMSim *sim, const uint8_t *command,
                            ISO7816Response *response)
{
    GSMSimFS *fs = &sim->fs;
    uint8_t p1 = command[ISO7816_OFFSET_P1];
    uint8_t p2 = command[ISO7816_OFFSET_P2];
    uint8_t p3 = command[ISO7816_OFFSET_P3];
    uint16_t size = gsm_sim_data_size(p3);
    const uint8_t *data = command + ISO7816_HEADER_SIZE;

    switch (command[ISO7816_OFFSET_INS]) {
    case GSM11_INS_SELECT:
        gsm_sim_select(sim, command, response);
        break;
    case GSM11_INS_GET_RESPONSE:
        gsm_sim_fs_get_response(fs, p3, response);
        break;
    case GSM11_INS_STATUS:
        gsm_sim_fs_status(fs, p3, response);
        break;
    case GSM11_INS_READ_BINARY:
        gsm_sim_fs_read_binary(fs, (p1 << 8) | p2, size, response);
        break;
    case GSM11_INS_UPDATE_BINARY:
        gsm_sim_fs_update_binary(fs, (p1 << 8) | p2, data, size, response);
        break;
    case GSM11_INS_READ_RECORD:
        gsm_sim_fs_read_record(fs, p1, p2, size, response);
        break;
    case GSM11_INS_UPDATE_RECORD:
        gsm_sim_fs_update_record(fs, p1, p2, data, size, response);
        break;
    case GSM11_INS_SEEK:
        gsm_sim_fs_seek(fs, p2, data, size, response);
        break;
    case GSM11_INS_INCREASE:
        gsm_sim_fs_increase(fs, data, size, response);
        break;
    default:
        iso7816_response_set_status(response, GSM11_STATUS_INS_NOT_SUPPORTED);
        break;
    }
}

static void gsm_sim_exchange(ISO7816Card *card, const uint8_t *command,
                             size_t size, uint32_t exchange_id)
{
    ISO7816Response response = { 0 };

    if (command[ISO7816_OFFSET_CLA] != GSM11_CLASS) {
        iso7816_response_set_status(&response,
                                    GSM11_STATUS_CLASS_NOT_SUPPORTED);
    } else {
        gsm_sim_execute(GSM_SIM(card), command, &response);
    }
    iso7816_card_complete(card, exchange_id, &response);
}

static void gsm_sim_encode_imsi(const char *imsi,
                                uint8_t encoded[GSM_SIM_IMSI_SIZE])
{
    encoded[0] = GSM_SIM_IMSI_BODY_SIZE;
    encoded[1] = ((imsi[0] - '0') << GSM_SIM_BCD_HIGH_NIBBLE_SHIFT) |
                 GSM_SIM_IMSI_IDENTITY_NIBBLE;
    for (size_t digit = 1; digit < GSM_SIM_IMSI_DIGIT_COUNT; digit += 2) {
        uint8_t low = imsi[digit] - '0';
        uint8_t high = imsi[digit + 1] - '0';

        encoded[2 + digit / 2] = low | (high << GSM_SIM_BCD_HIGH_NIBBLE_SHIFT);
    }
}

static bool gsm_sim_is_decimal(const char *text, size_t length)
{
    return strlen(text) == length && strspn(text, "0123456789") == length;
}

static bool gsm_sim_validate_operator(const char *operator_code, Error **errp)
{
    size_t length = strlen(operator_code);
    bool length_valid = length == GSM_SIM_OPERATOR_MIN_LENGTH ||
                        length == GSM_SIM_OPERATOR_MAX_LENGTH;

    if (!length_valid || !gsm_sim_is_decimal(operator_code, length)) {
        error_setg(errp,
                   "gsm-sim: operator must contain MCC+MNC as 5 or 6 decimal digits");
        return false;
    }
    return true;
}

static bool gsm_sim_validate_imsi(const char *imsi, const char *operator_code,
                                  Error **errp)
{
    if (!gsm_sim_is_decimal(imsi, GSM_SIM_IMSI_DIGIT_COUNT)) {
        error_setg(errp, "gsm-sim: imsi must contain exactly 15 decimal digits");
        return false;
    }
    if (strncmp(imsi, operator_code, strlen(operator_code)) != 0) {
        error_setg(errp, "gsm-sim: imsi must start with the operator");
        return false;
    }
    return true;
}

static void gsm_sim_write_identity(GSMSim *sim)
{
    GSMSimFile *imsi = gsm_sim_fs_find(&sim->fs, GSM_SIM_EF_IMSI,
                                       GSM_SIM_DF_GSM);
    GSMSimFile *ad = gsm_sim_fs_find(&sim->fs, GSM_SIM_EF_AD, GSM_SIM_DF_GSM);
    size_t operator_length = strlen(sim->operator_code);

    gsm_sim_encode_imsi(sim->imsi, imsi->data);
    ad->data[GSM_SIM_AD_MNC_LENGTH_OFFSET] =
        operator_length - GSM_SIM_MCC_LENGTH;
    if (operator_length == GSM_SIM_OPERATOR_MIN_LENGTH) {
        ad->size = GSM_SIM_AD_WITHOUT_MNC_LENGTH_SIZE;
    }
}

static void gsm_sim_free_sms_records(GSMSim *sim)
{
    GSMSimFile *sms = gsm_sim_fs_find(&sim->fs, GSM_SIM_EF_SMS,
                                      GSM_SIM_DF_TELECOM);

    for (size_t offset = 0; offset < sms->size;
         offset += GSM_SIM_SMS_RECORD_SIZE) {
        sms->data[offset] = GSM_SIM_SMS_STATUS_FREE;
    }
}

static void gsm_sim_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    GSMSim *sim = GSM_SIM(dev);

    if (!sim->operator_code) {
        sim->operator_code = g_strdup(GSM_SIM_DEFAULT_OPERATOR);
    }
    if (!gsm_sim_validate_operator(sim->operator_code, errp)) {
        return;
    }
    if (!sim->imsi) {
        size_t subscriber_length = GSM_SIM_IMSI_DIGIT_COUNT -
                                   strlen(sim->operator_code);
        const char *subscriber = GSM_SIM_DEFAULT_SUBSCRIBER +
                                 strlen(GSM_SIM_DEFAULT_SUBSCRIBER) -
                                 subscriber_length;

        sim->imsi = g_strconcat(sim->operator_code, subscriber, NULL);
    }
    if (!gsm_sim_validate_imsi(sim->imsi, sim->operator_code, errp)) {
        return;
    }

    GSM_SIM_GET_CLASS(dev)->parent_realize(dev, errp);
    if (*errp) {
        return;
    }

    gsm_sim_fs_init(&sim->fs, GSM_SIM_FILES, ARRAY_SIZE(GSM_SIM_FILES));
    gsm_sim_write_identity(sim);
    gsm_sim_free_sms_records(sim);
}

static void gsm_sim_unrealize(DeviceState *dev)
{
    GSM_SIM_GET_CLASS(dev)->parent_unrealize(dev);
    gsm_sim_fs_destroy(&GSM_SIM(dev)->fs);
}

static const Property gsm_sim_properties[] = {
    DEFINE_PROP_STRING("imsi", GSMSim, imsi),
    DEFINE_PROP_STRING("operator", GSMSim, operator_code),
};

static void gsm_sim_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ISO7816CardClass *cc = ISO7816_CARD_CLASS(klass);
    GSMSimClass *sc = GSM_SIM_CLASS(klass);

    device_class_set_parent_realize(dc, gsm_sim_realize, &sc->parent_realize);
    device_class_set_parent_unrealize(dc, gsm_sim_unrealize,
                                      &sc->parent_unrealize);
    device_class_set_props(dc, gsm_sim_properties);
    cc->get_atr = gsm_sim_get_atr;
    cc->cold_reset = gsm_sim_cold_reset;
    cc->command_has_data = gsm_sim_command_has_data;
    cc->exchange = gsm_sim_exchange;
    cc->processing_clocks = gsm_sim_processing_clocks;
    cc->atr_delay_clocks = GSM_SIM_ATR_DELAY_CLOCKS;
}

static const TypeInfo gsm_sim_info = {
    .name = TYPE_GSM_SIM,
    .parent = TYPE_ISO7816_CARD,
    .instance_size = sizeof(GSMSim),
    .class_size = sizeof(GSMSimClass),
    .class_init = gsm_sim_class_init,
};

static void gsm_sim_register_types(void)
{
    type_register_static(&gsm_sim_info);
}

type_init(gsm_sim_register_types)
