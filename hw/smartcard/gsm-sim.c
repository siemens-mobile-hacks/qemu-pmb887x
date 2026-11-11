/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"

#include "hw/smartcard/gsm11.h"
#include "hw/smartcard/iso7816.h"
#include "gsm-sim-fs.h"
#include "gsm-sim-storage.h"
#include "gsm-sim-toolkit.h"
#include "gsm-test-algorithm.h"

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
#define GSM_SIM_DIALING_FOOTER_SIZE 14
#define GSM_SIM_DIALING_NUMBER_EMPTY 0xFF
#define GSM_SIM_TON_INTERNATIONAL_MASK 0x70
#define GSM_SIM_TON_INTERNATIONAL 0x10
#define GSM_SIM_BCD_FILLER 0x0F

#define GSM_SIM_ACCESS_READ_ALWAYS_UPDATE_CHV1 { 0x01, 0xF0, 0x44 }
#define GSM_SIM_ACCESS_READ_ALWAYS_UPDATE_ADM { 0x04, 0xF0, 0x44 }
#define GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1 { 0x11, 0xF0, 0x44 }
#define GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV2 { 0x12, 0xF0, 0x44 }
#define GSM_SIM_ACCESS_READ_CHV1_UPDATE_ADM { 0x14, 0xF0, 0x44 }
#define GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV2_INCREASE_CHV1 { 0x12, 0x10, 0x44 }
#define GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1_INVALIDATE_CHV2 { 0x11, 0xF0, 0x22 }
#define GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1_REHABILITATE_CHV1 { 0x11, 0xF0, 0x14 }
#define GSM_SIM_ACCESS_READ_CHV1_UPDATE_ADM_REHABILITATE_CHV1 { 0x14, 0xF0, 0x14 }

#define GSM_SIM_DEFAULT_OPERATOR "00101"
#define GSM_SIM_DEFAULT_SUBSCRIBER "0123456789"
#define GSM_SIM_DEFAULT_KI "000102030405060708090A0B0C0D0E0F"
#define GSM_SIM_DEFAULT_PIN "0000"
#define GSM_SIM_DEFAULT_PUK "12345678"
#define GSM_SIM_PIN_MIN_LENGTH 4
#define GSM_SIM_CHV_PADDING 0xFF

/* Processing times of a reference SIM card; they do not depend on the SIM clock. */
#define GSM_SIM_ATR_DELAY_US 3680
#define GSM_SIM_SELECT_US 3138
#define GSM_SIM_STATUS_US 3770
#define GSM_SIM_READ_BINARY_US 295
#define GSM_SIM_READ_RECORD_US 313
#define GSM_SIM_GET_RESPONSE_US 91
#define GSM_SIM_UPDATE_BINARY_US 929
#define GSM_SIM_UPDATE_RECORD_US 3219
#define GSM_SIM_SEEK_US 925
#define GSM_SIM_INCREASE_US 1851
#define GSM_SIM_RUN_GSM_ALGORITHM_US 52815
#define GSM_SIM_TERMINAL_PROFILE_US 2541
#define GSM_SIM_FETCH_US 152
#define GSM_SIM_FETCH_NS_PER_BYTE 5640
#define GSM_SIM_TERMINAL_RESPONSE_US 83
#define GSM_SIM_ENVELOPE_US 488

struct GSMSim {
    ISO7816Card parent_obj;

    char *imsi;
    char *operator_code;
    char *ki_text;
    char *pin1;
    char *pin2;
    char *puk1;
    char *puk2;
    bool pin1_enabled;
    char *storage;

