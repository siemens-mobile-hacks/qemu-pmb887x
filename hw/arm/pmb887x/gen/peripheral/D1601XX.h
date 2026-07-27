#pragma once

#include "qemu/bitops.h"

// D1601XX
// Dialog D1601xx Mozart Boost+/Twigo4 Boost+ Power ASIC family
#define	D1601XX_I2C_ADDR									0x31

/* Device identification and revision */
#define	D1601XX_IDENTIFICATION								0x00
#define	D1601XX_IDENTIFICATION_MODEL						MAKE_64BIT_MASK(0, 3)	 // Base model code
#define	D1601XX_IDENTIFICATION_MODEL_SHIFT					0
#define	D1601XX_IDENTIFICATION_REVISION						MAKE_64BIT_MASK(3, 4)	 // Hardware revision code
#define	D1601XX_IDENTIFICATION_REVISION_SHIFT				3
#define	D1601XX_IDENTIFICATION_VENDOR						BIT(7)					 // Silicon vendor: 0 is ST Twigo4, 1 is Dialog Mozart
#define	D1601XX_IDENTIFICATION_VENDOR_ST					0x0
#define	D1601XX_IDENTIFICATION_VENDOR_DIALOG				0x80

/* First interrupt-status byte */
#define	D1601XX_IRQ_STATUS_1								0x01
#define	D1601XX_IRQ_STATUS_1_OVER_TEMP						BIT(2)					 // PMIC overtemperature
#define	D1601XX_IRQ_STATUS_1_CHARGER_EVENT					BIT(3)					 // Charger status event
#define	D1601XX_IRQ_STATUS_1_UV_AUDIO_REGA					BIT(5)					 // Audio regulator A undervoltage

/* Second interrupt-status byte */
#define	D1601XX_IRQ_STATUS_2								0x02
#define	D1601XX_IRQ_STATUS_2_UV_SIM_REGA					BIT(0)					 // SIM regulator A undervoltage
#define	D1601XX_IRQ_STATUS_2_SUPPLY_SHORT					BIT(1)					 // Supply short circuit
#define	D1601XX_IRQ_STATUS_2_UNEXP_CHARGE					BIT(2)					 // Unexpected charging
#define	D1601XX_IRQ_STATUS_2_VBATT_OV						BIT(5)					 // Battery overvoltage

/* First interrupt-mask byte */
#define	D1601XX_IRQ_MASK_1									0x03
#define	D1601XX_IRQ_MASK_1_VALUE							MAKE_64BIT_MASK(0, 8)	 // Interrupt mask bits
#define	D1601XX_IRQ_MASK_1_VALUE_SHIFT						0

/* Second interrupt-mask byte */
#define	D1601XX_IRQ_MASK_2									0x04
#define	D1601XX_IRQ_MASK_2_VALUE							MAKE_64BIT_MASK(0, 8)	 // Interrupt mask bits
#define	D1601XX_IRQ_MASK_2_VALUE_SHIFT						0

/* Stored shutdown reason */
#define	D1601XX_TURNOFF_REASON								0x05
#define	D1601XX_TURNOFF_REASON_VALUE						MAKE_64BIT_MASK(0, 8)	 // Shutdown reason code
#define	D1601XX_TURNOFF_REASON_VALUE_SHIFT					0
#define	D1601XX_TURNOFF_REASON_VALUE_UNDEFINED				0x0
#define	D1601XX_TURNOFF_REASON_VALUE_NO_REASON_STORED		0x1
#define	D1601XX_TURNOFF_REASON_VALUE_UNDERVOLTAGE_VBATT		0x2
#define	D1601XX_TURNOFF_REASON_VALUE_UNDERVOLTAGE_REG3		0x3
#define	D1601XX_TURNOFF_REASON_VALUE_UNDERVOLTAGE_REG2A		0x4
#define	D1601XX_TURNOFF_REASON_VALUE_UNDERVOLTAGE_REG1		0x5
#define	D1601XX_TURNOFF_REASON_VALUE_SHUTDOWN_BY_REGISTER	0x6
#define	D1601XX_TURNOFF_REASON_VALUE_WATCHDOG_MIN_TIME		0x7
#define	D1601XX_TURNOFF_REASON_VALUE_WATCHDOG_MAX_TIME		0x8
#define	D1601XX_TURNOFF_REASON_VALUE_OVERVOLTAGE_VBATT		0x9

