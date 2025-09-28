#include "hw/arm/pmb887x/board/keyboard.h"
#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/utils/config.h"

#include "qapi/qapi-types-ui.h"
#include "qapi/error.h"
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
	pmb887x_cfg_section_t *section = pmb887x_cfg_section(board->config, "keyboard", 0, true);
	for (size_t i = 0; i < section->items_count; i++) {
		pmb887x_cfg_item_t *item = &section->items[i];

		char **parts = g_strsplit(item->value, ":", -1);
		if (g_strv_length(parts) != 2) {
			g_strfreev(parts);
			error_report("Invalid [keyboard] config %s=%s", item->key, item->value);
			exit(1);
		}

		uint32_t keycode = 0;
		char **kp_in_arr = g_strsplit(parts[0], ",", -1);
		char **kp_out_arr = g_strsplit(parts[1], ",", -1);

		uint32_t kp_in_arr_len = g_strv_length(kp_in_arr);
		for (uint32_t j = 0; j < kp_in_arr_len; j++)
			keycode |= 1 << strtoll(kp_in_arr[j], NULL, 10);

		uint32_t kp_out_arr_len = g_strv_length(kp_out_arr);
		for (uint32_t j = 0; j < kp_out_arr_len; j++)
			keycode |= 1 << (8 + strtoll(kp_out_arr[j], NULL, 10));

		bool found = false;
		for (size_t j = 0; j < ARRAY_SIZE(keyboard_map); j++) {
			if (strcmp(item->key, keyboard_map[j].name) == 0) {
				board->keymap[keyboard_map[j].id] = keycode;
				found = true;
			}
		}

		g_strfreev(parts);
		g_strfreev(kp_in_arr);
		g_strfreev(kp_out_arr);

		if (!found)
			warn_report("Unknown key '%s' in %s", item->key, section->parent->file);
	}
}
