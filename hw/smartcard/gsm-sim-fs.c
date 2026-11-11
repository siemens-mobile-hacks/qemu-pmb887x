/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/smartcard/gsm11.h"
#include "gsm-sim-fs.h"

#define GSM_SIM_FS_DIRECTORY_RESPONSE_SIZE 23
#define GSM_SIM_FS_EF_RESPONSE_SIZE 15
#define GSM_SIM_FS_AVAILABLE_MEMORY 0x2000
#define GSM_SIM_FS_DIRECTORY_DATA_SIZE 10
#define GSM_SIM_FS_EF_DATA_SIZE 2
#define GSM_SIM_FS_CHARACTERISTICS 0x11
#define GSM_SIM_FS_CHARACTERISTICS_CHV1_DISABLED 0x80
#define GSM_SIM_FS_FILE_STATUS_VALID 0x01
#define GSM_SIM_FS_FILE_STATUS_INVALIDATED 0x00
#define GSM_SIM_FS_INCREASE_ALLOWED 0x40
#define GSM_SIM_FS_SECRET_CODE_COUNT 9
#define GSM_SIM_FS_SECRET_CODE_INITIALIZED 0x80
#define GSM_SIM_FS_CHV1 0x01
#define GSM_SIM_FS_CHV2 0x02
#define GSM_SIM_FS_UNBLOCK_CHV1 0x00
#define GSM_SIM_FS_CHANGE_CHV_DATA_SIZE (2 * GSM_SIM_FS_CHV_SIZE)
#define GSM_SIM_FS_SEEK_TYPE_SHIFT 4
#define GSM_SIM_FS_SEEK_MODE_MASK 0x0F
#define GSM_SIM_FS_SEEK_RECORD_NUMBER_SIZE 1
#define GSM_SIM_FS_INCREASE_SIZE 3
#define GSM_SIM_FS_INCREASE_MAX_VALUE 0xFFFFFF
#define GSM_SIM_FS_ACCESS_HIGH_SHIFT 4
#define GSM_SIM_FS_ACCESS_LOW_MASK 0x0F

enum {
    GSM_SIM_FS_RESPONSE_SIZE_MSB = 2,
    GSM_SIM_FS_RESPONSE_SIZE_LSB,
    GSM_SIM_FS_RESPONSE_FILE_ID_MSB,
    GSM_SIM_FS_RESPONSE_FILE_ID_LSB,
    GSM_SIM_FS_RESPONSE_FILE_TYPE,
    GSM_SIM_FS_RESPONSE_INCREASE_ALLOWED,
    GSM_SIM_FS_RESPONSE_ACCESS,
    GSM_SIM_FS_RESPONSE_FILE_STATUS =
        GSM_SIM_FS_RESPONSE_ACCESS + GSM_SIM_FS_ACCESS_SIZE,
    GSM_SIM_FS_RESPONSE_DATA_SIZE,
    GSM_SIM_FS_RESPONSE_EF_STRUCTURE,
    GSM_SIM_FS_RESPONSE_RECORD_SIZE,
};

enum {
    GSM_SIM_FS_RESPONSE_CHARACTERISTICS = 13,
    GSM_SIM_FS_RESPONSE_CHILD_DF_COUNT,
    GSM_SIM_FS_RESPONSE_CHILD_EF_COUNT,
    GSM_SIM_FS_RESPONSE_SECRET_CODE_COUNT,
    GSM_SIM_FS_RESPONSE_RESERVED,
    GSM_SIM_FS_RESPONSE_CHV1_STATUS,
    GSM_SIM_FS_RESPONSE_UNBLOCK_CHV1_STATUS,
    GSM_SIM_FS_RESPONSE_CHV2_STATUS,
    GSM_SIM_FS_RESPONSE_UNBLOCK_CHV2_STATUS,
};

enum {
    GSM_SIM_FS_FILE_TYPE_MF = 0x01,
    GSM_SIM_FS_FILE_TYPE_DF = 0x02,
    GSM_SIM_FS_FILE_TYPE_EF = 0x04,
};

enum {
    GSM_SIM_FS_EF_TRANSPARENT = 0x00,
    GSM_SIM_FS_EF_LINEAR_FIXED = 0x01,
    GSM_SIM_FS_EF_CYCLIC = 0x03,
};

enum {
    GSM_SIM_FS_ACCESS_ALWAYS = 0x0,
    GSM_SIM_FS_ACCESS_CHV1 = 0x1,
    GSM_SIM_FS_ACCESS_CHV2 = 0x2,
    GSM_SIM_FS_ACCESS_NEVER = 0xF,
};

typedef enum GSMSimFSAccessOperation {
    GSM_SIM_FS_ACCESS_READ,
    GSM_SIM_FS_ACCESS_UPDATE,
    GSM_SIM_FS_ACCESS_INCREASE,
    GSM_SIM_FS_ACCESS_INVALIDATE,
    GSM_SIM_FS_ACCESS_REHABILITATE,
} GSMSimFSAccessOperation;

enum {
    GSM_SIM_FS_SEEK_TYPE_1,
    GSM_SIM_FS_SEEK_TYPE_2,
};

