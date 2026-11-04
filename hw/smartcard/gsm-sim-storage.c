/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qobject/qbool.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"

#include "gsm-sim-storage.h"

#define GSM_SIM_STORAGE_CHV_PADDING 0xFF
#define GSM_SIM_STORAGE_PIN_MIN_LENGTH 4

static GSMSimFile *gsm_sim_storage_find_directory(GSMSimFS *fs, uint16_t id)
{
    for (size_t i = 0; i < fs->files_count; i++) {
        GSMSimFileStructure structure = fs->files[i].definition->structure;
        bool directory = structure == GSM_SIM_FILE_MF ||
                         structure == GSM_SIM_FILE_DF;

        if (directory && fs->files[i].definition->id == id) {
            return &fs->files[i];
        }
    }
    return NULL;
}

static char *gsm_sim_storage_path(GSMSimFS *fs, const GSMSimFile *file)
{
    g_autofree char *parent_path = NULL;
    GSMSimFile *parent;

    if (file->definition->parent_id == 0) {
        return g_strdup_printf("%04X", file->definition->id);
    }
    parent = gsm_sim_storage_find_directory(fs, file->definition->parent_id);
    parent_path = gsm_sim_storage_path(fs, parent);
    return g_strdup_printf("%s/%04X", parent_path, file->definition->id);
}

static bool gsm_sim_storage_is_saved(const GSMSimFile *file)
{
    GSMSimFileStructure structure = file->definition->structure;

    return structure != GSM_SIM_FILE_MF && structure != GSM_SIM_FILE_DF &&
           !file->definition->generated;
}

static char *gsm_sim_storage_hex(const uint8_t *data, size_t size)
{
    GString *text = g_string_sized_new(2 * size);

    for (size_t i = 0; i < size; i++) {
        g_string_append_printf(text, "%02X", data[i]);
    }
    return g_string_free(text, false);
}

static bool gsm_sim_storage_unhex(const char *text, uint8_t *data, size_t size)
{
    if (strlen(text) != 2 * size) {
        return false;
    }
    for (size_t i = 0; i < size; i++) {
        int high = g_ascii_xdigit_value(text[2 * i]);
        int low = g_ascii_xdigit_value(text[2 * i + 1]);

        if (high < 0 || low < 0) {
            return false;
        }
        data[i] = (high << 4) | low;
    }
    return true;
}

static char *gsm_sim_storage_chv_text(const uint8_t *value)
{
    size_t length = 0;

    while (length < GSM_SIM_FS_CHV_SIZE &&
           value[length] != GSM_SIM_STORAGE_CHV_PADDING) {
        length++;
    }
    return g_strndup((const char *)value, length);
}

static QDict *gsm_sim_storage_save_chv(const GSMSimCHV *chv)
{
    QDict *dict = qdict_new();
    g_autofree char *pin = gsm_sim_storage_chv_text(chv->value);
    g_autofree char *puk = gsm_sim_storage_chv_text(chv->unblock_value);

    qdict_put_str(dict, "pin", pin);
    qdict_put_int(dict, "attempts", chv->attempts);
    qdict_put_str(dict, "puk", puk);
    qdict_put_int(dict, "unblock_attempts", chv->unblock_attempts);
    return dict;
}

static QDict *gsm_sim_storage_save_file(const GSMSimFile *file)
{
    QDict *dict = qdict_new();
    uint8_t record_size = file->definition->record_size;

    qdict_put_bool(dict, "invalidated", file->invalidated);
    if (file->definition->structure == GSM_SIM_FILE_TRANSPARENT) {
        g_autofree char *data = gsm_sim_storage_hex(file->data, file->size);

        qdict_put_str(dict, "data", data);
    } else {
        QList *records = qlist_new();

        for (size_t offset = 0; offset < file->size; offset += record_size) {
            g_autofree char *record = gsm_sim_storage_hex(file->data + offset,
                                                          record_size);

            qlist_append_str(records, record);
        }
        qdict_put(dict, "records", records);
    }
    return dict;
}

GString *gsm_sim_storage_save(GSMSimFS *fs)
{
    g_autoptr(QDict) root = qdict_new();
    QDict *chv1 = gsm_sim_storage_save_chv(&fs->chv1);
    QDict *files = qdict_new();

    qdict_put_bool(chv1, "enabled", fs->chv1_enabled);
    qdict_put(root, "chv1", chv1);
    qdict_put(root, "chv2", gsm_sim_storage_save_chv(&fs->chv2));
    for (size_t i = 0; i < fs->files_count; i++) {
        g_autofree char *path = NULL;

        if (!gsm_sim_storage_is_saved(&fs->files[i])) {
            continue;
        }
        path = gsm_sim_storage_path(fs, &fs->files[i]);
        qdict_put(files, path, gsm_sim_storage_save_file(&fs->files[i]));
    }
    qdict_put(root, "files", files);
    return qobject_to_json_pretty(QOBJECT(root), true);
}

static bool gsm_sim_storage_load_chv_value(const QDict *dict, const char *key,
                                           size_t min_length, uint8_t *value,
                                           Error **errp)
{
    const char *text = qdict_get_try_str(dict, key);
    size_t length = text ? strlen(text) : 0;

    if (!text || length < min_length || length > GSM_SIM_FS_CHV_SIZE ||
        strspn(text, "0123456789") != length) {
        error_setg(errp, "gsm-sim: storage %s must contain %zu to %d digits",
                   key, min_length, GSM_SIM_FS_CHV_SIZE);
        return false;
    }
    memset(value, GSM_SIM_STORAGE_CHV_PADDING, GSM_SIM_FS_CHV_SIZE);
    memcpy(value, text, length);
    return true;
}

