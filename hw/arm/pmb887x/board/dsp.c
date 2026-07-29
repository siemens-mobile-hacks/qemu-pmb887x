#include "hw/arm/pmb887x/board/dsp.h"

#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/utils/toml.h"
#include "qapi/error.h"

void pmb887x_board_init_dsp(DeviceState *dsp) {
	pmb887x_board_t *board = pmb887x_board();
	uint32_t rom_version = toml_table_get_uint32(board->config, "dsp.rom_version", 0, false);
	if (rom_version == 0)
		rom_version = toml_table_get_uint32(board->config, "dsp.ram0_value", 0, false);
	object_property_set_uint(OBJECT(dsp), "rom_version", rom_version, &error_fatal);
}
