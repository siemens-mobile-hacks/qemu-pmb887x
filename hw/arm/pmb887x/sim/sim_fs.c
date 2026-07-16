#include "qemu/osdep.h"

#include "hw/arm/pmb887x/sim/sim_fs.h"

#define SIM_FS_DIRECTORY_RESPONSE_SIZE 23
#define SIM_FS_EF_RESPONSE_SIZE 15
#define SIM_FS_AVAILABLE_MEMORY 0x2000
#define SIM_FS_DIRECTORY_DATA_SIZE 10
#define SIM_FS_EF_DATA_SIZE 2
#define SIM_FS_CHARACTERISTICS_DEFAULT 0x9B
#define SIM_FS_FILE_STATUS_VALID 0x01
#define SIM_FS_SECRET_CODE_COUNT 4
#define SIM_FS_SECRET_STATUS(attempts) (BIT(7) | (attempts))
#define SIM_FS_PIN_ATTEMPTS_REMAINING 3
#define SIM_FS_UNBLOCK_ATTEMPTS_REMAINING 10
#define SIM_FS_NO_CURRENT_RECORD UINT16_MAX

enum sim_fs_metadata_offset_t {
	SIM_FS_METADATA_SIZE_MSB = 2,
	SIM_FS_METADATA_SIZE_LSB,
	SIM_FS_METADATA_FILE_ID_MSB,
	SIM_FS_METADATA_FILE_ID_LSB,
	SIM_FS_METADATA_FILE_TYPE,
	SIM_FS_METADATA_INCREASE_ALLOWED,
	SIM_FS_METADATA_ACCESS_CONDITION_1,
	SIM_FS_METADATA_ACCESS_CONDITION_2,
	SIM_FS_METADATA_ACCESS_CONDITION_3,
	SIM_FS_METADATA_FILE_STATUS,
	SIM_FS_METADATA_DATA_SIZE,
	SIM_FS_METADATA_STRUCTURE,
	SIM_FS_METADATA_RECORD_SIZE,
};

enum sim_fs_directory_metadata_offset_t {
	SIM_FS_DIRECTORY_CHARACTERISTICS = 13,
	SIM_FS_DIRECTORY_CHILD_DF_COUNT,
	SIM_FS_DIRECTORY_CHILD_EF_COUNT,
	SIM_FS_DIRECTORY_SECRET_CODE_COUNT,
	SIM_FS_DIRECTORY_RESERVED,
	SIM_FS_DIRECTORY_CHV1_STATUS,
	SIM_FS_DIRECTORY_UNBLOCK_CHV1_STATUS,
	SIM_FS_DIRECTORY_CHV2_STATUS,
	SIM_FS_DIRECTORY_UNBLOCK_CHV2_STATUS,
};

enum sim_fs_file_type_value_t {
	SIM_FS_FILE_TYPE_MF = 0x01,
	SIM_FS_FILE_TYPE_DF = 0x02,
	SIM_FS_FILE_TYPE_EF = 0x04,
};

enum sim_fs_file_structure_value_t {
	SIM_FS_FILE_STRUCTURE_TRANSPARENT = 0x00,
	SIM_FS_FILE_STRUCTURE_LINEAR_FIXED = 0x01,
	SIM_FS_FILE_STRUCTURE_CYCLIC = 0x03,
};

static void sim_fs_set_status(pmb887x_apdu_response_t *response, uint16_t status) {
	response->size = 0;
	pmb887x_apdu_response_set_status(response, status);
}

static bool sim_fs_is_directory(const pmb887x_sim_fs_file_t *file) {
	return file->definition->type == PMB887X_SIM_FS_MF || file->definition->type == PMB887X_SIM_FS_DF;
}

static pmb887x_sim_fs_file_t *sim_fs_find_file(pmb887x_sim_fs_t *fs, uint16_t id, uint16_t parent_id) {
	for (size_t i = 0; i < fs->files_count; i++) {
		if (fs->files[i].definition->id == id && fs->files[i].definition->parent_id == parent_id)
			return &fs->files[i];
	}
	return NULL;
}

static uint8_t sim_fs_directory_child_count(pmb887x_sim_fs_t *fs, uint16_t parent_id, bool directories) {
	uint8_t count = 0;
	for (size_t i = 0; i < fs->files_count; i++) {
		const pmb887x_sim_fs_file_t *file = &fs->files[i];
		if (file->definition->parent_id == parent_id && sim_fs_is_directory(file) == directories)
			count++;
	}
	return count;
}

