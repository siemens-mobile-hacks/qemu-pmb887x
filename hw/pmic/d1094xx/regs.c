#include "qemu/osdep.h"
#include "regs.h"

static const DebugRegValue d1094xx_identification_fields_vendor_values[] = {
    { "ST",     D1094XX_IDENTIFICATION_VENDOR_ST },
    { "DIALOG", D1094XX_IDENTIFICATION_VENDOR_DIALOG },
};

static const DebugRegField d1094xx_identification_fields[] = {
    { "MODEL",      D1094XX_IDENTIFICATION_MODEL,       D1094XX_IDENTIFICATION_MODEL_SHIFT,     NULL,                                           0 },
    { "REVISION",   D1094XX_IDENTIFICATION_REVISION,    D1094XX_IDENTIFICATION_REVISION_SHIFT,  NULL,                                           0 },
    { "VENDOR",     D1094XX_IDENTIFICATION_VENDOR,      D1094XX_IDENTIFICATION_VENDOR_SHIFT,    d1094xx_identification_fields_vendor_values,    ARRAY_SIZE(d1094xx_identification_fields_vendor_values) },
};

static const DebugRegField d1094xx_irq_status_1_fields[] = {
    { "OVER_TEMP",      D1094XX_IRQ_STATUS_1_OVER_TEMP,     D1094XX_IRQ_STATUS_1_OVER_TEMP_SHIFT,       NULL,   0 },
    { "CHARGER_EVENT",  D1094XX_IRQ_STATUS_1_CHARGER_EVENT, D1094XX_IRQ_STATUS_1_CHARGER_EVENT_SHIFT,   NULL,   0 },
    { "UV_AUDIO_REGA",  D1094XX_IRQ_STATUS_1_UV_AUDIO_REGA, D1094XX_IRQ_STATUS_1_UV_AUDIO_REGA_SHIFT,   NULL,   0 },
};

static const DebugRegField d1094xx_irq_status_2_fields[] = {
    { "UV_SIM_REGA",    D1094XX_IRQ_STATUS_2_UV_SIM_REGA,   D1094XX_IRQ_STATUS_2_UV_SIM_REGA_SHIFT,     NULL,   0 },
    { "SUPPLY_SHORT",   D1094XX_IRQ_STATUS_2_SUPPLY_SHORT,  D1094XX_IRQ_STATUS_2_SUPPLY_SHORT_SHIFT,    NULL,   0 },
    { "UNEXP_CHARGE",   D1094XX_IRQ_STATUS_2_UNEXP_CHARGE,  D1094XX_IRQ_STATUS_2_UNEXP_CHARGE_SHIFT,    NULL,   0 },
    { "VBATT_OV",       D1094XX_IRQ_STATUS_2_VBATT_OV,      D1094XX_IRQ_STATUS_2_VBATT_OV_SHIFT,        NULL,   0 },
};

static const DebugRegField d1094xx_irq_mask_1_fields[] = {
    { "VALUE",  D1094XX_IRQ_MASK_1_VALUE,   D1094XX_IRQ_MASK_1_VALUE_SHIFT, NULL,   0 },
};

static const DebugRegField d1094xx_irq_mask_2_fields[] = {
    { "VALUE",  D1094XX_IRQ_MASK_2_VALUE,   D1094XX_IRQ_MASK_2_VALUE_SHIFT, NULL,   0 },
};

static const DebugRegValue d1094xx_turnoff_reason_fields_value_values[] = {
    { "UNDEFINED",              D1094XX_TURNOFF_REASON_VALUE_UNDEFINED },
    { "NO_REASON_STORED",       D1094XX_TURNOFF_REASON_VALUE_NO_REASON_STORED },
    { "UNDERVOLTAGE_VBATT",     D1094XX_TURNOFF_REASON_VALUE_UNDERVOLTAGE_VBATT },
    { "UNDERVOLTAGE_REG3",      D1094XX_TURNOFF_REASON_VALUE_UNDERVOLTAGE_REG3 },
    { "UNDERVOLTAGE_REG2A",     D1094XX_TURNOFF_REASON_VALUE_UNDERVOLTAGE_REG2A },
    { "UNDERVOLTAGE_REG1",      D1094XX_TURNOFF_REASON_VALUE_UNDERVOLTAGE_REG1 },
    { "SHUTDOWN_BY_REGISTER",   D1094XX_TURNOFF_REASON_VALUE_SHUTDOWN_BY_REGISTER },
    { "WATCHDOG_MIN_TIME",      D1094XX_TURNOFF_REASON_VALUE_WATCHDOG_MIN_TIME },
    { "WATCHDOG_MAX_TIME",      D1094XX_TURNOFF_REASON_VALUE_WATCHDOG_MAX_TIME },
    { "OVERVOLTAGE_VBATT",      D1094XX_TURNOFF_REASON_VALUE_OVERVOLTAGE_VBATT },
};

static const DebugRegField d1094xx_turnoff_reason_fields[] = {
    { "VALUE",  D1094XX_TURNOFF_REASON_VALUE,   D1094XX_TURNOFF_REASON_VALUE_SHIFT, d1094xx_turnoff_reason_fields_value_values, ARRAY_SIZE(d1094xx_turnoff_reason_fields_value_values) },
};

