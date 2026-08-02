#ifndef HW_ARM_PMB887X_DSP_PERIPHERAL_CIPHER_KASUMI_H
#define HW_ARM_PMB887X_DSP_PERIPHERAL_CIPHER_KASUMI_H

void cipher_kgcore(uint8_t ca, uint8_t cb, uint32_t cc, uint8_t cd, uint16_t ce,
	const uint8_t key[16], uint8_t *output, size_t bits);

#endif
