#include "hw/arm/pmb887x/board/analog.h"

#include "hw/arm/pmb887x/adc.h"
#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/utils/config.h"
#include "hw/arm/pmb887x/utils/regexp.h"

#include "qemu/error-report.h"
#include "hw/hw.h"
#include "hw/sysbus.h"

static const int channels[] = {
	PMB887X_ADC_INPUT_M0,
	PMB887X_ADC_INPUT_M1,
	PMB887X_ADC_INPUT_M2,
	PMB887X_ADC_INPUT_M3,
	PMB887X_ADC_INPUT_M4,
	PMB887X_ADC_INPUT_M5,
	PMB887X_ADC_INPUT_M6,
	PMB887X_ADC_INPUT_M7,
	PMB887X_ADC_INPUT_M8,
	PMB887X_ADC_INPUT_M9,
	PMB887X_ADC_INPUT_M10,
};

void pmb887x_board_init_analog(void) {
	pmb887x_board_t *board = pmb887x_board();
	pmb887x_cfg_section_t *section = pmb887x_cfg_section(board->config, "analog", 0, true);

	DeviceState *adc = qdev_find_recursive(sysbus_get_default(), "ADC");
	g_assert(adc != NULL);

	g_autoptr(GRegex) resistor_regexp = g_regex_new("^resistor,(\\d+)$", G_REGEX_CASELESS, 0, NULL);
	g_autoptr(GRegex) resistor_divider_regexp = g_regex_new("^resistor_divider,(\\d+),(\\d+),(-?\\d+)$", G_REGEX_CASELESS, 0, NULL);
	g_autoptr(GRegex) voltage_regexp = g_regex_new("^(-?\\d+)$", G_REGEX_CASELESS, 0, NULL);

	for (int i = 0; i < ARRAY_SIZE(channels); i++) {
		char name[32];
		int ch = channels[i];
		sprintf(name, "M_%d", ch);

		const char *value = pmb887x_cfg_section_get(section, name, NULL, false);
		if (!value)
			continue;

		g_autoptr(GMatchInfo) resistor_match = regexp_match(resistor_regexp, value);
		g_autoptr(GMatchInfo) resistor_divider_match = regexp_match(resistor_divider_regexp, value);
		g_autoptr(GMatchInfo) voltage_match = regexp_match(voltage_regexp, value);

		pmb887x_adc_input_t input = {};
		if (resistor_match) {
			input.r1 = strtol(g_match_info_fetch(resistor_match, 1), NULL, 10);
			input.type = PMB887X_ADC_INPUT_RESISTOR;
		} else if (resistor_divider_match) {
			input.r1 = strtol(g_match_info_fetch(resistor_divider_match, 1), NULL, 10);
			input.r2 = strtol(g_match_info_fetch(resistor_divider_match, 2), NULL, 10);
			input.value = strtol(g_match_info_fetch(resistor_divider_match, 3), NULL, 10);
			input.type = PMB887X_ADC_INPUT_RESISTOR_DIV;
		} else if (voltage_match) {
			input.value = strtol(g_match_info_fetch(voltage_match, 1), NULL, 10);
			input.type = PMB887X_ADC_INPUT_VOLTAGE;
		} else {
			hw_error("Invalid ADC channel value: %s", value);
		}
		pmb887x_adc_set_input(adc, ch, &input);
	}
}
