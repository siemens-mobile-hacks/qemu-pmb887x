#pragma once

#include "qemu/osdep.h"

#define PMB887X_ADC_MAX_INPUTS 10

enum {
	PMB887X_ADC_INPUT_NONE,
	PMB887X_ADC_INPUT_RESISTOR,
	PMB887X_ADC_INPUT_RESISTOR_DIV,
	PMB887X_ADC_INPUT_VOLTAGE,
};

// Hardware ADC inputs
enum {
	PMB887X_ADC_INPUT_M0 = 0,
	PMB887X_ADC_INPUT_M1,
	PMB887X_ADC_INPUT_M2,
	PMB887X_ADC_INPUT_M3,
	PMB887X_ADC_INPUT_M4, // Not connected?
	PMB887X_ADC_INPUT_M5, // Not connected?
	PMB887X_ADC_INPUT_M6, // Not connected?
	PMB887X_ADC_INPUT_M7,
	PMB887X_ADC_INPUT_M8,
	PMB887X_ADC_INPUT_M9,
	PMB887X_ADC_INPUT_M10,
};

typedef struct {
	int type;
	uint32_t r1;
	uint32_t r2;
	uint32_t value;
} pmb887x_adc_input_t;

void pmb887x_adc_set_input(DeviceState *dev, uint32_t n, const pmb887x_adc_input_t *input);
