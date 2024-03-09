arm_ss = ss.source_set()
arm_ss.add(files('boot.c'), fdt)
arm_ss.add(when: 'CONFIG_ARM_VIRT', if_true: files('virt.c'))
arm_ss.add(when: 'CONFIG_ACPI', if_true: files('virt-acpi-build.c'))
arm_ss.add(when: 'CONFIG_DIGIC', if_true: files('digic_boards.c'))
arm_ss.add(when: 'CONFIG_EMCRAFT_SF2', if_true: files('msf2-som.c'))
arm_ss.add(when: 'CONFIG_HIGHBANK', if_true: files('highbank.c'))
arm_ss.add(when: 'CONFIG_INTEGRATOR', if_true: files('integratorcp.c'))
arm_ss.add(when: 'CONFIG_MAINSTONE', if_true: files('mainstone.c'))
arm_ss.add(when: 'CONFIG_MICROBIT', if_true: files('microbit.c'))
arm_ss.add(when: 'CONFIG_MPS3R', if_true: files('mps3r.c'))
arm_ss.add(when: 'CONFIG_MUSICPAL', if_true: files('musicpal.c'))
arm_ss.add(when: 'CONFIG_NETDUINOPLUS2', if_true: files('netduinoplus2.c'))
arm_ss.add(when: 'CONFIG_OLIMEX_STM32_H405', if_true: files('olimex-stm32-h405.c'))
arm_ss.add(when: 'CONFIG_NPCM7XX', if_true: files('npcm7xx.c', 'npcm7xx_boards.c'))
arm_ss.add(when: 'CONFIG_NSERIES', if_true: files('nseries.c'))
arm_ss.add(when: 'CONFIG_REALVIEW', if_true: files('realview.c'))
arm_ss.add(when: 'CONFIG_SBSA_REF', if_true: files('sbsa-ref.c'))
arm_ss.add(when: 'CONFIG_STELLARIS', if_true: files('stellaris.c'))
arm_ss.add(when: 'CONFIG_STM32VLDISCOVERY', if_true: files('stm32vldiscovery.c'))
arm_ss.add(when: 'CONFIG_ZYNQ', if_true: files('xilinx_zynq.c'))
arm_ss.add(when: 'CONFIG_SABRELITE', if_true: files('sabrelite.c'))

arm_ss.add(when: 'CONFIG_ARM_V7M', if_true: files('armv7m.c'))
arm_ss.add(when: 'CONFIG_EXYNOS4', if_true: files('exynos4210.c'))
arm_ss.add(when: 'CONFIG_PXA2XX', if_true: files('pxa2xx.c', 'pxa2xx_gpio.c', 'pxa2xx_pic.c'))
arm_ss.add(when: 'CONFIG_DIGIC', if_true: files('digic.c'))
arm_ss.add(when: 'CONFIG_OMAP', if_true: files('omap1.c'))
arm_ss.add(when: 'CONFIG_ALLWINNER_A10', if_true: files('allwinner-a10.c', 'cubieboard.c'))
arm_ss.add(when: 'CONFIG_ALLWINNER_H3', if_true: files('allwinner-h3.c', 'orangepi.c'))
arm_ss.add(when: 'CONFIG_ALLWINNER_R40', if_true: files('allwinner-r40.c', 'bananapi_m2u.c'))
arm_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2836.c', 'raspi.c'))
arm_ss.add(when: ['CONFIG_RASPI', 'TARGET_AARCH64'], if_true: files('bcm2838.c', 'raspi4b.c'))
arm_ss.add(when: 'CONFIG_STM32F100_SOC', if_true: files('stm32f100_soc.c'))
arm_ss.add(when: 'CONFIG_STM32F205_SOC', if_true: files('stm32f205_soc.c'))
arm_ss.add(when: 'CONFIG_STM32F405_SOC', if_true: files('stm32f405_soc.c'))
arm_ss.add(when: 'CONFIG_B_L475E_IOT01A', if_true: files('b-l475e-iot01a.c'))
arm_ss.add(when: 'CONFIG_STM32L4X5_SOC', if_true: files('stm32l4x5_soc.c'))
arm_ss.add(when: 'CONFIG_XLNX_ZYNQMP_ARM', if_true: files('xlnx-zynqmp.c', 'xlnx-zcu102.c'))
arm_ss.add(when: 'CONFIG_XLNX_VERSAL', if_true: files('xlnx-versal.c', 'xlnx-versal-virt.c'))
arm_ss.add(when: 'CONFIG_FSL_IMX25', if_true: files('fsl-imx25.c', 'imx25_pdk.c'))
arm_ss.add(when: 'CONFIG_FSL_IMX31', if_true: files('fsl-imx31.c', 'kzm.c'))
arm_ss.add(when: 'CONFIG_FSL_IMX6', if_true: files('fsl-imx6.c'))
arm_ss.add(when: 'CONFIG_ASPEED_SOC', if_true: files(
  'aspeed.c',
  'aspeed_soc_common.c',
  'aspeed_ast2400.c',
  'aspeed_ast2600.c',
  'aspeed_ast10x0.c',
  'aspeed_eeprom.c',
  'fby35.c'))