static const DebugRegField d1094xx_supply_enable_1_fields[] = {
    { "VREG1_EN",       D1094XX_SUPPLY_ENABLE_1_VREG1_EN,       D1094XX_SUPPLY_ENABLE_1_VREG1_EN_SHIFT,     NULL,   0 },
    { "VREG2B_EN",      D1094XX_SUPPLY_ENABLE_1_VREG2B_EN,      D1094XX_SUPPLY_ENABLE_1_VREG2B_EN_SHIFT,    NULL,   0 },
    { "VSIMREGA_EN",    D1094XX_SUPPLY_ENABLE_1_VSIMREGA_EN,    D1094XX_SUPPLY_ENABLE_1_VSIMREGA_EN_SHIFT,  NULL,   0 },
    { "VSIMREGB_EN",    D1094XX_SUPPLY_ENABLE_1_VSIMREGB_EN,    D1094XX_SUPPLY_ENABLE_1_VSIMREGB_EN_SHIFT,  NULL,   0 },
    { "VAUDREGA_EN",    D1094XX_SUPPLY_ENABLE_1_VAUDREGA_EN,    D1094XX_SUPPLY_ENABLE_1_VAUDREGA_EN_SHIFT,  NULL,   0 },
    { "VAUDREGB_EN",    D1094XX_SUPPLY_ENABLE_1_VAUDREGB_EN,    D1094XX_SUPPLY_ENABLE_1_VAUDREGB_EN_SHIFT,  NULL,   0 },
    { "VREGMEM1_EN",    D1094XX_SUPPLY_ENABLE_1_VREGMEM1_EN,    D1094XX_SUPPLY_ENABLE_1_VREGMEM1_EN_SHIFT,  NULL,   0 },
    { "VREGMEM2_EN",    D1094XX_SUPPLY_ENABLE_1_VREGMEM2_EN,    D1094XX_SUPPLY_ENABLE_1_VREGMEM2_EN_SHIFT,  NULL,   0 },
};

static const DebugRegField d1094xx_supply_enable_2_fields[] = {
    { "VIBRA_EN",   D1094XX_SUPPLY_ENABLE_2_VIBRA_EN,   D1094XX_SUPPLY_ENABLE_2_VIBRA_EN_SHIFT,     NULL,   0 },
    { "VREGUSB_EN", D1094XX_SUPPLY_ENABLE_2_VREGUSB_EN, D1094XX_SUPPLY_ENABLE_2_VREGUSB_EN_SHIFT,   NULL,   0 },
    { "VBOOST_EN",  D1094XX_SUPPLY_ENABLE_2_VBOOST_EN,  D1094XX_SUPPLY_ENABLE_2_VBOOST_EN_SHIFT,    NULL,   0 },
    { "VLPREG_EN",  D1094XX_SUPPLY_ENABLE_2_VLPREG_EN,  D1094XX_SUPPLY_ENABLE_2_VLPREG_EN_SHIFT,    NULL,   0 },
};

static const DebugRegField d1094xx_rf_enable_fields[] = {
    { "VRF1_EN",    D1094XX_RF_ENABLE_VRF1_EN,  D1094XX_RF_ENABLE_VRF1_EN_SHIFT,    NULL,   0 },
    { "VRF2_EN",    D1094XX_RF_ENABLE_VRF2_EN,  D1094XX_RF_ENABLE_VRF2_EN_SHIFT,    NULL,   0 },
    { "VRF3_EN",    D1094XX_RF_ENABLE_VRF3_EN,  D1094XX_RF_ENABLE_VRF3_EN_SHIFT,    NULL,   0 },
};

static const DebugRegField d1094xx_supply_control_1_fields[] = {
    { "VREG1_MODE",     D1094XX_SUPPLY_CONTROL_1_VREG1_MODE,    D1094XX_SUPPLY_CONTROL_1_VREG1_MODE_SHIFT,      NULL,   0 },
    { "VREG2_LEVEL",    D1094XX_SUPPLY_CONTROL_1_VREG2_LEVEL,   D1094XX_SUPPLY_CONTROL_1_VREG2_LEVEL_SHIFT,     NULL,   0 },
    { "VREG3_LEVEL",    D1094XX_SUPPLY_CONTROL_1_VREG3_LEVEL,   D1094XX_SUPPLY_CONTROL_1_VREG3_LEVEL_SHIFT,     NULL,   0 },
    { "STEPDOWN_MODE",  D1094XX_SUPPLY_CONTROL_1_STEPDOWN_MODE, D1094XX_SUPPLY_CONTROL_1_STEPDOWN_MODE_SHIFT,   NULL,   0 },
};