enum {
    GSM_SIM_FS_SEEK_FROM_BEGINNING,
    GSM_SIM_FS_SEEK_FROM_END,
    GSM_SIM_FS_SEEK_NEXT,
    GSM_SIM_FS_SEEK_PREVIOUS,
};

static void gsm_sim_fs_set_status(ISO7816Response *response, uint16_t status)
{
    response->size = 0;
    iso7816_response_set_status(response, status);
}

static bool gsm_sim_file_is_directory(const GSMSimFile *file)
{
    return file->definition->structure == GSM_SIM_FILE_MF ||
           file->definition->structure == GSM_SIM_FILE_DF;
}

static bool gsm_sim_file_has_records(const GSMSimFile *file)
{
    return file->definition->structure == GSM_SIM_FILE_LINEAR_FIXED ||
           file->definition->structure == GSM_SIM_FILE_CYCLIC;
}

static uint16_t gsm_sim_file_records(const GSMSimFile *file)
{
    return file->size / file->definition->record_size;
}

static uint8_t *gsm_sim_file_record(const GSMSimFile *file, uint16_t index)
{
    return file->data + index * file->definition->record_size;
}

static uint8_t gsm_sim_fs_access_condition(const GSMSimFile *file,
                                           GSMSimFSAccessOperation operation)
{
    const uint8_t *access = file->definition->access;

    switch (operation) {
    case GSM_SIM_FS_ACCESS_READ:
        return access[0] >> GSM_SIM_FS_ACCESS_HIGH_SHIFT;
    case GSM_SIM_FS_ACCESS_UPDATE:
        return access[0] & GSM_SIM_FS_ACCESS_LOW_MASK;
    case GSM_SIM_FS_ACCESS_INCREASE:
        return access[1] >> GSM_SIM_FS_ACCESS_HIGH_SHIFT;
    case GSM_SIM_FS_ACCESS_INVALIDATE:
        return access[2] & GSM_SIM_FS_ACCESS_LOW_MASK;
    case GSM_SIM_FS_ACCESS_REHABILITATE:
        return access[2] >> GSM_SIM_FS_ACCESS_HIGH_SHIFT;
    }
    g_assert_not_reached();
}

bool gsm_sim_fs_is_chv1_granted(GSMSimFS *fs)
{
    return !fs->chv1_enabled || fs->chv1.fulfilled;
}

static bool gsm_sim_fs_is_allowed(GSMSimFS *fs, const GSMSimFile *file,
                                  GSMSimFSAccessOperation operation)
{
    switch (gsm_sim_fs_access_condition(file, operation)) {
    case GSM_SIM_FS_ACCESS_ALWAYS:
        return true;
    case GSM_SIM_FS_ACCESS_CHV1:
        return gsm_sim_fs_is_chv1_granted(fs);
    case GSM_SIM_FS_ACCESS_CHV2:
        return fs->chv2.fulfilled;
    default:
        return false;
    }
}

static uint8_t gsm_sim_fs_count_children(GSMSimFS *fs, uint16_t parent_id,
                                         bool directories)
{
    uint8_t count = 0;

    for (size_t i = 0; i < fs->files_count; i++) {
        const GSMSimFile *file = &fs->files[i];

        if (file->definition->parent_id == parent_id &&
            gsm_sim_file_is_directory(file) == directories) {
            count++;
        }
    }
    return count;
}

static uint8_t gsm_sim_file_type(const GSMSimFile *file)
{
    switch (file->definition->structure) {
    case GSM_SIM_FILE_MF:
        return GSM_SIM_FS_FILE_TYPE_MF;
    case GSM_SIM_FILE_DF:
        return GSM_SIM_FS_FILE_TYPE_DF;
    default:
        return GSM_SIM_FS_FILE_TYPE_EF;
    }
}

static uint8_t gsm_sim_file_ef_structure(const GSMSimFile *file)
{
    switch (file->definition->structure) {
    case GSM_SIM_FILE_LINEAR_FIXED:
        return GSM_SIM_FS_EF_LINEAR_FIXED;
    case GSM_SIM_FILE_CYCLIC:
        return GSM_SIM_FS_EF_CYCLIC;
    default:
        return GSM_SIM_FS_EF_TRANSPARENT;
    }
}

