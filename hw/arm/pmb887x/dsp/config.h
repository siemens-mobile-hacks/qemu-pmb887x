#ifndef HW_ARM_PMB887X_DSP_CONFIG_H
#define HW_ARM_PMB887X_DSP_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define PMB887X_DSP_ADDRESS_SPACE_WORDS	0x10000U

typedef struct pmb887x_dsp_config_t pmb887x_dsp_config_t;
typedef struct pmb887x_dsp_peripheral_config_t pmb887x_dsp_peripheral_config_t;
typedef enum pmb887x_dsp_peripheral_type_t pmb887x_dsp_peripheral_type_t;

enum pmb887x_dsp_peripheral_type_t {
	PMB887X_DSP_PERIPHERAL_AFE,
	PMB887X_DSP_PERIPHERAL_BASEBAND,
	PMB887X_DSP_PERIPHERAL_CHANNEL_DECODER,
	PMB887X_DSP_PERIPHERAL_CIPHER,
	PMB887X_DSP_PERIPHERAL_DSP,
	PMB887X_DSP_PERIPHERAL_EQUALIZER,
	PMB887X_DSP_PERIPHERAL_I2S,
	PMB887X_DSP_PERIPHERAL_I2S_TX,
	PMB887X_DSP_PERIPHERAL_INTERRUPT,
	PMB887X_DSP_PERIPHERAL_MCS,
	PMB887X_DSP_PERIPHERAL_MODULATOR,
	PMB887X_DSP_PERIPHERAL_SSC,
	PMB887X_DSP_PERIPHERAL_TIMER1,
	PMB887X_DSP_PERIPHERAL_TIMER2,
	PMB887X_DSP_PERIPHERAL_XBIU,
};

struct pmb887x_dsp_peripheral_config_t {
	const char *name;
	pmb887x_dsp_peripheral_type_t type;
	uint16_t base;
	uint16_t size;
	uint16_t ram_base;
	uint16_t ram_size;
};

struct pmb887x_dsp_config_t {
	const char *name;
	uint16_t default_rom_version;
	uint16_t page_field_mask;
	uint8_t program_page_shift;
	uint16_t program_rom_base;
	uint16_t program_bank_base;
	size_t program_bank_count;
	uint16_t data_rom_base;
	uint16_t data_bank_base;
	size_t data_bank_count;
	uint16_t shared_base;
	uint16_t shared_size;
	uint16_t y_space_base;
	uint16_t mmio_base;
	uint16_t mmio_size;
	const pmb887x_dsp_peripheral_config_t *peripherals;
	size_t peripheral_count;
};

#endif