/* Primary regulator enables */
#define	D1601XX_SUPPLY_ENABLE_1								0x06
#define	D1601XX_SUPPLY_ENABLE_1_VREG1_EN					BIT(0)					 // Enable regulator VREG1
#define	D1601XX_SUPPLY_ENABLE_1_VREG2B_EN					BIT(1)					 // Enable regulator VREG2B
#define	D1601XX_SUPPLY_ENABLE_1_VSIMREGA_EN					BIT(2)					 // Enable SIM regulator A
#define	D1601XX_SUPPLY_ENABLE_1_VSIMREGB_EN					BIT(3)					 // Enable SIM regulator B
#define	D1601XX_SUPPLY_ENABLE_1_VAUDREGA_EN					BIT(4)					 // Enable audio regulator A
#define	D1601XX_SUPPLY_ENABLE_1_VAUDREGB_EN					BIT(5)					 // Enable audio regulator B
#define	D1601XX_SUPPLY_ENABLE_1_VREGMEM1_EN					BIT(6)					 // Enable memory regulator 1
#define	D1601XX_SUPPLY_ENABLE_1_VREGMEM2_EN					BIT(7)					 // Enable memory regulator 2

/* Auxiliary regulator enables */
#define	D1601XX_SUPPLY_ENABLE_2								0x07
#define	D1601XX_SUPPLY_ENABLE_2_VIBRA_EN					BIT(0)					 // Enable vibra supply
#define	D1601XX_SUPPLY_ENABLE_2_VREGUSB_EN					BIT(1)					 // Enable USB regulator
#define	D1601XX_SUPPLY_ENABLE_2_VBOOST_EN					BIT(2)					 // Enable internal VBOOST converter

/* RF regulator enables */
#define	D1601XX_RF_ENABLE									0x08
#define	D1601XX_RF_ENABLE_VRF1_EN							BIT(0)					 // Enable RF regulator 1
#define	D1601XX_RF_ENABLE_VRF2_EN							BIT(1)					 // Enable RF regulator 2
#define	D1601XX_RF_ENABLE_VRF3_EN							BIT(2)					 // Enable RF regulator 3

/* Primary regulator modes and levels */
#define	D1601XX_SUPPLY_CONTROL_1							0x09
#define	D1601XX_SUPPLY_CONTROL_1_VREG1_MODE					BIT(0)					 // VREG1 operating mode
#define	D1601XX_SUPPLY_CONTROL_1_VREG2_LEVEL				MAKE_64BIT_MASK(1, 3)	 // VREG2A and VREG2B voltage level
#define	D1601XX_SUPPLY_CONTROL_1_VREG2_LEVEL_SHIFT			1
#define	D1601XX_SUPPLY_CONTROL_1_VREG3_LEVEL				MAKE_64BIT_MASK(4, 3)	 // VREG3 voltage level
#define	D1601XX_SUPPLY_CONTROL_1_VREG3_LEVEL_SHIFT			4
#define	D1601XX_SUPPLY_CONTROL_1_STEPDOWN_MODE				BIT(7)					 // Buck-converter operating mode

/* Regulator operating modes */
#define	D1601XX_SUPPLY_MODE									0x0A
#define	D1601XX_SUPPLY_MODE_VSIMREGA_MODE					BIT(0)					 // SIM regulator A mode
#define	D1601XX_SUPPLY_MODE_VAUDREGA_MODE					BIT(1)					 // Audio regulator A mode
#define	D1601XX_SUPPLY_MODE_VAUDREGB_MODE					BIT(2)					 // Audio regulator B mode

/* RF regulator voltage selection */
#define	D1601XX_RF_VOLTAGE									0x0B
#define	D1601XX_RF_VOLTAGE_VRF1_LEVEL						MAKE_64BIT_MASK(0, 2)	 // RF regulator 1 level
#define	D1601XX_RF_VOLTAGE_VRF1_LEVEL_SHIFT					0
#define	D1601XX_RF_VOLTAGE_VRF2_LEVEL						MAKE_64BIT_MASK(2, 2)	 // RF regulator 2 level
#define	D1601XX_RF_VOLTAGE_VRF2_LEVEL_SHIFT					2
#define	D1601XX_RF_VOLTAGE_VRF3_LEVEL						MAKE_64BIT_MASK(4, 2)	 // RF regulator 3 level
#define	D1601XX_RF_VOLTAGE_VRF3_LEVEL_SHIFT					4

