#pragma once

#include "qemu/osdep.h"

const uint8_t *pmb887x_get_brom_image(uint32_t cpu_id, size_t *size);
