#pragma once

#include "qemu/osdep.h"

DeviceState *pmb887x_new_cpu_module(const char *name);
DeviceState *pmb887x_new_cpu_module_as(const char *name, const char *type);
void pmb887x_cpu_modules_post_init(void);
