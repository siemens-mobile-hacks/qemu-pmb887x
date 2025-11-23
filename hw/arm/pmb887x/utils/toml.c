#include "hw/arm/pmb887x/utils/toml.h"
#include "glib.h"
#include "hw/arm/pmb887x/utils/tomlc17.h"

static const char *toml_get_type_name(toml_type_t type) {
	switch (type) {
		case TOML_UNKNOWN:
			return "TOML_UNKNOWN";
		case TOML_STRING:
			return "TOML_STRING";
		case TOML_INT64:
			return "TOML_INT64";
		case TOML_FP64:
			return "TOML_FP64";
		case TOML_BOOLEAN:
			return "TOML_BOOLEAN";
		case TOML_DATE:
			return "TOML_DATE";
		case TOML_TIME:
			return "TOML_TIME";
		case TOML_DATETIME:
			return "TOML_DATETIME";
		case TOML_DATETIMETZ:
			return "TOML_DATETIMETZ";
		case TOML_ARRAY:
			return "TOML_ARRAY";
		case TOML_TABLE:
			return "TOML_TABLE";
	}
	return "TOML_UNKNOWN";
}

/**
 * Table helpers
 */
toml_datum_t toml_table_get(toml_datum_t table, toml_type_t type, const char *multipart_key, bool required) {
	toml_datum_t value = toml_seek(table, multipart_key);
	if (value.type == TOML_UNKNOWN) {
		if (required) {
			const char *sep = table.loc.key.len ? "." : "";
			error_report("%s: %s%s%s is required!", table.loc.file, table.loc.key.ptr ?: "", sep, multipart_key);
			exit(EXIT_FAILURE);
		}
		return value;
	}
	if (value.type != type) {
		error_report("%s: %s is not %s!", table.loc.file, value.loc.key.ptr, toml_get_type_name(type));
		exit(EXIT_FAILURE);
	}
	return value;
}

bool toml_table_get_bool(toml_datum_t table, const char *multipart_key, bool def, bool required) {
	toml_datum_t value = toml_table_get(table, TOML_BOOLEAN, multipart_key, required);
	if (value.type == TOML_UNKNOWN)
		return def;
	return value.u.boolean;
}

int toml_table_get_int32(toml_datum_t table, const char *multipart_key, int def, bool required) {
	toml_datum_t value = toml_table_get(table, TOML_INT64, multipart_key, required);
	if (value.type == TOML_UNKNOWN)
		return def;
	return value.u.int64;
}

uint32_t toml_table_get_uint32(toml_datum_t table, const char *multipart_key, uint32_t def, bool required) {
	toml_datum_t value = toml_table_get(table, TOML_INT64, multipart_key, required);
	if (value.type == TOML_UNKNOWN)
		return def;
	return value.u.int64;
}

const char *toml_table_get_string(toml_datum_t table, const char *multipart_key, const char *def, bool required) {
	toml_datum_t value = toml_table_get(table, TOML_STRING, multipart_key, required);
	if (value.type == TOML_UNKNOWN)
		return def;
	return value.u.s;
}

/**
 * Array helpers
 */
toml_datum_t toml_array_get(toml_datum_t arr, toml_type_t type, uint32_t index, bool required) {
	assert(arr.type == TOML_ARRAY);
	assert(index < arr.u.arr.size);

	if (arr.type != TOML_ARRAY) {
		error_report("%s: %s is not TOML_ARRAY!", arr.loc.file, arr.loc.key.ptr);
		exit(EXIT_FAILURE);
	}

	if (index >= arr.u.arr.size) {
		if (required) {
			error_report("%s: %s.[%d] is out of range (array size %d)!", arr.loc.file, arr.loc.key.ptr, index, arr.u.arr.size);
			exit(EXIT_FAILURE);
		}
		toml_datum_t unknown = {};
		unknown.type = TOML_UNKNOWN;
		return unknown;
	}

	toml_datum_t value = arr.u.arr.elem[index];
	if (value.type != type) {
		error_report("%s: %s is not %s!", value.loc.file, value.loc.key.ptr, toml_get_type_name(type));
		exit(EXIT_FAILURE);
	}

	return value;
}

bool toml_array_get_bool(toml_datum_t arr, int index, bool def, bool required) {
	toml_datum_t value = toml_array_get(arr, TOML_BOOLEAN, index, required);
	if (value.type == TOML_UNKNOWN)
		return def;
	return value.u.boolean;
}

int toml_array_get_int32(toml_datum_t arr, int index, int def, bool required) {
	toml_datum_t value = toml_array_get(arr, TOML_INT64, index, required);
	if (value.type == TOML_UNKNOWN)
		return def;
	return value.u.int64;
}

uint32_t toml_array_get_uint32(toml_datum_t arr, int index, uint32_t def, bool required) {
	toml_datum_t value = toml_array_get(arr, TOML_INT64, index, required);
	if (value.type == TOML_UNKNOWN)
		return def;
	return value.u.int64;
}

const char *toml_array_get_string(toml_datum_t arr, int index, const char *def, bool required) {
	toml_datum_t value = toml_array_get(arr, TOML_STRING, index, required);
	if (value.type == TOML_UNKNOWN)
		return def;
	return value.u.s;
}
