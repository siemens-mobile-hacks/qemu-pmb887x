#pragma once

#include "qemu/osdep.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"

void pmb887x_dump_io(uint32_t addr, uint32_t size, uint32_t value, bool is_write);
void pmb887x_print_dump_io(uint32_t addr, uint32_t size, uint32_t value, bool is_write, uint32_t pc, uint32_t lr);
void pmb887x_io_dump_init(void);
