#pragma once

#include "qemu/osdep.h"
#include "qemu/debug-regs.h"

typedef struct pmb887x_cpu_meta_gpio_t pmb887x_cpu_meta_gpio_t;
typedef struct pmb887x_cpu_meta_irq_t pmb887x_cpu_meta_irq_t;
typedef struct pmb887x_cpu_meta_t pmb887x_cpu_meta_t;
typedef struct pmb887x_cpu_io_t pmb887x_cpu_io_t;
typedef struct pmb887x_io_meta_t pmb887x_io_meta_t;

typedef enum pmb887x_trace_io_t {
	PMB887X_TRACE_IO_CPU,
	PMB887X_TRACE_IO_DSP,
	PMB887X_TRACE_IO_HD155153NP,
	PMB887X_TRACE_IO_ILI9320,
	PMB887X_TRACE_IO_JBT6K71,
	PMB887X_TRACE_IO_LH15KAH2,
	PMB887X_TRACE_IO_LM4946,
	PMB887X_TRACE_IO_PASIC,
	PMB887X_TRACE_IO_PCF8833,
	PMB887X_TRACE_IO_PCF8882,
	PMB887X_TRACE_IO_PMB6272,
	PMB887X_TRACE_IO_PMB6812,
	PMB887X_TRACE_IO_R61505U,
	PMB887X_TRACE_IO_SSD1286,
	PMB887X_TRACE_IO_TEA5760UK,
	PMB887X_TRACE_IO_TEA5761UK,
	PMB887X_TRACE_IO_COUNT,
} pmb887x_trace_io_t;

struct pmb887x_cpu_io_t {
	const char *name;
	uint32_t base;
	uint32_t size;
	const DebugRegs *regs;
};

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

	const pmb887x_cpu_io_t *modules;
	int modules_count;

	const pmb887x_cpu_io_t *dsp_modules;
	int dsp_modules_count;
};

struct pmb887x_io_meta_t {
	const char *name;
	const DebugRegs *regs;
};

const pmb887x_cpu_meta_t *pmb887x_get_cpu_meta(int cpu);
const pmb887x_io_meta_t *pmb887x_get_io_meta(pmb887x_trace_io_t id);

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
extern const DebugRegs pmb887x_jbt6k71_regs;
extern const DebugRegs pmb887x_lh15kah2_regs;
extern const DebugRegs pmb887x_lm4946_regs;
extern const DebugRegs pmb887x_pasic_regs;
extern const DebugRegs pmb887x_pcf8833_regs;
extern const DebugRegs pmb887x_pcf8882_regs;
extern const DebugRegs pmb887x_pmb6272_regs;
extern const DebugRegs pmb887x_pmb6812_regs;
extern const DebugRegs pmb887x_r61505u_regs;
extern const DebugRegs pmb887x_ssd1286_regs;
extern const DebugRegs pmb887x_tea5760uk_regs;
extern const DebugRegs pmb887x_tea5761uk_regs;
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