static uint8_t sim_fs_file_type(const pmb887x_sim_fs_file_t *file) {
	switch (file->definition->type) {
		case PMB887X_SIM_FS_MF:
			return SIM_FS_FILE_TYPE_MF;
		case PMB887X_SIM_FS_DF:
			return SIM_FS_FILE_TYPE_DF;
		default:
			return SIM_FS_FILE_TYPE_EF;
	}
}

static uint8_t sim_fs_file_structure(const pmb887x_sim_fs_file_t *file) {
	switch (file->definition->type) {
		case PMB887X_SIM_FS_EF_LINEAR_FIXED:
			return SIM_FS_FILE_STRUCTURE_LINEAR_FIXED;
		case PMB887X_SIM_FS_EF_CYCLIC:
			return SIM_FS_FILE_STRUCTURE_CYCLIC;
		default:
			return SIM_FS_FILE_STRUCTURE_TRANSPARENT;
	}
}

static size_t sim_fs_build_metadata(pmb887x_sim_fs_t *fs, pmb887x_sim_fs_file_t *file, uint8_t *metadata) {
	const pmb887x_sim_fs_file_definition_t *definition = file->definition;
	if (sim_fs_is_directory(file)) {
		memset(metadata, 0, SIM_FS_DIRECTORY_RESPONSE_SIZE);
		metadata[SIM_FS_METADATA_SIZE_MSB] = (uint8_t) (SIM_FS_AVAILABLE_MEMORY >> 8);
		metadata[SIM_FS_METADATA_SIZE_LSB] = (uint8_t) SIM_FS_AVAILABLE_MEMORY;
		metadata[SIM_FS_METADATA_FILE_ID_MSB] = (uint8_t) (definition->id >> 8);
		metadata[SIM_FS_METADATA_FILE_ID_LSB] = (uint8_t) definition->id;
		metadata[SIM_FS_METADATA_FILE_TYPE] = sim_fs_file_type(file);
		metadata[SIM_FS_METADATA_DATA_SIZE] = SIM_FS_DIRECTORY_DATA_SIZE;
		metadata[SIM_FS_DIRECTORY_CHARACTERISTICS] = SIM_FS_CHARACTERISTICS_DEFAULT;
		metadata[SIM_FS_DIRECTORY_CHILD_DF_COUNT] = sim_fs_directory_child_count(fs, definition->id, true);
		metadata[SIM_FS_DIRECTORY_CHILD_EF_COUNT] = sim_fs_directory_child_count(fs, definition->id, false);
		metadata[SIM_FS_DIRECTORY_SECRET_CODE_COUNT] = SIM_FS_SECRET_CODE_COUNT;
		metadata[SIM_FS_DIRECTORY_CHV1_STATUS] = SIM_FS_SECRET_STATUS(SIM_FS_PIN_ATTEMPTS_REMAINING);
		metadata[SIM_FS_DIRECTORY_UNBLOCK_CHV1_STATUS] = SIM_FS_SECRET_STATUS(SIM_FS_UNBLOCK_ATTEMPTS_REMAINING);
		metadata[SIM_FS_DIRECTORY_CHV2_STATUS] = SIM_FS_SECRET_STATUS(SIM_FS_PIN_ATTEMPTS_REMAINING);
		metadata[SIM_FS_DIRECTORY_UNBLOCK_CHV2_STATUS] = SIM_FS_SECRET_STATUS(SIM_FS_UNBLOCK_ATTEMPTS_REMAINING);
		return SIM_FS_DIRECTORY_RESPONSE_SIZE;
	}

	memset(metadata, 0, SIM_FS_EF_RESPONSE_SIZE);
	metadata[SIM_FS_METADATA_SIZE_MSB] = (uint8_t) (definition->size >> 8);
	metadata[SIM_FS_METADATA_SIZE_LSB] = (uint8_t) definition->size;
	metadata[SIM_FS_METADATA_FILE_ID_MSB] = (uint8_t) (definition->id >> 8);
	metadata[SIM_FS_METADATA_FILE_ID_LSB] = (uint8_t) definition->id;
	metadata[SIM_FS_METADATA_FILE_TYPE] = sim_fs_file_type(file);
	metadata[SIM_FS_METADATA_FILE_STATUS] = SIM_FS_FILE_STATUS_VALID;
	metadata[SIM_FS_METADATA_DATA_SIZE] = SIM_FS_EF_DATA_SIZE;
	metadata[SIM_FS_METADATA_STRUCTURE] = sim_fs_file_structure(file);
	metadata[SIM_FS_METADATA_RECORD_SIZE] = definition->record_size;
	return SIM_FS_EF_RESPONSE_SIZE;
}

