/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"

#include "../gsm-sim-storage.h"

#define TEST_DF 0x7F10
#define TEST_EF_TRANSPARENT 0x6F43
#define TEST_EF_RECORDS 0x6F3A
#define TEST_EF_GENERATED 0x6F07

static const GSMSimFileDefinition TEST_FILES[] = {
    { .id = GSM_SIM_FS_MF_ID, .structure = GSM_SIM_FILE_MF },
    { .id = TEST_DF, .parent_id = GSM_SIM_FS_MF_ID,
      .structure = GSM_SIM_FILE_DF },
    { .id = TEST_EF_TRANSPARENT, .parent_id = TEST_DF,
      .structure = GSM_SIM_FILE_TRANSPARENT, .size = 2, .fill = 0xFF },
    { .id = TEST_EF_RECORDS, .parent_id = TEST_DF,
      .structure = GSM_SIM_FILE_LINEAR_FIXED, .size = 4, .record_size = 2,
      .fill = 0xFF },
    { .id = TEST_EF_GENERATED, .parent_id = TEST_DF,
      .structure = GSM_SIM_FILE_TRANSPARENT, .size = 1, .fill = 0x11,
      .generated = true },
};

static const char FIXTURE[] =
    "{"
    "  \"chv1\": { \"enabled\": true, \"pin\": \"1234\", \"attempts\": 2,"
    "            \"puk\": \"87654321\", \"unblock_attempts\": 9 },"
    "  \"chv2\": { \"pin\": \"56789\", \"attempts\": 3,"
    "            \"puk\": \"11112222\", \"unblock_attempts\": 10 },"
    "  \"files\": {"
    "    \"3F00/7F10/6F43\": { \"invalidated\": false, \"data\": \"01fe\" },"
    "    \"3F00/7F10/6F3A\": { \"invalidated\": true,"
    "                        \"records\": [\"A1A2\", \"B1B2\"] }"
    "  }"
    "}";

static const uint8_t CHV1_VALUE[GSM_SIM_FS_CHV_SIZE] = {
    '1', '2', '3', '4', 0xFF, 0xFF, 0xFF, 0xFF
};
static const uint8_t CHV2_VALUE[GSM_SIM_FS_CHV_SIZE] = {
    '5', '6', '7', '8', '9', 0xFF, 0xFF, 0xFF
};
static const uint8_t CHV1_UNBLOCK_VALUE[GSM_SIM_FS_CHV_SIZE] = {
    '8', '7', '6', '5', '4', '3', '2', '1'
};
static const uint8_t TRANSPARENT_DATA[] = { 0x01, 0xFE };
static const uint8_t RECORDS_DATA[] = { 0xA1, 0xA2, 0xB1, 0xB2 };

static void test_load_fixture(void)
{
    GSMSimFS fs = { 0 };
    GSMSimFile *transparent;
    GSMSimFile *records;

    gsm_sim_fs_init(&fs, TEST_FILES, ARRAY_SIZE(TEST_FILES));
    g_assert_true(gsm_sim_storage_load(&fs, FIXTURE, &error_abort));

    g_assert_true(fs.chv1_enabled);
    g_assert_cmpmem(fs.chv1.value, GSM_SIM_FS_CHV_SIZE, CHV1_VALUE,
                    sizeof(CHV1_VALUE));
    g_assert_cmpmem(fs.chv1.unblock_value, GSM_SIM_FS_CHV_SIZE,
                    CHV1_UNBLOCK_VALUE, sizeof(CHV1_UNBLOCK_VALUE));
    g_assert_cmpuint(fs.chv1.attempts, ==, 2);
    g_assert_cmpuint(fs.chv1.unblock_attempts, ==, 9);
    g_assert_cmpmem(fs.chv2.value, GSM_SIM_FS_CHV_SIZE, CHV2_VALUE,
                    sizeof(CHV2_VALUE));

    transparent = gsm_sim_fs_find(&fs, TEST_EF_TRANSPARENT, TEST_DF);
    g_assert_cmpmem(transparent->data, transparent->size, TRANSPARENT_DATA,
                    sizeof(TRANSPARENT_DATA));
    g_assert_false(transparent->invalidated);
    records = gsm_sim_fs_find(&fs, TEST_EF_RECORDS, TEST_DF);
    g_assert_cmpmem(records->data, records->size, RECORDS_DATA,
                    sizeof(RECORDS_DATA));
    g_assert_true(records->invalidated);
    gsm_sim_fs_destroy(&fs);
}

static void test_save_skips_generated_files(void)
{
    GSMSimFS fs = { 0 };
    g_autoptr(GString) json = NULL;
    g_autoptr(QDict) root = NULL;
    QDict *files;
    QDict *records;

    gsm_sim_fs_init(&fs, TEST_FILES, ARRAY_SIZE(TEST_FILES));
    g_assert_true(gsm_sim_storage_load(&fs, FIXTURE, &error_abort));
    json = gsm_sim_storage_save(&fs);
    root = qobject_to(QDict, qobject_from_json(json->str, &error_abort));
    files = qdict_get_qdict(root, "files");

    g_assert_cmpint(qdict_size(files), ==, 2);
    g_assert_false(qdict_haskey(files, "3F00/7F10/6F07"));
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(files, "3F00/7F10/6F43"),
                                  "data"), ==, "01FE");
    records = qdict_get_qdict(files, "3F00/7F10/6F3A");
    g_assert_true(qdict_get_bool(records, "invalidated"));
    g_assert_cmpuint(qlist_size(qdict_get_qlist(records, "records")), ==, 2);
    gsm_sim_fs_destroy(&fs);
}

static void test_reject_wrong_record_count(void)
{
    static const char json[] =
        "{ \"chv1\": { \"pin\": \"0000\", \"attempts\": 3,"
        "              \"puk\": \"12345678\", \"unblock_attempts\": 10 },"
        "  \"chv2\": { \"pin\": \"0000\", \"attempts\": 3,"
        "              \"puk\": \"12345678\", \"unblock_attempts\": 10 },"
        "  \"files\": { \"3F00/7F10/6F3A\": { \"records\": [\"A1A2\"] } } }";
    GSMSimFS fs = { 0 };
    Error *err = NULL;

    gsm_sim_fs_init(&fs, TEST_FILES, ARRAY_SIZE(TEST_FILES));
    g_assert_false(gsm_sim_storage_load(&fs, json, &err));
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "gsm-sim: storage 3F00/7F10/6F3A must have 2 records");
    error_free(err);
    gsm_sim_fs_destroy(&fs);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/smartcard/gsm-sim-storage/load-fixture",
                    test_load_fixture);
    g_test_add_func("/smartcard/gsm-sim-storage/save-skips-generated-files",
                    test_save_skips_generated_files);
    g_test_add_func("/smartcard/gsm-sim-storage/reject-wrong-record-count",
                    test_reject_wrong_record_count);
    return g_test_run();
}
