#pragma once

#include "qemu/log.h"
#include "hw/arm/pmb887x/regs_dump.h"

enum pmb887x_modules_t {
	// CPU modules
	PMB887X_TRACE_GPTU		= 1 << 0,
	PMB887X_TRACE_TPU		= 1 << 1,
	PMB887X_TRACE_DMAC		= 1 << 2,
	PMB887X_TRACE_EBU		= 1 << 3,
	PMB887X_TRACE_STM		= 1 << 4,
	PMB887X_TRACE_PLL		= 1 << 5,
	PMB887X_TRACE_AMC		= 1 << 6,
	PMB887X_TRACE_CAPCOM	= 1 << 7,
	PMB887X_TRACE_DIF		= 1 << 8,
	PMB887X_TRACE_DSP		= 1 << 9,
	PMB887X_TRACE_MOD		= 1 << 10,
	PMB887X_TRACE_NVIC		= 1 << 11,
	PMB887X_TRACE_PCL		= 1 << 12,
	PMB887X_TRACE_RTC		= 1 << 13,
	PMB887X_TRACE_SCU		= 1 << 14,
	PMB887X_TRACE_USART		= 1 << 15,
	PMB887X_TRACE_KEYPAD	= 1 << 16,
	PMB887X_TRACE_I2C		= 1 << 17,
	PMB887X_TRACE_SCCU		= 1 << 18,
	
	// External
	PMB887X_TRACE_FLASH		= 1 << 29,
	PMB887X_TRACE_LCD		= 1 << 30,
	PMB887X_TRACE_D1601XX	= 1 << 31,
};

//extern uint32_t pmb887x_trace_flags;

static inline bool pmb887x_trace_log_enabled(uint32_t id) {
//	return true;
	return ((id & (PMB887X_TRACE_TPU | PMB887X_TRACE_LCD)) != 0);
//	return false;
	return ((
		PMB887X_TRACE_GPTU |
		PMB887X_TRACE_TPU |
//		PMB887X_TRACE_DMAC |
		PMB887X_TRACE_EBU |
//		PMB887X_TRACE_STM |
		PMB887X_TRACE_PLL |
		PMB887X_TRACE_AMC |
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
		
		PMB887X_TRACE_FLASH |
		PMB887X_TRACE_LCD |
		PMB887X_TRACE_D1601XX |
		0
	) & id) != 0;
}

static inline bool pmb887x_trace_io_enabled(uint32_t id) {
//	return false;
	return ((id & (PMB887X_TRACE_SCCU)) != 0);
	return ((
		PMB887X_TRACE_GPTU |
		PMB887X_TRACE_TPU |
		PMB887X_TRACE_DMAC |
		PMB887X_TRACE_EBU |
		PMB887X_TRACE_STM |
		PMB887X_TRACE_PLL |
		PMB887X_TRACE_AMC |
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
		
//		PMB887X_TRACE_FLASH |
		PMB887X_TRACE_LCD |
		PMB887X_TRACE_D1601XX |
		0
	) & id) != 0;
}