static uint8_t gsm_sim_fs_encode_directory(GSMSimFS *fs,
                                           const GSMSimFile *file,
                                           uint8_t *response)
{
    uint16_t id = file->definition->id;
    uint8_t characteristics = GSM_SIM_FS_CHARACTERISTICS;

    if (!fs->chv1_enabled) {
        characteristics |= GSM_SIM_FS_CHARACTERISTICS_CHV1_DISABLED;
    }

    memset(response, 0, GSM_SIM_FS_DIRECTORY_RESPONSE_SIZE);
    response[GSM_SIM_FS_RESPONSE_SIZE_MSB] =
        (uint8_t)(GSM_SIM_FS_AVAILABLE_MEMORY >> 8);
    response[GSM_SIM_FS_RESPONSE_SIZE_LSB] =
        (uint8_t)GSM_SIM_FS_AVAILABLE_MEMORY;
    response[GSM_SIM_FS_RESPONSE_FILE_ID_MSB] = (uint8_t)(id >> 8);
    response[GSM_SIM_FS_RESPONSE_FILE_ID_LSB] = (uint8_t)id;
    response[GSM_SIM_FS_RESPONSE_FILE_TYPE] = gsm_sim_file_type(file);
    response[GSM_SIM_FS_RESPONSE_DATA_SIZE] = GSM_SIM_FS_DIRECTORY_DATA_SIZE;
    response[GSM_SIM_FS_RESPONSE_CHARACTERISTICS] = characteristics;
    response[GSM_SIM_FS_RESPONSE_CHILD_DF_COUNT] =
        gsm_sim_fs_count_children(fs, id, true);
    response[GSM_SIM_FS_RESPONSE_CHILD_EF_COUNT] =
        gsm_sim_fs_count_children(fs, id, false);
    response[GSM_SIM_FS_RESPONSE_SECRET_CODE_COUNT] =
        GSM_SIM_FS_SECRET_CODE_COUNT;
    response[GSM_SIM_FS_RESPONSE_CHV1_STATUS] =
        GSM_SIM_FS_SECRET_CODE_INITIALIZED | fs->chv1.attempts;
    response[GSM_SIM_FS_RESPONSE_UNBLOCK_CHV1_STATUS] =
        GSM_SIM_FS_SECRET_CODE_INITIALIZED | fs->chv1.unblock_attempts;
    response[GSM_SIM_FS_RESPONSE_CHV2_STATUS] =
        GSM_SIM_FS_SECRET_CODE_INITIALIZED | fs->chv2.attempts;
    response[GSM_SIM_FS_RESPONSE_UNBLOCK_CHV2_STATUS] =
        GSM_SIM_FS_SECRET_CODE_INITIALIZED | fs->chv2.unblock_attempts;
    return GSM_SIM_FS_DIRECTORY_RESPONSE_SIZE;
}

static uint8_t gsm_sim_fs_encode_increase(const GSMSimFile *file)
{
    bool increase_allowed =
        file->definition->structure == GSM_SIM_FILE_CYCLIC &&
        gsm_sim_fs_access_condition(file, GSM_SIM_FS_ACCESS_INCREASE) !=
            GSM_SIM_FS_ACCESS_NEVER;

    return increase_allowed ? GSM_SIM_FS_INCREASE_ALLOWED : 0;
}

static uint8_t gsm_sim_fs_encode_ef(const GSMSimFile *file, uint8_t *response)
{
    uint16_t id = file->definition->id;

    memset(response, 0, GSM_SIM_FS_EF_RESPONSE_SIZE);
    response[GSM_SIM_FS_RESPONSE_SIZE_MSB] = (uint8_t)(file->size >> 8);
    response[GSM_SIM_FS_RESPONSE_SIZE_LSB] = (uint8_t)file->size;
    response[GSM_SIM_FS_RESPONSE_FILE_ID_MSB] = (uint8_t)(id >> 8);
    response[GSM_SIM_FS_RESPONSE_FILE_ID_LSB] = (uint8_t)id;
    response[GSM_SIM_FS_RESPONSE_FILE_TYPE] = gsm_sim_file_type(file);
    response[GSM_SIM_FS_RESPONSE_INCREASE_ALLOWED] =
        gsm_sim_fs_encode_increase(file);
    memcpy(response + GSM_SIM_FS_RESPONSE_ACCESS, file->definition->access,
           GSM_SIM_FS_ACCESS_SIZE);
    response[GSM_SIM_FS_RESPONSE_FILE_STATUS] =
        file->invalidated ? GSM_SIM_FS_FILE_STATUS_INVALIDATED
                          : GSM_SIM_FS_FILE_STATUS_VALID;
    response[GSM_SIM_FS_RESPONSE_DATA_SIZE] = GSM_SIM_FS_EF_DATA_SIZE;
    response[GSM_SIM_FS_RESPONSE_EF_STRUCTURE] =
        gsm_sim_file_ef_structure(file);
    response[GSM_SIM_FS_RESPONSE_RECORD_SIZE] = file->definition->record_size;
    return GSM_SIM_FS_EF_RESPONSE_SIZE;
}

static uint8_t gsm_sim_fs_encode_file(GSMSimFS *fs, const GSMSimFile *file,
                                      uint8_t *response)
{
    if (gsm_sim_file_is_directory(file)) {
        return gsm_sim_fs_encode_directory(fs, file, response);
    }
    return gsm_sim_fs_encode_ef(file, response);
}

static void gsm_sim_fs_copy_response(const uint8_t *data, uint8_t size,
                                     uint8_t length,
                                     ISO7816Response *response)
{
    size_t requested_size = length ? length : ISO7816_MAX_DATA_SIZE;

    if (requested_size > size) {
        gsm_sim_fs_set_status(response, GSM11_STATUS_CORRECT_LENGTH | size);
        return;
    }
    memcpy(response->data, data, requested_size);
    response->size = requested_size;
    iso7816_response_set_status(response, GSM11_STATUS_SUCCESS);
}