arm_ss.add(when: 'CONFIG_MPS2', if_true: files('mps2.c'))
arm_ss.add(when: 'CONFIG_MPS2', if_true: files('mps2-tz.c'))
arm_ss.add(when: 'CONFIG_MSF2', if_true: files('msf2-soc.c'))
arm_ss.add(when: 'CONFIG_MUSCA', if_true: files('musca.c'))
arm_ss.add(when: 'CONFIG_ARMSSE', if_true: files('armsse.c'))
arm_ss.add(when: 'CONFIG_FSL_IMX7', if_true: files('fsl-imx7.c', 'mcimx7d-sabre.c'))
arm_ss.add(when: 'CONFIG_ARM_SMMUV3', if_true: files('smmuv3.c'))
arm_ss.add(when: 'CONFIG_FSL_IMX6UL', if_true: files('fsl-imx6ul.c', 'mcimx6ul-evk.c'))
arm_ss.add(when: 'CONFIG_NRF51_SOC', if_true: files('nrf51_soc.c'))
arm_ss.add(when: 'CONFIG_XEN', if_true: files('xen_arm.c'))

arm_ss.add(when: 'CONFIG_PMB887X', if_true: files(
	'pmb887x.c',
	'pmb887x/fifo.c',
	'pmb887x/boards.c',
	'pmb887x/devices.c',
	'pmb887x/brom.c',
	'pmb887x/flash.c',
	'pmb887x/flash-blk.c',
	'pmb887x/flash-info.c',
	'pmb887x/adc.c',
	'pmb887x/ebu.c',
	'pmb887x/dmac.c',
	'pmb887x/stm.c',
	'pmb887x/pll.c',
	'pmb887x/scu.c',
	'pmb887x/sccu.c',
	'pmb887x/pcl.c',
	'pmb887x/usart.c',
	'pmb887x/mmci.c',
	'pmb887x/gptu.c',
	'pmb887x/nvic.c',
	'pmb887x/tpu.c',
	'pmb887x/rtc.c',
	'pmb887x/i2c.c',
	'pmb887x/i2c/d1094xx.c',
	'pmb887x/i2c/pmb6812.c',
	'pmb887x/i2c/tea5761uk.c',
	'pmb887x/capcom.c',
	'pmb887x/dsp.c',
	'pmb887x/dif.c',
	'pmb887x/mod.c',
	'pmb887x/keypad.c',
	'pmb887x/config.c',
	'pmb887x/io_bridge.c',
	'pmb887x/regs_dump.c',
	'pmb887x/regs_info.c',
	'pmb887x/dif/lcd_common.c',
	'pmb887x/dif/lcd_jbt6k71.c',
	'pmb887x/dif/lcd_ssd1286.c',
))

system_ss.add(when: 'CONFIG_ARM_SMMUV3', if_true: files('smmu-common.c'))
system_ss.add(when: 'CONFIG_CHEETAH', if_true: files('palm.c'))
system_ss.add(when: 'CONFIG_COLLIE', if_true: files('collie.c'))
system_ss.add(when: 'CONFIG_EXYNOS4', if_true: files('exynos4_boards.c'))
system_ss.add(when: 'CONFIG_GUMSTIX', if_true: files('gumstix.c'))
system_ss.add(when: 'CONFIG_NETDUINO2', if_true: files('netduino2.c'))
system_ss.add(when: 'CONFIG_OMAP', if_true: files('omap2.c'))
system_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2835_peripherals.c'))
system_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2838_peripherals.c'))
system_ss.add(when: 'CONFIG_SPITZ', if_true: files('spitz.c'))
system_ss.add(when: 'CONFIG_STRONGARM', if_true: files('strongarm.c'))
system_ss.add(when: 'CONFIG_SX1', if_true: files('omap_sx1.c'))
system_ss.add(when: 'CONFIG_TOSA', if_true: files('tosa.c'))
system_ss.add(when: 'CONFIG_VERSATILE', if_true: files('versatilepb.c'))
system_ss.add(when: 'CONFIG_VEXPRESS', if_true: files('vexpress.c'))
system_ss.add(when: 'CONFIG_Z2', if_true: files('z2.c'))

hw_arch += {'arm': arm_ss}
