#include "hw/arm/pmb887x/board/analog.h"

#include "hw/hw.h"
#include "hw/sysbus.h"

#include "hw/arm/pmb887x/adc.h"
#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/utils/toml.h"
#include "hw/arm/pmb887x/utils/tomlc17.h"

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

	DeviceState *adc = qdev_find_recursive(sysbus_get_default(), "ADC");
	g_assert(adc != NULL);

	for (int i = 0; i < ARRAY_SIZE(channels); i++) {
		char config_key[32];
		int ch = channels[i];
		sprintf(config_key, "analog.M_%d", ch);

		toml_datum_t channel_config = toml_table_get(board->config, TOML_TABLE, config_key, false);
		if (channel_config.type == TOML_UNKNOWN)
			continue;

		pmb887x_adc_input_t input = {};
		const char *channel_type = toml_table_get_string(channel_config, "type", NULL, true);

		if (strcmp(channel_type, "resistor") == 0) {
			input.r1 = toml_table_get_uint32(channel_config, "value", 0, true);
			input.type = PMB887X_ADC_INPUT_RESISTOR;
		} else if (strcmp(channel_type, "resistor_divider") == 0) {
			input.r1 = toml_table_get_uint32(channel_config, "r1", 0, true);
			input.r2 = toml_table_get_uint32(channel_config, "r2", 0, true);
			input.value = toml_table_get_uint32(channel_config, "value", 0, true);
			input.type = PMB887X_ADC_INPUT_RESISTOR_DIV;
		} else if (strcmp(channel_type, "voltage") == 0) {
			input.value = toml_table_get_uint32(channel_config, "value", 0, true);
			input.type = PMB887X_ADC_INPUT_VOLTAGE;
		} else {
			error_report("Invalid %s.type: %s", config_key, channel_type);
			exit(EXIT_FAILURE);
		}

		pmb887x_adc_set_input(adc, ch, &input);
	}
}
