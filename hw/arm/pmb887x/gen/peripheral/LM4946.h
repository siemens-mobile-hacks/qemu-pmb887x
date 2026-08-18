#pragma once

#include "qemu/bitops.h"

// LM4946
// TI/National LM4946 Boomer audio power amplifier
#define	LM4946_I2C_ADDR						0x7D

/* Output mode and shutdown control */
#define	LM4946_MODE							0x00
#define	LM4946_MODE_MC						MAKE_64BIT_MASK(0, 3)	 // Output mode select
#define	LM4946_MODE_MC_SHIFT				0
#define	LM4946_MODE_MC_ALL_SHUTDOWN			0x0
#define	LM4946_MODE_MC_MONO_IN_TO_SPK		0x1
#define	LM4946_MODE_MC_MONO_IN_TO_HP		0x2
#define	LM4946_MODE_MC_LINE_IN_TO_SPK		0x3
#define	LM4946_MODE_MC_LINE_IN_TO_HP		0x4
#define	LM4946_MODE_MC_LINE_MONO_TO_SPK		0x5
#define	LM4946_MODE_MC_LINE_MONO_TO_HP		0x6
#define	LM4946_MODE_MC_LINE_IN_TO_SPK_HP	0x7
#define	LM4946_MODE_OCL						BIT(3)					 // Single-ended output-capacitor-less mode

/* Programmable 3D enhancement */
#define	LM4946_N3D							0x40
#define	LM4946_N3D_EN						BIT(0)					 // Enable 3D enhancement
#define	LM4946_N3D_NARROW					BIT(1)					 // Soundstage width
#define	LM4946_N3D_NARROW_WIDE				0x0
#define	LM4946_N3D_NARROW_NARROW			0x2
#define	LM4946_N3D_LEVEL					MAKE_64BIT_MASK(2, 2)	 // Enhancement level
#define	LM4946_N3D_LEVEL_SHIFT				2
#define	LM4946_N3D_LEVEL_LOW				0x0
#define	LM4946_N3D_LEVEL_MEDIUM				0x4
#define	LM4946_N3D_LEVEL_HIGH				0x8
#define	LM4946_N3D_LEVEL_MAX				0xC

/* Mono (MONO_IN) volume */
#define	LM4946_MONO_VOL						0x80
#define	LM4946_MONO_VOL_VOL					MAKE_64BIT_MASK(0, 5)	 // Volume code from -54 dB (0x00) through 0 dB (0x13) to +18 dB (0x1F)
#define	LM4946_MONO_VOL_VOL_SHIFT			0

/* Left (LIN) volume */
#define	LM4946_LEFT_VOL						0xC0
#define	LM4946_LEFT_VOL_VOL					MAKE_64BIT_MASK(0, 5)	 // Volume code from -54 dB (0x00) through 0 dB (0x13) to +18 dB (0x1F)
#define	LM4946_LEFT_VOL_VOL_SHIFT			0

/* Right (RIN) volume */
#define	LM4946_RIGHT_VOL					0xE0
#define	LM4946_RIGHT_VOL_VOL				MAKE_64BIT_MASK(0, 5)	 // Volume code from -54 dB (0x00) through 0 dB (0x13) to +18 dB (0x1F)
#define	LM4946_RIGHT_VOL_VOL_SHIFT			0
