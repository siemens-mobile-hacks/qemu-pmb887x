#pragma once

#include "qemu/osdep.h"
#define PMB887X_IO_BRIDGE false

void pmb8876_io_bridge_init(void);
void pmb8876_io_bridge_write(unsigned int addr, unsigned int size, unsigned int value);
unsigned int pmb8876_io_bridge_read(unsigned int addr, unsigned int size);
void pmb8876_io_bridge_set_vic(DeviceState *cpu);
