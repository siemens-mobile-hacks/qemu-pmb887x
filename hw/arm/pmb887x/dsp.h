#pragma once

#include "hw/core/qdev.h"
#include "hw/arm/pmb887x/dsp/config.h"
#include "hw/arm/pmb887x/dsp/signals.h"

#define TYPE_PMB887X_DSP "pmb887x-dsp"

enum {
	PMB887X_DSP_INT_COUNT = 3,
	PMB887X_DSP_MCU_INT_COUNT = 4,
};

void pmb887x_dsp_set_config(DeviceState *dev, const pmb887x_dsp_config_t *config);
void pmb887x_dsp_set_gsm_signal(
	DeviceState *dev,
	pmb887x_dsp_gsm_signal_t signal,
	bool level,
	int64_t timestamp_ns
);
