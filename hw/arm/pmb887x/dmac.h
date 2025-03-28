#pragma once
#include "qemu/osdep.h"

typedef struct pmb887x_dmac_t pmb887x_dmac_t;

void pmb887x_dmac_request(pmb887x_dmac_t *p, uint32_t per_id, uint32_t size);
