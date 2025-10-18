#include "hw/arm/pmb887x/board/dsp.h"

#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/utils/config.h"
#include "qapi/error.h"

void pmb887x_board_init_dsp(DeviceState *dsp) {
	pmb887x_board_t *board = pmb887x_board();
	uint32_t ram0_value = pmb887x_cfg_get_uint(board->config, "dsp", "ram0_value", -1, true);
	object_property_set_uint(OBJECT(dsp), "ram0_value", ram0_value, &error_fatal);
}
