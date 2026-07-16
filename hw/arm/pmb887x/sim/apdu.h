#pragma once

#include "hw/core/qdev.h"
#include "qom/object.h"

#define TYPE_PMB887X_APDU_BACKEND "pmb887x-apdu-backend"
OBJECT_DECLARE_TYPE(pmb887x_apdu_backend_t, pmb887x_apdu_backend_class_t, PMB887X_APDU_BACKEND)

#define TYPE_PMB887X_GSM_SIM "gsm-sim-apdu"
#define TYPE_PMB887X_CACARD_APDU "cacard-apdu"

#define PMB887X_APDU_HEADER_SIZE 5
#define PMB887X_APDU_MAX_DATA_SIZE 256
#define PMB887X_APDU_MAX_COMMAND_SIZE (PMB887X_APDU_HEADER_SIZE + PMB887X_APDU_MAX_DATA_SIZE)
#define PMB887X_APDU_MAX_RESPONSE_SIZE PMB887X_APDU_MAX_DATA_SIZE
#define PMB887X_APDU_STATUS_SIZE 2
#define PMB887X_APDU_MAX_ATR_SIZE 64

#define PMB887X_T0_NULL_BYTE 0x60

enum pmb887x_apdu_header_offset_t {
	PMB887X_APDU_OFFSET_CLA,
	PMB887X_APDU_OFFSET_INS,
	PMB887X_APDU_OFFSET_P1,
	PMB887X_APDU_OFFSET_P2,
	PMB887X_APDU_OFFSET_P3,
};

enum pmb887x_apdu_instruction_t {
	PMB887X_APDU_INS_TERMINAL_PROFILE = 0x10,
	PMB887X_APDU_INS_FETCH = 0x12,
	PMB887X_APDU_INS_TERMINAL_RESPONSE = 0x14,
	PMB887X_APDU_INS_VERIFY_PIN = 0x20,
	PMB887X_APDU_INS_CHANGE_PIN = 0x24,
	PMB887X_APDU_INS_DISABLE_PIN = 0x26,
	PMB887X_APDU_INS_ENABLE_PIN = 0x28,
	PMB887X_APDU_INS_UNBLOCK_PIN = 0x2C,
	PMB887X_APDU_INS_INCREASE = 0x32,
	PMB887X_APDU_INS_RUN_GSM_ALGORITHM = 0x88,
	PMB887X_APDU_INS_SEEK = 0xA2,
	PMB887X_APDU_INS_SELECT = 0xA4,
	PMB887X_APDU_INS_GET_RESPONSE = 0xC0,
	PMB887X_APDU_INS_ENVELOPE = 0xC2,
	PMB887X_APDU_INS_STATUS = 0xF2,
	PMB887X_APDU_INS_READ_BINARY = 0xB0,
	PMB887X_APDU_INS_UPDATE_BINARY = 0xD6,
	PMB887X_APDU_INS_READ_RECORD = 0xB2,
	PMB887X_APDU_INS_UPDATE_RECORD = 0xDC,
};

enum pmb887x_apdu_status_t {
	PMB887X_APDU_STATUS_SUCCESS = 0x9000,
	PMB887X_APDU_STATUS_RESPONSE_AVAILABLE = 0x9F00,
	PMB887X_APDU_STATUS_FILE_NOT_FOUND = 0x6A82,
	PMB887X_APDU_STATUS_WRONG_PARAMETERS = 0x6B00,
	PMB887X_APDU_STATUS_CORRECT_LENGTH = 0x6C00,
	PMB887X_APDU_STATUS_WRONG_LENGTH = 0x6700,
	PMB887X_APDU_STATUS_INS_NOT_SUPPORTED = 0x6D00,
	PMB887X_APDU_STATUS_CLASS_NOT_SUPPORTED = 0x6E00,
	PMB887X_APDU_STATUS_TECHNICAL_PROBLEM = 0x6F00,
	PMB887X_APDU_STATUS_NO_CURRENT_EF = 0x9402,
};

typedef struct pmb887x_apdu_response_t {
	uint8_t data[PMB887X_APDU_MAX_RESPONSE_SIZE];
	size_t size;
	uint8_t sw1;
	uint8_t sw2;
} pmb887x_apdu_response_t;

static inline void pmb887x_apdu_response_set_status(pmb887x_apdu_response_t *response, uint16_t status) {
	response->sw1 = (uint8_t) (status >> 8);
	response->sw2 = (uint8_t) status;
}

typedef void (*pmb887x_apdu_complete_fn)(void *opaque, const pmb887x_apdu_response_t *response);

struct pmb887x_apdu_backend_t {
	DeviceState parent_obj;
};

struct pmb887x_apdu_backend_class_t {
	DeviceClass parent_class;
	void (*card_reset)(pmb887x_apdu_backend_t *backend);
	const uint8_t *(*get_atr)(pmb887x_apdu_backend_t *backend, size_t *size);
	void (*exchange)(
		pmb887x_apdu_backend_t *backend,
		const uint8_t *command,
		size_t command_size,
		pmb887x_apdu_complete_fn complete,
		void *opaque
	);
};

void pmb887x_apdu_backend_card_reset(pmb887x_apdu_backend_t *backend);
const uint8_t *pmb887x_apdu_backend_get_atr(pmb887x_apdu_backend_t *backend, size_t *size);
void pmb887x_apdu_backend_exchange(
	pmb887x_apdu_backend_t *backend,
	const uint8_t *command,
	size_t command_size,
	pmb887x_apdu_complete_fn complete,
	void *opaque
);
