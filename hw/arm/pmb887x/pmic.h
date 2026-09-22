#pragma once
#include "qemu/osdep.h"

/*
 * Every sound source in the machine reaches the speaker through one of the
 * analog codec's amplifier paths, and that amplifier is the only place the
 * phone's volume setting lands. The CAPCOM compare output that carries .srt
 * ringtones goes through path 0, and the DSP stream over I2S through path 4;
 * whichever path is not in use is parked silent.
 *
 * Returns the path's current level as a fraction of full scale, where
 * PMB887X_PMIC_GAIN_UNITY is 1.0. Boards with no codec read back unity.
 */
#define PMB887X_PMIC_GAIN_UNITY		65536u

typedef enum {
	PMB887X_PMIC_PATH_TONE,
	PMB887X_PMIC_PATH_STREAM,
	PMB887X_PMIC_PATH_COUNT,
} pmb887x_pmic_path_t;

uint32_t pmb887x_pmic_output_gain(pmb887x_pmic_path_t path);