/* LED channel enables */
#define	D1601XX_LIGHT_ENABLE								0x0C
#define	D1601XX_LIGHT_ENABLE_LED1_EN						BIT(0)					 // Enable LED channel 1
#define	D1601XX_LIGHT_ENABLE_LED2_EN						BIT(1)					 // Enable LED channel 2
#define	D1601XX_LIGHT_ENABLE_LED3_EN						BIT(2)					 // Enable LED channel 3

/* Watchdog and shutdown control */
#define	D1601XX_POWER										0x0E
#define	D1601XX_POWER_WDT_TIME								MAKE_64BIT_MASK(0, 2)	 // Watchdog timeout
#define	D1601XX_POWER_WDT_TIME_SHIFT						0
#define	D1601XX_POWER_WDT_TIME_3S							0x0
#define	D1601XX_POWER_WDT_TIME_6S							0x1
#define	D1601XX_POWER_WDT_TIME_12S							0x2
#define	D1601XX_POWER_WDT_TIME_24S							0x3
#define	D1601XX_POWER_POWEROFF								BIT(2)					 // Request system shutdown

/* Battery-charger control */
#define	D1601XX_CHARGE_CONTROL								0x10
#define	D1601XX_CHARGE_CONTROL_CURRENT						MAKE_64BIT_MASK(0, 2)	 // Charge current
#define	D1601XX_CHARGE_CONTROL_CURRENT_SHIFT				0
#define	D1601XX_CHARGE_CONTROL_CURRENT_75MA					0x0
#define	D1601XX_CHARGE_CONTROL_CURRENT_150MA				0x1
#define	D1601XX_CHARGE_CONTROL_CURRENT_300MA				0x2
#define	D1601XX_CHARGE_CONTROL_CURRENT_400MA				0x3
#define	D1601XX_CHARGE_CONTROL_CHARGE_EN					BIT(3)					 // Enable battery charging
#define	D1601XX_CHARGE_CONTROL_CURRENT_EN					BIT(7)					 // Enable programmed charge current

/* Charge-current measurement */
#define	D1601XX_CHARGE_STATUS								0x11
#define	D1601XX_CHARGE_STATUS_CURRENT						MAKE_64BIT_MASK(0, 8)	 // Raw current; firmware maps <0x1F to 0, otherwise ((value >> 5) + 1) x 200 mA
#define	D1601XX_CHARGE_STATUS_CURRENT_SHIFT					0

/* Light PWM channel 1 level */
#define	D1601XX_LIGHT_PWM1									0x12
#define	D1601XX_LIGHT_PWM1_LEVEL							MAKE_64BIT_MASK(0, 7)	 // PWM level from 0 to 80
#define	D1601XX_LIGHT_PWM1_LEVEL_SHIFT						0

/* Light PWM channel 2 level */
#define	D1601XX_LIGHT_PWM2									0x13
#define	D1601XX_LIGHT_PWM2_LEVEL							MAKE_64BIT_MASK(0, 7)	 // PWM level from 0 to 80
#define	D1601XX_LIGHT_PWM2_LEVEL_SHIFT						0

/* LED outputs and PWM control */
#define	D1601XX_LIGHT_CONTROL								0x14
#define	D1601XX_LIGHT_CONTROL_LED1_EN						BIT(1)					 // Enable LED output 1
#define	D1601XX_LIGHT_CONTROL_LED2_EN						BIT(2)					 // Enable LED output 2
#define	D1601XX_LIGHT_CONTROL_PWM1_EN						BIT(3)					 // Enable light PWM channel 1
#define	D1601XX_LIGHT_CONTROL_PWM2_EN						BIT(4)					 // Enable light PWM channel 2
#define	D1601XX_LIGHT_CONTROL_MASTER_EN						BIT(5)					 // Enable LIGHT block

/* LED1/LED2 blink-pattern byte 1 */
#define	D1601XX_LED_PATTERN_1								0x15
#define	D1601XX_LED_PATTERN_1_VALUE							MAKE_64BIT_MASK(0, 8)	 // Pattern bits
#define	D1601XX_LED_PATTERN_1_VALUE_SHIFT					0

/* LED1/LED2 blink-pattern byte 2 */
#define	D1601XX_LED_PATTERN_2								0x16
#define	D1601XX_LED_PATTERN_2_VALUE							MAKE_64BIT_MASK(0, 8)	 // Pattern bits
#define	D1601XX_LED_PATTERN_2_VALUE_SHIFT					0

