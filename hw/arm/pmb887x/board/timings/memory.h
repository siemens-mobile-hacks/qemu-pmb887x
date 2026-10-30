#pragma once

#include "qemu/osdep.h"

void pmb887x_memory_timing_init(CPUState *cpu, DeviceState *cgu, DeviceState *scu);
