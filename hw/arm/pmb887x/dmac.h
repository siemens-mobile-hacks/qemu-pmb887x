#pragma once
#include "qemu/osdep.h"

#define TYPE_PMB887X_DMAC	"pmb887x-dmac"
OBJECT_DECLARE_SIMPLE_TYPE(pmb887x_dmac_t, PMB887X_DMAC);

bool pmb887x_dmac_is_busy(pmb887x_dmac_t *p);
void pmb887x_dmac_request(pmb887x_dmac_t *p, uint32_t per_id, uint32_t size);