static const DebugRegField d1094xx_supply_mode_fields[] = {
    { "VSIMREGA_MODE",  D1094XX_SUPPLY_MODE_VSIMREGA_MODE,  D1094XX_SUPPLY_MODE_VSIMREGA_MODE_SHIFT,    NULL,   0 },
    { "VAUDREGA_MODE",  D1094XX_SUPPLY_MODE_VAUDREGA_MODE,  D1094XX_SUPPLY_MODE_VAUDREGA_MODE_SHIFT,    NULL,   0 },
    { "VAUDREGB_MODE",  D1094XX_SUPPLY_MODE_VAUDREGB_MODE,  D1094XX_SUPPLY_MODE_VAUDREGB_MODE_SHIFT,    NULL,   0 },
};

static const DebugRegValue d1094xx_rf_voltage_fields_vrf1_level_values[] = {
    { "2850MV", D1094XX_RF_VOLTAGE_VRF1_LEVEL_2850MV },
};

static const DebugRegValue d1094xx_rf_voltage_fields_vrf2_level_values[] = {
    { "2850MV", D1094XX_RF_VOLTAGE_VRF2_LEVEL_2850MV },
};

static const DebugRegField d1094xx_rf_voltage_fields[] = {
    { "VRF1_LEVEL", D1094XX_RF_VOLTAGE_VRF1_LEVEL,  D1094XX_RF_VOLTAGE_VRF1_LEVEL_SHIFT,    d1094xx_rf_voltage_fields_vrf1_level_values,    ARRAY_SIZE(d1094xx_rf_voltage_fields_vrf1_level_values) },
    { "VRF2_LEVEL", D1094XX_RF_VOLTAGE_VRF2_LEVEL,  D1094XX_RF_VOLTAGE_VRF2_LEVEL_SHIFT,    d1094xx_rf_voltage_fields_vrf2_level_values,    ARRAY_SIZE(d1094xx_rf_voltage_fields_vrf2_level_values) },
    { "VRF3_LEVEL", D1094XX_RF_VOLTAGE_VRF3_LEVEL,  D1094XX_RF_VOLTAGE_VRF3_LEVEL_SHIFT,    NULL,                                           0 },
};

static const DebugRegField d1094xx_light_enable_fields[] = {
    { "LED1_EN",    D1094XX_LIGHT_ENABLE_LED1_EN,   D1094XX_LIGHT_ENABLE_LED1_EN_SHIFT, NULL,   0 },
    { "LED2_EN",    D1094XX_LIGHT_ENABLE_LED2_EN,   D1094XX_LIGHT_ENABLE_LED2_EN_SHIFT, NULL,   0 },
    { "LED3_EN",    D1094XX_LIGHT_ENABLE_LED3_EN,   D1094XX_LIGHT_ENABLE_LED3_EN_SHIFT, NULL,   0 },
};

static const DebugRegValue d1094xx_power_fields_wdt_time_values[] = {
    { "3S",     D1094XX_POWER_WDT_TIME_3S },
    { "6S",     D1094XX_POWER_WDT_TIME_6S },
    { "12S",    D1094XX_POWER_WDT_TIME_12S },
    { "24S",    D1094XX_POWER_WDT_TIME_24S },
};

static const DebugRegField d1094xx_power_fields[] = {
    { "WDT_TIME",   D1094XX_POWER_WDT_TIME, D1094XX_POWER_WDT_TIME_SHIFT,   d1094xx_power_fields_wdt_time_values,   ARRAY_SIZE(d1094xx_power_fields_wdt_time_values) },
    { "POWEROFF",   D1094XX_POWER_POWEROFF, D1094XX_POWER_POWEROFF_SHIFT,   NULL,                                   0 },
};

static const DebugRegValue d1094xx_charge_control_fields_current_values[] = {
    { "75MA",   D1094XX_CHARGE_CONTROL_CURRENT_75MA },
    { "150MA",  D1094XX_CHARGE_CONTROL_CURRENT_150MA },
    { "300MA",  D1094XX_CHARGE_CONTROL_CURRENT_300MA },
    { "400MA",  D1094XX_CHARGE_CONTROL_CURRENT_400MA },
};

static const DebugRegField d1094xx_charge_control_fields[] = {
    { "CURRENT",    D1094XX_CHARGE_CONTROL_CURRENT,     D1094XX_CHARGE_CONTROL_CURRENT_SHIFT,       d1094xx_charge_control_fields_current_values,   ARRAY_SIZE(d1094xx_charge_control_fields_current_values) },
    { "CHARGE_EN",  D1094XX_CHARGE_CONTROL_CHARGE_EN,   D1094XX_CHARGE_CONTROL_CHARGE_EN_SHIFT,     NULL,                                           0 },
    { "CURRENT_EN", D1094XX_CHARGE_CONTROL_CURRENT_EN,  D1094XX_CHARGE_CONTROL_CURRENT_EN_SHIFT,    NULL,                                           0 },
};

static const DebugRegField d1094xx_charge_status_fields[] = {
    { "CURRENT",    D1094XX_CHARGE_STATUS_CURRENT,  D1094XX_CHARGE_STATUS_CURRENT_SHIFT,    NULL,   0 },
};

static const DebugRegField d1094xx_light_pwm1_fields[] = {
    { "LEVEL",  D1094XX_LIGHT_PWM1_LEVEL,   D1094XX_LIGHT_PWM1_LEVEL_SHIFT, NULL,   0 },
};

