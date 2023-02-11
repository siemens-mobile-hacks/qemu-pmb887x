#pragma once
#include "qemu/osdep.h"

struct pmb887x_sccu_t;

uint32_t pmb887x_sccu_clc_get(struct pmb887x_sccu_t *p);
void pmb887x_sccu_clc_set(struct pmb887x_sccu_t *p, uint32_t value);
