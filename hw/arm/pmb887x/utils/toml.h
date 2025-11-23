#pragma once
#include "qemu/osdep.h"
#include "qapi/qapi-types-ui.h"
#include "qemu/error-report.h"
#include "hw/arm/pmb887x/utils/tomlc17.h"

toml_datum_t toml_table_get(toml_datum_t table, toml_type_t type, const char *multipart_key, bool required);
bool toml_table_get_bool(toml_datum_t table, const char *multipart_key, bool def, bool required);
int toml_table_get_int32(toml_datum_t table, const char *multipart_key, int def, bool required);
uint32_t toml_table_get_uint32(toml_datum_t table, const char *multipart_key, uint32_t def, bool required);
const char *toml_table_get_string(toml_datum_t table, const char *multipart_key, const char *def, bool required);

toml_datum_t toml_array_get(toml_datum_t arr, toml_type_t type, uint32_t index, bool required);
int toml_array_get_int32(toml_datum_t arr, int index, int def, bool required);
uint32_t toml_array_get_uint32(toml_datum_t arr, int index, uint32_t def, bool required);
bool toml_array_get_bool(toml_datum_t arr, int index, bool def, bool required);
const char *toml_array_get_string(toml_datum_t arr, int index, const char *def, bool required);