static const DebugRegField d1094xx_light_pwm2_fields[] = {
    { "LEVEL",  D1094XX_LIGHT_PWM2_LEVEL,   D1094XX_LIGHT_PWM2_LEVEL_SHIFT, NULL,   0 },
};

static const DebugRegField d1094xx_light_control_fields[] = {
    { "LED1_EN",    D1094XX_LIGHT_CONTROL_LED1_EN,      D1094XX_LIGHT_CONTROL_LED1_EN_SHIFT,    NULL,   0 },
    { "LED2_EN",    D1094XX_LIGHT_CONTROL_LED2_EN,      D1094XX_LIGHT_CONTROL_LED2_EN_SHIFT,    NULL,   0 },
    { "PWM1_EN",    D1094XX_LIGHT_CONTROL_PWM1_EN,      D1094XX_LIGHT_CONTROL_PWM1_EN_SHIFT,    NULL,   0 },
    { "PWM2_EN",    D1094XX_LIGHT_CONTROL_PWM2_EN,      D1094XX_LIGHT_CONTROL_PWM2_EN_SHIFT,    NULL,   0 },
    { "MASTER_EN",  D1094XX_LIGHT_CONTROL_MASTER_EN,    D1094XX_LIGHT_CONTROL_MASTER_EN_SHIFT,  NULL,   0 },
};

static const DebugRegField d1094xx_led_pattern_1_fields[] = {
    { "VALUE",  D1094XX_LED_PATTERN_1_VALUE,    D1094XX_LED_PATTERN_1_VALUE_SHIFT,  NULL,   0 },
};

static const DebugRegField d1094xx_led_pattern_2_fields[] = {
    { "VALUE",  D1094XX_LED_PATTERN_2_VALUE,    D1094XX_LED_PATTERN_2_VALUE_SHIFT,  NULL,   0 },
};

static const DebugRegField d1094xx_led_pattern_3_fields[] = {
    { "VALUE",  D1094XX_LED_PATTERN_3_VALUE,    D1094XX_LED_PATTERN_3_VALUE_SHIFT,  NULL,   0 },
};

static const DebugRegField d1094xx_led_pattern_4_fields[] = {
    { "VALUE",  D1094XX_LED_PATTERN_4_VALUE,    D1094XX_LED_PATTERN_4_VALUE_SHIFT,  NULL,   0 },
};

static const DebugRegField d1094xx_led_pattern_5_fields[] = {
    { "VALUE",  D1094XX_LED_PATTERN_5_VALUE,    D1094XX_LED_PATTERN_5_VALUE_SHIFT,  NULL,   0 },
};

static const DebugRegField d1094xx_ddps_control_fields[] = {
    { "VALUE",  D1094XX_DDPS_CONTROL_VALUE, D1094XX_DDPS_CONTROL_VALUE_SHIFT,   NULL,   0 },
};

static const DebugRegField d1094xx_amplifier_gain_0_fields[] = {
    { "LEVEL",  D1094XX_AMPLIFIER_GAIN_0_LEVEL, D1094XX_AMPLIFIER_GAIN_0_LEVEL_SHIFT,   NULL,   0 },
};

static const DebugRegField d1094xx_amplifier_gain_1_fields[] = {
    { "LEVEL",  D1094XX_AMPLIFIER_GAIN_1_LEVEL, D1094XX_AMPLIFIER_GAIN_1_LEVEL_SHIFT,   NULL,   0 },
};

static const DebugRegField d1094xx_mono_control_fields[] = {
    { "RINGIN",         D1094XX_MONO_CONTROL_RINGIN,        D1094XX_MONO_CONTROL_RINGIN_SHIFT,          NULL,   0 },
    { "KEY_CLICK_EN",   D1094XX_MONO_CONTROL_KEY_CLICK_EN,  D1094XX_MONO_CONTROL_KEY_CLICK_EN_SHIFT,    NULL,   0 },
};

static const DebugRegField d1094xx_stereo_control_fields[] = {
    { "RINGIN", D1094XX_STEREO_CONTROL_RINGIN,  D1094XX_STEREO_CONTROL_RINGIN_SHIFT,    NULL,   0 },
};

static const DebugRegField d1094xx_amplifier_gain_2_fields[] = {
    { "GAIN",       D1094XX_AMPLIFIER_GAIN_2_GAIN,          D1094XX_AMPLIFIER_GAIN_2_GAIN_SHIFT,        NULL,   0 },
    { "TONE_LEVEL", D1094XX_AMPLIFIER_GAIN_2_TONE_LEVEL,    D1094XX_AMPLIFIER_GAIN_2_TONE_LEVEL_SHIFT,  NULL,   0 },
};

static const DebugRegField d1094xx_amplifier_gain_3_fields[] = {
    { "LEVEL",  D1094XX_AMPLIFIER_GAIN_3_LEVEL, D1094XX_AMPLIFIER_GAIN_3_LEVEL_SHIFT,   NULL,   0 },
};

