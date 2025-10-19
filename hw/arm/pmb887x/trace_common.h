#pragma once

#include "qemu/osdep.h"

#define PMB887X_TRACE_UNHANDLED_IO false

enum pmb887x_modules_t {
	// CPU modules
	PMB887X_TRACE_GPTU		= 1ULL << 0,
	PMB887X_TRACE_TPU		= 1ULL << 1,
	PMB887X_TRACE_DMAC		= 1ULL << 2,
	PMB887X_TRACE_EBU		= 1ULL << 3,
	PMB887X_TRACE_STM		= 1ULL << 4,
	PMB887X_TRACE_PLL		= 1ULL << 5,
	PMB887X_TRACE_ADC		= 1ULL << 6,
	PMB887X_TRACE_CAPCOM	= 1ULL << 7,
	PMB887X_TRACE_DIF		= 1ULL << 8,
	PMB887X_TRACE_DSP		= 1ULL << 9,
	PMB887X_TRACE_MOD		= 1ULL << 10,
	PMB887X_TRACE_VIC		= 1ULL << 11,
	PMB887X_TRACE_PCL		= 1ULL << 12,
	PMB887X_TRACE_RTC		= 1ULL << 13,
	PMB887X_TRACE_SCU		= 1ULL << 14,
	PMB887X_TRACE_USART		= 1ULL << 15,
	PMB887X_TRACE_KEYPAD	= 1ULL << 16,
	PMB887X_TRACE_I2C		= 1ULL << 17,
	PMB887X_TRACE_SCCU		= 1ULL << 18,
	PMB887X_TRACE_MMCI		= 1ULL << 19,
	PMB887X_TRACE_SSC		= 1ULL << 20,
	PMB887X_TRACE_TCM		= 1ULL << 21,

	// External
	PMB887X_TRACE_ACODEC	= 1ULL << 57,
	PMB887X_TRACE_GIMMICK	= 1ULL << 58,
	PMB887X_TRACE_FM_RADIO	= 1ULL << 59,
	PMB887X_TRACE_FLASH		= 1ULL << 60,
	PMB887X_TRACE_LCD		= 1ULL << 61,
	PMB887X_TRACE_PMIC		= 1ULL << 62,
	PMB887X_TRACE_UNKNOWN	= 1ULL << 63,

	PMB887X_TRACE_ALL		= (~0ULL),
};

//extern uint32_t pmb887x_trace_flags;

extern uint64_t pmb887x_trace_io_mask;
extern uint64_t pmb887x_trace_log_mask;

void pmb887x_trace_init(void);

static inline bool pmb887x_trace_log_enabled(uint64_t id) {
	return (pmb887x_trace_log_mask & id) != 0;
}

static inline bool pmb887x_trace_io_enabled(uint64_t id) {
	return (pmb887x_trace_io_mask & id) != 0;
}
