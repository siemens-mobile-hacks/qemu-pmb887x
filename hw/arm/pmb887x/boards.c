#include "hw/arm/pmb887x/boards.h"

static uint32_t board_cx75_keymap[] = {
	[Q_KEY_CODE_KP_1]	= CX75_KP_NUM1,
	[Q_KEY_CODE_1]		= CX75_KP_NUM1,
	[Q_KEY_CODE_KP_4]	= CX75_KP_NUM4,
	[Q_KEY_CODE_4]		= CX75_KP_NUM4,
	[Q_KEY_CODE_KP_7]	= CX75_KP_NUM7,
	[Q_KEY_CODE_7]		= CX75_KP_NUM7,
	[Q_KEY_CODE_KP_2]	= CX75_KP_NUM2,
	[Q_KEY_CODE_2]		= CX75_KP_NUM2,
	[Q_KEY_CODE_KP_5]	= CX75_KP_NUM5,
	[Q_KEY_CODE_5]		= CX75_KP_NUM5,
	[Q_KEY_CODE_KP_8]	= CX75_KP_NUM8,
	[Q_KEY_CODE_8]		= CX75_KP_NUM8,
	[Q_KEY_CODE_KP_3]	= CX75_KP_NUM3,
	[Q_KEY_CODE_3]		= CX75_KP_NUM3,
	[Q_KEY_CODE_KP_6]	= CX75_KP_NUM6,
	[Q_KEY_CODE_6]		= CX75_KP_NUM6,
	[Q_KEY_CODE_KP_9]	= CX75_KP_NUM9,
	[Q_KEY_CODE_9]		= CX75_KP_NUM9,
};

static pmb887x_fixed_gpio_t board_cx75_fixed_gpio[] = {
};

static uint32_t board_cx75_flashes[] = {
	0x0089880D,
};

static uint32_t board_el71_keymap[] = {
	[Q_KEY_CODE_KP_1]			= EL71_KP_NUM1,
	[Q_KEY_CODE_1]				= EL71_KP_NUM1,
	[Q_KEY_CODE_KP_4]			= EL71_KP_NUM4,
	[Q_KEY_CODE_4]				= EL71_KP_NUM4,
	[Q_KEY_CODE_KP_7]			= EL71_KP_NUM7,
	[Q_KEY_CODE_7]				= EL71_KP_NUM7,
	[Q_KEY_CODE_KP_DIVIDE]		= EL71_KP_STAR,
	[Q_KEY_CODE_KP_2]			= EL71_KP_NUM2,
	[Q_KEY_CODE_2]				= EL71_KP_NUM2,
	[Q_KEY_CODE_KP_5]			= EL71_KP_NUM5,
	[Q_KEY_CODE_5]				= EL71_KP_NUM5,
	[Q_KEY_CODE_KP_8]			= EL71_KP_NUM8,
	[Q_KEY_CODE_8]				= EL71_KP_NUM8,
	[Q_KEY_CODE_KP_0]			= EL71_KP_NUM0,
	[Q_KEY_CODE_0]				= EL71_KP_NUM0,
	[Q_KEY_CODE_KP_3]			= EL71_KP_NUM3,
	[Q_KEY_CODE_3]				= EL71_KP_NUM3,
	[Q_KEY_CODE_KP_6]			= EL71_KP_NUM6,
	[Q_KEY_CODE_6]				= EL71_KP_NUM6,
	[Q_KEY_CODE_KP_9]			= EL71_KP_NUM9,
	[Q_KEY_CODE_9]				= EL71_KP_NUM9,
	[Q_KEY_CODE_KP_MULTIPLY]	= EL71_KP_HASH,
	[Q_KEY_CODE_UP]				= EL71_KP_NAV_UP,
	[Q_KEY_CODE_DOWN]			= EL71_KP_NAV_RIGHT,
	[Q_KEY_CODE_RET]			= EL71_KP_NAV_CENTER,
	[Q_KEY_CODE_LEFT]			= EL71_KP_NAV_LEFT,
	[Q_KEY_CODE_DOWN]			= EL71_KP_NAV_DOWN,
	[Q_KEY_CODE_F3]				= EL71_KP_SEND,
	[Q_KEY_CODE_F5]				= EL71_KP_MUSIC,
	[Q_KEY_CODE_F6]				= EL71_KP_PLAY_PAUSE,
	[Q_KEY_CODE_F1]				= EL71_KP_SOFT_LEFT,
	[Q_KEY_CODE_F2]				= EL71_KP_SOFT_RIGHT,
	[Q_KEY_CODE_F4]				= EL71_KP_END_CALL,
	[Q_KEY_CODE_F8]				= EL71_KP_CAMERA,
	[Q_KEY_CODE_F7]				= EL71_KP_PTT,
	[Q_KEY_CODE_KP_ADD]			= EL71_KP_VOLUME_UP,
	[Q_KEY_CODE_KP_SUBTRACT]	= EL71_KP_VOLUME_DOWN,
};

static pmb887x_fixed_gpio_t board_el71_fixed_gpio[] = {
	{EL71_GPIO_HW_DET_MOB_TYPE3,	0},
	{EL71_GPIO_HW_DET_MOB_TYPE2,	0},
	{EL71_GPIO_HW_DET_MOB_TYPE1,	0},
	{EL71_GPIO_HW_DET_MOB_TYPE4,	1},
	{EL71_GPIO_HW_DET_BLUETOOTH,	0},
	{EL71_GPIO_HW_DET_BAND_SEL,		0},
};

static uint32_t board_el71_flashes[] = {
	0x00208819,
};

static pmb887x_board_t boards_list[] = {
	{"CX75",	CPU_PMB8875,	board_cx75_flashes,	ARRAY_SIZE(board_cx75_flashes),	board_cx75_keymap,	ARRAY_SIZE(board_cx75_keymap),	board_cx75_fixed_gpio,	ARRAY_SIZE(board_cx75_fixed_gpio)},
	{"EL71",	CPU_PMB8876,	board_el71_flashes,	ARRAY_SIZE(board_el71_flashes),	board_el71_keymap,	ARRAY_SIZE(board_el71_keymap),	board_el71_fixed_gpio,	ARRAY_SIZE(board_el71_fixed_gpio)},
};

pmb887x_board_t *pmb887x_get_board(int board) {
	return &boards_list[board];
}