static void sim_fs_copy_metadata(
	pmb887x_sim_fs_t *fs,
	pmb887x_sim_fs_file_t *file,
	uint8_t requested_size,
	pmb887x_apdu_response_t *response
) {
	uint8_t metadata[SIM_FS_DIRECTORY_RESPONSE_SIZE];
	size_t metadata_size = sim_fs_build_metadata(fs, file, metadata);
	size_t response_size = requested_size ? requested_size : PMB887X_APDU_MAX_RESPONSE_SIZE;
	if (response_size > metadata_size) {
		sim_fs_set_status(response, PMB887X_APDU_STATUS_CORRECT_LENGTH | metadata_size);
		return;
	}
	memcpy(response->data, metadata, response_size);
	response->size = response_size;
	pmb887x_apdu_response_set_status(response, PMB887X_APDU_STATUS_SUCCESS);
}

void pmb887x_sim_fs_init(
	pmb887x_sim_fs_t *fs,
	const pmb887x_sim_fs_file_definition_t *definitions,
	size_t definitions_count
) {
	fs->files = g_new0(pmb887x_sim_fs_file_t, definitions_count);
	fs->files_count = definitions_count;
	for (size_t i = 0; i < definitions_count; i++) {
		pmb887x_sim_fs_file_t *file = &fs->files[i];
		file->definition = &definitions[i];
		bool record_file = definitions[i].type == PMB887X_SIM_FS_EF_LINEAR_FIXED ||
			definitions[i].type == PMB887X_SIM_FS_EF_CYCLIC;
		g_assert(!record_file || (definitions[i].record_size != 0 && definitions[i].size % definitions[i].record_size == 0));
		if (definitions[i].size != 0) {
			file->data = g_malloc(definitions[i].size);
			memset(file->data, definitions[i].fill, definitions[i].size);
			if (definitions[i].initial_data)
				memcpy(file->data, definitions[i].initial_data, definitions[i].size);
		}
	}
	pmb887x_sim_fs_card_reset(fs);
}

void pmb887x_sim_fs_destroy(pmb887x_sim_fs_t *fs) {
	for (size_t i = 0; i < fs->files_count; i++)
		g_free(fs->files[i].data);
	g_free(fs->files);
	fs->files = NULL;
	fs->files_count = 0;
}

void pmb887x_sim_fs_card_reset(pmb887x_sim_fs_t *fs) {
	fs->current_directory = sim_fs_find_file(fs, PMB887X_SIM_FS_MF_ID, 0);
	g_assert(fs->current_directory && fs->current_directory->definition->type == PMB887X_SIM_FS_MF);
	fs->selected_file = fs->current_directory;
	fs->current_record = SIM_FS_NO_CURRENT_RECORD;
}

bool pmb887x_sim_fs_select(pmb887x_sim_fs_t *fs, uint16_t id, uint8_t *response_size) {
	pmb887x_sim_fs_file_t *file;
	if (id == PMB887X_SIM_FS_MF_ID) {
		file = sim_fs_find_file(fs, id, 0);
	} else {
		file = sim_fs_find_file(fs, id, fs->current_directory->definition->id);
		if (!file) {
			file = sim_fs_find_file(fs, id, PMB887X_SIM_FS_MF_ID);
			if (file && file->definition->type != PMB887X_SIM_FS_DF)
				file = NULL;
		}
	}
	if (!file)
		return false;

	if (sim_fs_is_directory(file))
		fs->current_directory = file;
	fs->selected_file = file;
	fs->current_record = SIM_FS_NO_CURRENT_RECORD;
	*response_size = sim_fs_is_directory(file) ? SIM_FS_DIRECTORY_RESPONSE_SIZE : SIM_FS_EF_RESPONSE_SIZE;
	return true;
}

void pmb887x_sim_fs_get_response(pmb887x_sim_fs_t *fs, uint8_t requested_size, pmb887x_apdu_response_t *response) {
	sim_fs_copy_metadata(fs, fs->selected_file, requested_size, response);
}

void pmb887x_sim_fs_status(pmb887x_sim_fs_t *fs, uint8_t requested_size, pmb887x_apdu_response_t *response) {
	sim_fs_copy_metadata(fs, fs->current_directory, requested_size, response);
}