static void gsm_sim_fs_publish_response(GSMSimFS *fs, uint8_t size,
                                        ISO7816Response *response)
{
    fs->response_size = size;
    gsm_sim_fs_set_status(response, GSM11_STATUS_RESPONSE_AVAILABLE | size);
}

void gsm_sim_fs_offer_response(GSMSimFS *fs, const uint8_t *data, uint8_t size,
                               ISO7816Response *response)
{
    memcpy(fs->response, data, size);
    gsm_sim_fs_publish_response(fs, size, response);
}

void gsm_sim_fs_init(GSMSimFS *fs, const GSMSimFileDefinition *definitions,
                     size_t count)
{
    fs->files = g_new0(GSMSimFile, count);
    fs->files_count = count;
    for (size_t i = 0; i < count; i++) {
        const GSMSimFileDefinition *definition = &definitions[i];
        GSMSimFile *file = &fs->files[i];

        file->definition = definition;
        file->size = definition->size;
        if (definition->size == 0) {
            continue;
        }
        file->data = g_malloc(definition->size);
        if (definition->content) {
            memcpy(file->data, definition->content, definition->size);
        } else {
            memset(file->data, definition->fill, definition->size);
        }
    }
    gsm_sim_fs_reset(fs);
}

void gsm_sim_fs_destroy(GSMSimFS *fs)
{
    for (size_t i = 0; i < fs->files_count; i++) {
        g_free(fs->files[i].data);
    }
    g_free(fs->files);
    fs->files = NULL;
    fs->files_count = 0;
}

GSMSimFile *gsm_sim_fs_find(GSMSimFS *fs, uint16_t id, uint16_t parent_id)
{
    for (size_t i = 0; i < fs->files_count; i++) {
        const GSMSimFileDefinition *definition = fs->files[i].definition;

        if (definition->id == id && definition->parent_id == parent_id) {
            return &fs->files[i];
        }
    }
    return NULL;
}

void gsm_sim_fs_reset(GSMSimFS *fs)
{
    fs->current_directory = gsm_sim_fs_find(fs, GSM_SIM_FS_MF_ID, 0);
    g_assert(fs->current_directory &&
             fs->current_directory->definition->structure == GSM_SIM_FILE_MF);
    fs->selected_file = fs->current_directory;
    fs->record_selected = false;
    fs->response_size = 0;
}

static GSMSimFile *gsm_sim_fs_find_selectable(GSMSimFS *fs, uint16_t id)
{
    GSMSimFile *file;

    if (id == GSM_SIM_FS_MF_ID) {
        return gsm_sim_fs_find(fs, id, 0);
    }

    file = gsm_sim_fs_find(fs, id, fs->current_directory->definition->id);
    if (file) {
        return file;
    }

    file = gsm_sim_fs_find(fs, id, GSM_SIM_FS_MF_ID);
    if (file && file->definition->structure == GSM_SIM_FILE_DF) {
        return file;
    }
    return NULL;
}

void gsm_sim_fs_select(GSMSimFS *fs, uint16_t id, ISO7816Response *response)
{
    GSMSimFile *file = gsm_sim_fs_find_selectable(fs, id);
    uint8_t size;

    if (!file) {
        gsm_sim_fs_set_status(response, GSM11_STATUS_FILE_ID_NOT_FOUND);
        return;
    }

    if (gsm_sim_file_is_directory(file)) {
        fs->current_directory = file;
    }
    fs->selected_file = file;
    fs->record_selected = file->definition->structure == GSM_SIM_FILE_CYCLIC;
    fs->current_record = 0;
    size = gsm_sim_fs_encode_file(fs, file, fs->response);
    gsm_sim_fs_publish_response(fs, size, response);
}

void gsm_sim_fs_get_response(GSMSimFS *fs, uint8_t length,
                             ISO7816Response *response)
{
    gsm_sim_fs_copy_response(fs->response, fs->response_size, length,
                             response);
}

void gsm_sim_fs_status(GSMSimFS *fs, uint8_t length,
                       ISO7816Response *response)
{
    uint8_t encoded[GSM_SIM_FS_DIRECTORY_RESPONSE_SIZE];
    uint8_t size = gsm_sim_fs_encode_directory(fs, fs->current_directory,
                                               encoded);

    gsm_sim_fs_copy_response(encoded, size, length, response);
}

static uint16_t gsm_sim_fs_validate_binary(GSMSimFS *fs,
                                           GSMSimFSAccessOperation operation,
                                           uint16_t offset, uint16_t length)
{
    const GSMSimFile *file = fs->selected_file;

    if (file->definition->structure != GSM_SIM_FILE_TRANSPARENT) {
        return GSM11_STATUS_FILE_INCONSISTENT;
    }
    if (!gsm_sim_fs_is_allowed(fs, file, operation)) {
        return GSM11_STATUS_ACCESS_DENIED;
    }
    if (file->invalidated) {
        return GSM11_STATUS_INVALIDATION_CONTRADICTION;
    }
    if (offset >= file->size) {
        return GSM11_STATUS_WRONG_PARAMETERS;
    }
    if (length > file->size - offset) {
        return GSM11_STATUS_WRONG_LENGTH | (file->size - offset);
    }
    return GSM11_STATUS_SUCCESS;
}

