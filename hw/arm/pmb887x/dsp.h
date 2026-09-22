#pragma once

#include "hw/core/qdev.h"

#include "hw/arm/pmb887x/dsp/config.h"
#include "hw/arm/pmb887x/dsp/signals.h"

enum {
	PMB887X_DSP_INT_COUNT = 3,
	PMB887X_DSP_MCU_INT_COUNT = 4,
};

#define TYPE_PMB887X_DSP_STUB	"pmb887x-dsp-stub"

void pmb887x_dsp_set_config(DeviceState *dev, const pmb887x_dsp_config_t *config);
void pmb887x_dsp_stub_set_config(DeviceState *dev, const pmb887x_dsp_config_t *config);
