#include "hw/arm/pmb887x/gen/cpu_modules.h"

#include "hw/arm/pmb887x/gen/cpu_meta.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/gen/dsp.h"

#include "hw/core/hw-error.h"

static const pmb887x_dsp_peripheral_config_t pmb8876_dsp_peripherals[] = {
	{
		.name = "INT",
		.type = PMB887X_DSP_PERIPHERAL_INTERRUPT,
		.base = 0xDE00,
		.size = 0x0016,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "CIPH",
		.type = PMB887X_DSP_PERIPHERAL_CIPHER,
		.base = 0xDE20,
		.size = 0x0010,
		.ram_base = PMB8876_TEAK_CIPHER_RAM_BASE,
		.ram_size = PMB8876_TEAK_CIPHER_RAM_SIZE,
	},
	{
		.name = "TMR1",
		.type = PMB887X_DSP_PERIPHERAL_TIMER1,
		.base = 0xDE30,
		.size = 0x0004,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "TMR2",
		.type = PMB887X_DSP_PERIPHERAL_TIMER2,
		.base = 0xDE34,
		.size = 0x0003,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "EQ",
		.type = PMB887X_DSP_PERIPHERAL_EQUALIZER,
		.base = 0xDE40,
		.size = 0x0007,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "CHDEC",
		.type = PMB887X_DSP_PERIPHERAL_CHANNEL_DECODER,
		.base = 0xDE50,
		.size = 0x0010,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "AFE",
		.type = PMB887X_DSP_PERIPHERAL_AFE,
		.base = 0xDE70,
		.size = 0x0007,
		.ram_base = PMB8876_TEAK_AFE_RAM_BASE,
		.ram_size = PMB8876_TEAK_AFE_RAM_SIZE,
	},
	{
		.name = "BB",
		.type = PMB887X_DSP_PERIPHERAL_BASEBAND,
		.base = 0xDE80,
		.size = 0x000D,
		.ram_base = PMB8876_TEAK_DEMODULATOR_RAM_BASE,
		.ram_size = PMB8876_TEAK_DEMODULATOR_RAM_SIZE,
	},
	{
		.name = "MCS",
		.type = PMB887X_DSP_PERIPHERAL_MCS,
		.base = 0xDE90,
		.size = 0x0006,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "DSP",
		.type = PMB887X_DSP_PERIPHERAL_DSP,
		.base = 0xDEA0,
		.size = 0x0009,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "MOD",
		.type = PMB887X_DSP_PERIPHERAL_MODULATOR,
		.base = 0xDEB0,
		.size = 0x000B,
		.ram_base = PMB8876_TEAK_MODULATOR_RAM_BASE,
		.ram_size = PMB8876_TEAK_MODULATOR_RAM_SIZE,
	},
	{
		.name = "SSC",
		.type = PMB887X_DSP_PERIPHERAL_SSC,
		.base = 0xDEC0,
		.size = 0x0009,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "I2S1",
		.type = PMB887X_DSP_PERIPHERAL_I2S,
		.base = 0xDED0,
		.size = 0x000B,
		.ram_base = PMB8876_TEAK_I2S1_RAM_BASE,
		.ram_size = PMB8876_TEAK_I2S1_RAM_SIZE,
	},
	{
		.name = "I2S2",
		.type = PMB887X_DSP_PERIPHERAL_I2S,
		.base = 0xDEE0,
		.size = 0x000B,
		.ram_base = PMB8876_TEAK_I2S2_RAM_BASE,
		.ram_size = PMB8876_TEAK_I2S2_RAM_SIZE,
	},
	{
		.name = "I2S3",
		.type = PMB887X_DSP_PERIPHERAL_I2S_TX,
		.base = 0xDEF0,
		.size = 0x000B,
		.ram_base = PMB8876_TEAK_I2S3_RAM_BASE,
		.ram_size = PMB8876_TEAK_I2S3_RAM_SIZE,
	},
};

static const pmb887x_dsp_config_t pmb8876_dsp_config = {
	.name = "pmb8876",
	.default_rom_version = 0x0801,
	.page_field_mask = 0x0003,
	.program_page_shift = 2,
	.program_rom_base = PMB8876_TEAK_PROM_FIXED_BASE,
	.program_bank_base = PMB8876_TEAK_PROM_PAGE0_BASE,
	.program_bank_count = 3,
	.data_rom_base = PMB8876_TEAK_DROM_FIXED_BASE,
	.data_bank_base = PMB8876_TEAK_DROM_PAGE0_BASE,
	.data_bank_count = 4,
	.shared_base = PMB8876_TEAK_SHARED_RAM_BASE,
	.shared_size = PMB8876_TEAK_SHARED_RAM_SIZE,
	.y_space_base = PMB8876_TEAK_YRAM_BASE,
	.mmio_base = PMB8876_TEAK_INT_BASE,
	.mmio_size = 0x0100,
	.peripherals = pmb8876_dsp_peripherals,
	.peripheral_count = ARRAY_SIZE(pmb8876_dsp_peripherals),
};

