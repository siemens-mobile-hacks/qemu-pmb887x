#pragma once

#include "qemu/osdep.h"
#include "qom/object.h"
#include "hw/boards.h"
#include "target/arm/cpu.h"

void pmb887x_init(MachineState *machine, uint32_t board_id);
