#pragma once

#include "qemu/bitops.h"

// ILI9320
// Ilitek ILI9320 TFT LCD controller
/* Oscillator startup and device-code access */
#define	ILI9320_OSCILLATION							0x00
#define	ILI9320_OSCILLATION_OSC						BIT(0)					 // Internal oscillator enable

/* Source shift direction and gate scan arrangement */
#define	ILI9320_DRIVER_OUTPUT_CONTROL				0x01
#define	ILI9320_DRIVER_OUTPUT_CONTROL_SS			BIT(8)					 // Source output shift direction
#define	ILI9320_DRIVER_OUTPUT_CONTROL_SM			BIT(10)					 // Gate driver pin arrangement

/* LCD AC-drive waveform selection */
#define	ILI9320_LCD_DRIVING_CONTROL					0x02
#define	ILI9320_LCD_DRIVING_CONTROL_EOR				BIT(8)					 // Frame and line inversion XOR control
#define	ILI9320_LCD_DRIVING_CONTROL_B_C				BIT(9)					 // Frame or line inversion selection

/* MPU format, color order, and GRAM address update */
#define	ILI9320_ENTRY_MODE							0x03
#define	ILI9320_ENTRY_MODE_AM						BIT(3)					 // Address counter update axis
#define	ILI9320_ENTRY_MODE_ID						MAKE_64BIT_MASK(4, 2)	 // Horizontal and vertical address directions
#define	ILI9320_ENTRY_MODE_ID_SHIFT					4
#define	ILI9320_ENTRY_MODE_ORG						BIT(7)					 // Window origin relocation enable
#define	ILI9320_ENTRY_MODE_HWM						BIT(9)					 // High-speed GRAM write enable
#define	ILI9320_ENTRY_MODE_BGR						BIT(12)					 // Source RGB component order
#define	ILI9320_ENTRY_MODE_DFM						BIT(14)					 // 262k-color transfer arrangement
#define	ILI9320_ENTRY_MODE_TRI						BIT(15)					 // 262k-color transfer enable

/* GRAM write resizing control */
#define	ILI9320_RESIZE_CONTROL						0x04
#define	ILI9320_RESIZE_CONTROL_RSZ					MAKE_64BIT_MASK(0, 2)	 // Image resizing factor
#define	ILI9320_RESIZE_CONTROL_RSZ_SHIFT			0
#define	ILI9320_RESIZE_CONTROL_RCH					MAKE_64BIT_MASK(4, 2)	 // Horizontal remainder pixel count
#define	ILI9320_RESIZE_CONTROL_RCH_SHIFT			4
#define	ILI9320_RESIZE_CONTROL_RCV					MAKE_64BIT_MASK(8, 2)	 // Vertical remainder pixel count
#define	ILI9320_RESIZE_CONTROL_RCV_SHIFT			8

/* Display, partial image, and gate output control */
#define	ILI9320_DISPLAY_CONTROL_1					0x07
#define	ILI9320_DISPLAY_CONTROL_1_D					MAKE_64BIT_MASK(0, 2)	 // Display operating state
#define	ILI9320_DISPLAY_CONTROL_1_D_SHIFT			0
#define	ILI9320_DISPLAY_CONTROL_1_CL				BIT(3)					 // Eight-color display mode enable
#define	ILI9320_DISPLAY_CONTROL_1_DTE				BIT(4)					 // Display timing enable
#define	ILI9320_DISPLAY_CONTROL_1_GON				BIT(5)					 // Gate output enable
#define	ILI9320_DISPLAY_CONTROL_1_BASEE				BIT(8)					 // Base image display enable
#define	ILI9320_DISPLAY_CONTROL_1_PTDE				MAKE_64BIT_MASK(12, 2)	 // Partial image enables
#define	ILI9320_DISPLAY_CONTROL_1_PTDE_SHIFT		12

/* Vertical front and back porch timing */
#define	ILI9320_DISPLAY_CONTROL_2					0x08
#define	ILI9320_DISPLAY_CONTROL_2_BP				MAKE_64BIT_MASK(0, 4)	 // Vertical back porch in lines
#define	ILI9320_DISPLAY_CONTROL_2_BP_SHIFT			0
#define	ILI9320_DISPLAY_CONTROL_2_FP				MAKE_64BIT_MASK(8, 4)	 // Vertical front porch in lines
#define	ILI9320_DISPLAY_CONTROL_2_FP_SHIFT			8