static const int pmb8876_usart0_irqs[] = {
	PMB8876_USART0_TX_IRQ,
	PMB8876_USART0_TBUF_IRQ,
	PMB8876_USART0_RX_IRQ,
	PMB8876_USART0_ERR_IRQ,
	PMB8876_USART0_CTS_IRQ,
	PMB8876_USART0_ABDET_IRQ,
	PMB8876_USART0_ABSTART_IRQ,
	PMB8876_USART0_TMO_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8876_usart0_gpios[] = {
	{"RXD_IN",	PMB8876_GPIO_USART0_RXD,	0},
	{"TXD_OUT",	PMB8876_GPIO_USART0_TXD,	0},
	{"RTS_OUT",	PMB8876_GPIO_USART0_RTS,	0},
	{"CTS_IN",	PMB8876_GPIO_USART0_CTS,	0},
};

static const pmb887x_cpu_module_dma_t pmb8876_usart0_dma[] = {
	{"TX",	PMB887X_DMAC_BUS_AHB1,	0,	0},
	{"RX",	PMB887X_DMAC_BUS_AHB1,	1,	0},
};

static const int pmb8876_ssc_irqs[] = {
	PMB8876_SSC_TX_IRQ,
	PMB8876_SSC_RX_IRQ,
	PMB8876_SSC_ERR_IRQ,
	PMB8876_SSC_TMO_IRQ
};

static const pmb887x_cpu_module_dma_t pmb8876_ssc_dma[] = {
	{"TX",	PMB887X_DMAC_BUS_AHB1,	2,	0},
	{"RX",	PMB887X_DMAC_BUS_AHB1,	3,	0},
};

static const int pmb8876_sim_irqs[] = {
	PMB8876_SIM_ERR_IRQ,
	PMB8876_SIM_IN_IRQ,
	PMB8876_SIM_OK_IRQ
};

static const pmb887x_cpu_module_dma_t pmb8876_sim_dma[] = {
	{"TX",	PMB887X_DMAC_BUS_AHB1,	8,	0},
	{"RX",	PMB887X_DMAC_BUS_AHB1,	8,	0},
};

static const int pmb8876_usart1_irqs[] = {
	PMB8876_USART1_TX_IRQ,
	PMB8876_USART1_TBUF_IRQ,
	PMB8876_USART1_RX_IRQ,
	PMB8876_USART1_ERR_IRQ,
	PMB8876_USART1_CTS_IRQ,
	PMB8876_USART1_ABDET_IRQ,
	PMB8876_USART1_ABSTART_IRQ,
	PMB8876_USART1_TMO_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8876_usart1_gpios[] = {
	{"RXD_IN",	PMB8876_GPIO_USART1_RXD,	0},
	{"TXD_OUT",	PMB8876_GPIO_USART1_TXD,	0},
	{"RTS_OUT",	PMB8876_GPIO_USART1_RTS,	0},
	{"CTS_IN",	PMB8876_GPIO_USART1_CTS,	0},
};

static const pmb887x_cpu_module_dma_t pmb8876_usart1_dma[] = {
	{"TX",	PMB887X_DMAC_BUS_AHB1,	6,	0},
	{"RX",	PMB887X_DMAC_BUS_AHB1,	7,	0},
};

static const int pmb8876_usb_irqs[] = {
	PMB8876_USB_IRQ
};

static const int pmb8876_dmac_irqs[] = {
	PMB8876_DMAC_ERR_IRQ,
	PMB8876_DMAC_CH0_IRQ,
	PMB8876_DMAC_CH1_IRQ,
	PMB8876_DMAC_CH2_IRQ,
	PMB8876_DMAC_CH3_IRQ,
	PMB8876_DMAC_CH4_IRQ,
	PMB8876_DMAC_CH5_IRQ,
	PMB8876_DMAC_CH6_IRQ,
	PMB8876_DMAC_CH7_IRQ
};

static const int pmb8876_capcom0_irqs[] = {
	PMB8876_CAPCOM0_T0_IRQ,
	PMB8876_CAPCOM0_T1_IRQ,
	PMB8876_CAPCOM0_CC0_IRQ,
	PMB8876_CAPCOM0_CC1_IRQ,
	PMB8876_CAPCOM0_CC2_IRQ,
	PMB8876_CAPCOM0_CC3_IRQ,
	PMB8876_CAPCOM0_CC4_IRQ,
	PMB8876_CAPCOM0_CC5_IRQ,
	PMB8876_CAPCOM0_CC6_IRQ,
	PMB8876_CAPCOM0_CC7_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8876_capcom0_gpios[] = {
	{"CC4_IN",	PMB8876_GPIO_DSPIN1,	2},
};

static const int pmb8876_capcom1_irqs[] = {
	PMB8876_CAPCOM1_T0_IRQ,
	PMB8876_CAPCOM1_T1_IRQ,
	PMB8876_CAPCOM1_CC0_IRQ,
	PMB8876_CAPCOM1_CC1_IRQ,
	PMB8876_CAPCOM1_CC2_IRQ,
	PMB8876_CAPCOM1_CC3_IRQ,
	PMB8876_CAPCOM1_CC4_IRQ,
	PMB8876_CAPCOM1_CC5_IRQ,
	PMB8876_CAPCOM1_CC6_IRQ,
	PMB8876_CAPCOM1_CC7_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8876_capcom1_gpios[] = {
	{"CC3_IN",	PMB8876_GPIO_CIF_RESET,	1},
};

static const int pmb8876_scu_irqs[] = {
	PMB8876_SCU_EXTI0_IRQ,
	PMB8876_SCU_EXTI1_IRQ,
	PMB8876_SCU_EXTI2_IRQ,
	PMB8876_SCU_EXTI3_IRQ,
	PMB8876_SCU_EXTI4_IRQ,
	PMB8876_SCU_EXTI5_IRQ,
	PMB8876_SCU_EXTI6_IRQ,
	PMB8876_SCU_EXTI7_IRQ,
	PMB8876_SCU_PM_INT_IRQ,
	PMB8876_SCU_DSP0_IRQ,
	PMB8876_SCU_DSP1_IRQ,
	PMB8876_SCU_DSP2_IRQ,
	PMB8876_SCU_DSP3_IRQ,
	PMB8876_SCU_UNK0_IRQ,
	PMB8876_SCU_UNK1_IRQ,
	PMB8876_SCU_UNK2_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8876_scu_gpios[] = {
	{"EXTI0_IN",	PMB8876_GPIO_KP_IN0,		2},
	{"EXTI1_IN",	PMB8876_GPIO_KP_OUT0,		2},
	{"EXTI3_IN",	PMB8876_GPIO_USART1_RXD,	3},
	{"EXTI0_IN",	PMB8876_GPIO_USART1_RTS,	3},
	{"EXTI6_IN",	PMB8876_GPIO_I2C_SCL,		2},
	{"EXTI2_IN",	PMB8876_GPIO_I2C_SDA,		2},
	{"EXTI3_IN",	PMB8876_GPIO_PIN31,			3},
	{"EXTI1_IN",	PMB8876_GPIO_DIF_HD,		2},
	{"EXTI5_IN",	PMB8876_GPIO_T_OUT1,		4},
	{"EXTI6_IN",	PMB8876_GPIO_T_OUT4,		4},
	{"EXTI5_IN",	PMB8876_GPIO_T_OUT7,		4},
	{"EXTI7_IN",	PMB8876_GPIO_T_OUT8,		4},
	{"EXTI4_IN",	PMB8876_GPIO_RF_STR0,		1},
	{"EXTI4_IN",	PMB8876_GPIO_CLKOUT0,		3},
	{"EXTI4_IN",	PMB8876_GPIO_DIF_RD,		1},
	{"EXTI6_IN",	PMB8876_GPIO_PIN101,		2},
	{"EXTI5_IN",	PMB8876_GPIO_PIN108,		1},
};

static const int pmb8876_pll_irqs[] = {
	PMB8876_PLL_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8876_pll_gpios[] = {
	{"CLK32_OUT",	PMB8876_GPIO_DSPIN0,	1},
};

static const int pmb8876_sccu_irqs[] = {
	PMB8876_SCCU_UNK_IRQ,
	PMB8876_SCCU_WAKE_IRQ
};

static const int pmb8876_rtc_irqs[] = {
	PMB8876_RTC_IRQ
};

static const int pmb8876_gptu0_irqs[] = {
	PMB8876_GPTU0_SRC7_IRQ,
	PMB8876_GPTU0_SRC6_IRQ,
	PMB8876_GPTU0_SRC5_IRQ,
	PMB8876_GPTU0_SRC4_IRQ,
	PMB8876_GPTU0_SRC3_IRQ,
	PMB8876_GPTU0_SRC2_IRQ,
	PMB8876_GPTU0_SRC1_IRQ,
	PMB8876_GPTU0_SRC0_IRQ
};

static const int pmb8876_gptu1_irqs[] = {
	PMB8876_GPTU1_SRC7_IRQ,
	PMB8876_GPTU1_SRC6_IRQ,
	PMB8876_GPTU1_SRC5_IRQ,
	PMB8876_GPTU1_SRC4_IRQ,
	PMB8876_GPTU1_SRC3_IRQ,
	PMB8876_GPTU1_SRC2_IRQ,
	PMB8876_GPTU1_SRC1_IRQ,
	PMB8876_GPTU1_SRC0_IRQ
};

static const int pmb8876_adc_irqs[] = {
	PMB8876_ADC_INT0_IRQ,
	PMB8876_ADC_INT1_IRQ
};

static const int pmb8876_keypad_irqs[] = {
	PMB8876_KEYPAD_INT0_IRQ,
	PMB8876_KEYPAD_INT1_IRQ,
	PMB8876_KEYPAD_INT2_IRQ,
	PMB8876_KEYPAD_INT3_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8876_keypad_gpios[] = {
	{"IN0_IN",		PMB8876_GPIO_KP_IN0,	0},
	{"IN1_IN",		PMB8876_GPIO_KP_IN1,	0},
	{"IN2_IN",		PMB8876_GPIO_KP_IN2,	0},
	{"IN3_IN",		PMB8876_GPIO_KP_IN3,	0},
	{"IN4_IN",		PMB8876_GPIO_KP_IN4,	0},
	{"IN5_IN",		PMB8876_GPIO_KP_IN5,	0},
	{"IN6_IN",		PMB8876_GPIO_KP_IN6,	0},
	{"OUT0_OUT",	PMB8876_GPIO_KP_OUT0,	0},
	{"OUT1_OUT",	PMB8876_GPIO_KP_OUT1,	0},
	{"OUT2_OUT",	PMB8876_GPIO_KP_OUT2,	0},
	{"OUT3_OUT",	PMB8876_GPIO_KP_OUT3,	0},
};

static const pmb887x_cpu_module_gpio_t pmb8876_dsp_gpios[] = {
	{"DSPOUT0_OUT",	PMB8876_GPIO_DSPOUT0,	0},
	{"DSPIN0_IN",	PMB8876_GPIO_DSPIN0,	0},
	{"DSPOUT1_OUT",	PMB8876_GPIO_DSPOUT1,	0},
	{"DSPIN1_IN",	PMB8876_GPIO_DSPIN1,	0},
};

static const int pmb8876_gprscu_irqs[] = {
	PMB8876_GPRSCU_INT0_IRQ,
	PMB8876_GPRSCU_INT1_IRQ
};

static const int pmb8876_tpu_irqs[] = {
	PMB8876_TPU_INT_GP0_IRQ,
	PMB8876_TPU_INT_GP1_IRQ,
	PMB8876_TPU_INT_GP2_IRQ,
	PMB8876_TPU_INT_GP3_IRQ,
	PMB8876_TPU_INT_GP4_IRQ,
	PMB8876_TPU_INT_GP5_IRQ,
	PMB8876_TPU_INT0_IRQ,
	PMB8876_TPU_INT1_IRQ
};

static const int pmb8876_dif_irqs[] = {
	PMB8876_DIF_RX_SINGLE_IRQ,
	PMB8876_DIF_RX_BURST_IRQ,
	PMB8876_DIF_TX_IRQ,
	PMB8876_DIF_ERR_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8876_dif_gpios[] = {
	{"D2_IN",	PMB8876_GPIO_DIF_D2,	0},
	{"D2_OUT",	PMB8876_GPIO_DIF_D2,	0},
	{"D0_IN",	PMB8876_GPIO_DIF_D0,	0},
	{"D0_OUT",	PMB8876_GPIO_DIF_D0,	0},
	{"CD_OUT",	PMB8876_GPIO_DIF_CD,	0},
	{"CS1_OUT",	PMB8876_GPIO_DIF_CS1,	0},
	{"D1_IN",	PMB8876_GPIO_DIF_D1,	0},
	{"D1_OUT",	PMB8876_GPIO_DIF_D1,	0},
	{"D3_IN",	PMB8876_GPIO_DIF_D3,	0},
	{"D3_OUT",	PMB8876_GPIO_DIF_D3,	0},
	{"D4_IN",	PMB8876_GPIO_DIF_D4,	0},
	{"D4_OUT",	PMB8876_GPIO_DIF_D4,	0},
	{"D5_IN",	PMB8876_GPIO_DIF_D5,	0},
	{"D5_OUT",	PMB8876_GPIO_DIF_D5,	0},
	{"D6_IN",	PMB8876_GPIO_DIF_D6,	0},
	{"D6_OUT",	PMB8876_GPIO_DIF_D6,	0},
	{"D7_IN",	PMB8876_GPIO_DIF_D7,	0},
	{"D7_OUT",	PMB8876_GPIO_DIF_D7,	0},
	{"CS2_OUT",	PMB8876_GPIO_DIF_CS2,	0},
	{"WR_OUT",	PMB8876_GPIO_DIF_WR,	0},
	{"RD_OUT",	PMB8876_GPIO_DIF_RD,	0},
};

static const pmb887x_cpu_module_dma_t pmb8876_dif_dma[] = {
	{"TX",	PMB887X_DMAC_BUS_AHB2,	4,	0},
	{"RX",	PMB887X_DMAC_BUS_AHB2,	5,	0},
};

static const pmb887x_cpu_module_gpio_t pmb8876_mmci_gpios[] = {
	{"DAT1_IN",		PMB8876_GPIO_MMCI_DAT1,	0},
	{"DAT1_OUT",	PMB8876_GPIO_MMCI_DAT1,	0},
	{"CMD_IN",		PMB8876_GPIO_MMCI_CMD,	0},
	{"CMD_OUT",		PMB8876_GPIO_MMCI_CMD,	0},
	{"DAT0_IN",		PMB8876_GPIO_MMCI_DAT0,	0},
	{"DAT0_OUT",	PMB8876_GPIO_MMCI_DAT0,	0},
	{"CLK_IN",		PMB8876_GPIO_MMCI_CLK,	0},
	{"CLK_OUT",		PMB8876_GPIO_MMCI_CLK,	0},
};

static const int pmb8876_i2c_irqs[] = {
	PMB8876_I2C_SINGLE_REQ_IRQ,
	PMB8876_I2C_BURST_REQ_IRQ,
	PMB8876_I2C_ERROR_IRQ,
	PMB8876_I2C_PROTOCOL_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8876_i2c_gpios[] = {
	{"SCL_IN",	PMB8876_GPIO_I2C_SCL,	0},
	{"SCL_OUT",	PMB8876_GPIO_I2C_SCL,	0},
	{"SDA_IN",	PMB8876_GPIO_I2C_SDA,	0},
	{"SDA_OUT",	PMB8876_GPIO_I2C_SDA,	0},
};

static const pmb887x_cpu_module_dma_t pmb8876_i2c_dma[] = {
	{"TX",	PMB887X_DMAC_BUS_AHB2,	9,	1},
	{"RX",	PMB887X_DMAC_BUS_AHB2,	9,	1},
};

static const pmb887x_cpu_module_t pmb8876_modules[] = {
	{"EBU",		0x0014C005,	PMB8876_EBU_BASE,		"pmb887x-ebu",		NULL,					0,									NULL,					0,									NULL,				0},
	{"USART0",	0x000044F1,	PMB8876_USART0_BASE,	"pmb887x-usart",	pmb8876_usart0_irqs,	ARRAY_SIZE(pmb8876_usart0_irqs),	pmb8876_usart0_gpios,	ARRAY_SIZE(pmb8876_usart0_gpios),	pmb8876_usart0_dma,	ARRAY_SIZE(pmb8876_usart0_dma)},
	{"SSC",		0x00004531,	PMB8876_SSC_BASE,		"pmb887x-ssc",		pmb8876_ssc_irqs,		ARRAY_SIZE(pmb8876_ssc_irqs),		NULL,					0,									pmb8876_ssc_dma,	ARRAY_SIZE(pmb8876_ssc_dma)},
	{"SIM",		0xF000C032,	PMB8876_SIM_BASE,		"pmb887x-sim",		pmb8876_sim_irqs,		ARRAY_SIZE(pmb8876_sim_irqs),		NULL,					0,									pmb8876_sim_dma,	ARRAY_SIZE(pmb8876_sim_dma)},
	{"USART1",	0x000044F1,	PMB8876_USART1_BASE,	"pmb887x-usart",	pmb8876_usart1_irqs,	ARRAY_SIZE(pmb8876_usart1_irqs),	pmb8876_usart1_gpios,	ARRAY_SIZE(pmb8876_usart1_gpios),	pmb8876_usart1_dma,	ARRAY_SIZE(pmb8876_usart1_dma)},
	{"USB",		0xF047C012,	PMB8876_USB_BASE,		"pmb887x-usb",		pmb8876_usb_irqs,		ARRAY_SIZE(pmb8876_usb_irqs),		NULL,					0,									NULL,				0},
	{"VIC",		0x0031C011,	PMB8876_VIC_BASE,		"pmb887x-vic",		NULL,					0,									NULL,					0,									NULL,				0},
	{"DMAC",	0x0A141080,	PMB8876_DMAC_BASE,		"pmb887x-dmac",		pmb8876_dmac_irqs,		ARRAY_SIZE(pmb8876_dmac_irqs),		NULL,					0,									NULL,				0},
	{"CAPCOM0",	0x00005011,	PMB8876_CAPCOM0_BASE,	"pmb887x-capcom",	pmb8876_capcom0_irqs,	ARRAY_SIZE(pmb8876_capcom0_irqs),	pmb8876_capcom0_gpios,	ARRAY_SIZE(pmb8876_capcom0_gpios),	NULL,				0},
	{"CAPCOM1",	0x00005011,	PMB8876_CAPCOM1_BASE,	"pmb887x-capcom",	pmb8876_capcom1_irqs,	ARRAY_SIZE(pmb8876_capcom1_irqs),	pmb8876_capcom1_gpios,	ARRAY_SIZE(pmb8876_capcom1_gpios),	NULL,				0},
	{"GPIO",	0xF023C032,	PMB8876_GPIO_BASE,		"pmb887x-gpio",		NULL,					0,									NULL,					0,									NULL,				0},
	{"SCU",		0xF040C012,	PMB8876_SCU_BASE,		"pmb887x-scu",		pmb8876_scu_irqs,		ARRAY_SIZE(pmb8876_scu_irqs),		pmb8876_scu_gpios,		ARRAY_SIZE(pmb8876_scu_gpios),		NULL,				0},
	{"PLL",		0x00000001,	PMB8876_PLL_BASE,		"pmb887x-pll",		pmb8876_pll_irqs,		ARRAY_SIZE(pmb8876_pll_irqs),		pmb8876_pll_gpios,		ARRAY_SIZE(pmb8876_pll_gpios),		NULL,				0},
	{"SCCU",	0x00000002,	PMB8876_SCCU_BASE,		"pmb887x-sccu",		pmb8876_sccu_irqs,		ARRAY_SIZE(pmb8876_sccu_irqs),		NULL,					0,									NULL,				0},
	{"RTC",		0xF049C011,	PMB8876_RTC_BASE,		"pmb887x-rtc",		pmb8876_rtc_irqs,		ARRAY_SIZE(pmb8876_rtc_irqs),		NULL,					0,									NULL,				0},
	{"GPTU0",	0x0001C011,	PMB8876_GPTU0_BASE,		"pmb887x-gptu",		pmb8876_gptu0_irqs,		ARRAY_SIZE(pmb8876_gptu0_irqs),		NULL,					0,									NULL,				0},
	{"GPTU1",	0x0001C011,	PMB8876_GPTU1_BASE,		"pmb887x-gptu",		pmb8876_gptu1_irqs,		ARRAY_SIZE(pmb8876_gptu1_irqs),		NULL,					0,									NULL,				0},
	{"STM",		0x0000C011,	PMB8876_STM_BASE,		"pmb887x-stm",		NULL,					0,									NULL,					0,									NULL,				0},
	{"ADC",		0xF024C021,	PMB8876_ADC_BASE,		"pmb887x-adc",		pmb8876_adc_irqs,		ARRAY_SIZE(pmb8876_adc_irqs),		NULL,					0,									NULL,				0},
	{"KEYPAD",	0xF046C021,	PMB8876_KEYPAD_BASE,	"pmb887x-keypad",	pmb8876_keypad_irqs,	ARRAY_SIZE(pmb8876_keypad_irqs),	pmb8876_keypad_gpios,	ARRAY_SIZE(pmb8876_keypad_gpios),	NULL,				0},
	{"DSP",		0xF022C031,	PMB8876_DSP_BASE,		"pmb887x-dsp",		NULL,					0,									pmb8876_dsp_gpios,		ARRAY_SIZE(pmb8876_dsp_gpios),		NULL,				0},
	{"GPRSCU",	0xF003C022,	PMB8876_GPRSCU_BASE,	"pmb887x-gprscu",	pmb8876_gprscu_irqs,	ARRAY_SIZE(pmb8876_gprscu_irqs),	NULL,					0,									NULL,				0},
	{"AFC",		0xF004C011,	PMB8876_AFC_BASE,		"pmb887x-afc",		NULL,					0,									NULL,					0,									NULL,				0},
	{"TPU",		0xF021C012,	PMB8876_TPU_BASE,		"pmb887x-tpu",		pmb8876_tpu_irqs,		ARRAY_SIZE(pmb8876_tpu_irqs),		NULL,					0,									NULL,				0},
	{"DIF",		0xF043C012,	PMB8876_DIF_BASE,		"pmb887x-dif-v2",	pmb8876_dif_irqs,		ARRAY_SIZE(pmb8876_dif_irqs),		pmb8876_dif_gpios,		ARRAY_SIZE(pmb8876_dif_gpios),		pmb8876_dif_dma,	ARRAY_SIZE(pmb8876_dif_dma)},
	{"MMCI",	0xF041C022,	PMB8876_MMCI_BASE,		"pmb887x-mmci",		NULL,					0,									pmb8876_mmci_gpios,		ARRAY_SIZE(pmb8876_mmci_gpios),		NULL,				0},
	{"I2C",		0xF057C012,	PMB8876_I2C_BASE,		"pmb887x-i2c-v2",	pmb8876_i2c_irqs,		ARRAY_SIZE(pmb8876_i2c_irqs),		pmb8876_i2c_gpios,		ARRAY_SIZE(pmb8876_i2c_gpios),		pmb8876_i2c_dma,	ARRAY_SIZE(pmb8876_i2c_dma)},
	{"MMICIF",	0xF053C012,	PMB8876_MMICIF_BASE,	"pmb887x-mmicif",	NULL,					0,									NULL,					0,									NULL,				0},
};

static const pmb887x_cpu_t pmb8876_cpu = {
	.modules = pmb8876_modules,
	.modules_count = ARRAY_SIZE(pmb8876_modules),
	.dsp_config = &pmb8876_dsp_config,
};

static const pmb887x_dsp_peripheral_config_t pmb8875_dsp_peripherals[] = {
	{
		.name = "INT",
		.type = PMB887X_DSP_PERIPHERAL_INTERRUPT,
		.base = 0xE600,
		.size = 0x0016,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "CIPH",
		.type = PMB887X_DSP_PERIPHERAL_CIPHER,
		.base = 0xE620,
		.size = 0x0010,
		.ram_base = PMB8875_TEAK_CIPHER_RAM_BASE,
		.ram_size = PMB8875_TEAK_CIPHER_RAM_SIZE,
	},
	{
		.name = "TMR1",
		.type = PMB887X_DSP_PERIPHERAL_TIMER1,
		.base = 0xE630,
		.size = 0x0004,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "TMR2",
		.type = PMB887X_DSP_PERIPHERAL_TIMER2,
		.base = 0xE634,
		.size = 0x0003,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "EQ",
		.type = PMB887X_DSP_PERIPHERAL_EQUALIZER,
		.base = 0xE640,
		.size = 0x0007,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "CHDEC",
		.type = PMB887X_DSP_PERIPHERAL_CHANNEL_DECODER,
		.base = 0xE650,
		.size = 0x0010,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "AFE",
		.type = PMB887X_DSP_PERIPHERAL_AFE,
		.base = 0xE670,
		.size = 0x0007,
		.ram_base = PMB8875_TEAK_AFE_RAM_BASE,
		.ram_size = PMB8875_TEAK_AFE_RAM_SIZE,
	},
	{
		.name = "BB",
		.type = PMB887X_DSP_PERIPHERAL_BASEBAND,
		.base = 0xE680,
		.size = 0x000D,
		.ram_base = PMB8875_TEAK_DEMODULATOR_RAM_BASE,
		.ram_size = PMB8875_TEAK_DEMODULATOR_RAM_SIZE,
	},
	{
		.name = "MCS",
		.type = PMB887X_DSP_PERIPHERAL_MCS,
		.base = 0xE690,
		.size = 0x0006,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "DSP",
		.type = PMB887X_DSP_PERIPHERAL_DSP,
		.base = 0xE6A0,
		.size = 0x0009,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "MOD",
		.type = PMB887X_DSP_PERIPHERAL_MODULATOR,
		.base = 0xE6B0,
		.size = 0x000B,
		.ram_base = PMB8875_TEAK_MODULATOR_RAM_BASE,
		.ram_size = PMB8875_TEAK_MODULATOR_RAM_SIZE,
	},
	{
		.name = "SSC",
		.type = PMB887X_DSP_PERIPHERAL_SSC,
		.base = 0xE6C0,
		.size = 0x0009,
		.ram_base = 0,
		.ram_size = 0,
	},
	{
		.name = "I2S1",
		.type = PMB887X_DSP_PERIPHERAL_I2S,
		.base = 0xE6D0,
		.size = 0x000B,
		.ram_base = PMB8875_TEAK_I2S1_RAM_BASE,
		.ram_size = PMB8875_TEAK_I2S1_RAM_SIZE,
	},
	{
		.name = "I2S2",
		.type = PMB887X_DSP_PERIPHERAL_I2S,
		.base = 0xE6E0,
		.size = 0x000B,
		.ram_base = PMB8875_TEAK_I2S2_RAM_BASE,
		.ram_size = PMB8875_TEAK_I2S2_RAM_SIZE,
	},
	{
		.name = "I2S3",
		.type = PMB887X_DSP_PERIPHERAL_I2S_TX,
		.base = 0xE6F0,
		.size = 0x000B,
		.ram_base = PMB8875_TEAK_I2S3_RAM_BASE,
		.ram_size = PMB8875_TEAK_I2S3_RAM_SIZE,
	},
};

static const pmb887x_dsp_config_t pmb8875_dsp_config = {
	.name = "pmb8875",
	.default_rom_version = 0x0602,
	.page_field_mask = 0x0001,
	.program_page_shift = 1,
	.program_rom_base = PMB8875_TEAK_PROM_FIXED_BASE,
	.program_bank_base = PMB8875_TEAK_PROM_PAGE0_BASE,
	.program_bank_count = 2,
	.data_rom_base = PMB8875_TEAK_DROM_FIXED_BASE,
	.data_bank_base = PMB8875_TEAK_DROM_PAGE0_BASE,
	.data_bank_count = 2,
	.shared_base = PMB8875_TEAK_SHARED_RAM_BASE,
	.shared_size = PMB8875_TEAK_SHARED_RAM_SIZE,
	.y_space_base = PMB8875_TEAK_YRAM_BASE,
	.mmio_base = PMB8875_TEAK_INT_BASE,
	.mmio_size = 0x0100,
	.peripherals = pmb8875_dsp_peripherals,
	.peripheral_count = ARRAY_SIZE(pmb8875_dsp_peripherals),
};

static const int pmb8875_usart0_irqs[] = {
	PMB8875_USART0_TX_IRQ,
	PMB8875_USART0_TBUF_IRQ,
	PMB8875_USART0_RX_IRQ,
	PMB8875_USART0_ERR_IRQ,
	PMB8875_USART0_CTS_IRQ,
	PMB8875_USART0_ABDET_IRQ,
	PMB8875_USART0_ABSTART_IRQ,
	PMB8875_USART0_TMO_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8875_usart0_gpios[] = {
	{"RXD_IN",	PMB8875_GPIO_USART0_RXD,	0},
	{"TXD_OUT",	PMB8875_GPIO_USART0_TXD,	0},
	{"RTS_OUT",	PMB8875_GPIO_USART0_RTS,	0},
	{"CTS_IN",	PMB8875_GPIO_USART0_CTS,	0},
};

static const int pmb8875_ssc_irqs[] = {
	PMB8875_SSC_TX_IRQ,
	PMB8875_SSC_RX_IRQ,
	PMB8875_SSC_ERR_IRQ,
	PMB8875_SSC_TMO_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8875_ssc_gpios[] = {
	{"SCLK_OUT",	PMB8875_GPIO_SSC2_SCLK,	0},
	{"MTSR_OUT",	PMB8875_GPIO_SSC2_MTSR,	0},
	{"MRST_IN",		PMB8875_GPIO_SSC2_MRST,	0},
};

static const pmb887x_cpu_module_dma_t pmb8875_ssc_dma[] = {
	{"TX",	PMB887X_DMAC_BUS_AHB1,	2,	0},
	{"RX",	PMB887X_DMAC_BUS_AHB1,	3,	0},
};

static const int pmb8875_sim_irqs[] = {
	PMB8875_SIM_ERR_IRQ,
	PMB8875_SIM_IN_IRQ,
	PMB8875_SIM_OK_IRQ
};

static const pmb887x_cpu_module_dma_t pmb8875_sim_dma[] = {
	{"TX",	PMB887X_DMAC_BUS_AHB1,	8,	0},
	{"RX",	PMB887X_DMAC_BUS_AHB1,	8,	0},
};

static const int pmb8875_usart1_irqs[] = {
	PMB8875_USART1_TX_IRQ,
	PMB8875_USART1_TBUF_IRQ,
	PMB8875_USART1_RX_IRQ,
	PMB8875_USART1_ERR_IRQ,
	PMB8875_USART1_CTS_IRQ,
	PMB8875_USART1_ABDET_IRQ,
	PMB8875_USART1_ABSTART_IRQ,
	PMB8875_USART1_TMO_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8875_usart1_gpios[] = {
	{"RXD_IN",	PMB8875_GPIO_USART1_RXD,	0},
	{"TXD_OUT",	PMB8875_GPIO_USART1_TXD,	0},
	{"RTS_OUT",	PMB8875_GPIO_USART1_RTS,	0},
	{"CTS_IN",	PMB8875_GPIO_USART1_CTS,	0},
};

static const pmb887x_cpu_module_dma_t pmb8875_usart1_dma[] = {
	{"TX",	PMB887X_DMAC_BUS_AHB1,	6,	0},
	{"RX",	PMB887X_DMAC_BUS_AHB1,	7,	0},
};

static const int pmb8875_dif_irqs[] = {
	PMB8875_DIF_TX_IRQ,
	PMB8875_DIF_RX_IRQ,
	PMB8875_DIF_ERR_IRQ,
	PMB8875_DIF_TMO_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8875_dif_gpios[] = {
	{"SCLK_OUT",	PMB8875_GPIO_DIF_CLK,	0},
	{"MTSR_OUT",	PMB8875_GPIO_DIF_DAT,	0},
	{"RS_OUT",		PMB8875_GPIO_DIF_RS,	0},
	{"CS_OUT",		PMB8875_GPIO_DIF_CS,	0},
	{"RESET_OUT",	PMB8875_GPIO_DIF_RESET,	0},
	{"MRST_IN",		PMB8875_GPIO_DSPIN1,	3},
};

static const pmb887x_cpu_module_dma_t pmb8875_dif_dma[] = {
	{"TX",	PMB887X_DMAC_BUS_AHB1,	4,	0},
	{"RX",	PMB887X_DMAC_BUS_AHB1,	5,	0},
};

static const int pmb8875_usb_irqs[] = {
	PMB8875_USB_IRQ
};

static const int pmb8875_dmac_irqs[] = {
	PMB8875_DMAC_ERR_IRQ,
	PMB8875_DMAC_CH0_IRQ,
	PMB8875_DMAC_CH1_IRQ,
	PMB8875_DMAC_CH2_IRQ,
	PMB8875_DMAC_CH3_IRQ,
	PMB8875_DMAC_CH4_IRQ,
	PMB8875_DMAC_CH5_IRQ,
	PMB8875_DMAC_CH6_IRQ,
	PMB8875_DMAC_CH7_IRQ
};

static const int pmb8875_capcom0_irqs[] = {
	PMB8875_CAPCOM0_T0_IRQ,
	PMB8875_CAPCOM0_T1_IRQ,
	PMB8875_CAPCOM0_CC0_IRQ,
	PMB8875_CAPCOM0_CC1_IRQ,
	PMB8875_CAPCOM0_CC2_IRQ,
	PMB8875_CAPCOM0_CC3_IRQ,
	PMB8875_CAPCOM0_CC4_IRQ,
	PMB8875_CAPCOM0_CC5_IRQ,
	PMB8875_CAPCOM0_CC6_IRQ,
	PMB8875_CAPCOM0_CC7_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8875_capcom0_gpios[] = {
	{"CC6_IN",	PMB8875_GPIO_USART0_RTS,	2},
};

static const int pmb8875_capcom1_irqs[] = {
	PMB8875_CAPCOM1_T0_IRQ,
	PMB8875_CAPCOM1_T1_IRQ,
	PMB8875_CAPCOM1_CC0_IRQ,
	PMB8875_CAPCOM1_CC1_IRQ,
	PMB8875_CAPCOM1_CC2_IRQ,
	PMB8875_CAPCOM1_CC3_IRQ,
	PMB8875_CAPCOM1_CC4_IRQ,
	PMB8875_CAPCOM1_CC5_IRQ,
	PMB8875_CAPCOM1_CC6_IRQ,
	PMB8875_CAPCOM1_CC7_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8875_capcom1_gpios[] = {
	{"CC0_IN",	PMB8875_GPIO_KP_IN5,		2},
	{"CC2_IN",	PMB8875_GPIO_USART0_CTS,	2},
	{"CC6_IN",	PMB8875_GPIO_DSPOUT0,		2},
	{"CC7_IN",	PMB8875_GPIO_SSC2_MRST,		2},
};

static const int pmb8875_scu_irqs[] = {
	PMB8875_SCU_EXTI0_IRQ,
	PMB8875_SCU_EXTI1_IRQ,
	PMB8875_SCU_EXTI2_IRQ,
	PMB8875_SCU_EXTI3_IRQ,
	PMB8875_SCU_EXTI4_IRQ,
	PMB8875_SCU_EXTI5_IRQ,
	PMB8875_SCU_EXTI6_IRQ,
	PMB8875_SCU_EXTI7_IRQ,
	PMB8875_SCU_PM_INT_IRQ,
	PMB8875_SCU_DSP0_IRQ,
	PMB8875_SCU_DSP1_IRQ,
	PMB8875_SCU_DSP2_IRQ,
	PMB8875_SCU_DSP3_IRQ,
	PMB8875_SCU_UNK0_IRQ,
	PMB8875_SCU_UNK1_IRQ,
	PMB8875_SCU_UNK2_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8875_scu_gpios[] = {
	{"EXTI0_IN",	PMB8875_GPIO_KP_IN0,		2},
	{"EXTI0_IN",	PMB8875_GPIO_KP_IN0,		6},
	{"EXTI1_IN",	PMB8875_GPIO_KP_OUT0,		2},
	{"EXTI1_IN",	PMB8875_GPIO_KP_OUT0,		6},
	{"EXTI3_IN",	PMB8875_GPIO_USART1_RXD,	3},
	{"EXTI2_IN",	PMB8875_GPIO_I2C_SDA,		2},
	{"EXTI2_IN",	PMB8875_GPIO_I2C_SDA,		6},
	{"EXTI4_IN",	PMB8875_GPIO_RF_STR0,		1},
	{"EXTI4_IN",	PMB8875_GPIO_RF_STR0,		5},
	{"EXTI4_IN",	PMB8875_GPIO_CLKOUT0,		3},
};

static const int pmb8875_pll_irqs[] = {
	PMB8875_PLL_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8875_pll_gpios[] = {
	{"CLK32_OUT",	PMB8875_GPIO_SSC2_MTSR,	3},
	{"CLK32_OUT",	PMB8875_GPIO_DSPIN0,	1},
};

static const int pmb8875_sccu_irqs[] = {
	PMB8875_SCCU_UNK_IRQ,
	PMB8875_SCCU_WAKE_IRQ
};

static const int pmb8875_rtc_irqs[] = {
	PMB8875_RTC_IRQ
};

static const int pmb8875_i2c_irqs[] = {
	PMB8875_I2C_DATA_IRQ,
	PMB8875_I2C_PROTO_IRQ,
	PMB8875_I2C_END_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8875_i2c_gpios[] = {
	{"SCL_IN",	PMB8875_GPIO_I2C_SCL,	0},
	{"SCL_OUT",	PMB8875_GPIO_I2C_SCL,	0},
	{"SDA_IN",	PMB8875_GPIO_I2C_SDA,	0},
	{"SDA_OUT",	PMB8875_GPIO_I2C_SDA,	0},
};

static const int pmb8875_gptu0_irqs[] = {
	PMB8875_GPTU0_SRC7_IRQ,
	PMB8875_GPTU0_SRC6_IRQ,
	PMB8875_GPTU0_SRC5_IRQ,
	PMB8875_GPTU0_SRC4_IRQ,
	PMB8875_GPTU0_SRC3_IRQ,
	PMB8875_GPTU0_SRC2_IRQ,
	PMB8875_GPTU0_SRC1_IRQ,
	PMB8875_GPTU0_SRC0_IRQ
};

static const int pmb8875_gptu1_irqs[] = {
	PMB8875_GPTU1_SRC7_IRQ,
	PMB8875_GPTU1_SRC6_IRQ,
	PMB8875_GPTU1_SRC5_IRQ,
	PMB8875_GPTU1_SRC4_IRQ,
	PMB8875_GPTU1_SRC3_IRQ,
	PMB8875_GPTU1_SRC2_IRQ,
	PMB8875_GPTU1_SRC1_IRQ,
	PMB8875_GPTU1_SRC0_IRQ
};

static const int pmb8875_adc_irqs[] = {
	PMB8875_ADC_INT0_IRQ,
	PMB8875_ADC_INT1_IRQ
};

static const int pmb8875_keypad_irqs[] = {
	PMB8875_KEYPAD_INT0_IRQ,
	PMB8875_KEYPAD_INT1_IRQ,
	PMB8875_KEYPAD_INT2_IRQ,
	PMB8875_KEYPAD_INT3_IRQ
};

static const pmb887x_cpu_module_gpio_t pmb8875_keypad_gpios[] = {
	{"IN0_IN",		PMB8875_GPIO_KP_IN0,	0},
	{"IN1_IN",		PMB8875_GPIO_KP_IN1,	0},
	{"IN2_IN",		PMB8875_GPIO_KP_IN2,	0},
	{"IN3_IN",		PMB8875_GPIO_KP_IN3,	0},
	{"IN4_IN",		PMB8875_GPIO_KP_IN4,	0},
	{"IN5_IN",		PMB8875_GPIO_KP_IN5,	0},
	{"IN6_IN",		PMB8875_GPIO_KP_IN6,	0},
	{"OUT0_OUT",	PMB8875_GPIO_KP_OUT0,	0},
	{"OUT1_OUT",	PMB8875_GPIO_KP_OUT1,	0},
	{"OUT2_OUT",	PMB8875_GPIO_KP_OUT2,	0},
	{"OUT3_OUT",	PMB8875_GPIO_KP_OUT3,	0},
};

static const pmb887x_cpu_module_gpio_t pmb8875_dsp_gpios[] = {
	{"DSPOUT0_OUT",	PMB8875_GPIO_DSPOUT0,	0},
	{"DSPIN0_IN",	PMB8875_GPIO_DSPIN0,	0},
	{"DSPOUT1_OUT",	PMB8875_GPIO_DSPOUT1,	0},
	{"DSPIN1_IN",	PMB8875_GPIO_DSPIN1,	0},
};

static const int pmb8875_gprscu_irqs[] = {
	PMB8875_GPRSCU_INT0_IRQ,
	PMB8875_GPRSCU_INT1_IRQ
};

static const int pmb8875_tpu_irqs[] = {
	PMB8875_TPU_INT_GP0_IRQ,
	PMB8875_TPU_INT_GP1_IRQ,
	PMB8875_TPU_INT_GP2_IRQ,
	PMB8875_TPU_INT_GP3_IRQ,
	PMB8875_TPU_INT_GP4_IRQ,
	PMB8875_TPU_INT_GP5_IRQ,
	PMB8875_TPU_INT0_IRQ,
	PMB8875_TPU_INT1_IRQ
};

static const pmb887x_cpu_module_t pmb8875_modules[] = {
	{"EBU",		0x0014C004,	PMB8875_EBU_BASE,		"pmb887x-ebu",		NULL,					0,									NULL,					0,									NULL,				0},
	{"USART0",	0x000044E2,	PMB8875_USART0_BASE,	"pmb887x-usart",	pmb8875_usart0_irqs,	ARRAY_SIZE(pmb8875_usart0_irqs),	pmb8875_usart0_gpios,	ARRAY_SIZE(pmb8875_usart0_gpios),	NULL,				0},
	{"SSC",		0x00004525,	PMB8875_SSC_BASE,		"pmb887x-ssc",		pmb8875_ssc_irqs,		ARRAY_SIZE(pmb8875_ssc_irqs),		pmb8875_ssc_gpios,		ARRAY_SIZE(pmb8875_ssc_gpios),		pmb8875_ssc_dma,	ARRAY_SIZE(pmb8875_ssc_dma)},
	{"SIM",		0xF000C032,	PMB8875_SIM_BASE,		"pmb887x-sim",		pmb8875_sim_irqs,		ARRAY_SIZE(pmb8875_sim_irqs),		NULL,					0,									pmb8875_sim_dma,	ARRAY_SIZE(pmb8875_sim_dma)},
	{"USART1",	0x000044E2,	PMB8875_USART1_BASE,	"pmb887x-usart",	pmb8875_usart1_irqs,	ARRAY_SIZE(pmb8875_usart1_irqs),	pmb8875_usart1_gpios,	ARRAY_SIZE(pmb8875_usart1_gpios),	pmb8875_usart1_dma,	ARRAY_SIZE(pmb8875_usart1_dma)},
	{"DIF",		0xF043C000,	PMB8875_DIF_BASE,		"pmb887x-dif-v1",	pmb8875_dif_irqs,		ARRAY_SIZE(pmb8875_dif_irqs),		pmb8875_dif_gpios,		ARRAY_SIZE(pmb8875_dif_gpios),		pmb8875_dif_dma,	ARRAY_SIZE(pmb8875_dif_dma)},
	{"USB",		0xF047C000,	PMB8875_USB_BASE,		"pmb887x-usb",		pmb8875_usb_irqs,		ARRAY_SIZE(pmb8875_usb_irqs),		NULL,					0,									NULL,				0},
	{"VIC",		0x0031C001,	PMB8875_VIC_BASE,		"pmb887x-vic",		NULL,					0,									NULL,					0,									NULL,				0},
	{"DMAC",	0x02041080,	PMB8875_DMAC_BASE,		"pmb887x-dmac",		pmb8875_dmac_irqs,		ARRAY_SIZE(pmb8875_dmac_irqs),		NULL,					0,									NULL,				0},
	{"CAPCOM0",	0x00005003,	PMB8875_CAPCOM0_BASE,	"pmb887x-capcom",	pmb8875_capcom0_irqs,	ARRAY_SIZE(pmb8875_capcom0_irqs),	pmb8875_capcom0_gpios,	ARRAY_SIZE(pmb8875_capcom0_gpios),	NULL,				0},
	{"CAPCOM1",	0x00005003,	PMB8875_CAPCOM1_BASE,	"pmb887x-capcom",	pmb8875_capcom1_irqs,	ARRAY_SIZE(pmb8875_capcom1_irqs),	pmb8875_capcom1_gpios,	ARRAY_SIZE(pmb8875_capcom1_gpios),	NULL,				0},
	{"GPIO",	0xF023C000,	PMB8875_GPIO_BASE,		"pmb887x-gpio",		NULL,					0,									NULL,					0,									NULL,				0},
	{"SCU",		0xF040C000,	PMB8875_SCU_BASE,		"pmb887x-scu",		pmb8875_scu_irqs,		ARRAY_SIZE(pmb8875_scu_irqs),		pmb8875_scu_gpios,		ARRAY_SIZE(pmb8875_scu_gpios),		NULL,				0},
	{"PLL",		0x00000001,	PMB8875_PLL_BASE,		"pmb887x-pll",		pmb8875_pll_irqs,		ARRAY_SIZE(pmb8875_pll_irqs),		pmb8875_pll_gpios,		ARRAY_SIZE(pmb8875_pll_gpios),		NULL,				0},
	{"SCCU",	0x00000002,	PMB8875_SCCU_BASE,		"pmb887x-sccu",		pmb8875_sccu_irqs,		ARRAY_SIZE(pmb8875_sccu_irqs),		NULL,					0,									NULL,				0},
	{"RTC",		0xF049C000,	PMB8875_RTC_BASE,		"pmb887x-rtc",		pmb8875_rtc_irqs,		ARRAY_SIZE(pmb8875_rtc_irqs),		NULL,					0,									NULL,				0},
	{"I2C",		0x00004604,	PMB8875_I2C_BASE,		"pmb887x-i2c-v1",	pmb8875_i2c_irqs,		ARRAY_SIZE(pmb8875_i2c_irqs),		pmb8875_i2c_gpios,		ARRAY_SIZE(pmb8875_i2c_gpios),		NULL,				0},
	{"GPTU0",	0x0001C002,	PMB8875_GPTU0_BASE,		"pmb887x-gptu",		pmb8875_gptu0_irqs,		ARRAY_SIZE(pmb8875_gptu0_irqs),		NULL,					0,									NULL,				0},
	{"GPTU1",	0x0001C002,	PMB8875_GPTU1_BASE,		"pmb887x-gptu",		pmb8875_gptu1_irqs,		ARRAY_SIZE(pmb8875_gptu1_irqs),		NULL,					0,									NULL,				0},
	{"STM",		0x0000C002,	PMB8875_STM_BASE,		"pmb887x-stm",		NULL,					0,									NULL,					0,									NULL,				0},
	{"ADC",		0xF024C010,	PMB8875_ADC_BASE,		"pmb887x-adc",		pmb8875_adc_irqs,		ARRAY_SIZE(pmb8875_adc_irqs),		NULL,					0,									NULL,				0},
	{"KEYPAD",	0xF046C000,	PMB8875_KEYPAD_BASE,	"pmb887x-keypad",	pmb8875_keypad_irqs,	ARRAY_SIZE(pmb8875_keypad_irqs),	pmb8875_keypad_gpios,	ARRAY_SIZE(pmb8875_keypad_gpios),	NULL,				0},
	{"DSP",		0xF022C010,	PMB8875_DSP_BASE,		"pmb887x-dsp",		NULL,					0,									pmb8875_dsp_gpios,		ARRAY_SIZE(pmb8875_dsp_gpios),		NULL,				0},
	{"GPRSCU",	0xF003C010,	PMB8875_GPRSCU_BASE,	"pmb887x-gprscu",	pmb8875_gprscu_irqs,	ARRAY_SIZE(pmb8875_gprscu_irqs),	NULL,					0,									NULL,				0},
	{"AFC",		0xF004C000,	PMB8875_AFC_BASE,		"pmb887x-afc",		NULL,					0,									NULL,					0,									NULL,				0},
	{"TPU",		0xF021C000,	PMB8875_TPU_BASE,		"pmb887x-tpu",		pmb8875_tpu_irqs,		ARRAY_SIZE(pmb8875_tpu_irqs),		NULL,					0,									NULL,				0},
};

static const pmb887x_cpu_t pmb8875_cpu = {
	.modules = pmb8875_modules,
	.modules_count = ARRAY_SIZE(pmb8875_modules),
	.dsp_config = &pmb8875_dsp_config,
};

const pmb887x_cpu_t *pmb887x_cpu_get(int cpu_id) {
	switch (cpu_id) {
		case CPU_PMB8875:
			return &pmb8875_cpu;

		case CPU_PMB8876:
			return &pmb8876_cpu;

		default:
			hw_error("Invalid CPU type: %d", cpu_id);
	}
	return NULL;
}
