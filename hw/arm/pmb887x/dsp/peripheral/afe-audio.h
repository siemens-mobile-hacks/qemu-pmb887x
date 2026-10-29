#ifndef HW_ARM_PMB887X_DSP_PERIPHERAL_AFE_AUDIO_H
#define HW_ARM_PMB887X_DSP_PERIPHERAL_AFE_AUDIO_H

typedef struct afe_audio_t afe_audio_t;

afe_audio_t *afe_audio_create(void);
void afe_audio_destroy(afe_audio_t *audio);
void afe_audio_reset(afe_audio_t *audio);
void afe_audio_set_rate(afe_audio_t *audio, unsigned frequency_hz);
void afe_audio_produce(afe_audio_t *audio, uint16_t sample_word);

#endif
