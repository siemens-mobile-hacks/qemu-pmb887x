#pragma once

#include "hw/arm/pmb887x/sim/apdu.h"

enum pmb887x_sim_fs_file_type_t {
	PMB887X_SIM_FS_MF,
	PMB887X_SIM_FS_DF,
	PMB887X_SIM_FS_EF_TRANSPARENT,
	PMB887X_SIM_FS_EF_LINEAR_FIXED,
	PMB887X_SIM_FS_EF_CYCLIC,
};

enum pmb887x_sim_fs_record_mode_t {
	PMB887X_SIM_FS_RECORD_NEXT = 0x02,
	PMB887X_SIM_FS_RECORD_PREVIOUS = 0x03,
	PMB887X_SIM_FS_RECORD_ABSOLUTE = 0x04,
};

#define PMB887X_SIM_FS_MF_ID 0x3F00

typedef struct pmb887x_sim_fs_file_definition_t {
	uint16_t id;
	uint16_t parent_id;
	enum pmb887x_sim_fs_file_type_t type;
	const uint8_t *initial_data;
	uint16_t size;
	uint8_t fill;
	uint8_t record_size;
} pmb887x_sim_fs_file_definition_t;

typedef struct pmb887x_sim_fs_file_t {
	const pmb887x_sim_fs_file_definition_t *definition;
	uint8_t *data;
} pmb887x_sim_fs_file_t;

typedef struct pmb887x_sim_fs_t {
	pmb887x_sim_fs_file_t *files;
	size_t files_count;
	pmb887x_sim_fs_file_t *current_directory;
	pmb887x_sim_fs_file_t *selected_file;
	uint16_t current_record;
} pmb887x_sim_fs_t;

void pmb887x_sim_fs_init(
	pmb887x_sim_fs_t *fs,
	const pmb887x_sim_fs_file_definition_t *definitions,
	size_t definitions_count
);
void pmb887x_sim_fs_destroy(pmb887x_sim_fs_t *fs);
void pmb887x_sim_fs_card_reset(pmb887x_sim_fs_t *fs);
bool pmb887x_sim_fs_select(pmb887x_sim_fs_t *fs, uint16_t id, uint8_t *response_size);
void pmb887x_sim_fs_get_response(pmb887x_sim_fs_t *fs, uint8_t requested_size, pmb887x_apdu_response_t *response);
void pmb887x_sim_fs_status(pmb887x_sim_fs_t *fs, uint8_t requested_size, pmb887x_apdu_response_t *response);
void pmb887x_sim_fs_read_binary(
	pmb887x_sim_fs_t *fs,
	uint16_t offset,
	uint16_t size,
	pmb887x_apdu_response_t *response
);
void pmb887x_sim_fs_update_binary(
	pmb887x_sim_fs_t *fs,
	uint16_t offset,
	const uint8_t *data,
	uint16_t size,
	pmb887x_apdu_response_t *response
);
void pmb887x_sim_fs_read_record(
	pmb887x_sim_fs_t *fs,
	uint8_t record,
	uint8_t mode,
	uint16_t size,
	pmb887x_apdu_response_t *response
);
void pmb887x_sim_fs_update_record(
	pmb887x_sim_fs_t *fs,
	uint8_t record,
	uint8_t mode,
	const uint8_t *data,
	uint16_t size,
	pmb887x_apdu_response_t *response
);
