#pragma once

#include "qemu/bitops.h"

// R61505U
// Renesas R61505U extensions to the ILI9320-compatible register map
/* Display control 1 extension */
#define	R61505U_DISPLAY_CONTROL_1			0x07
#define	R61505U_DISPLAY_CONTROL_1_VON		BIT(6)					 // VCOM output enable

/* Power control 3 extension */
#define	R61505U_POWER_CONTROL_3				0x12
#define	R61505U_POWER_CONTROL_3_PSON		BIT(5)					 // Internal power supply operation enable

/* Power supply startup control */
#define	R61505U_POWER_CONTROL_5				0x17
#define	R61505U_POWER_CONTROL_5_PSE			BIT(0)					 // Power supply startup enable

/* Gamma control 4 */
#define	R61505U_GAMMA_33					0x33
#define	R61505U_GAMMA_33_VALUE				MAKE_64BIT_MASK(0, 16)	 // Raw gamma value
#define	R61505U_GAMMA_33_VALUE_SHIFT		0

/* Gamma control 5 */
#define	R61505U_GAMMA_34					0x34
#define	R61505U_GAMMA_34_VALUE				MAKE_64BIT_MASK(0, 16)	 // Raw gamma value
#define	R61505U_GAMMA_34_VALUE_SHIFT		0

/* Gamma control 11 */
#define	R61505U_GAMMA_3A					0x3A
#define	R61505U_GAMMA_3A_VALUE				MAKE_64BIT_MASK(0, 16)	 // Raw gamma value
#define	R61505U_GAMMA_3A_VALUE_SHIFT		0

/* Gamma control 12 */
#define	R61505U_GAMMA_3B					0x3B
#define	R61505U_GAMMA_3B_VALUE				MAKE_64BIT_MASK(0, 16)	 // Raw gamma value
#define	R61505U_GAMMA_3B_VALUE_SHIFT		0

/* NVM calibration control */
#define	R61505U_CALIBRATION_CONTROL			0xA4
#define	R61505U_CALIBRATION_CONTROL_CALB	BIT(0)					 // Load NVM data and calibrate the oscillator