static const DebugRegField d1094xx_tone_control_fields[] = {
    { "DURATION",   D1094XX_TONE_CONTROL_DURATION,      D1094XX_TONE_CONTROL_DURATION_SHIFT,    NULL,   0 },
    { "MODULATION", D1094XX_TONE_CONTROL_MODULATION,    D1094XX_TONE_CONTROL_MODULATION_SHIFT,  NULL,   0 },
};

static const DebugRegField d1094xx_vibra_fields[] = {
    { "LEVEL",  D1094XX_VIBRA_LEVEL,    D1094XX_VIBRA_LEVEL_SHIFT,  NULL,   0 },
};

static const DebugRegField d1094xx_pp_amplifier_1_fields[] = {
    { "PATH_1_SOURCE",  D1094XX_PP_AMPLIFIER_1_PATH_1_SOURCE,   D1094XX_PP_AMPLIFIER_1_PATH_1_SOURCE_SHIFT, NULL,   0 },
    { "PATH_1_EN",      D1094XX_PP_AMPLIFIER_1_PATH_1_EN,       D1094XX_PP_AMPLIFIER_1_PATH_1_EN_SHIFT,     NULL,   0 },
    { "PATH_2_SOURCE",  D1094XX_PP_AMPLIFIER_1_PATH_2_SOURCE,   D1094XX_PP_AMPLIFIER_1_PATH_2_SOURCE_SHIFT, NULL,   0 },
    { "PATH_2_EN",      D1094XX_PP_AMPLIFIER_1_PATH_2_EN,       D1094XX_PP_AMPLIFIER_1_PATH_2_EN_SHIFT,     NULL,   0 },
};

static const DebugRegField d1094xx_switch_mux_1_fields[] = {
    { "VALUE",  D1094XX_SWITCH_MUX_1_VALUE, D1094XX_SWITCH_MUX_1_VALUE_SHIFT,   NULL,   0 },
};

static const DebugRegField d1094xx_pp_route_1_fields[] = {
    { "VALUE",  D1094XX_PP_ROUTE_1_VALUE,   D1094XX_PP_ROUTE_1_VALUE_SHIFT, NULL,   0 },
};

static const DebugRegField d1094xx_adc_control_fields[] = {
    { "VALUE",  D1094XX_ADC_CONTROL_VALUE,  D1094XX_ADC_CONTROL_VALUE_SHIFT,    NULL,   0 },
};

static const DebugRegField d1094xx_pp_amplifier_2_fields[] = {
    { "PATH_1_SOURCE",  D1094XX_PP_AMPLIFIER_2_PATH_1_SOURCE,   D1094XX_PP_AMPLIFIER_2_PATH_1_SOURCE_SHIFT, NULL,   0 },
    { "PATH_1_EN",      D1094XX_PP_AMPLIFIER_2_PATH_1_EN,       D1094XX_PP_AMPLIFIER_2_PATH_1_EN_SHIFT,     NULL,   0 },
    { "PATH_2_SOURCE",  D1094XX_PP_AMPLIFIER_2_PATH_2_SOURCE,   D1094XX_PP_AMPLIFIER_2_PATH_2_SOURCE_SHIFT, NULL,   0 },
    { "PATH_2_EN",      D1094XX_PP_AMPLIFIER_2_PATH_2_EN,       D1094XX_PP_AMPLIFIER_2_PATH_2_EN_SHIFT,     NULL,   0 },
};

static const DebugRegField d1094xx_switch_mux_2_fields[] = {
    { "VALUE",      D1094XX_SWITCH_MUX_2_VALUE,     D1094XX_SWITCH_MUX_2_VALUE_SHIFT,       NULL,   0 },
    { "VLPREG_EN",  D1094XX_SWITCH_MUX_2_VLPREG_EN, D1094XX_SWITCH_MUX_2_VLPREG_EN_SHIFT,   NULL,   0 },
};

static const DebugRegField d1094xx_pp_route_2_fields[] = {
    { "VALUE",  D1094XX_PP_ROUTE_2_VALUE,   D1094XX_PP_ROUTE_2_VALUE_SHIFT, NULL,   0 },
};

static const DebugRegField d1094xx_switch_mux_4_fields[] = {
    { "VALUE",  D1094XX_SWITCH_MUX_4_VALUE, D1094XX_SWITCH_MUX_4_VALUE_SHIFT,   NULL,   0 },
};

static const DebugRegField d1094xx_amplifier_gain_4_5_fields[] = {
    { "GAIN",           D1094XX_AMPLIFIER_GAIN_4_5_GAIN,            D1094XX_AMPLIFIER_GAIN_4_5_GAIN_SHIFT,          NULL,   0 },
    { "PATH_4_SELECT",  D1094XX_AMPLIFIER_GAIN_4_5_PATH_4_SELECT,   D1094XX_AMPLIFIER_GAIN_4_5_PATH_4_SELECT_SHIFT, NULL,   0 },
};

static const DebugRegField d1094xx_amplifier_gain_6_fields[] = {
    { "LEVEL",  D1094XX_AMPLIFIER_GAIN_6_LEVEL, D1094XX_AMPLIFIER_GAIN_6_LEVEL_SHIFT,   NULL,   0 },
};