void pmb887x_sim_fs_read_binary(
	pmb887x_sim_fs_t *fs,
	uint16_t offset,
	uint16_t size,
	pmb887x_apdu_response_t *response
) {
	pmb887x_sim_fs_file_t *file = fs->selected_file;
	if (file->definition->type != PMB887X_SIM_FS_EF_TRANSPARENT || offset + size > file->definition->size) {
		sim_fs_set_status(response, PMB887X_APDU_STATUS_WRONG_PARAMETERS);
		return;
	}
	memcpy(response->data, file->data + offset, size);
	response->size = size;
	pmb887x_apdu_response_set_status(response, PMB887X_APDU_STATUS_SUCCESS);
}

void pmb887x_sim_fs_update_binary(
	pmb887x_sim_fs_t *fs,
	uint16_t offset,
	const uint8_t *data,
	uint16_t size,
	pmb887x_apdu_response_t *response
) {
	pmb887x_sim_fs_file_t *file = fs->selected_file;
	if (file->definition->type != PMB887X_SIM_FS_EF_TRANSPARENT || offset + size > file->definition->size) {
		sim_fs_set_status(response, PMB887X_APDU_STATUS_WRONG_PARAMETERS);
		return;
	}
	memcpy(file->data + offset, data, size);
	pmb887x_apdu_response_set_status(response, PMB887X_APDU_STATUS_SUCCESS);
}

static bool sim_fs_select_record(pmb887x_sim_fs_t *fs, uint8_t record, uint8_t mode) {
	uint16_t records_count = fs->selected_file->definition->size / fs->selected_file->definition->record_size;
	uint16_t selected_record = fs->current_record;
	switch (mode) {
		case PMB887X_SIM_FS_RECORD_NEXT:
			selected_record = selected_record == SIM_FS_NO_CURRENT_RECORD ? 0 : selected_record + 1;
			break;
		case PMB887X_SIM_FS_RECORD_PREVIOUS:
			if (selected_record == SIM_FS_NO_CURRENT_RECORD || selected_record == 0)
				return false;
			selected_record--;
			break;
		case PMB887X_SIM_FS_RECORD_ABSOLUTE:
			if (record != 0) {
				selected_record = record - 1;
			} else if (selected_record == SIM_FS_NO_CURRENT_RECORD) {
				return false;
			}
			break;
		default:
			return false;
	}
	if (selected_record >= records_count)
		return false;
	fs->current_record = selected_record;
	return true;
}

void pmb887x_sim_fs_read_record(
	pmb887x_sim_fs_t *fs,
	uint8_t record,
	uint8_t mode,
	uint16_t size,
	pmb887x_apdu_response_t *response
) {
	pmb887x_sim_fs_file_t *file = fs->selected_file;
	bool record_file = file->definition->type == PMB887X_SIM_FS_EF_LINEAR_FIXED ||
		file->definition->type == PMB887X_SIM_FS_EF_CYCLIC;
	if (!record_file || !sim_fs_select_record(fs, record, mode)) {
		sim_fs_set_status(response, PMB887X_APDU_STATUS_NO_CURRENT_EF);
		return;
	}
	if (size != file->definition->record_size) {
		sim_fs_set_status(response, PMB887X_APDU_STATUS_CORRECT_LENGTH | file->definition->record_size);
		return;
	}
	memcpy(response->data, file->data + fs->current_record * file->definition->record_size, size);
	response->size = size;
	pmb887x_apdu_response_set_status(response, PMB887X_APDU_STATUS_SUCCESS);
}

void pmb887x_sim_fs_update_record(
	pmb887x_sim_fs_t *fs,
	uint8_t record,
	uint8_t mode,
	const uint8_t *data,
	uint16_t size,
	pmb887x_apdu_response_t *response
) {
	pmb887x_sim_fs_file_t *file = fs->selected_file;
	bool record_file = file->definition->type == PMB887X_SIM_FS_EF_LINEAR_FIXED ||
		file->definition->type == PMB887X_SIM_FS_EF_CYCLIC;
	if (!record_file || !sim_fs_select_record(fs, record, mode)) {
		sim_fs_set_status(response, PMB887X_APDU_STATUS_NO_CURRENT_EF);
		return;
	}
	if (size != file->definition->record_size) {
		sim_fs_set_status(response, PMB887X_APDU_STATUS_WRONG_LENGTH);
		return;
	}
	memcpy(file->data + fs->current_record * file->definition->record_size, data, size);
	pmb887x_apdu_response_set_status(response, PMB887X_APDU_STATUS_SUCCESS);
}
