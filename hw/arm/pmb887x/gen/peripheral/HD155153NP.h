#pragma once

#include "qemu/bitops.h"

// HD155153NP
// Hitachi HD155153NP GSM RF transceiver
/* Channel synthesizer configuration */
#define	HD155153NP_CHANNEL_PLL						0x01
#define	HD155153NP_CHANNEL_PLL_SYNTHESIZER			MAKE_64BIT_MASK(0, 19)	 // Synthesizer channel word
#define	HD155153NP_CHANNEL_PLL_SYNTHESIZER_SHIFT	0
#define	HD155153NP_CHANNEL_PLL_MODE					MAKE_64BIT_MASK(19, 2)	 // Operating mode
#define	HD155153NP_CHANNEL_PLL_MODE_SHIFT			19

/* Gain configuration */
#define	HD155153NP_GAIN_CONTROL						0x02
#define	HD155153NP_GAIN_CONTROL_PGA_GAIN			MAKE_64BIT_MASK(0, 6)	 // Programmable-gain amplifier step
#define	HD155153NP_GAIN_CONTROL_PGA_GAIN_SHIFT		0
#define	HD155153NP_GAIN_CONTROL_MIXER2_LOW_GAIN		BIT(6)					 // Second mixer low-gain mode
#define	HD155153NP_GAIN_CONTROL_MIXER1_LOW_GAIN		BIT(7)					 // First mixer low-gain mode

/* Undocumented 8-bit control register */
#define	HD155153NP_CONTROL_R4						0x04
#define	HD155153NP_CONTROL_R4_DATA					MAKE_64BIT_MASK(0, 5)	 // Raw control data
#define	HD155153NP_CONTROL_R4_DATA_SHIFT			0

/* Undocumented 24-bit configuration register */
#define	HD155153NP_CONFIG_R5						0x05
#define	HD155153NP_CONFIG_R5_DATA					MAKE_64BIT_MASK(0, 21)	 // Raw configuration data
#define	HD155153NP_CONFIG_R5_DATA_SHIFT				0

/* Calibration and configuration commands */
#define	HD155153NP_CALIBRATION_R6					0x06
#define	HD155153NP_CALIBRATION_R6_SLOT				MAKE_64BIT_MASK(0, 3)	 // Command slot
#define	HD155153NP_CALIBRATION_R6_SLOT_SHIFT		0
#define	HD155153NP_CALIBRATION_R6_DATA				MAKE_64BIT_MASK(3, 18)	 // Raw command data
#define	HD155153NP_CALIBRATION_R6_DATA_SHIFT		3

/* Undocumented 8-bit control register */
#define	HD155153NP_CONTROL_R7						0x07
#define	HD155153NP_CONTROL_R7_DATA					MAKE_64BIT_MASK(0, 5)	 // Raw control data
#define	HD155153NP_CONTROL_R7_DATA_SHIFT			0
