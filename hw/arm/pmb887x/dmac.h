#pragma once
#include "qemu/osdep.h"

#define TYPE_PMB887X_DMAC	"pmb887x-dmac"
OBJECT_DECLARE_SIMPLE_TYPE(pmb887x_dmac_t, PMB887X_DMAC);

void pmb887x_dmac_set_sel(pmb887x_dmac_t *p, uint32_t value);
uint32_t pmb887x_dmac_get_sel(pmb887x_dmac_t *p);