void gsm_sim_fs_read_binary(GSMSimFS *fs, uint16_t offset, uint16_t length,
                            ISO7816Response *response)
{
    uint16_t status = gsm_sim_fs_validate_binary(fs, GSM_SIM_FS_ACCESS_READ,
                                                 offset, length);

    if (status != GSM11_STATUS_SUCCESS) {
        gsm_sim_fs_set_status(response, status);
        return;
    }
    memcpy(response->data, fs->selected_file->data + offset, length);
    response->size = length;
    iso7816_response_set_status(response, GSM11_STATUS_SUCCESS);
}

void gsm_sim_fs_update_binary(GSMSimFS *fs, uint16_t offset,
                              const uint8_t *data, uint16_t length,
                              ISO7816Response *response)
{
    uint16_t status = gsm_sim_fs_validate_binary(fs, GSM_SIM_FS_ACCESS_UPDATE,
                                                 offset, length);

    if (status == GSM11_STATUS_SUCCESS) {
        memcpy(fs->selected_file->data + offset, data, length);
    }
    gsm_sim_fs_set_status(response, status);
}

static bool gsm_sim_fs_step_record(GSMSimFS *fs, bool forward)
{
    const GSMSimFile *file = fs->selected_file;
    uint16_t records = gsm_sim_file_records(file);
    bool cyclic = file->definition->structure == GSM_SIM_FILE_CYCLIC;
    uint16_t record;

    if (!fs->record_selected) {
        record = forward ? 0 : records - 1;
    } else if (forward) {
        if (fs->current_record + 1 == records && !cyclic) {
            return false;
        }
        record = (fs->current_record + 1) % records;
    } else {
        if (fs->current_record == 0 && !cyclic) {
            return false;
        }
        record = (fs->current_record + records - 1) % records;
    }

    fs->current_record = record;
    fs->record_selected = true;
    return true;
}

static uint16_t gsm_sim_fs_locate_record(GSMSimFS *fs, uint8_t record,
                                         uint8_t mode, uint16_t *index)
{
    switch (mode) {
    case GSM_SIM_RECORD_NEXT:
    case GSM_SIM_RECORD_PREVIOUS:
        if (!gsm_sim_fs_step_record(fs, mode == GSM_SIM_RECORD_NEXT)) {
            return GSM11_STATUS_OUT_OF_RANGE;
        }
        *index = fs->current_record;
        return GSM11_STATUS_SUCCESS;
    case GSM_SIM_RECORD_ABSOLUTE:
        if (record == 0) {
            if (!fs->record_selected) {
                return GSM11_STATUS_OUT_OF_RANGE;
            }
            *index = fs->current_record;
            return GSM11_STATUS_SUCCESS;
        }
        if (record > gsm_sim_file_records(fs->selected_file)) {
            return GSM11_STATUS_OUT_OF_RANGE;
        }
        *index = record - 1;
        return GSM11_STATUS_SUCCESS;
    default:
        return GSM11_STATUS_WRONG_PARAMETERS;
    }
}

static uint16_t gsm_sim_fs_validate_record(GSMSimFS *fs,
                                           GSMSimFSAccessOperation operation,
                                           uint16_t length)
{
    const GSMSimFile *file = fs->selected_file;

    if (!gsm_sim_file_has_records(file)) {
        return GSM11_STATUS_FILE_INCONSISTENT;
    }
    if (!gsm_sim_fs_is_allowed(fs, file, operation)) {
        return GSM11_STATUS_ACCESS_DENIED;
    }
    if (file->invalidated) {
        return GSM11_STATUS_INVALIDATION_CONTRADICTION;
    }
    if (length != file->definition->record_size) {
        return GSM11_STATUS_WRONG_LENGTH | file->definition->record_size;
    }
    return GSM11_STATUS_SUCCESS;
}

void gsm_sim_fs_read_record(GSMSimFS *fs, uint8_t record, uint8_t mode,
                            uint16_t length, ISO7816Response *response)
{
    uint16_t status = gsm_sim_fs_validate_record(fs, GSM_SIM_FS_ACCESS_READ,
                                                 length);
    uint16_t index;

    if (status != GSM11_STATUS_SUCCESS) {
        gsm_sim_fs_set_status(response, status);
        return;
    }
    status = gsm_sim_fs_locate_record(fs, record, mode, &index);
    if (status != GSM11_STATUS_SUCCESS) {
        gsm_sim_fs_set_status(response, status);
        return;
    }
    memcpy(response->data, gsm_sim_file_record(fs->selected_file, index),
           length);
    response->size = length;
    iso7816_response_set_status(response, GSM11_STATUS_SUCCESS);
}

static void gsm_sim_fs_push_cyclic_record(GSMSimFS *fs, const uint8_t *data)
{
    const GSMSimFile *file = fs->selected_file;
    uint8_t record_size = file->definition->record_size;

    memmove(file->data + record_size, file->data, file->size - record_size);
    memcpy(file->data, data, record_size);
    fs->current_record = 0;
    fs->record_selected = true;
}

