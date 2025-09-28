#include "hw/arm/pmb887x/gen/cpu_modules.h"

#include "hw/arm/pmb887x/gen/cpu_meta.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"

#include "hw/hw.h"

static const pmb887x_cpu_module_t pmb8876_modules[] = {
	{
		.name	= "SCU",
		.dev	= "pmb887x-scu",
		.base	= PMB8876_SCU_BASE,
		.irqs	= {
			PMB8876_SCU_EXTI0_IRQ,
			PMB8876_SCU_EXTI1_IRQ,
			PMB8876_SCU_EXTI2_IRQ,
			PMB8876_SCU_EXTI3_IRQ,
			PMB8876_SCU_EXTI4_IRQ,
			PMB8876_SCU_EXTI5_IRQ,
			PMB8876_SCU_EXTI6_IRQ,
			PMB8876_SCU_EXTI7_IRQ,
			PMB8876_SCU_DSP0_IRQ,
			PMB8876_SCU_DSP1_IRQ,
			PMB8876_SCU_DSP2_IRQ,
			PMB8876_SCU_DSP3_IRQ,
			PMB8876_SCU_DSP4_IRQ,
			PMB8876_SCU_UNK0_IRQ,
			PMB8876_SCU_UNK1_IRQ,
			PMB8876_SCU_UNK2_IRQ,
			0
		}
	},
	{
		.name	= "VIC",
		.dev	= "pmb887x-vic",
		.base	= PMB8876_VIC_BASE,
		.irqs	= { 0 }
	},
	{
		.name	= "PLL",
		.dev	= "pmb887x-pll",
		.base	= PMB8876_PLL_BASE,
		.irqs	= { PMB8876_PLL_IRQ, 0 }
	},
	{
		.name	= "STM",
		.dev	= "pmb887x-stm",
		.base	= PMB8876_STM_BASE,
		.irqs	= { 0 }
	},
	{
		.name	= "TPU",
		.dev	= "pmb887x-tpu",
		.base	= PMB8876_TPU_BASE,
		.irqs	= {
			PMB8876_TPU_INT0_IRQ,
			PMB8876_TPU_INT1_IRQ,
			PMB8876_TPU_INT_UNK0_IRQ,
			PMB8876_TPU_INT_UNK1_IRQ,
			PMB8876_TPU_INT_UNK2_IRQ,
			PMB8876_TPU_INT_UNK3_IRQ,
			PMB8876_TPU_INT_UNK4_IRQ,
			PMB8876_TPU_INT_UNK5_IRQ,
			0
		}
	},
	{
		.name	= "CAPCOM0",
		.dev	= "pmb887x-capcom",
		.base	= PMB8876_CAPCOM0_BASE,
		.irqs	= {
			PMB8876_CAPCOM0_T0_IRQ,
			PMB8876_CAPCOM0_T1_IRQ,
			PMB8876_CAPCOM0_CC0_IRQ,
			PMB8876_CAPCOM0_CC1_IRQ,
			PMB8876_CAPCOM0_CC2_IRQ,
			PMB8876_CAPCOM0_CC3_IRQ,
			PMB8876_CAPCOM0_CC4_IRQ,
			PMB8876_CAPCOM0_CC5_IRQ,
			PMB8876_CAPCOM0_CC6_IRQ,
			PMB8876_CAPCOM0_CC7_IRQ,
			0
		}
	},
	{
		.name	= "CAPCOM1",
		.dev	= "pmb887x-capcom",
		.base	= PMB8876_CAPCOM1_BASE,
		.irqs	= {
			PMB8876_CAPCOM1_T0_IRQ,
			PMB8876_CAPCOM1_T1_IRQ,
			PMB8876_CAPCOM1_CC0_IRQ,
			PMB8876_CAPCOM1_CC1_IRQ,
			PMB8876_CAPCOM1_CC2_IRQ,
			PMB8876_CAPCOM1_CC3_IRQ,
			PMB8876_CAPCOM1_CC4_IRQ,
			PMB8876_CAPCOM1_CC5_IRQ,
			PMB8876_CAPCOM1_CC6_IRQ,
			PMB8876_CAPCOM1_CC7_IRQ,
			0
		}
	},
	{
		.name	= "PCL",
		.dev	= "pmb887x-pcl",
		.base	= PMB8876_GPIO_BASE,
		.irqs	= { 0 }
	},
	{
		.name	= "RTC",
		.dev	= "pmb887x-rtc",
		.base	= PMB8876_RTC_BASE,
		.irqs	= { PMB8876_RTC_IRQ, 0 }
	},
	{
		.name	= "I2C",
		.dev	= "pmb887x-i2c-v2",
		.base	= PMB8876_I2C_BASE,
		.irqs	= {
			PMB8876_I2C_SINGLE_REQ_IRQ,
			PMB8876_I2C_BURST_REQ_IRQ,
			PMB8876_I2C_ERROR_IRQ,
			PMB8876_I2C_PROTOCOL_IRQ,
			0
		}
	},
	{
		.name	= "USART0",
		.dev	= "pmb887x-usart",
		.base	= PMB8876_USART0_BASE,
		.irqs	= {
			PMB8876_USART0_TX_IRQ,
			PMB8876_USART0_TBUF_IRQ,
			PMB8876_USART0_RX_IRQ,
			PMB8876_USART0_ERR_IRQ,
			PMB8876_USART0_CTS_IRQ,
			PMB8876_USART0_ABSTART_IRQ,
			PMB8876_USART0_ABDET_IRQ,
			PMB8876_USART0_TMO_IRQ,
			0
		}
	},
	{
		.name	= "USART1",
		.dev	= "pmb887x-usart",
		.base	= PMB8876_USART1_BASE,
		.irqs	= {
			PMB8876_USART1_TX_IRQ,
			PMB8876_USART1_TBUF_IRQ,
			PMB8876_USART1_RX_IRQ,
			PMB8876_USART1_ERR_IRQ,
			PMB8876_USART1_CTS_IRQ,
			PMB8876_USART1_ABSTART_IRQ,
			PMB8876_USART1_ABDET_IRQ,
			PMB8876_USART1_TMO_IRQ,
			0
		}
	},
	{
		.name	= "GPTU0",
		.dev	= "pmb887x-gptu",
		.base	= PMB8876_GPTU0_BASE,
		.irqs	= {
			PMB8876_GPTU0_SRC0_IRQ,
			PMB8876_GPTU0_SRC1_IRQ,
			PMB8876_GPTU0_SRC2_IRQ,
			PMB8876_GPTU0_SRC3_IRQ,
			PMB8876_GPTU0_SRC4_IRQ,
			PMB8876_GPTU0_SRC5_IRQ,
			PMB8876_GPTU0_SRC6_IRQ,
			PMB8876_GPTU0_SRC7_IRQ,
			0
		}
	},
	{
		.name	= "GPTU1",
		.dev	= "pmb887x-gptu",
		.base	= PMB8876_GPTU1_BASE,
		.irqs	= {
			PMB8876_GPTU1_SRC0_IRQ,
			PMB8876_GPTU1_SRC1_IRQ,
			PMB8876_GPTU1_SRC2_IRQ,
			PMB8876_GPTU1_SRC3_IRQ,
			PMB8876_GPTU1_SRC4_IRQ,
			PMB8876_GPTU1_SRC5_IRQ,
			PMB8876_GPTU1_SRC6_IRQ,
			PMB8876_GPTU1_SRC7_IRQ,
			0
		}
	},
	{
		.name	= "EBU",
		.dev	= "pmb887x-ebu",
		.base	= PMB8876_EBU_BASE,
		.irqs	= { 0 }
	},
	{
		.name	= "DSP",
		.dev	= "pmb887x-dsp",
		.base	= PMB8876_DSP_BASE,
		.irqs	= { 0 }
	},
	{
		.name	= "DIF",
		.dev	= "pmb887x-dif-v2",
		.base	= PMB8876_DIF_BASE,
		.irqs	= { 0 }
	},
	{
		.name	= "DMA",
		.dev	= "pmb887x-dmac",
		.base	= PMB8876_DMAC_BASE,
		.irqs	= {
			PMB8876_DMAC_ERR_IRQ,
			PMB8876_DMAC_CH0_IRQ,
			PMB8876_DMAC_CH1_IRQ,
			PMB8876_DMAC_CH2_IRQ,
			PMB8876_DMAC_CH3_IRQ,
			PMB8876_DMAC_CH4_IRQ,
			PMB8876_DMAC_CH5_IRQ,
			PMB8876_DMAC_CH6_IRQ,
			PMB8876_DMAC_CH7_IRQ,
			0
		}
	},
	{
		.name	= "ADC",
		.dev	= "pmb887x-adc",
		.base	= PMB8876_ADC_BASE,
		.irqs	= {
			PMB8876_ADC_INT0_IRQ,
			PMB8876_ADC_INT1_IRQ,
			0
		}
	},
	{
		.name	= "KEYPAD",
		.dev	= "pmb887x-keypad",
		.base	= PMB8876_KEYPAD_BASE,
		.irqs	= {
			PMB8876_KEYPAD_PRESS_IRQ,
			PMB8876_KEYPAD_UNK0_IRQ,
			PMB8876_KEYPAD_UNK1_IRQ,
			PMB8876_KEYPAD_RELEASE_IRQ,
			0
		}
	},
	{
		.name	= "SCCU",
		.dev	= "pmb887x-sccu",
		.base	= PMB8876_SCCU_BASE,
		.irqs	= {
			PMB8876_SCCU_WAKE_IRQ,
			PMB8876_SCCU_UNK_IRQ,
			0
		}
	},
	{
		.name	= "MMCI",
		.dev	= "pmb887x-mmci",
		.base	= PMB8876_MMCI_BASE,
		.irqs	= {
			0
		}
	},
	{ }
};

