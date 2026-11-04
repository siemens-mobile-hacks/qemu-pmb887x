#pragma once

#include "hw/core/sysbus.h"
#include "hw/smartcard/iso7816.h"

#define TYPE_PMB887X_SIM "pmb887x-sim"
OBJECT_DECLARE_SIMPLE_TYPE(pmb887x_sim_t, PMB887X_SIM)

void pmb887x_sim_connect_slot(pmb887x_sim_t *sim, ISO7816Slot *slot);
