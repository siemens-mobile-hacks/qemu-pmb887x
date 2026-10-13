#include "qemu/osdep.h"

#include "hw/arm/pmb887x/audio/pasic.h"
#include "hw/arm/pmb887x/gen/peripheral/PASIC.h"

#define PASIC_AMPLIFIER_PATH_4_MUTE_GAIN	0x39
#define PASIC_AMPLIFIER_PATH_5_MUTE_GAIN	0x00

bool pmb887x_pasic_audio_output_is_enabled(uint8_t mono_control, uint8_t stereo_control, uint8_t amplifier) {
	if (mono_control == 0 && stereo_control == 0)
		return false;

	uint8_t gain = amplifier & PASIC_AMPLIFIER_GAIN_4_5_GAIN;
	if ((amplifier & PASIC_AMPLIFIER_GAIN_4_5_PATH_4_SELECT) != 0)
		return gain != PASIC_AMPLIFIER_PATH_4_MUTE_GAIN;
	return gain != PASIC_AMPLIFIER_PATH_5_MUTE_GAIN;
}
