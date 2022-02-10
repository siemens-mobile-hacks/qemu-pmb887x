#pragma once
#include "qemu/osdep.h"

struct pmb887x_pll_t;

void pmb887x_pll_add_freq_update_callback(struct pmb887x_pll_t *p, void (*callback)(void *), void *opaque);
uint32_t pmb887x_pll_get_fsys(struct pmb887x_pll_t *p);
