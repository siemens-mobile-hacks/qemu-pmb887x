#pragma once
#include "qemu/osdep.h"

struct pmb887x_dmac_t;
typedef struct pmb887x_dmac_t pmb887x_dmac_t;

void pmb887x_dmac_request(pmb887x_dmac_t *p, int per_id, uint32_t size);