static const DebugRegField d1094xx_dac_control_fields[] = {
    { "VALUE",  D1094XX_DAC_CONTROL_VALUE,  D1094XX_DAC_CONTROL_VALUE_SHIFT,    NULL,   0 },
};

static const DebugRegField d1094xx_amplifier_gain_7_8_fields[] = {
    { "GAIN",           D1094XX_AMPLIFIER_GAIN_7_8_GAIN,            D1094XX_AMPLIFIER_GAIN_7_8_GAIN_SHIFT,          NULL,   0 },
    { "PATH_7_SELECT",  D1094XX_AMPLIFIER_GAIN_7_8_PATH_7_SELECT,   D1094XX_AMPLIFIER_GAIN_7_8_PATH_7_SELECT_SHIFT, NULL,   0 },
};

static const DebugRegField d1094xx_amplifier_gain_9_fields[] = {
    { "GAIN",   D1094XX_AMPLIFIER_GAIN_9_GAIN,  D1094XX_AMPLIFIER_GAIN_9_GAIN_SHIFT,    NULL,   0 },
};

static const DebugRegField d1094xx_switch_mux_5_fields[] = {
    { "VALUE",  D1094XX_SWITCH_MUX_5_VALUE, D1094XX_SWITCH_MUX_5_VALUE_SHIFT,   NULL,   0 },
};

static const DebugRegField d1094xx_mux_input_fields[] = {
    { "VALUE",  D1094XX_MUX_INPUT_VALUE,    D1094XX_MUX_INPUT_VALUE_SHIFT,  NULL,   0 },
};

static const DebugRegField d1094xx_switch_mux_3_fields[] = {
    { "VALUE",  D1094XX_SWITCH_MUX_3_VALUE, D1094XX_SWITCH_MUX_3_VALUE_SHIFT,   NULL,   0 },
};

static const DebugRegField d1094xx_outport_control_fields[] = {
    { "MODE",   D1094XX_OUTPORT_CONTROL_MODE,   D1094XX_OUTPORT_CONTROL_MODE_SHIFT,     NULL,   0 },
    { "LEVEL",  D1094XX_OUTPORT_CONTROL_LEVEL,  D1094XX_OUTPORT_CONTROL_LEVEL_SHIFT,    NULL,   0 },
};

static const DebugRegField d1094xx_sample_rate_fields[] = {
    { "ADC",    D1094XX_SAMPLE_RATE_ADC,    D1094XX_SAMPLE_RATE_ADC_SHIFT,  NULL,   0 },
    { "DAC",    D1094XX_SAMPLE_RATE_DAC,    D1094XX_SAMPLE_RATE_DAC_SHIFT,  NULL,   0 },
};

static const DebugRegField d1094xx_stereo_mode_fields[] = {
    { "VALUE",  D1094XX_STEREO_MODE_VALUE,  D1094XX_STEREO_MODE_VALUE_SHIFT,    NULL,   0 },
};

static const DebugRegField d1094xx_i2s_rx_control_fields[] = {
    { "VALUE",  D1094XX_I2S_RX_CONTROL_VALUE,   D1094XX_I2S_RX_CONTROL_VALUE_SHIFT, NULL,   0 },
};

static const DebugRegField d1094xx_i2s_tx_control_fields[] = {
    { "VALUE",  D1094XX_I2S_TX_CONTROL_VALUE,   D1094XX_I2S_TX_CONTROL_VALUE_SHIFT, NULL,   0 },
};

static const DebugRegField d1094xx_audio_control_2_fields[] = {
    { "VALUE",  D1094XX_AUDIO_CONTROL_2_VALUE,  D1094XX_AUDIO_CONTROL_2_VALUE_SHIFT,    NULL,   0 },
};

