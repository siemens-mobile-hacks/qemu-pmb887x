#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"

#include "hw/arm/pmb887x/sim/apdu.h"
#include "hw/arm/pmb887x/sim/sim_fs.h"

OBJECT_DECLARE_SIMPLE_TYPE(pmb887x_gsm_sim_t, PMB887X_GSM_SIM)

#define GSM_SIM_CLASS 0xA0
#define GSM_SIM_SELECT_DATA_SIZE 2
#define GSM_SIM_IMSI_DIGIT_COUNT 15
#define GSM_SIM_IMSI_BODY_SIZE 8
#define GSM_SIM_IMSI_DATA_SIZE (1 + GSM_SIM_IMSI_BODY_SIZE)
#define GSM_SIM_IMSI_IDENTITY_NIBBLE 0x09
#define GSM_SIM_BCD_HIGH_NIBBLE_SHIFT 4
#define GSM_SIM_MCC_LENGTH 3
#define GSM_SIM_OPERATOR_MIN_LENGTH (GSM_SIM_MCC_LENGTH + 2)
#define GSM_SIM_OPERATOR_MAX_LENGTH (GSM_SIM_MCC_LENGTH + 3)
#define GSM_SIM_AD_SIZE 4
#define GSM_SIM_AD_MNC_LENGTH_OFFSET 3
#define GSM_SIM_SST_SIZE 5
#define GSM_SIM_SST_MSISDN_BYTE 2
#define GSM_SIM_SERVICE_ALLOCATED_AND_ACTIVATED 0x03
#define GSM_SIM_PLMNSEL_SIZE 24
#define GSM_SIM_CBMI_SIZE 20
#define GSM_SIM_BCCH_SIZE 16
#define GSM_SIM_FPLMN_SIZE 12
#define GSM_SIM_SPN_SIZE 17
#define GSM_SIM_MSISDN_ALPHA_SIZE 18
#define GSM_SIM_MSISDN_DIALING_NUMBER_SIZE 14
#define GSM_SIM_MSISDN_RECORD_SIZE (GSM_SIM_MSISDN_ALPHA_SIZE + GSM_SIM_MSISDN_DIALING_NUMBER_SIZE)
#define GSM_SIM_MSISDN_BCD_SIZE 6
#define GSM_SIM_MSISDN_NUMBER_LENGTH (1 + GSM_SIM_MSISDN_BCD_SIZE)
#define GSM_SIM_MSISDN_TON_NPI 0x91

#define GSM_SIM_DEFAULT_OPERATOR "00101"
#define GSM_SIM_DEFAULT_SUBSCRIBER "0123456789"

struct pmb887x_gsm_sim_t {
	pmb887x_apdu_backend_t parent_obj;
	pmb887x_sim_fs_t fs;
	char *imsi;
	char *operator_code;
	pmb887x_sim_fs_file_definition_t *file_definitions;
	uint8_t imsi_data[GSM_SIM_IMSI_DATA_SIZE];
	uint8_t ad_data[GSM_SIM_AD_SIZE];
};

enum gsm_sim_file_id_t {
	GSM_SIM_FILE_MF = PMB887X_SIM_FS_MF_ID,
	GSM_SIM_FILE_EF_ICCID = 0x2FE2,
	GSM_SIM_FILE_DF_TELECOM = 0x7F10,
	GSM_SIM_FILE_DF_GSM = 0x7F20,
	GSM_SIM_FILE_EF_LP = 0x6F05,
	GSM_SIM_FILE_EF_IMSI = 0x6F07,
	GSM_SIM_FILE_EF_KC = 0x6F20,
	GSM_SIM_FILE_EF_PLMNSEL = 0x6F30,
	GSM_SIM_FILE_EF_HPLMN = 0x6F31,
	GSM_SIM_FILE_EF_ACMAX = 0x6F37,
	GSM_SIM_FILE_EF_SST = 0x6F38,
	GSM_SIM_FILE_EF_CBMI = 0x6F45,
	GSM_SIM_FILE_EF_SPN = 0x6F46,
	GSM_SIM_FILE_EF_BCCH = 0x6F74,
	GSM_SIM_FILE_EF_ACC = 0x6F78,
	GSM_SIM_FILE_EF_FPLMN = 0x6F7B,
	GSM_SIM_FILE_EF_LOCI = 0x6F7E,
	GSM_SIM_FILE_EF_AD = 0x6FAD,
	GSM_SIM_FILE_EF_PHASE = 0x6FAE,
	GSM_SIM_FILE_EF_MSISDN = 0x6F40,
};

