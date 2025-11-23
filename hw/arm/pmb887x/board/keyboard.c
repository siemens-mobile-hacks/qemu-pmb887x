#include "hw/arm/pmb887x/board/keyboard.h"
#include "hw/arm/pmb887x/board/board.h"

#include "hw/arm/pmb887x/utils/toml.h"
#include "hw/arm/pmb887x/utils/tomlc17.h"
#include "qapi/qapi-types-ui.h"
#include "qemu/error-report.h"

static struct {
	char name[64];
	uint32_t id;
} keyboard_map[] = {
	{ "NAV_UP", Q_KEY_CODE_UP },
	{ "NAV_RIGHT", Q_KEY_CODE_RIGHT },
	{ "NAV_CENTER", Q_KEY_CODE_KP_ENTER },
	{ "NAV_CENTER", Q_KEY_CODE_RET },
	{ "NAV_LEFT", Q_KEY_CODE_LEFT },
	{ "NAV_DOWN", Q_KEY_CODE_DOWN },

	{ "SOFT_LEFT", Q_KEY_CODE_F1 },
	{ "SOFT_RIGHT", Q_KEY_CODE_F2 },
	{ "SEND", Q_KEY_CODE_F3 },
	{ "END_CALL", Q_KEY_CODE_F4 },

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
};

void pmb887x_board_keymap_init(void) {
	pmb887x_board_t *board = pmb887x_board();

	toml_datum_t table = toml_table_get(board->config, TOML_TABLE, "keyboard", false);
	if (table.type == TOML_UNKNOWN)
		return;
	for (int i = 0; i < table.u.tab.size; i++) {
		const char *key_name = table.u.tab.key[i];
		uint32_t keycode = 0;
		toml_datum_t arr = toml_table_get(table, TOML_ARRAY, key_name, true);
		if (arr.u.arr.size < 2) {
			warn_report("Invalid key '%s' board config!", key_name);
			continue;
		}

		uint32_t kp_in = toml_array_get_uint32(arr, 0, 0, true);
		keycode |= 1 << kp_in;

		for (int j = 1; j < arr.u.arr.size; j++) {
			uint32_t kp_out = toml_array_get_uint32(arr, j, 0, true);
			keycode |= 1 << (8 + kp_out);
		}

		bool found = false;
		for (size_t j = 0; j < ARRAY_SIZE(keyboard_map); j++) {
			if (strcmp(key_name, keyboard_map[j].name) == 0) {
				board->keymap[keyboard_map[j].id] = keycode;
				found = true;
			}
		}

		if (!found)
			warn_report("Unknown key '%s' in board config!", key_name);
	}
}