/* Non-display scan and source output control */
#define	ILI9320_DISPLAY_CONTROL_3					0x09
#define	ILI9320_DISPLAY_CONTROL_3_ISC				MAKE_64BIT_MASK(0, 4)	 // Interval scan cycle
#define	ILI9320_DISPLAY_CONTROL_3_ISC_SHIFT			0
#define	ILI9320_DISPLAY_CONTROL_3_PTG				MAKE_64BIT_MASK(4, 2)	 // Non-display gate scan mode
#define	ILI9320_DISPLAY_CONTROL_3_PTG_SHIFT			4
#define	ILI9320_DISPLAY_CONTROL_3_PTS				MAKE_64BIT_MASK(8, 3)	 // Non-display source output level
#define	ILI9320_DISPLAY_CONTROL_3_PTS_SHIFT			8

/* Frame-marker output control */
#define	ILI9320_DISPLAY_CONTROL_4					0x0A
#define	ILI9320_DISPLAY_CONTROL_4_FMI				MAKE_64BIT_MASK(0, 3)	 // Frame-marker output interval
#define	ILI9320_DISPLAY_CONTROL_4_FMI_SHIFT			0
#define	ILI9320_DISPLAY_CONTROL_4_FMARKOE			BIT(3)					 // Frame-marker output enable

/* GRAM interface and display synchronization source */
#define	ILI9320_RGB_INTERFACE_CONTROL_1				0x0C
#define	ILI9320_RGB_INTERFACE_CONTROL_1_RIM			MAKE_64BIT_MASK(0, 2)	 // RGB interface width
#define	ILI9320_RGB_INTERFACE_CONTROL_1_RIM_SHIFT	0
#define	ILI9320_RGB_INTERFACE_CONTROL_1_DM			MAKE_64BIT_MASK(4, 2)	 // Display synchronization source
#define	ILI9320_RGB_INTERFACE_CONTROL_1_DM_SHIFT	4
#define	ILI9320_RGB_INTERFACE_CONTROL_1_RM			BIT(8)					 // GRAM access interface
#define	ILI9320_RGB_INTERFACE_CONTROL_1_ENC			MAKE_64BIT_MASK(13, 3)	 // RGB-interface GRAM write cycle
#define	ILI9320_RGB_INTERFACE_CONTROL_1_ENC_SHIFT	13

/* Frame-marker output position */
#define	ILI9320_FRAME_MARKER						0x0D
#define	ILI9320_FRAME_MARKER_FMP					MAKE_64BIT_MASK(0, 9)	 // Frame-marker output line
#define	ILI9320_FRAME_MARKER_FMP_SHIFT				0

/* External display-signal polarities */
#define	ILI9320_RGB_INTERFACE_CONTROL_2				0x0F
#define	ILI9320_RGB_INTERFACE_CONTROL_2_DPL			BIT(0)					 // DOTCLK sampling edge
#define	ILI9320_RGB_INTERFACE_CONTROL_2_EPL			BIT(1)					 // ENABLE active polarity
#define	ILI9320_RGB_INTERFACE_CONTROL_2_HSPL		BIT(3)					 // HSYNC active polarity
#define	ILI9320_RGB_INTERFACE_CONTROL_2_VSPL		BIT(4)					 // VSYNC active polarity

/* Step-up circuits, amplifiers, and standby control */
#define	ILI9320_POWER_CONTROL_1						0x10
#define	ILI9320_POWER_CONTROL_1_SLP					BIT(1)					 // Sleep mode enable
#define	ILI9320_POWER_CONTROL_1_DSTB				BIT(2)					 // Deep standby enable
#define	ILI9320_POWER_CONTROL_1_AP					MAKE_64BIT_MASK(4, 3)	 // Operational-amplifier drive current
#define	ILI9320_POWER_CONTROL_1_AP_SHIFT			4
#define	ILI9320_POWER_CONTROL_1_APE					BIT(7)					 // Power supply enable
#define	ILI9320_POWER_CONTROL_1_BT					MAKE_64BIT_MASK(8, 4)	 // Step-up circuit factors
#define	ILI9320_POWER_CONTROL_1_BT_SHIFT			8
#define	ILI9320_POWER_CONTROL_1_SAP					BIT(12)					 // Source driver output enable

