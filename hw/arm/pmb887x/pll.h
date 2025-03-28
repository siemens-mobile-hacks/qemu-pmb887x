#pragma once
#include "qemu/osdep.h"

typedef struct pmb887x_pll_t pmb887x_pll_t;

void pmb887x_pll_add_freq_update_callback(pmb887x_pll_t *p, void (*callback)(void *), void *opaque);

pmb887x_pll_t *pmb887x_pll_get_self(DeviceState *dev);
uint32_t pmb887x_pll_get_fosc(pmb887x_pll_t *p);
uint32_t pmb887x_pll_get_frtc(pmb887x_pll_t *p);
uint32_t pmb887x_pll_get_fsys(pmb887x_pll_t *p);
uint32_t pmb887x_pll_get_fstm(pmb887x_pll_t *p);
uint32_t pmb887x_pll_get_fcpu(pmb887x_pll_t *p);
uint32_t pmb887x_pll_get_fahb(pmb887x_pll_t *p);
uint32_t pmb887x_pll_get_fgptu(pmb887x_pll_t *p);
