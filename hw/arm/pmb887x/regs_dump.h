#pragma once

#include "qemu/osdep.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"

void pmb887x_dump_io_read(pmb887x_trace_io_t trace_io, uint32_t addr, uint32_t size, uint32_t value);
void pmb887x_dump_io_write(pmb887x_trace_io_t trace_io, uint32_t addr, uint32_t size, uint32_t value);
void pmb887x_io_dump_init(void);
