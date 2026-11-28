#include "hw/arm/pmb887x/board/keyboard.h"

#include "hw/arm/pmb887x/utils/toml.h"
#include "tomlc17.h"
#include "hw/core/qdev-properties.h"
#include "qobject/qlist.h"

typedef struct keyboard_key_t keyboard_key_t;

struct keyboard_key_t {
	const char *name;
	QKeyCode qcode;
};

static const keyboard_key_t keyboard_map[] = {
	{ "NAV_UP", Q_KEY_CODE_UP },
	{ "NAV_RIGHT", Q_KEY_CODE_RIGHT },
	{ "NAV_CENTER", Q_KEY_CODE_KP_ENTER },
	{ "NAV_LEFT", Q_KEY_CODE_LEFT },
	{ "NAV_DOWN", Q_KEY_CODE_DOWN },

	{ "SOFT_LEFT", Q_KEY_CODE_F1 },
	{ "SOFT_RIGHT", Q_KEY_CODE_F2 },
	{ "SEND", Q_KEY_CODE_F3 },
	{ "END_CALL", Q_KEY_CODE_F4 },
	{ "CLEAR", Q_KEY_CODE_BACKSPACE },

	{ "MUSIC", Q_KEY_CODE_F5 },
	{ "PLAY_PAUSE", Q_KEY_CODE_F6 },
	{ "PTT", Q_KEY_CODE_F7 },
	{ "CAMERA", Q_KEY_CODE_F8 },
	{ "BROWSER", Q_KEY_CODE_F9 },

	{ "VOLUME_UP", Q_KEY_CODE_KP_ADD },
	{ "VOLUME_DOWN", Q_KEY_CODE_KP_SUBTRACT },

	{ "NUM0", Q_KEY_CODE_KP_0 },
	{ "NUM0", Q_KEY_CODE_0 },

	{ "NUM1", Q_KEY_CODE_KP_7 },
	{ "NUM2", Q_KEY_CODE_KP_8 },
	{ "NUM3", Q_KEY_CODE_KP_9 },
	{ "NUM1", Q_KEY_CODE_1 },
	{ "NUM2", Q_KEY_CODE_2 },
	{ "NUM3", Q_KEY_CODE_3 },

	{ "NUM4", Q_KEY_CODE_KP_4 },
	{ "NUM5", Q_KEY_CODE_KP_5 },
	{ "NUM6", Q_KEY_CODE_KP_6 },
	{ "NUM4", Q_KEY_CODE_4 },
	{ "NUM5", Q_KEY_CODE_5 },
	{ "NUM6", Q_KEY_CODE_6 },

	{ "NUM7", Q_KEY_CODE_KP_1 },
	{ "NUM8", Q_KEY_CODE_KP_2 },
	{ "NUM9", Q_KEY_CODE_KP_3 },
	{ "NUM7", Q_KEY_CODE_7 },
	{ "NUM8", Q_KEY_CODE_8 },
	{ "NUM9", Q_KEY_CODE_9 },

	{ "STAR", Q_KEY_CODE_KP_MULTIPLY },
	{ "HASH", Q_KEY_CODE_KP_DIVIDE },

	{ "QWERTY_A", Q_KEY_CODE_A },
	{ "QWERTY_B", Q_KEY_CODE_B },
	{ "QWERTY_C", Q_KEY_CODE_C },
	{ "QWERTY_D", Q_KEY_CODE_D },
	{ "QWERTY_E", Q_KEY_CODE_E },
	{ "QWERTY_F", Q_KEY_CODE_F },
	{ "QWERTY_G", Q_KEY_CODE_G },
	{ "QWERTY_H", Q_KEY_CODE_H },
	{ "QWERTY_I", Q_KEY_CODE_I },
	{ "QWERTY_J", Q_KEY_CODE_J },
	{ "QWERTY_K", Q_KEY_CODE_K },
	{ "QWERTY_L", Q_KEY_CODE_L },
	{ "QWERTY_M", Q_KEY_CODE_M },
	{ "QWERTY_N", Q_KEY_CODE_N },
	{ "QWERTY_O", Q_KEY_CODE_O },
	{ "QWERTY_P", Q_KEY_CODE_P },
	{ "QWERTY_Q", Q_KEY_CODE_Q },
	{ "QWERTY_R", Q_KEY_CODE_R },
	{ "QWERTY_S", Q_KEY_CODE_S },
	{ "QWERTY_T", Q_KEY_CODE_T },
	{ "QWERTY_U", Q_KEY_CODE_U },
	{ "QWERTY_V", Q_KEY_CODE_V },
	{ "QWERTY_W", Q_KEY_CODE_W },
	{ "QWERTY_X", Q_KEY_CODE_X },
	{ "QWERTY_Y", Q_KEY_CODE_Y },
	{ "QWERTY_Z", Q_KEY_CODE_Z },
	{ "QWERTY_SPACE", Q_KEY_CODE_SPC },
	{ "QWERTY_SPACE_BACKSPACE", Q_KEY_CODE_DELETE },
	{ "QWERTY_BACKSPACE", Q_KEY_CODE_BACKSPACE },
	{ "QWERTY_ENTER", Q_KEY_CODE_RET },
	{ "QWERTY_SHIFT_LEFT", Q_KEY_CODE_SHIFT },
	{ "QWERTY_SHIFT_RIGHT", Q_KEY_CODE_SHIFT_R },
	{ "QWERTY_ALT", Q_KEY_CODE_ALT },
	{ "QWERTY_BLUE", Q_KEY_CODE_CTRL },
	{ "QWERTY_COMMA", Q_KEY_CODE_COMMA },
	{ "QWERTY_DOT", Q_KEY_CODE_DOT },
	{ "QWERTY_MINUS", Q_KEY_CODE_MINUS },
	{ "QWERTY_QUESTION", Q_KEY_CODE_SLASH },
};

