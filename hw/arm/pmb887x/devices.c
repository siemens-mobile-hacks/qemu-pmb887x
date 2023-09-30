#include "hw/arm/pmb887x/devices.h"

#include "qemu/error-report.h"
#include "qom/object.h"
#include "hw/qdev-core.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/hw.h"

#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"

static const struct pmb887x_dev pmb8876_devices[] = {
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
		.name	= "NVIC",
		.dev	= "pmb887x-nvic",
		.base	= PMB8876_NVIC_BASE,
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
		.dev	= "pmb887x-i2c",
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
		.dev	= "pmb887x-dif",
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
	}
};

static const struct pmb887x_dev pmb8875_devices[] = {
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
		.name	= "NVIC",
		.dev	= "pmb887x-nvic",
		.base	= PMB8875_NVIC_BASE,
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
		.dev	= "pmb887x-dif",
		.base	= PMB8875_DIF_BASE,
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
	}
};

DeviceState *pmb887x_new_lcd_dev(const char *name) {
	char tmp[64];
	snprintf(tmp, sizeof(tmp), "pmb887x-lcd-%s", name);
	return qdev_new(tmp);
}

DeviceState *pmb887x_new_dev(uint32_t cpu_type, const char *name, DeviceState *nvic) {
	const struct pmb887x_dev *devices = NULL;
	uint32_t devices_count = 0;
	
	switch (cpu_type) {
		case CPU_PMB8875:
			devices = pmb8875_devices;
			devices_count = ARRAY_SIZE(pmb8875_devices);
		break;
		
		case CPU_PMB8876:
			devices = pmb8876_devices;
			devices_count = ARRAY_SIZE(pmb8876_devices);
		break;
	}
	
	for (int i = 0; i < devices_count; i++) {
		const struct pmb887x_dev *device = &devices[i];
		
		if (strcmp(name, device->name) != 0)
			continue;
		
		DeviceState *dev = qdev_new(device->dev);
		
		uint32_t irq_n = 0;
		while (device->irqs[irq_n]) {
			if (!nvic)
				hw_error("Can't find nvic for: %s\n", name);
			
			sysbus_connect_irq(SYS_BUS_DEVICE(dev), irq_n, qdev_get_gpio_in(nvic, device->irqs[irq_n]));
			irq_n++;
		}
		
		if (object_property_find(dev, "cpu_type"))
			qdev_prop_set_uint32(dev, "cpu_type", cpu_type);
		
		sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, device->base);
		
		return dev;
	}
	
	hw_error("Can't find device: %s\n", name);
	
	return NULL;
}