/* Step-up clocks and VCI reference control */
#define	ILI9320_POWER_CONTROL_2						0x11
#define	ILI9320_POWER_CONTROL_2_VC					MAKE_64BIT_MASK(0, 3)	 // VCI reference voltage ratio
#define	ILI9320_POWER_CONTROL_2_VC_SHIFT			0
#define	ILI9320_POWER_CONTROL_2_DC0					MAKE_64BIT_MASK(4, 3)	 // Step-up circuit 1 frequency
#define	ILI9320_POWER_CONTROL_2_DC0_SHIFT			4
#define	ILI9320_POWER_CONTROL_2_DC1					MAKE_64BIT_MASK(8, 3)	 // Step-up circuit 2 frequency
#define	ILI9320_POWER_CONTROL_2_DC1_SHIFT			8

/* VGL, VREG1OUT, and VCOM reference control */
#define	ILI9320_POWER_CONTROL_3						0x12
#define	ILI9320_POWER_CONTROL_3_VRH					MAKE_64BIT_MASK(0, 4)	 // VREG1OUT amplification ratio
#define	ILI9320_POWER_CONTROL_3_VRH_SHIFT			0
#define	ILI9320_POWER_CONTROL_3_PON					BIT(4)					 // VGL output enable
#define	ILI9320_POWER_CONTROL_3_VCMR				BIT(8)					 // Internal VCOMH adjustment enable

/* VCOM alternating amplitude control */
#define	ILI9320_POWER_CONTROL_4						0x13
#define	ILI9320_POWER_CONTROL_4_VDV					MAKE_64BIT_MASK(8, 5)	 // VCOM alternating amplitude
#define	ILI9320_POWER_CONTROL_4_VDV_SHIFT			8

/* GRAM horizontal address */
#define	ILI9320_GRAM_X								0x20
#define	ILI9320_GRAM_X_AD							MAKE_64BIT_MASK(0, 8)	 // Horizontal address counter value
#define	ILI9320_GRAM_X_AD_SHIFT						0

/* GRAM vertical address */
#define	ILI9320_GRAM_Y								0x21
#define	ILI9320_GRAM_Y_AD							MAKE_64BIT_MASK(0, 9)	 // Vertical address counter value
#define	ILI9320_GRAM_Y_AD_SHIFT						0

/* Interface-formatted GRAM pixel access */
#define	ILI9320_GRAM_DATA							0x22
#define	ILI9320_GRAM_DATA_DATA						MAKE_64BIT_MASK(0, 18)	 // Display pixel data
#define	ILI9320_GRAM_DATA_DATA_SHIFT				0

/* Internal VCOMH voltage control */
#define	ILI9320_POWER_CONTROL_7						0x29
#define	ILI9320_POWER_CONTROL_7_VCM					MAKE_64BIT_MASK(0, 5)	 // VCOMH voltage ratio
#define	ILI9320_POWER_CONTROL_7_VCM_SHIFT			0

/* Internal oscillator and frame-rate control */
#define	ILI9320_FRAME_RATE_CONTROL					0x2B
#define	ILI9320_FRAME_RATE_CONTROL_FR_SEL			MAKE_64BIT_MASK(4, 2)	 // Internal oscillator frame rate
#define	ILI9320_FRAME_RATE_CONTROL_FR_SEL_SHIFT		4
#define	ILI9320_FRAME_RATE_CONTROL_EXT_R			BIT(7)					 // External oscillator resistor enable

/* Gamma control 1 */
#define	ILI9320_GAMMA_30							0x30
#define	ILI9320_GAMMA_30_VALUE						MAKE_64BIT_MASK(0, 16)	 // Raw gamma value
#define	ILI9320_GAMMA_30_VALUE_SHIFT				0

/* Gamma control 2 */
#define	ILI9320_GAMMA_31							0x31
#define	ILI9320_GAMMA_31_VALUE						MAKE_64BIT_MASK(0, 16)	 // Raw gamma value
#define	ILI9320_GAMMA_31_VALUE_SHIFT				0

/* Gamma control 3 */
#define	ILI9320_GAMMA_32							0x32
#define	ILI9320_GAMMA_32_VALUE						MAKE_64BIT_MASK(0, 16)	 // Raw gamma value
#define	ILI9320_GAMMA_32_VALUE_SHIFT				0

/* Gamma control 4 */
#define	ILI9320_GAMMA_35							0x35
#define	ILI9320_GAMMA_35_VALUE						MAKE_64BIT_MASK(0, 16)	 // Raw gamma value
#define	ILI9320_GAMMA_35_VALUE_SHIFT				0

