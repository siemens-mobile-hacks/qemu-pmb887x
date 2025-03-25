#pragma once

#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/arm/pmb887x/regs_dump.h"

// #define PMB887X_TRACE_UNHANDLED_IO 1

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
	PMB887X_TRACE_NVIC		= 1ULL << 11,
	PMB887X_TRACE_PCL		= 1ULL << 12,
	PMB887X_TRACE_RTC		= 1ULL << 13,
	PMB887X_TRACE_SCU		= 1ULL << 14,
	PMB887X_TRACE_USART		= 1ULL << 15,
	PMB887X_TRACE_KEYPAD	= 1ULL << 16,
	PMB887X_TRACE_I2C		= 1ULL << 17,
	PMB887X_TRACE_SCCU		= 1ULL << 18,
	PMB887X_TRACE_MMCI		= 1ULL << 19,
	
	// External
	PMB887X_TRACE_FM_RADIO	= 1ULL << 28,
	PMB887X_TRACE_FLASH		= 1ULL << 29,
	PMB887X_TRACE_LCD		= 1ULL << 30,
	PMB887X_TRACE_PMIC		= 1ULL << 31,
};

//extern uint32_t pmb887x_trace_flags;

static inline bool pmb887x_trace_log_enabled(uint64_t id) {
	//return ((id & (PMB887X_TRACE_GPTU)) != 0);
	return ((id & (PMB887X_TRACE_PMIC)) != 0);
	return false;
	return ((id & (PMB887X_TRACE_ADC)) != 0);
	return ((id & (PMB887X_TRACE_FLASH)) != 0);
	return ((id & (PMB887X_TRACE_LCD)) != 0);
	return ((id & (PMB887X_TRACE_TPU | PMB887X_TRACE_FLASH | PMB887X_TRACE_LCD | PMB887X_TRACE_EBU | PMB887X_TRACE_I2C)) != 0);
//	return false;
	return ((
		PMB887X_TRACE_GPTU |
		PMB887X_TRACE_TPU |
//		PMB887X_TRACE_DMAC |
		PMB887X_TRACE_EBU |
//		PMB887X_TRACE_STM |
		PMB887X_TRACE_PLL |
		PMB887X_TRACE_ADC |
		PMB887X_TRACE_CAPCOM |
		PMB887X_TRACE_DIF |
		PMB887X_TRACE_DSP |
		PMB887X_TRACE_MOD |
		PMB887X_TRACE_NVIC |
		PMB887X_TRACE_PCL |
		PMB887X_TRACE_RTC |
		PMB887X_TRACE_SCU |
		PMB887X_TRACE_USART |
		PMB887X_TRACE_KEYPAD |
		PMB887X_TRACE_I2C |
		PMB887X_TRACE_SCCU |
		PMB887X_TRACE_MMCI |
		
		PMB887X_TRACE_FLASH |
		PMB887X_TRACE_LCD |
		PMB887X_TRACE_PMIC |
		PMB887X_TRACE_FM_RADIO |
		0
	) & id) != 0;
}

static inline bool pmb887x_trace_io_enabled(uint64_t id) {
	//return ((id & (PMB887X_TRACE_GPTU)) != 0);
	//return true;
	//return ((id & (PMB887X_TRACE_GPTU)) != 0);
	//return ((id & (PMB887X_TRACE_ADC)) != 0);
	//return ((id & (PMB887X_TRACE_PCL)) != 0);
	//return ((id & (PMB887X_TRACE_I2C)) != 0);
	return false;
	return ((id & (PMB887X_TRACE_I2C)) != 0);
	return ((
		PMB887X_TRACE_GPTU |
		PMB887X_TRACE_TPU |
		PMB887X_TRACE_DMAC |
		PMB887X_TRACE_EBU |
		PMB887X_TRACE_STM |
		PMB887X_TRACE_PLL |
		PMB887X_TRACE_ADC |
		PMB887X_TRACE_CAPCOM |
		PMB887X_TRACE_DIF |
		PMB887X_TRACE_DSP |
		PMB887X_TRACE_MOD |
		PMB887X_TRACE_NVIC |
		PMB887X_TRACE_PCL |
		PMB887X_TRACE_RTC |
		PMB887X_TRACE_SCU |
		PMB887X_TRACE_USART |
		PMB887X_TRACE_KEYPAD |
		PMB887X_TRACE_I2C |
		PMB887X_TRACE_SCCU |
		PMB887X_TRACE_MMCI |
		
//		PMB887X_TRACE_FLASH |
		PMB887X_TRACE_LCD |
		PMB887X_TRACE_PMIC |
		PMB887X_TRACE_FM_RADIO |
		0
	) & id) != 0;
}