void gsm_sim_fs_update_record(GSMSimFS *fs, uint8_t record, uint8_t mode,
                              const uint8_t *data, uint16_t length,
                              ISO7816Response *response)
{
    uint16_t status = gsm_sim_fs_validate_record(fs, GSM_SIM_FS_ACCESS_UPDATE,
                                                 length);
    uint16_t index;

    if (status != GSM11_STATUS_SUCCESS) {
        gsm_sim_fs_set_status(response, status);
        return;
    }

    if (fs->selected_file->definition->structure == GSM_SIM_FILE_CYCLIC) {
        if (mode != GSM_SIM_RECORD_PREVIOUS) {
            gsm_sim_fs_set_status(response, GSM11_STATUS_WRONG_PARAMETERS);
            return;
        }
        gsm_sim_fs_push_cyclic_record(fs, data);
        gsm_sim_fs_set_status(response, GSM11_STATUS_SUCCESS);
        return;
    }

    status = gsm_sim_fs_locate_record(fs, record, mode, &index);
    if (status == GSM11_STATUS_SUCCESS) {
        memcpy(gsm_sim_file_record(fs->selected_file, index), data, length);
    }
    gsm_sim_fs_set_status(response, status);
}

static bool gsm_sim_fs_find_pattern(GSMSimFS *fs, uint8_t mode,
                                    const uint8_t *pattern, uint16_t length)
{
    const GSMSimFile *file = fs->selected_file;
    int records = gsm_sim_file_records(file);
    bool forward = mode == GSM_SIM_FS_SEEK_FROM_BEGINNING ||
                   mode == GSM_SIM_FS_SEEK_NEXT;
    int step = forward ? 1 : -1;
    int start;

    switch (mode) {
    case GSM_SIM_FS_SEEK_FROM_BEGINNING:
        start = 0;
        break;
    case GSM_SIM_FS_SEEK_FROM_END:
        start = records - 1;
        break;
    case GSM_SIM_FS_SEEK_NEXT:
        start = fs->record_selected ? fs->current_record + 1 : 0;
        break;
    default:
        start = fs->record_selected ? fs->current_record - 1 : records - 1;
        break;
    }

    for (int record = start; record >= 0 && record < records; record += step) {
        if (memcmp(gsm_sim_file_record(file, record), pattern, length) == 0) {
            fs->current_record = record;
            fs->record_selected = true;
            return true;
        }
    }
    return false;
}

static uint16_t gsm_sim_fs_validate_seek(GSMSimFS *fs, uint8_t type,
                                         uint8_t mode, uint16_t length)
{
    const GSMSimFile *file = fs->selected_file;

    if (file->definition->structure != GSM_SIM_FILE_LINEAR_FIXED) {
        return GSM11_STATUS_FILE_INCONSISTENT;
    }
    if (!gsm_sim_fs_is_allowed(fs, file, GSM_SIM_FS_ACCESS_READ)) {
        return GSM11_STATUS_ACCESS_DENIED;
    }
    if (file->invalidated) {
        return GSM11_STATUS_INVALIDATION_CONTRADICTION;
    }
    if (type > GSM_SIM_FS_SEEK_TYPE_2 || mode > GSM_SIM_FS_SEEK_PREVIOUS) {
        return GSM11_STATUS_WRONG_PARAMETERS;
    }
    if (length == 0 || length > file->definition->record_size) {
        return GSM11_STATUS_WRONG_LENGTH;
    }
    return GSM11_STATUS_SUCCESS;
}

void gsm_sim_fs_seek(GSMSimFS *fs, uint8_t type_mode, const uint8_t *pattern,
                     uint16_t length, ISO7816Response *response)
{
    uint8_t type = type_mode >> GSM_SIM_FS_SEEK_TYPE_SHIFT;
    uint8_t mode = type_mode & GSM_SIM_FS_SEEK_MODE_MASK;
    uint16_t status = gsm_sim_fs_validate_seek(fs, type, mode, length);

    if (status != GSM11_STATUS_SUCCESS) {
        gsm_sim_fs_set_status(response, status);
        return;
    }
    if (!gsm_sim_fs_find_pattern(fs, mode, pattern, length)) {
        gsm_sim_fs_set_status(response, GSM11_STATUS_PATTERN_NOT_FOUND);
        return;
    }
    if (type == GSM_SIM_FS_SEEK_TYPE_1) {
        gsm_sim_fs_set_status(response, GSM11_STATUS_SUCCESS);
        return;
    }

    fs->response[0] = fs->current_record + 1;
    gsm_sim_fs_publish_response(fs, GSM_SIM_FS_SEEK_RECORD_NUMBER_SIZE,
                                response);
}

static uint16_t gsm_sim_fs_validate_increase(GSMSimFS *fs, uint16_t length)
{
    const GSMSimFile *file = fs->selected_file;

    if (file->definition->structure != GSM_SIM_FILE_CYCLIC) {
        return GSM11_STATUS_FILE_INCONSISTENT;
    }
    if (file->definition->record_size != GSM_SIM_FS_INCREASE_SIZE) {
        return GSM11_STATUS_FILE_INCONSISTENT;
    }
    if (!gsm_sim_fs_is_allowed(fs, file, GSM_SIM_FS_ACCESS_INCREASE)) {
        return GSM11_STATUS_ACCESS_DENIED;
    }
    if (file->invalidated) {
        return GSM11_STATUS_INVALIDATION_CONTRADICTION;
    }
    if (length != GSM_SIM_FS_INCREASE_SIZE) {
        return GSM11_STATUS_WRONG_LENGTH | GSM_SIM_FS_INCREASE_SIZE;
    }
    return GSM11_STATUS_SUCCESS;
}

