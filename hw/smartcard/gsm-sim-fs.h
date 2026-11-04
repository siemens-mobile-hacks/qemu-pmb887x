/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SMARTCARD_GSM_SIM_FS_H
#define HW_SMARTCARD_GSM_SIM_FS_H

#include "hw/smartcard/iso7816.h"

#define GSM_SIM_FS_MF_ID 0x3F00
#define GSM_SIM_FS_ACCESS_SIZE 3
#define GSM_SIM_FS_RESPONSE_MAX_SIZE 23
#define GSM_SIM_FS_CHV_SIZE 8
#define GSM_SIM_FS_CHV_ATTEMPTS 3
#define GSM_SIM_FS_UNBLOCK_CHV_ATTEMPTS 10

typedef enum GSMSimFileStructure {
    GSM_SIM_FILE_MF,
    GSM_SIM_FILE_DF,
    GSM_SIM_FILE_TRANSPARENT,
    GSM_SIM_FILE_LINEAR_FIXED,
    GSM_SIM_FILE_CYCLIC,
} GSMSimFileStructure;

typedef enum GSMSimRecordMode {
    GSM_SIM_RECORD_NEXT = 0x02,
    GSM_SIM_RECORD_PREVIOUS = 0x03,
    GSM_SIM_RECORD_ABSOLUTE = 0x04,
} GSMSimRecordMode;

typedef struct GSMSimFileDefinition {
    uint16_t id;
    uint16_t parent_id;
    GSMSimFileStructure structure;
    uint16_t size;
    uint8_t record_size;
    uint8_t fill;
    const uint8_t *content;
    uint8_t access[GSM_SIM_FS_ACCESS_SIZE];
} GSMSimFileDefinition;

typedef struct GSMSimFile {
    const GSMSimFileDefinition *definition;
    uint16_t size;
    uint8_t *data;
    bool invalidated;
} GSMSimFile;

typedef struct GSMSimCHV {
    uint8_t value[GSM_SIM_FS_CHV_SIZE];
    uint8_t unblock_value[GSM_SIM_FS_CHV_SIZE];
    uint8_t attempts;
    uint8_t unblock_attempts;
    bool fulfilled;
} GSMSimCHV;

typedef struct GSMSimFS {
    GSMSimFile *files;
    size_t files_count;
    GSMSimFile *current_directory;
    GSMSimFile *selected_file;
    bool record_selected;
    uint16_t current_record;
    uint8_t response[GSM_SIM_FS_RESPONSE_MAX_SIZE];
    uint8_t response_size;
    GSMSimCHV chv1;
    GSMSimCHV chv2;
    bool chv1_enabled;
} GSMSimFS;

void gsm_sim_fs_init(GSMSimFS *fs, const GSMSimFileDefinition *definitions,
                     size_t count);
void gsm_sim_fs_destroy(GSMSimFS *fs);
void gsm_sim_fs_reset(GSMSimFS *fs);
GSMSimFile *gsm_sim_fs_find(GSMSimFS *fs, uint16_t id, uint16_t parent_id);
void gsm_sim_fs_select(GSMSimFS *fs, uint16_t id, ISO7816Response *response);
void gsm_sim_fs_get_response(GSMSimFS *fs, uint8_t length,
                             ISO7816Response *response);
void gsm_sim_fs_status(GSMSimFS *fs, uint8_t length,
                       ISO7816Response *response);
void gsm_sim_fs_read_binary(GSMSimFS *fs, uint16_t offset, uint16_t length,
                            ISO7816Response *response);
void gsm_sim_fs_update_binary(GSMSimFS *fs, uint16_t offset,
                              const uint8_t *data, uint16_t length,
                              ISO7816Response *response);
void gsm_sim_fs_read_record(GSMSimFS *fs, uint8_t record, uint8_t mode,
                            uint16_t length, ISO7816Response *response);
void gsm_sim_fs_update_record(GSMSimFS *fs, uint8_t record, uint8_t mode,
                              const uint8_t *data, uint16_t length,
                              ISO7816Response *response);
void gsm_sim_fs_seek(GSMSimFS *fs, uint8_t type_mode, const uint8_t *pattern,
                     uint16_t length, ISO7816Response *response);
void gsm_sim_fs_increase(GSMSimFS *fs, const uint8_t *value, uint16_t length,
                         ISO7816Response *response);
void gsm_sim_fs_invalidate(GSMSimFS *fs, ISO7816Response *response);
void gsm_sim_fs_rehabilitate(GSMSimFS *fs, ISO7816Response *response);
bool gsm_sim_fs_is_chv1_granted(GSMSimFS *fs);
void gsm_sim_fs_verify_chv(GSMSimFS *fs, uint8_t chv_number,
                           const uint8_t *data, uint16_t length,
                           ISO7816Response *response);
void gsm_sim_fs_change_chv(GSMSimFS *fs, uint8_t chv_number,
                           const uint8_t *data, uint16_t length,
                           ISO7816Response *response);
void gsm_sim_fs_disable_chv(GSMSimFS *fs, uint8_t chv_number,
                            const uint8_t *data, uint16_t length,
                            ISO7816Response *response);
void gsm_sim_fs_enable_chv(GSMSimFS *fs, uint8_t chv_number,
                           const uint8_t *data, uint16_t length,
                           ISO7816Response *response);
void gsm_sim_fs_unblock_chv(GSMSimFS *fs, uint8_t chv_number,
                            const uint8_t *data, uint16_t length,
                            ISO7816Response *response);

#endif