static const uint8_t GSM_SIM_ATR[] = { 0x3B, 0x00 };
static const uint8_t GSM_SIM_ICCID[] = { 0x98, 0x88, 0x12, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20 };
static const uint8_t GSM_SIM_LP[] = { 0xFF };
static const uint8_t GSM_SIM_KC[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07 };
static const uint8_t GSM_SIM_HPLMN[] = { 0xFF };
static const uint8_t GSM_SIM_ACMAX[] = { 0x00, 0x00, 0x00 };
static const uint8_t GSM_SIM_SST[GSM_SIM_SST_SIZE] = {
	[GSM_SIM_SST_MSISDN_BYTE] = GSM_SIM_SERVICE_ALLOCATED_AND_ACTIVATED,
};
static const uint8_t GSM_SIM_ACC[] = { 0x00, 0x00 };
static const uint8_t GSM_SIM_LOCI[] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00
};
static const uint8_t GSM_SIM_PHASE[] = { 0x02 };
static const uint8_t GSM_SIM_SPN[GSM_SIM_SPN_SIZE] = {
	0x00, 'V', 'i', 'k', 't', 'o', 'r', '8', '9', 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
static const uint8_t GSM_SIM_MSISDN[GSM_SIM_MSISDN_RECORD_SIZE] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	GSM_SIM_MSISDN_NUMBER_LENGTH, GSM_SIM_MSISDN_TON_NPI, 0x99, 0x09, 0x00, 0x00, 0x00, 0x98,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static const pmb887x_sim_fs_file_definition_t GSM_SIM_FILES[] = {
	{ GSM_SIM_FILE_MF, 0, PMB887X_SIM_FS_MF, NULL, 0, 0x00, 0 },
	{ GSM_SIM_FILE_EF_ICCID, GSM_SIM_FILE_MF, PMB887X_SIM_FS_EF_TRANSPARENT, GSM_SIM_ICCID, sizeof(GSM_SIM_ICCID), 0xFF, 0 },
	{ GSM_SIM_FILE_DF_GSM, GSM_SIM_FILE_MF, PMB887X_SIM_FS_DF, NULL, 0, 0x00, 0 },
	{ GSM_SIM_FILE_DF_TELECOM, GSM_SIM_FILE_MF, PMB887X_SIM_FS_DF, NULL, 0, 0x00, 0 },

	{ GSM_SIM_FILE_EF_LP, GSM_SIM_FILE_DF_GSM, PMB887X_SIM_FS_EF_TRANSPARENT, GSM_SIM_LP, sizeof(GSM_SIM_LP), 0xFF, 0 },
	{ GSM_SIM_FILE_EF_IMSI, GSM_SIM_FILE_DF_GSM, PMB887X_SIM_FS_EF_TRANSPARENT, NULL, GSM_SIM_IMSI_DATA_SIZE, 0xFF, 0 },
	{ GSM_SIM_FILE_EF_KC, GSM_SIM_FILE_DF_GSM, PMB887X_SIM_FS_EF_TRANSPARENT, GSM_SIM_KC, sizeof(GSM_SIM_KC), 0xFF, 0 },
	{ GSM_SIM_FILE_EF_PLMNSEL, GSM_SIM_FILE_DF_GSM, PMB887X_SIM_FS_EF_TRANSPARENT, NULL, GSM_SIM_PLMNSEL_SIZE, 0xFF, 0 },
	{ GSM_SIM_FILE_EF_HPLMN, GSM_SIM_FILE_DF_GSM, PMB887X_SIM_FS_EF_TRANSPARENT, GSM_SIM_HPLMN, sizeof(GSM_SIM_HPLMN), 0xFF, 0 },
	{ GSM_SIM_FILE_EF_ACMAX, GSM_SIM_FILE_DF_GSM, PMB887X_SIM_FS_EF_TRANSPARENT, GSM_SIM_ACMAX, sizeof(GSM_SIM_ACMAX), 0x00, 0 },
	{ GSM_SIM_FILE_EF_SST, GSM_SIM_FILE_DF_GSM, PMB887X_SIM_FS_EF_TRANSPARENT, GSM_SIM_SST, sizeof(GSM_SIM_SST), 0x00, 0 },
	{ GSM_SIM_FILE_EF_CBMI, GSM_SIM_FILE_DF_GSM, PMB887X_SIM_FS_EF_TRANSPARENT, NULL, GSM_SIM_CBMI_SIZE, 0xFF, 0 },
	{ GSM_SIM_FILE_EF_SPN, GSM_SIM_FILE_DF_GSM, PMB887X_SIM_FS_EF_TRANSPARENT, GSM_SIM_SPN, sizeof(GSM_SIM_SPN), 0xFF, 0 },
	{ GSM_SIM_FILE_EF_BCCH, GSM_SIM_FILE_DF_GSM, PMB887X_SIM_FS_EF_TRANSPARENT, NULL, GSM_SIM_BCCH_SIZE, 0xFF, 0 },
	{ GSM_SIM_FILE_EF_ACC, GSM_SIM_FILE_DF_GSM, PMB887X_SIM_FS_EF_TRANSPARENT, GSM_SIM_ACC, sizeof(GSM_SIM_ACC), 0x00, 0 },
	{ GSM_SIM_FILE_EF_FPLMN, GSM_SIM_FILE_DF_GSM, PMB887X_SIM_FS_EF_TRANSPARENT, NULL, GSM_SIM_FPLMN_SIZE, 0xFF, 0 },
	{ GSM_SIM_FILE_EF_LOCI, GSM_SIM_FILE_DF_GSM, PMB887X_SIM_FS_EF_TRANSPARENT, GSM_SIM_LOCI, sizeof(GSM_SIM_LOCI), 0xFF, 0 },
	{ GSM_SIM_FILE_EF_AD, GSM_SIM_FILE_DF_GSM, PMB887X_SIM_FS_EF_TRANSPARENT, NULL, GSM_SIM_AD_SIZE, 0x00, 0 },
	{ GSM_SIM_FILE_EF_PHASE, GSM_SIM_FILE_DF_GSM, PMB887X_SIM_FS_EF_TRANSPARENT, GSM_SIM_PHASE, sizeof(GSM_SIM_PHASE), 0xFF, 0 },

	{ GSM_SIM_FILE_EF_MSISDN, GSM_SIM_FILE_DF_TELECOM, PMB887X_SIM_FS_EF_LINEAR_FIXED, GSM_SIM_MSISDN, sizeof(GSM_SIM_MSISDN), 0xFF, sizeof(GSM_SIM_MSISDN) },
};

static uint16_t gsm_sim_response_size(uint8_t length) {
	return length ? length : PMB887X_APDU_MAX_RESPONSE_SIZE;
}

static void gsm_sim_card_reset(pmb887x_apdu_backend_t *backend) {
	pmb887x_sim_fs_card_reset(&PMB887X_GSM_SIM(backend)->fs);
}

static const uint8_t *gsm_sim_get_atr(pmb887x_apdu_backend_t *backend, size_t *size) {
	*size = sizeof(GSM_SIM_ATR);
	return GSM_SIM_ATR;
}

static void gsm_sim_exchange(
	pmb887x_apdu_backend_t *backend,
	const uint8_t *command,
	size_t command_size,
	pmb887x_apdu_complete_fn complete,
	void *opaque
) {
	pmb887x_gsm_sim_t *sim = PMB887X_GSM_SIM(backend);
	pmb887x_apdu_response_t response = { 0 };
	if (command_size < PMB887X_APDU_HEADER_SIZE) {
		pmb887x_apdu_response_set_status(&response, PMB887X_APDU_STATUS_WRONG_LENGTH);
		goto done;
	}
	if (command[PMB887X_APDU_OFFSET_CLA] != GSM_SIM_CLASS) {
		pmb887x_apdu_response_set_status(&response, PMB887X_APDU_STATUS_CLASS_NOT_SUPPORTED);
		goto done;
	}

	switch (command[PMB887X_APDU_OFFSET_INS]) {
		case PMB887X_APDU_INS_SELECT: {
			if (command[PMB887X_APDU_OFFSET_P3] != GSM_SIM_SELECT_DATA_SIZE ||
				command_size != PMB887X_APDU_HEADER_SIZE + GSM_SIM_SELECT_DATA_SIZE) {
				pmb887x_apdu_response_set_status(&response, PMB887X_APDU_STATUS_WRONG_LENGTH);
				break;
			}
			uint16_t id = ((uint16_t) command[PMB887X_APDU_HEADER_SIZE] << 8) |
				command[PMB887X_APDU_HEADER_SIZE + 1];
			uint8_t response_size;
			if (pmb887x_sim_fs_select(&sim->fs, id, &response_size)) {
				pmb887x_apdu_response_set_status(
					&response,
					PMB887X_APDU_STATUS_RESPONSE_AVAILABLE | response_size
				);
			} else {
				pmb887x_apdu_response_set_status(&response, PMB887X_APDU_STATUS_FILE_NOT_FOUND);
			}
			break;
		}
		case PMB887X_APDU_INS_GET_RESPONSE:
			pmb887x_sim_fs_get_response(&sim->fs, command[PMB887X_APDU_OFFSET_P3], &response);
			break;
		case PMB887X_APDU_INS_STATUS:
			pmb887x_sim_fs_status(&sim->fs, command[PMB887X_APDU_OFFSET_P3], &response);
			break;
		case PMB887X_APDU_INS_READ_BINARY:
			pmb887x_sim_fs_read_binary(
				&sim->fs,
				((uint16_t) command[PMB887X_APDU_OFFSET_P1] << 8) | command[PMB887X_APDU_OFFSET_P2],
				gsm_sim_response_size(command[PMB887X_APDU_OFFSET_P3]),
				&response
			);
			break;
		case PMB887X_APDU_INS_UPDATE_BINARY:
			if (command_size != PMB887X_APDU_HEADER_SIZE + command[PMB887X_APDU_OFFSET_P3]) {
				pmb887x_apdu_response_set_status(&response, PMB887X_APDU_STATUS_WRONG_LENGTH);
				break;
			}
			pmb887x_sim_fs_update_binary(
				&sim->fs,
				((uint16_t) command[PMB887X_APDU_OFFSET_P1] << 8) | command[PMB887X_APDU_OFFSET_P2],
				command + PMB887X_APDU_HEADER_SIZE,
				command[PMB887X_APDU_OFFSET_P3],
				&response
			);
			break;
		case PMB887X_APDU_INS_READ_RECORD:
			pmb887x_sim_fs_read_record(
				&sim->fs,
				command[PMB887X_APDU_OFFSET_P1],
				command[PMB887X_APDU_OFFSET_P2],
				gsm_sim_response_size(command[PMB887X_APDU_OFFSET_P3]),
				&response
			);
			break;
		case PMB887X_APDU_INS_UPDATE_RECORD:
			if (command_size != PMB887X_APDU_HEADER_SIZE + command[PMB887X_APDU_OFFSET_P3]) {
				pmb887x_apdu_response_set_status(&response, PMB887X_APDU_STATUS_WRONG_LENGTH);
				break;
			}
			pmb887x_sim_fs_update_record(
				&sim->fs,
				command[PMB887X_APDU_OFFSET_P1],
				command[PMB887X_APDU_OFFSET_P2],
				command + PMB887X_APDU_HEADER_SIZE,
				command[PMB887X_APDU_OFFSET_P3],
				&response
			);
			break;
		default:
			pmb887x_apdu_response_set_status(&response, PMB887X_APDU_STATUS_INS_NOT_SUPPORTED);
			break;
	}

done:
	complete(opaque, &response);
}

static void gsm_sim_encode_imsi(pmb887x_gsm_sim_t *sim) {
	sim->imsi_data[0] = GSM_SIM_IMSI_BODY_SIZE;
	uint8_t *encoded_imsi = &sim->imsi_data[1];
	encoded_imsi[0] = (sim->imsi[0] - '0') << GSM_SIM_BCD_HIGH_NIBBLE_SHIFT | GSM_SIM_IMSI_IDENTITY_NIBBLE;
	for (size_t digit_index = 1, byte_index = 1; digit_index < GSM_SIM_IMSI_DIGIT_COUNT; digit_index += 2, byte_index++) {
		uint8_t low_digit = sim->imsi[digit_index] - '0';
		uint8_t high_digit = sim->imsi[digit_index + 1] - '0';
		encoded_imsi[byte_index] = low_digit | high_digit << GSM_SIM_BCD_HIGH_NIBBLE_SHIFT;
	}
}

static bool gsm_sim_prepare_identity(pmb887x_gsm_sim_t *sim, Error **errp) {
	if (!sim->operator_code)
		sim->operator_code = g_strdup(GSM_SIM_DEFAULT_OPERATOR);

	size_t operator_length = strlen(sim->operator_code);
	if ((operator_length != GSM_SIM_OPERATOR_MIN_LENGTH && operator_length != GSM_SIM_OPERATOR_MAX_LENGTH) ||
		strspn(sim->operator_code, "0123456789") != operator_length) {
		error_setg(errp, "gsm-sim-apdu: operator_code must contain MCC+MNC as 5 or 6 decimal digits");
		return false;
	}

	if (!sim->imsi) {
		size_t subscriber_length = GSM_SIM_IMSI_DIGIT_COUNT - operator_length;
		size_t default_subscriber_length = strlen(GSM_SIM_DEFAULT_SUBSCRIBER);
		sim->imsi = g_strdup_printf(
			"%s%s",
			sim->operator_code,
			GSM_SIM_DEFAULT_SUBSCRIBER + default_subscriber_length - subscriber_length
		);
	}

	size_t imsi_length = strlen(sim->imsi);
	if (imsi_length != GSM_SIM_IMSI_DIGIT_COUNT || strspn(sim->imsi, "0123456789") != imsi_length) {
		error_setg(errp, "gsm-sim-apdu: imsi must contain exactly 15 decimal digits");
		return false;
	}
	if (strncmp(sim->imsi, sim->operator_code, operator_length) != 0) {
		error_setg(errp, "gsm-sim-apdu: imsi must start with operator_code");
		return false;
	}

	gsm_sim_encode_imsi(sim);
	memset(sim->ad_data, 0, sizeof(sim->ad_data));
	sim->ad_data[GSM_SIM_AD_MNC_LENGTH_OFFSET] = operator_length - GSM_SIM_MCC_LENGTH;
	return true;
}

static void gsm_sim_prepare_file_definitions(pmb887x_gsm_sim_t *sim) {
	sim->file_definitions = g_memdup2(GSM_SIM_FILES, sizeof(GSM_SIM_FILES));
	for (size_t i = 0; i < ARRAY_SIZE(GSM_SIM_FILES); i++) {
		if (sim->file_definitions[i].id == GSM_SIM_FILE_EF_IMSI) {
			sim->file_definitions[i].initial_data = sim->imsi_data;
		} else if (sim->file_definitions[i].id == GSM_SIM_FILE_EF_AD) {
			sim->file_definitions[i].initial_data = sim->ad_data;
		}
	}
}

static void gsm_sim_realize(DeviceState *dev, Error **errp) {
	pmb887x_gsm_sim_t *sim = PMB887X_GSM_SIM(dev);
	if (!gsm_sim_prepare_identity(sim, errp))
		return;
	gsm_sim_prepare_file_definitions(sim);
	pmb887x_sim_fs_init(&sim->fs, sim->file_definitions, ARRAY_SIZE(GSM_SIM_FILES));
}

static void gsm_sim_reset(DeviceState *dev) {
	gsm_sim_card_reset(PMB887X_APDU_BACKEND(dev));
}

static void gsm_sim_finalize(Object *obj) {
	pmb887x_gsm_sim_t *sim = PMB887X_GSM_SIM(obj);
	pmb887x_sim_fs_destroy(&sim->fs);
	g_free(sim->file_definitions);
}

static const Property gsm_sim_properties[] = {
	DEFINE_PROP_STRING("imsi", pmb887x_gsm_sim_t, imsi),
	DEFINE_PROP_STRING("operator_code", pmb887x_gsm_sim_t, operator_code),
};

static void gsm_sim_class_init(ObjectClass *klass, const void *data) {
	pmb887x_apdu_backend_class_t *apdu_class = PMB887X_APDU_BACKEND_CLASS(klass);
	DeviceClass *device_class = DEVICE_CLASS(klass);

	apdu_class->card_reset = gsm_sim_card_reset;
	apdu_class->get_atr = gsm_sim_get_atr;
	apdu_class->exchange = gsm_sim_exchange;
	device_class->realize = gsm_sim_realize;
	device_class_set_legacy_reset(device_class, gsm_sim_reset);
	device_class_set_props(device_class, gsm_sim_properties);
}

static const TypeInfo gsm_sim_info = {
	.name = TYPE_PMB887X_GSM_SIM,
	.parent = TYPE_PMB887X_APDU_BACKEND,
	.instance_size = sizeof(pmb887x_gsm_sim_t),
	.instance_finalize = gsm_sim_finalize,
	.class_init = gsm_sim_class_init,
};

static void gsm_sim_register_types(void) {
	type_register_static(&gsm_sim_info);
}
type_init(gsm_sim_register_types)
