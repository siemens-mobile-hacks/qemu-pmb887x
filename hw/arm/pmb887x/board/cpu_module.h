#pragma once

#include "qemu/osdep.h"

DeviceState *pmb887x_new_cpu_module(const char *name);
void pmb887x_cpu_modules_post_init(void);
