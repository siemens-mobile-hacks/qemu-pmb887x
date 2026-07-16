#pragma once

#include "chardev/char.h"
#include "hw/core/qdev.h"

#define TYPE_PMB887X_SIM_CARD "sim-card"
OBJECT_DECLARE_SIMPLE_TYPE(pmb887x_sim_card_t, PMB887X_SIM_CARD)

Chardev *pmb887x_sim_card_get_chardev(pmb887x_sim_card_t *card);