/* LED1/LED2 blink-pattern byte 3 */
#define	D1601XX_LED_PATTERN_3								0x17
#define	D1601XX_LED_PATTERN_3_VALUE							MAKE_64BIT_MASK(0, 8)	 // Pattern bits
#define	D1601XX_LED_PATTERN_3_VALUE_SHIFT					0

/* LED1/LED2 blink-pattern byte 4 */
#define	D1601XX_LED_PATTERN_4								0x18
#define	D1601XX_LED_PATTERN_4_VALUE							MAKE_64BIT_MASK(0, 8)	 // Pattern bits
#define	D1601XX_LED_PATTERN_4_VALUE_SHIFT					0

/* LED1/LED2 blink-pattern byte 5 */
#define	D1601XX_LED_PATTERN_5								0x19
#define	D1601XX_LED_PATTERN_5_VALUE							MAKE_64BIT_MASK(0, 8)	 // Pattern bits
#define	D1601XX_LED_PATTERN_5_VALUE_SHIFT					0

/* Dynamic-power-supply control */
#define	D1601XX_DDPS_CONTROL								0x20
#define	D1601XX_DDPS_CONTROL_VALUE							MAKE_64BIT_MASK(0, 8)	 // Raw control value
#define	D1601XX_DDPS_CONTROL_VALUE_SHIFT					0

/* Amplifier path 0 gain */
#define	D1601XX_AMPLIFIER_GAIN_0							0x40
#define	D1601XX_AMPLIFIER_GAIN_0_LEVEL						MAKE_64BIT_MASK(0, 8)	 // Gain code
#define	D1601XX_AMPLIFIER_GAIN_0_LEVEL_SHIFT				0

/* Amplifier path 1 gain */
#define	D1601XX_AMPLIFIER_GAIN_1							0x41
#define	D1601XX_AMPLIFIER_GAIN_1_LEVEL						MAKE_64BIT_MASK(0, 8)	 // Gain code
#define	D1601XX_AMPLIFIER_GAIN_1_LEVEL_SHIFT				0

/* Mono audio-path control */
#define	D1601XX_MONO_CONTROL								0x42
#define	D1601XX_MONO_CONTROL_KEY_CLICK_EN					BIT(3)					 // Enable key-click generator

/* Stereo audio-path control */
#define	D1601XX_STEREO_CONTROL								0x43
#define	D1601XX_STEREO_CONTROL_VALUE						MAKE_64BIT_MASK(0, 8)	 // Stereo control bits
#define	D1601XX_STEREO_CONTROL_VALUE_SHIFT					0

/* Amplifier path 2 and tone level */
#define	D1601XX_AMPLIFIER_GAIN_2							0x44
#define	D1601XX_AMPLIFIER_GAIN_2_GAIN						MAKE_64BIT_MASK(0, 6)	 // Amplifier gain code
#define	D1601XX_AMPLIFIER_GAIN_2_GAIN_SHIFT					0
#define	D1601XX_AMPLIFIER_GAIN_2_TONE_LEVEL					MAKE_64BIT_MASK(6, 2)	 // Key-click and ringing tone level
#define	D1601XX_AMPLIFIER_GAIN_2_TONE_LEVEL_SHIFT			6

/* Amplifier path 3 gain */
#define	D1601XX_AMPLIFIER_GAIN_3							0x45
#define	D1601XX_AMPLIFIER_GAIN_3_LEVEL						MAKE_64BIT_MASK(0, 8)	 // Gain code
#define	D1601XX_AMPLIFIER_GAIN_3_LEVEL_SHIFT				0

/* Key-click and ringing tone generator control */
#define	D1601XX_TONE_CONTROL								0x46
#define	D1601XX_TONE_CONTROL_DURATION						MAKE_64BIT_MASK(0, 5)	 // Key-click duration code
#define	D1601XX_TONE_CONTROL_DURATION_SHIFT					0
#define	D1601XX_TONE_CONTROL_MODULATION						MAKE_64BIT_MASK(5, 2)	 // Tone modulation mode
#define	D1601XX_TONE_CONTROL_MODULATION_SHIFT				5

/* Vibra drive level */
#define	D1601XX_VIBRA										0x47
#define	D1601XX_VIBRA_LEVEL									MAKE_64BIT_MASK(0, 8)	 // Drive level code
#define	D1601XX_VIBRA_LEVEL_SHIFT							0

