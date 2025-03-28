#pragma once

#include "qemu/osdep.h"
#include "qapi/qapi-types-ui.h"
#include "adc.h"

enum {
	PMB887X_MEMORY_TYPE_NONE = 0,
	PMB887X_MEMORY_TYPE_RAM,
	PMB887X_MEMORY_TYPE_FLASH,
};

typedef struct {
	uint32_t id;
	char name[64];
	char func_name[64];
	char full_name[64];
	bool value;
} pmb887x_board_gpio_t;

typedef struct {
	int type;
	uint32_t size;
	uint16_t vid;
	uint16_t pid;
} pmb887x_board_memory_t;

typedef struct {
	char type[32];
	uint32_t width;
	uint32_t height;
	uint32_t rotation;
	bool flip_horizontal;
	bool flip_vertical;
} pmb887x_board_display_t;

typedef struct {
	char type[64];
	uint8_t addr;
} pmb887x_board_i2c_dev_t;

typedef struct {
	uint32_t ram0_value;
} pmb887x_board_dsp_dev_t;

typedef struct {
	char vendor[64];
	char model[64];
	uint32_t cpu;
	
	// ADC inputs
	pmb887x_adc_input_t adc_inputs[PMB887X_ADC_MAX_INPUTS];
	
	// Hardware CSx to memory
	pmb887x_board_memory_t cs2memory[4];
	
	pmb887x_board_display_t display;
	
	uint32_t keymap[Q_KEY_CODE__MAX];
	
	pmb887x_board_dsp_dev_t dsp;

	pmb887x_board_i2c_dev_t *i2c_devices;
	uint32_t i2c_devices_count;
	
	pmb887x_board_gpio_t *gpios;
	uint32_t gpios_count;
} pmb887x_board_t;

const pmb887x_board_t *pmb887x_get_board(const char *config);
const uint8_t *pmb887x_get_brom_image(uint32_t cpu, size_t *size);
