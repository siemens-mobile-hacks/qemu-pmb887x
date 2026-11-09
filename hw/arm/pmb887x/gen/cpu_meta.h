#pragma once

#include "qemu/osdep.h"
#include "qemu/debug-regs.h"

typedef struct pmb887x_cpu_meta_gpio_t pmb887x_cpu_meta_gpio_t;
typedef struct pmb887x_cpu_meta_irq_t pmb887x_cpu_meta_irq_t;
typedef struct pmb887x_cpu_meta_t pmb887x_cpu_meta_t;

struct pmb887x_cpu_meta_irq_t {
	const char *name;
	uint32_t id;
	uint32_t addr;
};

struct pmb887x_cpu_meta_gpio_t {
	const char *name;
	const char *func_name;
	const char *full_name;
	uint32_t id;
};

struct pmb887x_cpu_meta_t {
	const char *name;

	const pmb887x_cpu_meta_irq_t *irqs;
	int irqs_count;

	const pmb887x_cpu_meta_gpio_t *gpios;
	int gpios_count;
};

const pmb887x_cpu_meta_t *pmb887x_get_cpu_meta(int cpu);

extern const DebugRegs pmb887x_adc_regs;
extern const DebugRegs pmb887x_afc_regs;
extern const DebugRegs pmb887x_dmac_regs;
extern const DebugRegs pmb887x_mci_regs;
extern const DebugRegs pmb887x_capcom_regs;
extern const DebugRegs pmb887x_cgu_regs;
extern const DebugRegs pmb887x_cif_regs;
extern const DebugRegs pmb887x_difv1_regs;
extern const DebugRegs pmb887x_difv2_regs;
extern const DebugRegs pmb887x_dsp_regs;
extern const DebugRegs pmb887x_ebu_regs;
extern const DebugRegs pmb887x_gpio_regs;
extern const DebugRegs pmb887x_gprscu_regs;
extern const DebugRegs pmb887x_gptu_regs;
extern const DebugRegs pmb887x_i2cv1_regs;
extern const DebugRegs pmb887x_i2cv2_regs;
extern const DebugRegs pmb887x_keypad_regs;
extern const DebugRegs pmb887x_mmci_regs;
extern const DebugRegs pmb887x_mmicif_regs;
extern const DebugRegs pmb887x_rtc_regs;
extern const DebugRegs pmb887x_sccu_regs;
extern const DebugRegs pmb887x_scu_regs;
extern const DebugRegs pmb887x_sim_regs;
extern const DebugRegs pmb887x_ssc_regs;
extern const DebugRegs pmb887x_stm_regs;
extern const DebugRegs pmb887x_tpu_regs;
extern const DebugRegs pmb887x_usart_regs;
extern const DebugRegs pmb887x_usb_regs;
extern const DebugRegs pmb887x_usif_regs;
extern const DebugRegs pmb887x_vic_regs;
extern const DebugRegs pmb887x_hd155153np_regs;
extern const DebugRegs pmb887x_ili9320_regs;
extern const DebugRegs pmb887x_pcf8833_regs;
extern const DebugRegs pmb887x_pmb6272_regs;
extern const DebugRegs pmb887x_r61505u_regs;
extern const DebugRegs pmb887x_tea5760uk_regs;
extern const DebugRegs pmb887x_dsp_afe_regs;
extern const DebugRegs pmb887x_dsp_baseband_regs;
extern const DebugRegs pmb887x_dsp_channel_decoder_regs;
extern const DebugRegs pmb887x_dsp_cipher_regs;
extern const DebugRegs pmb887x_dsp_dsp_regs;
extern const DebugRegs pmb887x_dsp_dsp_8876_regs;
extern const DebugRegs pmb887x_dsp_equalizer_regs;
extern const DebugRegs pmb887x_dsp_i2s_regs;
extern const DebugRegs pmb887x_dsp_i2s_tx_regs;
extern const DebugRegs pmb887x_dsp_interrupt_regs;
extern const DebugRegs pmb887x_dsp_mcs_regs;
extern const DebugRegs pmb887x_dsp_modulator_regs;
extern const DebugRegs pmb887x_dsp_ssc_regs;
extern const DebugRegs pmb887x_dsp_timer1_regs;
extern const DebugRegs pmb887x_dsp_timer2_regs;