static const DebugReg d1094xx_regs[] = {
    { "IDENTIFICATION",     D1094XX_IDENTIFICATION,     d1094xx_identification_fields,      ARRAY_SIZE(d1094xx_identification_fields) },
    { "IRQ_STATUS_1",       D1094XX_IRQ_STATUS_1,       d1094xx_irq_status_1_fields,        ARRAY_SIZE(d1094xx_irq_status_1_fields) },
    { "IRQ_STATUS_2",       D1094XX_IRQ_STATUS_2,       d1094xx_irq_status_2_fields,        ARRAY_SIZE(d1094xx_irq_status_2_fields) },
    { "IRQ_MASK_1",         D1094XX_IRQ_MASK_1,         d1094xx_irq_mask_1_fields,          ARRAY_SIZE(d1094xx_irq_mask_1_fields) },
    { "IRQ_MASK_2",         D1094XX_IRQ_MASK_2,         d1094xx_irq_mask_2_fields,          ARRAY_SIZE(d1094xx_irq_mask_2_fields) },
    { "TURNOFF_REASON",     D1094XX_TURNOFF_REASON,     d1094xx_turnoff_reason_fields,      ARRAY_SIZE(d1094xx_turnoff_reason_fields) },
    { "SUPPLY_ENABLE_1",    D1094XX_SUPPLY_ENABLE_1,    d1094xx_supply_enable_1_fields,     ARRAY_SIZE(d1094xx_supply_enable_1_fields) },
    { "SUPPLY_ENABLE_2",    D1094XX_SUPPLY_ENABLE_2,    d1094xx_supply_enable_2_fields,     ARRAY_SIZE(d1094xx_supply_enable_2_fields) },
    { "RF_ENABLE",          D1094XX_RF_ENABLE,          d1094xx_rf_enable_fields,           ARRAY_SIZE(d1094xx_rf_enable_fields) },
    { "SUPPLY_CONTROL_1",   D1094XX_SUPPLY_CONTROL_1,   d1094xx_supply_control_1_fields,    ARRAY_SIZE(d1094xx_supply_control_1_fields) },
    { "SUPPLY_MODE",        D1094XX_SUPPLY_MODE,        d1094xx_supply_mode_fields,         ARRAY_SIZE(d1094xx_supply_mode_fields) },
    { "RF_VOLTAGE",         D1094XX_RF_VOLTAGE,         d1094xx_rf_voltage_fields,          ARRAY_SIZE(d1094xx_rf_voltage_fields) },
    { "LIGHT_ENABLE",       D1094XX_LIGHT_ENABLE,       d1094xx_light_enable_fields,        ARRAY_SIZE(d1094xx_light_enable_fields) },
    { "POWER",              D1094XX_POWER,              d1094xx_power_fields,               ARRAY_SIZE(d1094xx_power_fields) },
    { "CHARGE_CONTROL",     D1094XX_CHARGE_CONTROL,     d1094xx_charge_control_fields,      ARRAY_SIZE(d1094xx_charge_control_fields) },
    { "CHARGE_STATUS",      D1094XX_CHARGE_STATUS,      d1094xx_charge_status_fields,       ARRAY_SIZE(d1094xx_charge_status_fields) },
    { "LIGHT_PWM1",         D1094XX_LIGHT_PWM1,         d1094xx_light_pwm1_fields,          ARRAY_SIZE(d1094xx_light_pwm1_fields) },
    { "LIGHT_PWM2",         D1094XX_LIGHT_PWM2,         d1094xx_light_pwm2_fields,          ARRAY_SIZE(d1094xx_light_pwm2_fields) },
    { "LIGHT_CONTROL",      D1094XX_LIGHT_CONTROL,      d1094xx_light_control_fields,       ARRAY_SIZE(d1094xx_light_control_fields) },
    { "LED_PATTERN_1",      D1094XX_LED_PATTERN_1,      d1094xx_led_pattern_1_fields,       ARRAY_SIZE(d1094xx_led_pattern_1_fields) },
    { "LED_PATTERN_2",      D1094XX_LED_PATTERN_2,      d1094xx_led_pattern_2_fields,       ARRAY_SIZE(d1094xx_led_pattern_2_fields) },
    { "LED_PATTERN_3",      D1094XX_LED_PATTERN_3,      d1094xx_led_pattern_3_fields,       ARRAY_SIZE(d1094xx_led_pattern_3_fields) },
    { "LED_PATTERN_4",      D1094XX_LED_PATTERN_4,      d1094xx_led_pattern_4_fields,       ARRAY_SIZE(d1094xx_led_pattern_4_fields) },
    { "LED_PATTERN_5",      D1094XX_LED_PATTERN_5,      d1094xx_led_pattern_5_fields,       ARRAY_SIZE(d1094xx_led_pattern_5_fields) },
    { "DDPS_CONTROL",       D1094XX_DDPS_CONTROL,       d1094xx_ddps_control_fields,        ARRAY_SIZE(d1094xx_ddps_control_fields) },
    { "AMPLIFIER_GAIN_0",   D1094XX_AMPLIFIER_GAIN_0,   d1094xx_amplifier_gain_0_fields,    ARRAY_SIZE(d1094xx_amplifier_gain_0_fields) },
    { "AMPLIFIER_GAIN_1",   D1094XX_AMPLIFIER_GAIN_1,   d1094xx_amplifier_gain_1_fields,    ARRAY_SIZE(d1094xx_amplifier_gain_1_fields) },
    { "MONO_CONTROL",       D1094XX_MONO_CONTROL,       d1094xx_mono_control_fields,        ARRAY_SIZE(d1094xx_mono_control_fields) },
    { "STEREO_CONTROL",     D1094XX_STEREO_CONTROL,     d1094xx_stereo_control_fields,      ARRAY_SIZE(d1094xx_stereo_control_fields) },
    { "AMPLIFIER_GAIN_2",   D1094XX_AMPLIFIER_GAIN_2,   d1094xx_amplifier_gain_2_fields,    ARRAY_SIZE(d1094xx_amplifier_gain_2_fields) },
    { "AMPLIFIER_GAIN_3",   D1094XX_AMPLIFIER_GAIN_3,   d1094xx_amplifier_gain_3_fields,    ARRAY_SIZE(d1094xx_amplifier_gain_3_fields) },
    { "TONE_CONTROL",       D1094XX_TONE_CONTROL,       d1094xx_tone_control_fields,        ARRAY_SIZE(d1094xx_tone_control_fields) },
    { "VIBRA",              D1094XX_VIBRA,              d1094xx_vibra_fields,               ARRAY_SIZE(d1094xx_vibra_fields) },
    { "PP_AMPLIFIER_1",     D1094XX_PP_AMPLIFIER_1,     d1094xx_pp_amplifier_1_fields,      ARRAY_SIZE(d1094xx_pp_amplifier_1_fields) },
    { "SWITCH_MUX_1",       D1094XX_SWITCH_MUX_1,       d1094xx_switch_mux_1_fields,        ARRAY_SIZE(d1094xx_switch_mux_1_fields) },
    { "PP_ROUTE_1",         D1094XX_PP_ROUTE_1,         d1094xx_pp_route_1_fields,          ARRAY_SIZE(d1094xx_pp_route_1_fields) },
    { "ADC_CONTROL",        D1094XX_ADC_CONTROL,        d1094xx_adc_control_fields,         ARRAY_SIZE(d1094xx_adc_control_fields) },
    { "PP_AMPLIFIER_2",     D1094XX_PP_AMPLIFIER_2,     d1094xx_pp_amplifier_2_fields,      ARRAY_SIZE(d1094xx_pp_amplifier_2_fields) },
    { "SWITCH_MUX_2",       D1094XX_SWITCH_MUX_2,       d1094xx_switch_mux_2_fields,        ARRAY_SIZE(d1094xx_switch_mux_2_fields) },
    { "PP_ROUTE_2",         D1094XX_PP_ROUTE_2,         d1094xx_pp_route_2_fields,          ARRAY_SIZE(d1094xx_pp_route_2_fields) },
    { "SWITCH_MUX_4",       D1094XX_SWITCH_MUX_4,       d1094xx_switch_mux_4_fields,        ARRAY_SIZE(d1094xx_switch_mux_4_fields) },
    { "AMPLIFIER_GAIN_4_5", D1094XX_AMPLIFIER_GAIN_4_5, d1094xx_amplifier_gain_4_5_fields,  ARRAY_SIZE(d1094xx_amplifier_gain_4_5_fields) },
    { "AMPLIFIER_GAIN_6",   D1094XX_AMPLIFIER_GAIN_6,   d1094xx_amplifier_gain_6_fields,    ARRAY_SIZE(d1094xx_amplifier_gain_6_fields) },
    { "DAC_CONTROL",        D1094XX_DAC_CONTROL,        d1094xx_dac_control_fields,         ARRAY_SIZE(d1094xx_dac_control_fields) },
    { "AMPLIFIER_GAIN_7_8", D1094XX_AMPLIFIER_GAIN_7_8, d1094xx_amplifier_gain_7_8_fields,  ARRAY_SIZE(d1094xx_amplifier_gain_7_8_fields) },
    { "AMPLIFIER_GAIN_9",   D1094XX_AMPLIFIER_GAIN_9,   d1094xx_amplifier_gain_9_fields,    ARRAY_SIZE(d1094xx_amplifier_gain_9_fields) },
    { "SWITCH_MUX_5",       D1094XX_SWITCH_MUX_5,       d1094xx_switch_mux_5_fields,        ARRAY_SIZE(d1094xx_switch_mux_5_fields) },
    { "MUX_INPUT",          D1094XX_MUX_INPUT,          d1094xx_mux_input_fields,           ARRAY_SIZE(d1094xx_mux_input_fields) },
    { "SWITCH_MUX_3",       D1094XX_SWITCH_MUX_3,       d1094xx_switch_mux_3_fields,        ARRAY_SIZE(d1094xx_switch_mux_3_fields) },
    { "OUTPORT_CONTROL",    D1094XX_OUTPORT_CONTROL,    d1094xx_outport_control_fields,     ARRAY_SIZE(d1094xx_outport_control_fields) },
    { "SAMPLE_RATE",        D1094XX_SAMPLE_RATE,        d1094xx_sample_rate_fields,         ARRAY_SIZE(d1094xx_sample_rate_fields) },
    { "STEREO_MODE",        D1094XX_STEREO_MODE,        d1094xx_stereo_mode_fields,         ARRAY_SIZE(d1094xx_stereo_mode_fields) },
    { "I2S_RX_CONTROL",     D1094XX_I2S_RX_CONTROL,     d1094xx_i2s_rx_control_fields,      ARRAY_SIZE(d1094xx_i2s_rx_control_fields) },
    { "I2S_TX_CONTROL",     D1094XX_I2S_TX_CONTROL,     d1094xx_i2s_tx_control_fields,      ARRAY_SIZE(d1094xx_i2s_tx_control_fields) },
    { "AUDIO_CONTROL_2",    D1094XX_AUDIO_CONTROL_2,    d1094xx_audio_control_2_fields,     ARRAY_SIZE(d1094xx_audio_control_2_fields) },
};

const DebugRegs d1094xx_debug_regs = {
    .regs = d1094xx_regs,
    .count = ARRAY_SIZE(d1094xx_regs),
    .reg_size = 1,
    .addr_digits = 2,
};
