#pragma once

#include "qemu/osdep.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"

void pmb887x_dump_io_read(uint32_t addr, uint32_t size, uint32_t value);
void pmb887x_dump_io_write(uint32_t addr, uint32_t size, uint32_t value);
void pmb887x_io_dump_init(void);