static bool gsm_sim_storage_load_counter(const QDict *dict, const char *key,
                                         uint8_t max, uint8_t *counter,
                                         Error **errp)
{
    int64_t value = qdict_get_try_int(dict, key, -1);

    if (value < 0 || value > max) {
        error_setg(errp, "gsm-sim: storage %s must be 0 to %u", key, max);
        return false;
    }
    *counter = value;
    return true;
}

static bool gsm_sim_storage_load_chv(const QDict *root, const char *name,
                                     GSMSimCHV *chv, Error **errp)
{
    QDict *dict = qdict_get_qdict(root, name);

    if (!dict) {
        error_setg(errp, "gsm-sim: storage has no %s", name);
        return false;
    }
    return gsm_sim_storage_load_chv_value(dict, "pin",
                                          GSM_SIM_STORAGE_PIN_MIN_LENGTH,
                                          chv->value, errp) &&
           gsm_sim_storage_load_counter(dict, "attempts",
                                        GSM_SIM_FS_CHV_ATTEMPTS,
                                        &chv->attempts, errp) &&
           gsm_sim_storage_load_chv_value(dict, "puk", GSM_SIM_FS_CHV_SIZE,
                                          chv->unblock_value, errp) &&
           gsm_sim_storage_load_counter(dict, "unblock_attempts",
                                        GSM_SIM_FS_UNBLOCK_CHV_ATTEMPTS,
                                        &chv->unblock_attempts, errp);
}

static bool gsm_sim_storage_load_records(GSMSimFile *file, QList *records,
                                         const char *path, Error **errp)
{
    uint8_t record_size = file->definition->record_size;
    size_t offset = 0;
    QListEntry *entry;

    if (qlist_size(records) * record_size != file->size) {
        error_setg(errp, "gsm-sim: storage %s must have %u records", path,
                   file->size / record_size);
        return false;
    }
    QLIST_FOREACH_ENTRY(records, entry) {
        QString *record = qobject_to(QString, qlist_entry_obj(entry));

        if (!record || !gsm_sim_storage_unhex(qstring_get_str(record),
                                              file->data + offset,
                                              record_size)) {
            error_setg(errp,
                       "gsm-sim: storage %s records must contain %u hex bytes",
                       path, record_size);
            return false;
        }
        offset += record_size;
    }
    return true;
}

static bool gsm_sim_storage_load_file(GSMSimFile *file, const QDict *dict,
                                      const char *path, Error **errp)
{
    QList *records;

    file->invalidated = qdict_get_try_bool(dict, "invalidated", false);
    if (file->definition->structure == GSM_SIM_FILE_TRANSPARENT) {
        const char *data = qdict_get_try_str(dict, "data");

        if (!data || !gsm_sim_storage_unhex(data, file->data, file->size)) {
            error_setg(errp,
                       "gsm-sim: storage %s data must contain %u hex bytes",
                       path, file->size);
            return false;
        }
        return true;
    }

    records = qdict_get_qlist(dict, "records");
    if (!records) {
        error_setg(errp, "gsm-sim: storage %s has no records", path);
        return false;
    }
    return gsm_sim_storage_load_records(file, records, path, errp);
}

static GSMSimFile *gsm_sim_storage_find_file(GSMSimFS *fs, const char *path)
{
    for (size_t i = 0; i < fs->files_count; i++) {
        g_autofree char *file_path = NULL;

        if (!gsm_sim_storage_is_saved(&fs->files[i])) {
            continue;
        }
        file_path = gsm_sim_storage_path(fs, &fs->files[i]);
        if (g_ascii_strcasecmp(file_path, path) == 0) {
            return &fs->files[i];
        }
    }
    return NULL;
}

static bool gsm_sim_storage_load_files(GSMSimFS *fs, const QDict *files,
                                       Error **errp)
{
    for (const QDictEntry *entry = qdict_first(files); entry;
         entry = qdict_next(files, entry)) {
        const char *path = qdict_entry_key(entry);
        QDict *dict = qobject_to(QDict, qdict_entry_value(entry));
        GSMSimFile *file = gsm_sim_storage_find_file(fs, path);

        if (!file) {
            error_setg(errp, "gsm-sim: storage has unknown file %s", path);
            return false;
        }
        if (!dict) {
            error_setg(errp, "gsm-sim: storage %s must be an object", path);
            return false;
        }
        if (!gsm_sim_storage_load_file(file, dict, path, errp)) {
            return false;
        }
    }
    return true;
}

static bool gsm_sim_storage_load_root(GSMSimFS *fs, const QDict *root,
                                      Error **errp)
{
    QDict *chv1;
    QDict *files;

    if (!gsm_sim_storage_load_chv(root, "chv1", &fs->chv1, errp) ||
        !gsm_sim_storage_load_chv(root, "chv2", &fs->chv2, errp)) {
        return false;
    }
    chv1 = qdict_get_qdict(root, "chv1");
    fs->chv1_enabled = qdict_get_try_bool(chv1, "enabled", false);

    files = qdict_get_qdict(root, "files");
    if (!files) {
        error_setg(errp, "gsm-sim: storage has no files");
        return false;
    }
    return gsm_sim_storage_load_files(fs, files, errp);
}

bool gsm_sim_storage_load(GSMSimFS *fs, const char *json, Error **errp)
{
    QObject *object = qobject_from_json(json, errp);
    QDict *root = qobject_to(QDict, object);
    bool loaded;

    if (!object) {
        return false;
    }
    if (!root) {
        error_setg(errp, "gsm-sim: storage must be a JSON object");
        qobject_unref(object);
        return false;
    }
    loaded = gsm_sim_storage_load_root(fs, root, errp);
    qobject_unref(object);
    return loaded;
}
