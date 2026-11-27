/* PMB887X-WEB FEATURE (mic): the AFE's microphone input (dsp/peripheral/afe-mic.c) */
#ifndef HW_ARM_PMB887X_DSP_AFE_MIC_H
#define HW_ARM_PMB887X_DSP_AFE_MIC_H

/* the next 8 kHz sample for the mic ADC: PMB887X_MIC_FILE, else the live samples, else silence */
uint16_t pmb887x_afe_mic_next(void);

/* live microphone (the browser page, ui/wasm.c): queue @count samples, returns how many were kept */
size_t pmb887x_afe_mic_push(const int16_t *samples, size_t count);

/* the live microphone stopped: drop what it queued */
void pmb887x_afe_mic_flush(void);

#endif