bool pmb887x_board_find_keycode(const char *name, QKeyCode *qcode) {
	for (size_t i = 0; i < ARRAY_SIZE(keyboard_map); i++) {
		if (strcmp(name, keyboard_map[i].name) == 0) {
			*qcode = keyboard_map[i].qcode;
			return true;
		}
	}
	return false;
}

static size_t keyboard_append_key_codes(QList *codes, const char *name, toml_datum_t where) {
	size_t count = 0;
	for (size_t i = 0; i < ARRAY_SIZE(keyboard_map); i++) {
		if (strcmp(name, keyboard_map[i].name) == 0) {
			qlist_append_int(codes, keyboard_map[i].qcode);
			count++;
		}
	}
	if (count == 0)
		toml_fail(where, "unknown key '%s'", name);
	return count;
}

static void keyboard_add_contacts(QList *codes, QList *rows, QList *columns, const char *name, toml_datum_t contact) {
	if (contact.u.arr.size != 2)
		toml_fail(contact, "key '%s' must be [out, in]", name);
	uint32_t row = toml_array_get_uint32(contact, 0, 0, true);
	uint32_t column = toml_array_get_uint32(contact, 1, 0, true);

	size_t count = keyboard_append_key_codes(codes, name, contact);
	for (size_t i = 0; i < count; i++) {
		qlist_append_int(rows, row);
		qlist_append_int(columns, column);
	}
}

void pmb887x_board_key_matrix_configure(DeviceState *dev, toml_datum_t table) {
	toml_datum_t keys = toml_table_get(table, TOML_TABLE, "keys", true);
	QList *codes = qlist_new();
	QList *rows = qlist_new();
	QList *columns = qlist_new();

	for (int i = 0; i < keys.u.tab.size; i++) {
		const char *name = keys.u.tab.key[i];
		toml_datum_t value = toml_table_get(keys, TOML_ARRAY, name, true);
		bool has_several_contacts = value.u.arr.size > 0 && value.u.arr.elem[0].type == TOML_ARRAY;

		if (!has_several_contacts) {
			keyboard_add_contacts(codes, rows, columns, name, value);
			continue;
		}
		for (int j = 0; j < value.u.arr.size; j++)
			keyboard_add_contacts(codes, rows, columns, name, toml_array_get(value, TOML_ARRAY, j, true));
	}
	qdev_prop_set_array(dev, "key-codes", codes);
	qdev_prop_set_array(dev, "key-rows", rows);
	qdev_prop_set_array(dev, "key-columns", columns);
}

void pmb887x_board_key_switch_configure(DeviceState *dev, toml_datum_t table) {
	const char *name = toml_table_get_string(table, "key", NULL, true);
	uint32_t level = toml_table_get_uint32(table, "level", 0, true);
	if (level > 1)
		toml_fail(table, "key switch level must be 0 or 1");

	QList *codes = qlist_new();
	keyboard_append_key_codes(codes, name, table);
	qdev_prop_set_array(dev, "key-codes", codes);
	qdev_prop_set_bit(dev, "level", level);
}