void gsm_sim_fs_increase(GSMSimFS *fs, const uint8_t *value, uint16_t length,
                         ISO7816Response *response)
{
    uint16_t status = gsm_sim_fs_validate_increase(fs, length);
    const uint8_t *newest;
    uint32_t current;
    uint32_t added;
    uint32_t result;
    uint8_t encoded[GSM_SIM_FS_INCREASE_SIZE];

    if (status != GSM11_STATUS_SUCCESS) {
        gsm_sim_fs_set_status(response, status);
        return;
    }

    newest = gsm_sim_file_record(fs->selected_file, 0);
    current = (newest[0] << 16) | (newest[1] << 8) | newest[2];
    added = (value[0] << 16) | (value[1] << 8) | value[2];
    result = current + added;
    if (result > GSM_SIM_FS_INCREASE_MAX_VALUE) {
        gsm_sim_fs_set_status(response, GSM11_STATUS_MAX_VALUE_REACHED);
        return;
    }

    encoded[0] = (uint8_t)(result >> 16);
    encoded[1] = (uint8_t)(result >> 8);
    encoded[2] = (uint8_t)result;
    gsm_sim_fs_push_cyclic_record(fs, encoded);
    memcpy(fs->response, encoded, GSM_SIM_FS_INCREASE_SIZE);
    memcpy(fs->response + GSM_SIM_FS_INCREASE_SIZE, value,
           GSM_SIM_FS_INCREASE_SIZE);
    gsm_sim_fs_publish_response(fs, 2 * GSM_SIM_FS_INCREASE_SIZE, response);
}

static void gsm_sim_fs_set_invalidated(GSMSimFS *fs,
                                       GSMSimFSAccessOperation operation,
                                       bool invalidated,
                                       ISO7816Response *response)
{
    GSMSimFile *file = fs->selected_file;

    if (gsm_sim_file_is_directory(file)) {
        gsm_sim_fs_set_status(response, GSM11_STATUS_NO_EF_SELECTED);
        return;
    }
    if (!gsm_sim_fs_is_allowed(fs, file, operation)) {
        gsm_sim_fs_set_status(response, GSM11_STATUS_ACCESS_DENIED);
        return;
    }
    file->invalidated = invalidated;
    gsm_sim_fs_set_status(response, GSM11_STATUS_SUCCESS);
}

void gsm_sim_fs_invalidate(GSMSimFS *fs, ISO7816Response *response)
{
    gsm_sim_fs_set_invalidated(fs, GSM_SIM_FS_ACCESS_INVALIDATE, true,
                               response);
}

void gsm_sim_fs_rehabilitate(GSMSimFS *fs, ISO7816Response *response)
{
    gsm_sim_fs_set_invalidated(fs, GSM_SIM_FS_ACCESS_REHABILITATE, false,
                               response);
}

static GSMSimCHV *gsm_sim_fs_find_chv(GSMSimFS *fs, uint8_t chv_number)
{
    switch (chv_number) {
    case GSM_SIM_FS_CHV1:
        return &fs->chv1;
    case GSM_SIM_FS_CHV2:
        return &fs->chv2;
    default:
        return NULL;
    }
}

static GSMSimCHV *gsm_sim_fs_find_unblock_chv(GSMSimFS *fs, uint8_t chv_number)
{
    switch (chv_number) {
    case GSM_SIM_FS_UNBLOCK_CHV1:
        return &fs->chv1;
    case GSM_SIM_FS_CHV2:
        return &fs->chv2;
    default:
        return NULL;
    }
}

static uint16_t gsm_sim_fs_present_chv(GSMSimCHV *chv, const uint8_t *value)
{
    if (memcmp(chv->value, value, GSM_SIM_FS_CHV_SIZE) != 0) {
        chv->attempts--;
        chv->fulfilled = false;
        return chv->attempts ? GSM11_STATUS_ACCESS_DENIED
                             : GSM11_STATUS_CHV_BLOCKED;
    }
    chv->attempts = GSM_SIM_FS_CHV_ATTEMPTS;
    chv->fulfilled = true;
    return GSM11_STATUS_SUCCESS;
}

static uint16_t gsm_sim_fs_validate_chv(GSMSimFS *fs, GSMSimCHV *chv,
                                        uint16_t length,
                                        uint16_t expected_length)
{
    if (length != expected_length) {
        return GSM11_STATUS_WRONG_LENGTH | expected_length;
    }
    if (!chv) {
        return GSM11_STATUS_WRONG_PARAMETERS;
    }
    if (chv->attempts == 0) {
        return GSM11_STATUS_CHV_BLOCKED;
    }
    if (chv == &fs->chv1 && !fs->chv1_enabled) {
        return GSM11_STATUS_CHV_CONTRADICTION;
    }
    return GSM11_STATUS_SUCCESS;
}