    uint8_t ki[GSM_TEST_ALGORITHM_KI_SIZE];
    GSMSimFS fs;
    GSMSimToolkit toolkit;
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
 * MSISDN, EXT1, EXT2, SMSP, LND, CBMI, menu selection, proactive SIM, GPRS.
 */
static const uint8_t GSM_SIM_SST[] = {
    0xFF, 0x3F, 0xFF, 0x0F, 0x00, 0x00, 0x30, 0x03, 0x00, 0x0C
};
static const uint8_t GSM_SIM_ACC[] = { 0x00, 0x01 };
static const uint8_t GSM_SIM_LOCI[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x01
};
static const uint8_t GSM_SIM_LOCIGPRS[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0x01
};
static const uint8_t GSM_SIM_PHASE[] = { 0x03 };
static const uint8_t GSM_SIM_MSISDN[GSM_SIM_DIALING_RECORD_SIZE] = {
    'V', 'i', 'k', 't', 'o', 'r', '8', '9',
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    GSM_SIM_MSISDN_NUMBER_LENGTH, GSM_SIM_MSISDN_TON_NPI,
    0x99, 0x09, 0x00, 0x00, 0x00, 0x98,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

/* GSM 11.11 10.5.6: SMSC "+1234" as the only parameter. */
static const uint8_t GSM_SIM_SMSP[GSM_SIM_SMSP_RECORD_SIZE] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFD,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x03, 0x91, 0x21, 0x43, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF
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
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_ADM_REHABILITATE_CHV1,
      .generated = true },
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
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1_REHABILITATE_CHV1 },
    { .id = GSM_SIM_EF_AD, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = GSM_SIM_AD_SIZE,
      .access = GSM_SIM_ACCESS_READ_ALWAYS_UPDATE_ADM, .generated = true },
    { .id = GSM_SIM_EF_PHASE, .parent_id = GSM_SIM_DF_GSM,
      .structure = GSM_SIM_FILE_TRANSPARENT,
      .size = sizeof(GSM_SIM_PHASE), .content = GSM_SIM_PHASE,
      .access = GSM_SIM_ACCESS_READ_ALWAYS_UPDATE_ADM },

    { .id = GSM_SIM_EF_ADN, .parent_id = GSM_SIM_DF_TELECOM,
      .structure = GSM_SIM_FILE_LINEAR_FIXED,
      .size = 250 * GSM_SIM_DIALING_RECORD_SIZE,
      .record_size = GSM_SIM_DIALING_RECORD_SIZE, .fill = 0xFF,
      .access = GSM_SIM_ACCESS_READ_CHV1_UPDATE_CHV1_INVALIDATE_CHV2 },
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
      .size = sizeof(GSM_SIM_SMSP),
      .record_size = GSM_SIM_SMSP_RECORD_SIZE, .content = GSM_SIM_SMSP,
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
    GSMSim *sim = GSM_SIM(card);
    GSMSimFS *fs = &sim->fs;
    GSMSimFile *adn = gsm_sim_fs_find(fs, GSM_SIM_EF_ADN, GSM_SIM_DF_TELECOM);
    bool fdn_enabled = adn->invalidated;

    gsm_sim_toolkit_reset(&sim->toolkit);
    gsm_sim_fs_reset(fs);
    fs->chv1.fulfilled = false;
    fs->chv2.fulfilled = false;
    if (fdn_enabled) {
        GSMSimFile *imsi = gsm_sim_fs_find(fs, GSM_SIM_EF_IMSI, GSM_SIM_DF_GSM);
        GSMSimFile *loci = gsm_sim_fs_find(fs, GSM_SIM_EF_LOCI, GSM_SIM_DF_GSM);

        imsi->invalidated = true;
        loci->invalidated = true;
    }
}

static bool gsm_sim_command_has_data(ISO7816Card *card, const uint8_t *header)
{
    return gsm11_command_has_data(header[ISO7816_OFFSET_INS]);
}

static int64_t gsm_sim_processing_us(const uint8_t *header)
{
    switch (header[ISO7816_OFFSET_INS]) {
    case GSM11_INS_SELECT:
        return GSM_SIM_SELECT_US;
    case GSM11_INS_STATUS:
        return GSM_SIM_STATUS_US;
    case GSM11_INS_READ_BINARY:
        return GSM_SIM_READ_BINARY_US;
    case GSM11_INS_READ_RECORD:
        return GSM_SIM_READ_RECORD_US;
    case GSM11_INS_GET_RESPONSE:
        return GSM_SIM_GET_RESPONSE_US;
    case GSM11_INS_UPDATE_BINARY:
        return GSM_SIM_UPDATE_BINARY_US;
    case GSM11_INS_UPDATE_RECORD:
        return GSM_SIM_UPDATE_RECORD_US;
    case GSM11_INS_SEEK:
        return GSM_SIM_SEEK_US;
    case GSM11_INS_INCREASE:
        return GSM_SIM_INCREASE_US;
    case GSM11_INS_RUN_GSM_ALGORITHM:
        return GSM_SIM_RUN_GSM_ALGORITHM_US;
    case GSM11_INS_TERMINAL_PROFILE:
        return GSM_SIM_TERMINAL_PROFILE_US;
    case GSM11_INS_FETCH:
        return GSM_SIM_FETCH_US;
    case GSM11_INS_TERMINAL_RESPONSE:
        return GSM_SIM_TERMINAL_RESPONSE_US;
    case GSM11_INS_ENVELOPE:
        return GSM_SIM_ENVELOPE_US;
    default:
        return 0;
    }
}