/* First push-pull amplifier route */
#define	D1601XX_PP_AMPLIFIER_1								0x48
#define	D1601XX_PP_AMPLIFIER_1_PATH_1_SOURCE				MAKE_64BIT_MASK(0, 3)	 // First path source
#define	D1601XX_PP_AMPLIFIER_1_PATH_1_SOURCE_SHIFT			0
#define	D1601XX_PP_AMPLIFIER_1_PATH_1_EN					BIT(3)					 // Enable first path
#define	D1601XX_PP_AMPLIFIER_1_PATH_2_SOURCE				MAKE_64BIT_MASK(4, 3)	 // Second path source
#define	D1601XX_PP_AMPLIFIER_1_PATH_2_SOURCE_SHIFT			4
#define	D1601XX_PP_AMPLIFIER_1_PATH_2_EN					BIT(7)					 // Enable second path

/* Audio switch-mux path 1 */
#define	D1601XX_SWITCH_MUX_1								0x49
#define	D1601XX_SWITCH_MUX_1_VALUE							MAKE_64BIT_MASK(0, 8)	 // Mux control value
#define	D1601XX_SWITCH_MUX_1_VALUE_SHIFT					0

/* First push-pull route selection */
#define	D1601XX_PP_ROUTE_1									0x4A
#define	D1601XX_PP_ROUTE_1_VALUE							MAKE_64BIT_MASK(0, 4)	 // Route selection from 0 to 8
#define	D1601XX_PP_ROUTE_1_VALUE_SHIFT						0

/* Audio ADC control */
#define	D1601XX_ADC_CONTROL									0x4B
#define	D1601XX_ADC_CONTROL_VALUE							MAKE_64BIT_MASK(0, 8)	 // ADC control bits
#define	D1601XX_ADC_CONTROL_VALUE_SHIFT						0

/* Second push-pull amplifier route */
#define	D1601XX_PP_AMPLIFIER_2								0x4C
#define	D1601XX_PP_AMPLIFIER_2_PATH_1_SOURCE				MAKE_64BIT_MASK(0, 3)	 // First path source
#define	D1601XX_PP_AMPLIFIER_2_PATH_1_SOURCE_SHIFT			0
#define	D1601XX_PP_AMPLIFIER_2_PATH_1_EN					BIT(3)					 // Enable first path
#define	D1601XX_PP_AMPLIFIER_2_PATH_2_SOURCE				MAKE_64BIT_MASK(4, 3)	 // Second path source
#define	D1601XX_PP_AMPLIFIER_2_PATH_2_SOURCE_SHIFT			4
#define	D1601XX_PP_AMPLIFIER_2_PATH_2_EN					BIT(7)					 // Enable second path

/* Audio switch-mux path 2 */
#define	D1601XX_SWITCH_MUX_2								0x4D
#define	D1601XX_SWITCH_MUX_2_VALUE							MAKE_64BIT_MASK(0, 6)	 // Mux control value
#define	D1601XX_SWITCH_MUX_2_VALUE_SHIFT					0
#define	D1601XX_SWITCH_MUX_2_VLPREG_EN						BIT(6)					 // Enable low-power regulator

/* Second push-pull route selection */
#define	D1601XX_PP_ROUTE_2									0x4E
#define	D1601XX_PP_ROUTE_2_VALUE							MAKE_64BIT_MASK(0, 4)	 // Route selection from 0 to 8
#define	D1601XX_PP_ROUTE_2_VALUE_SHIFT						0

/* Audio switch-mux path 4 */
#define	D1601XX_SWITCH_MUX_4								0x4F
#define	D1601XX_SWITCH_MUX_4_VALUE							MAKE_64BIT_MASK(0, 8)	 // Mux control value
#define	D1601XX_SWITCH_MUX_4_VALUE_SHIFT					0

/* Amplifier paths 4 and 5 gain */
#define	D1601XX_AMPLIFIER_GAIN_4_5							0x50
#define	D1601XX_AMPLIFIER_GAIN_4_5_GAIN						MAKE_64BIT_MASK(0, 7)	 // Amplifier gain code
#define	D1601XX_AMPLIFIER_GAIN_4_5_GAIN_SHIFT				0
#define	D1601XX_AMPLIFIER_GAIN_4_5_PATH_4_SELECT			BIT(7)					 // Select amplifier path 4

/* Amplifier path 6 gain */
#define	D1601XX_AMPLIFIER_GAIN_6							0x51
#define	D1601XX_AMPLIFIER_GAIN_6_LEVEL						MAKE_64BIT_MASK(0, 8)	 // Gain code
#define	D1601XX_AMPLIFIER_GAIN_6_LEVEL_SHIFT				0