/* Gamma control 5 */
#define	ILI9320_GAMMA_36							0x36
#define	ILI9320_GAMMA_36_VALUE						MAKE_64BIT_MASK(0, 16)	 // Raw gamma value
#define	ILI9320_GAMMA_36_VALUE_SHIFT				0

/* Gamma control 6 */
#define	ILI9320_GAMMA_37							0x37
#define	ILI9320_GAMMA_37_VALUE						MAKE_64BIT_MASK(0, 16)	 // Raw gamma value
#define	ILI9320_GAMMA_37_VALUE_SHIFT				0

/* Gamma control 7 */
#define	ILI9320_GAMMA_38							0x38
#define	ILI9320_GAMMA_38_VALUE						MAKE_64BIT_MASK(0, 16)	 // Raw gamma value
#define	ILI9320_GAMMA_38_VALUE_SHIFT				0

/* Gamma control 8 */
#define	ILI9320_GAMMA_39							0x39
#define	ILI9320_GAMMA_39_VALUE						MAKE_64BIT_MASK(0, 16)	 // Raw gamma value
#define	ILI9320_GAMMA_39_VALUE_SHIFT				0

/* Gamma control 9 */
#define	ILI9320_GAMMA_3C							0x3C
#define	ILI9320_GAMMA_3C_VALUE						MAKE_64BIT_MASK(0, 16)	 // Raw gamma value
#define	ILI9320_GAMMA_3C_VALUE_SHIFT				0

/* Gamma control 10 */
#define	ILI9320_GAMMA_3D							0x3D
#define	ILI9320_GAMMA_3D_VALUE						MAKE_64BIT_MASK(0, 16)	 // Raw gamma value
#define	ILI9320_GAMMA_3D_VALUE_SHIFT				0

/* Horizontal GRAM window start */
#define	ILI9320_WINDOW_X_START						0x50
#define	ILI9320_WINDOW_X_START_HSA					MAKE_64BIT_MASK(0, 8)	 // Horizontal start address
#define	ILI9320_WINDOW_X_START_HSA_SHIFT			0

/* Horizontal GRAM window end */
#define	ILI9320_WINDOW_X_END						0x51
#define	ILI9320_WINDOW_X_END_HEA					MAKE_64BIT_MASK(0, 8)	 // Horizontal end address
#define	ILI9320_WINDOW_X_END_HEA_SHIFT				0

/* Vertical GRAM window start */
#define	ILI9320_WINDOW_Y_START						0x52
#define	ILI9320_WINDOW_Y_START_VSA					MAKE_64BIT_MASK(0, 9)	 // Vertical start address
#define	ILI9320_WINDOW_Y_START_VSA_SHIFT			0

/* Vertical GRAM window end */
#define	ILI9320_WINDOW_Y_END						0x53
#define	ILI9320_WINDOW_Y_END_VEA					MAKE_64BIT_MASK(0, 9)	 // Vertical end address
#define	ILI9320_WINDOW_Y_END_VEA_SHIFT				0

/* Gate scan direction, line count, and start position */
#define	ILI9320_GATE_SCAN							0x60
#define	ILI9320_GATE_SCAN_SCN						MAKE_64BIT_MASK(0, 6)	 // Gate scan start position
#define	ILI9320_GATE_SCAN_SCN_SHIFT					0
#define	ILI9320_GATE_SCAN_NL						MAKE_64BIT_MASK(8, 6)	 // Driven LCD line count
#define	ILI9320_GATE_SCAN_NL_SHIFT					8
#define	ILI9320_GATE_SCAN_GS						BIT(15)					 // Gate output shift direction

/* Base image polarity and scrolling control */
#define	ILI9320_BASE_IMAGE							0x61
#define	ILI9320_BASE_IMAGE_REV						BIT(0)					 // Grayscale inversion enable
#define	ILI9320_BASE_IMAGE_VLE						BIT(1)					 // Vertical scrolling enable
#define	ILI9320_BASE_IMAGE_NDL						BIT(2)					 // Non-display source output level

/* Base image vertical scroll offset */
#define	ILI9320_VERTICAL_SCROLL						0x6A
#define	ILI9320_VERTICAL_SCROLL_VL					MAKE_64BIT_MASK(0, 9)	 // Vertical scroll amount
#define	ILI9320_VERTICAL_SCROLL_VL_SHIFT			0