static int64_t gsm_sim_processing_ns(ISO7816Card *card, const uint8_t *header)
{
    int64_t processing_ns = gsm_sim_processing_us(header) * SCALE_US;

    if (header[ISO7816_OFFSET_INS] == GSM11_INS_FETCH) {
        processing_ns += header[ISO7816_OFFSET_P3] * GSM_SIM_FETCH_NS_PER_BYTE;
    }
    return processing_ns;
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

static void gsm_sim_run_gsm_algorithm(GSMSim *sim, const uint8_t *command,
                                      ISO7816Response *response)
{
    uint8_t sres_kc[GSM_TEST_ALGORITHM_SRES_SIZE + GSM_TEST_ALGORITHM_KC_SIZE];

    if (command[ISO7816_OFFSET_P3] != GSM_TEST_ALGORITHM_RAND_SIZE) {
        iso7816_response_set_status(response,
                                    GSM11_STATUS_WRONG_LENGTH |
                                    GSM_TEST_ALGORITHM_RAND_SIZE);
        return;
    }
    if (sim->fs.current_directory->definition->id != GSM_SIM_DF_GSM) {
        iso7816_response_set_status(response, GSM11_STATUS_FILE_INCONSISTENT);
        return;
    }
    if (!gsm_sim_fs_is_chv1_granted(&sim->fs)) {
        iso7816_response_set_status(response, GSM11_STATUS_ACCESS_DENIED);
        return;
    }

    gsm_test_algorithm_run(sim->ki, command + ISO7816_HEADER_SIZE, sres_kc,
                           sres_kc + GSM_TEST_ALGORITHM_SRES_SIZE);
    gsm_sim_fs_offer_response(&sim->fs, sres_kc, sizeof(sres_kc), response);
}

static void gsm_sim_append_bcd(GString *text, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < 2 * size; i++) {
        uint8_t digit = i % 2 ? data[i / 2] >> GSM_SIM_BCD_HIGH_NIBBLE_SHIFT
                              : data[i / 2] & GSM_SIM_BCD_FILLER;

        if (digit == GSM_SIM_BCD_FILLER) {
            break;
        }
        g_string_append_c(text, '0' + digit);
    }
}

static void gsm_sim_append_msisdn(GSMSim *sim, GString *text)
{
    GSMSimFile *msisdn = gsm_sim_fs_find(&sim->fs, GSM_SIM_EF_MSISDN,
                                         GSM_SIM_DF_TELECOM);
    const uint8_t *footer = msisdn->data + GSM_SIM_DIALING_RECORD_SIZE -
                            GSM_SIM_DIALING_FOOTER_SIZE;
    uint8_t length = footer[0];
    bool international = (footer[1] & GSM_SIM_TON_INTERNATIONAL_MASK) ==
                         GSM_SIM_TON_INTERNATIONAL;

    if (length == GSM_SIM_DIALING_NUMBER_EMPTY || length < 2) {
        g_string_append(text, "none");
        return;
    }
    if (international) {
        g_string_append_c(text, '+');
    }
    gsm_sim_append_bcd(text, footer + 2, length - 1);
}

static char *gsm_sim_describe(GSMSim *sim)
{
    GString *text = g_string_new(NULL);
    GSMSimFile *iccid = gsm_sim_fs_find(&sim->fs, GSM_SIM_EF_ICCID,
                                        GSM_SIM_FS_MF_ID);

    g_string_append_printf(text, "MCC-MNC: %.*s-%s\nIMSI: %s\nICCID: ",
                           GSM_SIM_MCC_LENGTH, sim->operator_code,
                           sim->operator_code + GSM_SIM_MCC_LENGTH, sim->imsi);
    gsm_sim_append_bcd(text, iccid->data, iccid->size);
    g_string_append(text, "\nMSISDN: ");
    gsm_sim_append_msisdn(sim, text);
    g_string_append_printf(text, "\nPIN1: %s",
                           sim->fs.chv1_enabled ? "enabled" : "disabled");
    return g_string_free(text, false);
}

