#pragma once

#include "chardev/char.h"
#include "hw/core/sysbus.h"

#define TYPE_PMB887X_SIM "pmb887x-sim"
OBJECT_DECLARE_SIMPLE_TYPE(pmb887x_sim_t, PMB887X_SIM)

void pmb887x_sim_set_chardev(pmb887x_sim_t *sim, Chardev *chardev, Error **errp);