/* Partial image 1 display position */
#define	ILI9320_PARTIAL_1_POSITION					0x80
#define	ILI9320_PARTIAL_1_POSITION_PTDP				MAKE_64BIT_MASK(0, 9)	 // Display start line
#define	ILI9320_PARTIAL_1_POSITION_PTDP_SHIFT		0

/* Partial image 1 GRAM start line */
#define	ILI9320_PARTIAL_1_START						0x81
#define	ILI9320_PARTIAL_1_START_PTSA				MAKE_64BIT_MASK(0, 9)	 // GRAM start line
#define	ILI9320_PARTIAL_1_START_PTSA_SHIFT			0

/* Partial image 1 GRAM end line */
#define	ILI9320_PARTIAL_1_END						0x82
#define	ILI9320_PARTIAL_1_END_PTEA					MAKE_64BIT_MASK(0, 9)	 // GRAM end line
#define	ILI9320_PARTIAL_1_END_PTEA_SHIFT			0

/* Partial image 2 display position */
#define	ILI9320_PARTIAL_2_POSITION					0x83
#define	ILI9320_PARTIAL_2_POSITION_PTDP				MAKE_64BIT_MASK(0, 9)	 // Display start line
#define	ILI9320_PARTIAL_2_POSITION_PTDP_SHIFT		0

/* Partial image 2 GRAM start line */
#define	ILI9320_PARTIAL_2_START						0x84
#define	ILI9320_PARTIAL_2_START_PTSA				MAKE_64BIT_MASK(0, 9)	 // GRAM start line
#define	ILI9320_PARTIAL_2_START_PTSA_SHIFT			0

/* Partial image 2 GRAM end line */
#define	ILI9320_PARTIAL_2_END						0x85
#define	ILI9320_PARTIAL_2_END_PTEA					MAKE_64BIT_MASK(0, 9)	 // GRAM end line
#define	ILI9320_PARTIAL_2_END_PTEA_SHIFT			0

/* Internal-clock line timing */
#define	ILI9320_PANEL_INTERFACE_1					0x90
#define	ILI9320_PANEL_INTERFACE_1_RTNI				MAKE_64BIT_MASK(0, 5)	 // Internal 1H clock-cycle count
#define	ILI9320_PANEL_INTERFACE_1_RTNI_SHIFT		0
#define	ILI9320_PANEL_INTERFACE_1_DIVI				MAKE_64BIT_MASK(8, 2)	 // Internal oscillator division ratio
#define	ILI9320_PANEL_INTERFACE_1_DIVI_SHIFT		8

/* Internal-clock gate non-overlap timing */
#define	ILI9320_PANEL_INTERFACE_2					0x92
#define	ILI9320_PANEL_INTERFACE_2_NOWI				MAKE_64BIT_MASK(8, 3)	 // Gate non-overlap period
#define	ILI9320_PANEL_INTERFACE_2_NOWI_SHIFT		8

/* Internal-clock source output timing */
#define	ILI9320_PANEL_INTERFACE_3					0x93
#define	ILI9320_PANEL_INTERFACE_3_MCPI				MAKE_64BIT_MASK(0, 3)	 // Source output position
#define	ILI9320_PANEL_INTERFACE_3_MCPI_SHIFT		0

/* External-DOTCLK line timing */
#define	ILI9320_PANEL_INTERFACE_4					0x95
#define	ILI9320_PANEL_INTERFACE_4_RTNE				MAKE_64BIT_MASK(0, 6)	 // External 1H clock-cycle count
#define	ILI9320_PANEL_INTERFACE_4_RTNE_SHIFT		0
#define	ILI9320_PANEL_INTERFACE_4_DIVE				MAKE_64BIT_MASK(8, 2)	 // DOTCLK division ratio
#define	ILI9320_PANEL_INTERFACE_4_DIVE_SHIFT		8

/* External-clock gate non-overlap timing */
#define	ILI9320_PANEL_INTERFACE_5					0x97
#define	ILI9320_PANEL_INTERFACE_5_NOWE				MAKE_64BIT_MASK(8, 4)	 // Gate non-overlap period
#define	ILI9320_PANEL_INTERFACE_5_NOWE_SHIFT		8

/* External-clock source output timing */
#define	ILI9320_PANEL_INTERFACE_6					0x98
#define	ILI9320_PANEL_INTERFACE_6_MCPE				MAKE_64BIT_MASK(0, 3)	 // Source output position
#define	ILI9320_PANEL_INTERFACE_6_MCPE_SHIFT		0