static void gsm_sim_envelope(GSMSim *sim, const uint8_t *data, uint16_t size,
                             ISO7816Response *response)
{
    g_autofree char *info = NULL;

    if (gsm_sim_toolkit_envelope(data, size, response) !=
        GSM_SIM_TOOLKIT_INFO_ITEM) {
        return;
    }
    info = gsm_sim_describe(sim);
    gsm_sim_toolkit_display_text(&sim->toolkit, info);
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
    case GSM11_INS_INVALIDATE:
        gsm_sim_fs_invalidate(fs, response);
        break;
    case GSM11_INS_REHABILITATE:
        gsm_sim_fs_rehabilitate(fs, response);
        break;
    case GSM11_INS_RUN_GSM_ALGORITHM:
        gsm_sim_run_gsm_algorithm(sim, command, response);
        break;
    case GSM11_INS_VERIFY_CHV:
        gsm_sim_fs_verify_chv(fs, p2, data, size, response);
        break;
    case GSM11_INS_CHANGE_CHV:
        gsm_sim_fs_change_chv(fs, p2, data, size, response);
        break;
    case GSM11_INS_DISABLE_CHV:
        gsm_sim_fs_disable_chv(fs, p2, data, size, response);
        break;
    case GSM11_INS_ENABLE_CHV:
        gsm_sim_fs_enable_chv(fs, p2, data, size, response);
        break;
    case GSM11_INS_UNBLOCK_CHV:
        gsm_sim_fs_unblock_chv(fs, p2, data, size, response);
        break;
    case GSM11_INS_TERMINAL_PROFILE:
        gsm_sim_toolkit_terminal_profile(&sim->toolkit, response);
        break;
    case GSM11_INS_FETCH:
        gsm_sim_toolkit_fetch(&sim->toolkit, p3, response);
        break;
    case GSM11_INS_TERMINAL_RESPONSE:
        iso7816_response_set_status(response, GSM11_STATUS_SUCCESS);
        break;
    case GSM11_INS_ENVELOPE:
        gsm_sim_envelope(sim, data, size, response);
        break;
    default:
        iso7816_response_set_status(response, GSM11_STATUS_INS_NOT_SUPPORTED);
        break;
    }
}

static bool gsm_sim_changes_state(uint8_t instruction)
{
    switch (instruction) {
    case GSM11_INS_UPDATE_BINARY:
    case GSM11_INS_UPDATE_RECORD:
    case GSM11_INS_INCREASE:
    case GSM11_INS_INVALIDATE:
    case GSM11_INS_REHABILITATE:
    case GSM11_INS_VERIFY_CHV:
    case GSM11_INS_CHANGE_CHV:
    case GSM11_INS_DISABLE_CHV:
    case GSM11_INS_ENABLE_CHV:
    case GSM11_INS_UNBLOCK_CHV:
        return true;
    default:
        return false;
    }
}

static bool gsm_sim_write_storage(GSMSim *sim, Error **errp)
{
    g_autoptr(GString) json = gsm_sim_storage_save(&sim->fs);
    g_autoptr(GError) error = NULL;

    if (!g_file_set_contents(sim->storage, json->str, json->len, &error)) {
        error_setg(errp, "gsm-sim: cannot write storage %s: %s", sim->storage,
                   error->message);
        return false;
    }
    return true;
}

static void gsm_sim_commit_storage(GSMSim *sim)
{
    Error *err = NULL;

    if (!gsm_sim_write_storage(sim, &err)) {
        error_report_err(err);
    }
}