const struct pmb887x_cpu_module_t pmb8875_modules[] = {
	{
		.name	= "SCU",
		.dev	= "pmb887x-scu",
		.base	= PMB8875_SCU_BASE,
		.irqs	= {
			PMB8875_SCU_EXTI0_IRQ,
			PMB8875_SCU_EXTI1_IRQ,
			PMB8875_SCU_EXTI2_IRQ,
			PMB8875_SCU_EXTI3_IRQ,
			PMB8875_SCU_EXTI4_IRQ,
			PMB8875_SCU_EXTI5_IRQ,
			PMB8875_SCU_EXTI6_IRQ,
			PMB8875_SCU_EXTI7_IRQ,
			PMB8875_SCU_DSP0_IRQ,
			PMB8875_SCU_DSP1_IRQ,
			PMB8875_SCU_DSP2_IRQ,
			PMB8875_SCU_DSP3_IRQ,
			PMB8875_SCU_DSP4_IRQ,
			PMB8875_SCU_UNK0_IRQ,
			PMB8875_SCU_UNK1_IRQ,
			PMB8875_SCU_UNK2_IRQ,
			0
		}
	},
	{
		.name	= "VIC",
		.dev	= "pmb887x-vic",
		.base	= PMB8875_VIC_BASE,
		.irqs	= { 0 }
	},
	{
		.name	= "PLL",
		.dev	= "pmb887x-pll",
		.base	= PMB8875_PLL_BASE,
		.irqs	= { PMB8875_PLL_IRQ, 0 }
	},
	{
		.name	= "STM",
		.dev	= "pmb887x-stm",
		.base	= PMB8875_STM_BASE,
		.irqs	= { 0 }
	},
	{
		.name	= "TPU",
		.dev	= "pmb887x-tpu",
		.base	= PMB8875_TPU_BASE,
		.irqs	= {
			PMB8875_TPU_INT0_IRQ,
			PMB8875_TPU_INT1_IRQ,
			PMB8875_TPU_INT_UNK0_IRQ,
			PMB8875_TPU_INT_UNK1_IRQ,
			PMB8875_TPU_INT_UNK2_IRQ,
			PMB8875_TPU_INT_UNK3_IRQ,
			PMB8875_TPU_INT_UNK4_IRQ,
			PMB8875_TPU_INT_UNK5_IRQ,
		}
	},
	{
		.name	= "CAPCOM0",
		.dev	= "pmb887x-capcom",
		.base	= PMB8875_CAPCOM0_BASE,
		.irqs	= {
			PMB8875_CAPCOM0_T0_IRQ,
			PMB8875_CAPCOM0_T1_IRQ,
			PMB8875_CAPCOM0_CC0_IRQ,
			PMB8875_CAPCOM0_CC1_IRQ,
			PMB8875_CAPCOM0_CC2_IRQ,
			PMB8875_CAPCOM0_CC3_IRQ,
			PMB8875_CAPCOM0_CC4_IRQ,
			PMB8875_CAPCOM0_CC5_IRQ,
			PMB8875_CAPCOM0_CC6_IRQ,
			PMB8875_CAPCOM0_CC7_IRQ,
			0
		}
	},
	{
		.name	= "CAPCOM1",
		.dev	= "pmb887x-capcom",
		.base	= PMB8875_CAPCOM1_BASE,
		.irqs	= {
			PMB8875_CAPCOM1_T0_IRQ,
			PMB8875_CAPCOM1_T1_IRQ,
			PMB8875_CAPCOM1_CC0_IRQ,
			PMB8875_CAPCOM1_CC1_IRQ,
			PMB8875_CAPCOM1_CC2_IRQ,
			PMB8875_CAPCOM1_CC3_IRQ,
			PMB8875_CAPCOM1_CC4_IRQ,
			PMB8875_CAPCOM1_CC5_IRQ,
			PMB8875_CAPCOM1_CC6_IRQ,
			PMB8875_CAPCOM1_CC7_IRQ,
			0
		}
	},
	{
		.name	= "PCL",
		.dev	= "pmb887x-pcl",
		.base	= PMB8875_GPIO_BASE,
		.irqs	= { 0 }
	},
	{
		.name	= "RTC",
		.dev	= "pmb887x-rtc",
		.base	= PMB8875_RTC_BASE,
		.irqs	= { PMB8875_RTC_IRQ, 0 }
	},
	{
		.name	= "I2C",
		.dev	= "pmb887x-i2c-v1",
		.base	= PMB8875_I2C_BASE,
		.irqs	= {
			PMB8875_I2C_DATA_IRQ,
			PMB8875_I2C_PROTO_IRQ,
			PMB8875_I2C_END_IRQ,
			0
		}
	},
	{
		.name	= "USART0",
		.dev	= "pmb887x-usart",
		.base	= PMB8875_USART0_BASE,
		.irqs	= {
			PMB8875_USART0_TX_IRQ,
			PMB8875_USART0_TBUF_IRQ,
			PMB8875_USART0_RX_IRQ,
			PMB8875_USART0_ERR_IRQ,
			PMB8875_USART0_CTS_IRQ,
			PMB8875_USART0_ABSTART_IRQ,
			PMB8875_USART0_ABDET_IRQ,
			PMB8875_USART0_TMO_IRQ,
			0
		}
	},
	{
		.name	= "USART1",
		.dev	= "pmb887x-usart",
		.base	= PMB8875_USART1_BASE,
		.irqs	= {
			PMB8875_USART1_TX_IRQ,
			PMB8875_USART1_TBUF_IRQ,
			PMB8875_USART1_RX_IRQ,
			PMB8875_USART1_ERR_IRQ,
			PMB8875_USART1_CTS_IRQ,
			PMB8875_USART1_ABSTART_IRQ,
			PMB8875_USART1_ABDET_IRQ,
			PMB8875_USART1_TMO_IRQ,
			0
		}
	},
	{
		.name	= "GPTU0",
		.dev	= "pmb887x-gptu",
		.base	= PMB8875_GPTU0_BASE,
		.irqs	= {
			PMB8875_GPTU0_SRC0_IRQ,
			PMB8875_GPTU0_SRC1_IRQ,
			PMB8875_GPTU0_SRC2_IRQ,
			PMB8875_GPTU0_SRC3_IRQ,
			PMB8875_GPTU0_SRC4_IRQ,
			PMB8875_GPTU0_SRC5_IRQ,
			PMB8875_GPTU0_SRC6_IRQ,
			PMB8875_GPTU0_SRC7_IRQ,
			0
		}
	},
	{
		.name	= "GPTU1",
		.dev	= "pmb887x-gptu",
		.base	= PMB8875_GPTU1_BASE,
		.irqs	= {
			PMB8875_GPTU1_SRC0_IRQ,
			PMB8875_GPTU1_SRC1_IRQ,
			PMB8875_GPTU1_SRC2_IRQ,
			PMB8875_GPTU1_SRC3_IRQ,
			PMB8875_GPTU1_SRC4_IRQ,
			PMB8875_GPTU1_SRC5_IRQ,
			PMB8875_GPTU1_SRC6_IRQ,
			PMB8875_GPTU1_SRC7_IRQ,
			0
		}
	},
	{
		.name	= "EBU",
		.dev	= "pmb887x-ebu",
		.base	= PMB8875_EBU_BASE,
		.irqs	= { 0 }
	},
	{
		.name	= "DSP",
		.dev	= "pmb887x-dsp",
		.base	= PMB8875_DSP_BASE,
		.irqs	= { 0 }
	},
	{
		.name	= "DIF",
		.dev	= "pmb887x-dif-v1",
		.base	= PMB8875_DIF_BASE,
		.irqs	= { 0 }
	},
	{
		.name	= "SSC",
		.dev	= "pmb887x-ssc",
		.base	= PMB8875_SSC_BASE,
		.irqs	= { 0 }
	},
	{
		.name	= "DMA",
		.dev	= "pmb887x-dmac",
		.base	= PMB8875_DMAC_BASE,
		.irqs	= {
			PMB8875_DMAC_ERR_IRQ,
			PMB8875_DMAC_CH0_IRQ,
			PMB8875_DMAC_CH1_IRQ,
			PMB8875_DMAC_CH2_IRQ,
			PMB8875_DMAC_CH3_IRQ,
			PMB8875_DMAC_CH4_IRQ,
			PMB8875_DMAC_CH5_IRQ,
			PMB8875_DMAC_CH6_IRQ,
			PMB8875_DMAC_CH7_IRQ,
			0
		}
	},
	{
		.name	= "ADC",
		.dev	= "pmb887x-adc",
		.base	= PMB8875_ADC_BASE,
		.irqs	= {
			PMB8875_ADC_INT0_IRQ,
			PMB8875_ADC_INT1_IRQ,
			0
		}
	},
	{
		.name	= "KEYPAD",
		.dev	= "pmb887x-keypad",
		.base	= PMB8875_KEYPAD_BASE,
		.irqs	= {
			PMB8875_KEYPAD_PRESS_IRQ,
			PMB8875_KEYPAD_UNK0_IRQ,
			PMB8875_KEYPAD_UNK1_IRQ,
			PMB8875_KEYPAD_RELEASE_IRQ,
			0
		}
	},
	{
		.name	= "SCCU",
		.dev	= "pmb887x-sccu",
		.base	= PMB8875_SCCU_BASE,
		.irqs	= {
			PMB8875_SCCU_WAKE_IRQ,
			PMB8875_SCCU_UNK_IRQ,
			0
		}
	},
	{ }
};

const pmb887x_cpu_module_t *pmb887x_cpu_get_modules_list(int cpu_id) {
	switch (cpu_id) {
		case CPU_PMB8875:
			return pmb8875_modules;

		case CPU_PMB8876:
			return pmb8876_modules;

		default:
			hw_error("Invalid CPU type: %d", cpu_id);
	}
	return NULL;
}