void gsm_sim_fs_verify_chv(GSMSimFS *fs, uint8_t chv_number,
                           const uint8_t *data, uint16_t length,
                           ISO7816Response *response)
{
    GSMSimCHV *chv = gsm_sim_fs_find_chv(fs, chv_number);
    uint16_t status = gsm_sim_fs_validate_chv(fs, chv, length,
                                              GSM_SIM_FS_CHV_SIZE);

    if (status == GSM11_STATUS_SUCCESS) {
        status = gsm_sim_fs_present_chv(chv, data);
    }
    gsm_sim_fs_set_status(response, status);
}

void gsm_sim_fs_change_chv(GSMSimFS *fs, uint8_t chv_number,
                           const uint8_t *data, uint16_t length,
                           ISO7816Response *response)
{
    GSMSimCHV *chv = gsm_sim_fs_find_chv(fs, chv_number);
    uint16_t status = gsm_sim_fs_validate_chv(fs, chv, length,
                                              GSM_SIM_FS_CHANGE_CHV_DATA_SIZE);

    if (status == GSM11_STATUS_SUCCESS) {
        status = gsm_sim_fs_present_chv(chv, data);
    }
    if (status == GSM11_STATUS_SUCCESS) {
        memcpy(chv->value, data + GSM_SIM_FS_CHV_SIZE, GSM_SIM_FS_CHV_SIZE);
    }
    gsm_sim_fs_set_status(response, status);
}

static void gsm_sim_fs_set_chv1_enabled(GSMSimFS *fs, uint8_t chv_number,
                                        const uint8_t *data, uint16_t length,
                                        bool enabled,
                                        ISO7816Response *response)
{
    uint16_t status;

    if (length != GSM_SIM_FS_CHV_SIZE) {
        gsm_sim_fs_set_status(response,
                              GSM11_STATUS_WRONG_LENGTH | GSM_SIM_FS_CHV_SIZE);
        return;
    }
    if (chv_number != GSM_SIM_FS_CHV1) {
        gsm_sim_fs_set_status(response, GSM11_STATUS_WRONG_PARAMETERS);
        return;
    }
    if (fs->chv1.attempts == 0) {
        gsm_sim_fs_set_status(response, GSM11_STATUS_CHV_BLOCKED);
        return;
    }
    if (fs->chv1_enabled == enabled) {
        gsm_sim_fs_set_status(response, GSM11_STATUS_CHV_CONTRADICTION);
        return;
    }

    status = gsm_sim_fs_present_chv(&fs->chv1, data);
    if (status == GSM11_STATUS_SUCCESS) {
        fs->chv1_enabled = enabled;
    }
    gsm_sim_fs_set_status(response, status);
}

void gsm_sim_fs_disable_chv(GSMSimFS *fs, uint8_t chv_number,
                            const uint8_t *data, uint16_t length,
                            ISO7816Response *response)
{
    gsm_sim_fs_set_chv1_enabled(fs, chv_number, data, length, false, response);
}

void gsm_sim_fs_enable_chv(GSMSimFS *fs, uint8_t chv_number,
                           const uint8_t *data, uint16_t length,
                           ISO7816Response *response)
{
    gsm_sim_fs_set_chv1_enabled(fs, chv_number, data, length, true, response);
}

void gsm_sim_fs_unblock_chv(GSMSimFS *fs, uint8_t chv_number,
                            const uint8_t *data, uint16_t length,
                            ISO7816Response *response)
{
    GSMSimCHV *chv = gsm_sim_fs_find_unblock_chv(fs, chv_number);

    if (length != GSM_SIM_FS_CHANGE_CHV_DATA_SIZE) {
        gsm_sim_fs_set_status(response, GSM11_STATUS_WRONG_LENGTH |
                                        GSM_SIM_FS_CHANGE_CHV_DATA_SIZE);
        return;
    }
    if (!chv) {
        gsm_sim_fs_set_status(response, GSM11_STATUS_WRONG_PARAMETERS);
        return;
    }
    if (chv->unblock_attempts == 0) {
        gsm_sim_fs_set_status(response, GSM11_STATUS_CHV_BLOCKED);
        return;
    }
    if (memcmp(chv->unblock_value, data, GSM_SIM_FS_CHV_SIZE) != 0) {
        chv->unblock_attempts--;
        gsm_sim_fs_set_status(response, chv->unblock_attempts
                                            ? GSM11_STATUS_ACCESS_DENIED
                                            : GSM11_STATUS_CHV_BLOCKED);
        return;
    }

    memcpy(chv->value, data + GSM_SIM_FS_CHV_SIZE, GSM_SIM_FS_CHV_SIZE);
    chv->attempts = GSM_SIM_FS_CHV_ATTEMPTS;
    chv->unblock_attempts = GSM_SIM_FS_UNBLOCK_CHV_ATTEMPTS;
    chv->fulfilled = true;
    if (chv == &fs->chv1) {
        fs->chv1_enabled = true;
    }
    gsm_sim_fs_set_status(response, GSM11_STATUS_SUCCESS);
}