static void gsm_sim_exchange(ISO7816Card *card, const uint8_t *command,
                             size_t size, uint32_t exchange_id)
{
    GSMSim *sim = GSM_SIM(card);
    ISO7816Response response = { 0 };

    if (command[ISO7816_OFFSET_CLA] != GSM11_CLASS) {
        iso7816_response_set_status(&response,
                                    GSM11_STATUS_CLASS_NOT_SUPPORTED);
    } else {
        gsm_sim_execute(sim, command, &response);
    }
    if (sim->storage && gsm_sim_changes_state(command[ISO7816_OFFSET_INS])) {
        gsm_sim_commit_storage(sim);
    }
    gsm_sim_toolkit_announce(&sim->toolkit, &response);
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

static bool gsm_sim_parse_ki(GSMSim *sim, Error **errp)
{
    const char *text = sim->ki_text ? sim->ki_text : GSM_SIM_DEFAULT_KI;
    size_t digits = 2 * GSM_TEST_ALGORITHM_KI_SIZE;

    if (strlen(text) != digits ||
        strspn(text, "0123456789abcdefABCDEF") != digits) {
        error_setg(errp, "gsm-sim: ki must contain exactly 32 hex digits");
        return false;
    }
    for (size_t i = 0; i < GSM_TEST_ALGORITHM_KI_SIZE; i++) {
        sim->ki[i] = (g_ascii_xdigit_value(text[2 * i]) << 4) |
                     g_ascii_xdigit_value(text[2 * i + 1]);
    }
    return true;
}

static bool gsm_sim_encode_chv(const char *name, const char *text,
                               size_t min_length, uint8_t *encoded,
                               Error **errp)
{
    size_t length = strlen(text);

    if (length < min_length || length > GSM_SIM_FS_CHV_SIZE ||
        !gsm_sim_is_decimal(text, length)) {
        error_setg(errp, "gsm-sim: %s must contain %zu to %d decimal digits",
                   name, min_length, GSM_SIM_FS_CHV_SIZE);
        return false;
    }
    memset(encoded, GSM_SIM_CHV_PADDING, GSM_SIM_FS_CHV_SIZE);
    memcpy(encoded, text, length);
    return true;
}

static bool gsm_sim_configure_chv(GSMSimCHV *chv, const char *pin_name,
                                  const char *pin, const char *puk_name,
                                  const char *puk, Error **errp)
{
    if (!gsm_sim_encode_chv(pin_name, pin ? pin : GSM_SIM_DEFAULT_PIN,
                            GSM_SIM_PIN_MIN_LENGTH, chv->value, errp)) {
        return false;
    }
    if (!gsm_sim_encode_chv(puk_name, puk ? puk : GSM_SIM_DEFAULT_PUK,
                            GSM_SIM_FS_CHV_SIZE, chv->unblock_value, errp)) {
        return false;
    }
    chv->attempts = GSM_SIM_FS_CHV_ATTEMPTS;
    chv->unblock_attempts = GSM_SIM_FS_UNBLOCK_CHV_ATTEMPTS;
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

static bool gsm_sim_open_storage(GSMSim *sim, Error **errp)
{
    g_autofree char *json = NULL;
    g_autoptr(GError) error = NULL;

    if (!g_file_test(sim->storage, G_FILE_TEST_EXISTS)) {
        return gsm_sim_write_storage(sim, errp);
    }
    if (!g_file_get_contents(sim->storage, &json, NULL, &error)) {
        error_setg(errp, "gsm-sim: cannot read storage %s: %s", sim->storage,
                   error->message);
        return false;
    }
    return gsm_sim_storage_load(&sim->fs, json, errp);
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
    if (!gsm_sim_parse_ki(sim, errp)) {
        return;
    }
    if (!gsm_sim_configure_chv(&sim->fs.chv1, "pin1", sim->pin1, "puk1",
                               sim->puk1, errp)) {
        return;
    }
    if (!gsm_sim_configure_chv(&sim->fs.chv2, "pin2", sim->pin2, "puk2",
                               sim->puk2, errp)) {
        return;
    }
    sim->fs.chv1_enabled = sim->pin1_enabled;

    GSM_SIM_GET_CLASS(dev)->parent_realize(dev, errp);
    if (*errp) {
        return;
    }

    gsm_sim_fs_init(&sim->fs, GSM_SIM_FILES, ARRAY_SIZE(GSM_SIM_FILES));
    gsm_sim_write_identity(sim);
    gsm_sim_free_sms_records(sim);
    if (sim->storage && !gsm_sim_open_storage(sim, errp)) {
        GSM_SIM_GET_CLASS(dev)->parent_unrealize(dev);
        gsm_sim_fs_destroy(&sim->fs);
    }
}

static void gsm_sim_unrealize(DeviceState *dev)
{
    GSM_SIM_GET_CLASS(dev)->parent_unrealize(dev);
    gsm_sim_fs_destroy(&GSM_SIM(dev)->fs);
}

static const Property gsm_sim_properties[] = {
    DEFINE_PROP_STRING("imsi", GSMSim, imsi),
    DEFINE_PROP_STRING("operator", GSMSim, operator_code),
    DEFINE_PROP_STRING("ki", GSMSim, ki_text),
    DEFINE_PROP_STRING("pin1", GSMSim, pin1),
    DEFINE_PROP_STRING("pin2", GSMSim, pin2),
    DEFINE_PROP_STRING("puk1", GSMSim, puk1),
    DEFINE_PROP_STRING("puk2", GSMSim, puk2),
    DEFINE_PROP_BOOL("pin1-enabled", GSMSim, pin1_enabled, false),
    DEFINE_PROP_STRING("storage", GSMSim, storage),
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
    cc->processing_ns = gsm_sim_processing_ns;
    cc->atr_delay_ns = GSM_SIM_ATR_DELAY_US * SCALE_US;
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