/* Audio DAC control */
#define	D1601XX_DAC_CONTROL									0x52
#define	D1601XX_DAC_CONTROL_VALUE							MAKE_64BIT_MASK(0, 8)	 // DAC control bits
#define	D1601XX_DAC_CONTROL_VALUE_SHIFT						0

/* Amplifier paths 7 and 8 gain */
#define	D1601XX_AMPLIFIER_GAIN_7_8							0x53
#define	D1601XX_AMPLIFIER_GAIN_7_8_GAIN						MAKE_64BIT_MASK(0, 7)	 // Amplifier gain code
#define	D1601XX_AMPLIFIER_GAIN_7_8_GAIN_SHIFT				0
#define	D1601XX_AMPLIFIER_GAIN_7_8_PATH_7_SELECT			BIT(7)					 // Select amplifier path 7

/* Amplifier path 9 gain */
#define	D1601XX_AMPLIFIER_GAIN_9							0x54
#define	D1601XX_AMPLIFIER_GAIN_9_GAIN						MAKE_64BIT_MASK(0, 7)	 // Amplifier gain code
#define	D1601XX_AMPLIFIER_GAIN_9_GAIN_SHIFT					0

/* Audio switch-mux path 5 */
#define	D1601XX_SWITCH_MUX_5								0x55
#define	D1601XX_SWITCH_MUX_5_VALUE							MAKE_64BIT_MASK(0, 8)	 // Mux control value
#define	D1601XX_SWITCH_MUX_5_VALUE_SHIFT					0

/* Audio mux-input selection */
#define	D1601XX_MUX_INPUT									0x56
#define	D1601XX_MUX_INPUT_VALUE								MAKE_64BIT_MASK(0, 8)	 // Mux input code
#define	D1601XX_MUX_INPUT_VALUE_SHIFT						0

/* Audio switch-mux path 3 */
#define	D1601XX_SWITCH_MUX_3								0x57
#define	D1601XX_SWITCH_MUX_3_VALUE							MAKE_64BIT_MASK(0, 8)	 // Mux control value
#define	D1601XX_SWITCH_MUX_3_VALUE_SHIFT					0

/* OUTPORT control */
#define	D1601XX_OUTPORT_CONTROL								0x58
#define	D1601XX_OUTPORT_CONTROL_MODE						MAKE_64BIT_MASK(0, 2)	 // OUTPORT operating mode
#define	D1601XX_OUTPORT_CONTROL_MODE_SHIFT					0
#define	D1601XX_OUTPORT_CONTROL_LEVEL						BIT(2)					 // OUTPORT output level

/* Audio ADC and DAC sample rates */
#define	D1601XX_SAMPLE_RATE									0x59
#define	D1601XX_SAMPLE_RATE_ADC								MAKE_64BIT_MASK(0, 4)	 // ADC sample-rate code from 0 to 8
#define	D1601XX_SAMPLE_RATE_ADC_SHIFT						0
#define	D1601XX_SAMPLE_RATE_DAC								MAKE_64BIT_MASK(4, 4)	 // DAC sample-rate code from 0 to 8
#define	D1601XX_SAMPLE_RATE_DAC_SHIFT						4

/* Stereo audio operating mode */
#define	D1601XX_STEREO_MODE									0x5A
#define	D1601XX_STEREO_MODE_VALUE							MAKE_64BIT_MASK(0, 8)	 // Stereo mode code
#define	D1601XX_STEREO_MODE_VALUE_SHIFT						0

/* I2S receive-path control */
#define	D1601XX_I2S_RX_CONTROL								0x5B
#define	D1601XX_I2S_RX_CONTROL_VALUE						MAKE_64BIT_MASK(0, 8)	 // Receive control bits
#define	D1601XX_I2S_RX_CONTROL_VALUE_SHIFT					0

/* I2S transmit-path control */
#define	D1601XX_I2S_TX_CONTROL								0x5C
#define	D1601XX_I2S_TX_CONTROL_VALUE						MAKE_64BIT_MASK(0, 8)	 // Transmit control bits
#define	D1601XX_I2S_TX_CONTROL_VALUE_SHIFT					0

/* Additional audio-path control */
#define	D1601XX_AUDIO_CONTROL_2								0x5D
#define	D1601XX_AUDIO_CONTROL_2_VALUE						MAKE_64BIT_MASK(0, 8)	 // Raw control value
#define	D1601XX_AUDIO_CONTROL_2_VALUE_SHIFT					0
