#pragma once

#include "hw/core/qdev.h"

#include "hw/arm/pmb887x/dsp/config.h"
#include "hw/arm/pmb887x/dsp/signals.h"

typedef struct pmb887x_rf_iq_source_t pmb887x_rf_iq_source_t;

enum {
	PMB887X_DSP_INT_COUNT = 3,
	PMB887X_DSP_MCU_INT_COUNT = 4,
};

void pmb887x_dsp_set_config(DeviceState *dev, const pmb887x_dsp_config_t *config);
void pmb887x_dsp_set_iq_source(DeviceState *dev, pmb887x_rf_iq_source_t *source);
