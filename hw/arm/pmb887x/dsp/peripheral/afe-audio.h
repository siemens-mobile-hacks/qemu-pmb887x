#ifndef HW_ARM_PMB887X_DSP_PERIPHERAL_AFE_AUDIO_H
#define HW_ARM_PMB887X_DSP_PERIPHERAL_AFE_AUDIO_H

#include "qemu/debug-channel.h"

typedef struct afe_audio_t afe_audio_t;

afe_audio_t *afe_audio_create(const DebugChannel *dbg);
void afe_audio_destroy(afe_audio_t *audio);
void afe_audio_set_active(afe_audio_t *audio, bool active);
void afe_audio_set_format(afe_audio_t *audio, unsigned frequency_hz, unsigned channels);
void afe_audio_produce(afe_audio_t *audio, const uint16_t *frame, size_t word_count);

#endif
