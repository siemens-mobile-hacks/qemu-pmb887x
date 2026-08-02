dsp_core_sources = files('dsp/core.c')
dsp_peripheral_sources = files(
	'dsp/peripheral.c',
	'dsp/peripheral/afe.c',
	'dsp/peripheral/baseband.c',
	'dsp/peripheral/channel-decoder.c',
	'dsp/peripheral/cipher.c',
	'dsp/peripheral/cipher-kasumi.c',
	'dsp/peripheral/dsp.c',
	'dsp/peripheral/equalizer.c',
	'dsp/peripheral/i2s.c',
	'dsp/peripheral/i2s-tx.c',
	'dsp/peripheral/interrupt.c',
	'dsp/peripheral/mcs.c',
	'dsp/peripheral/modulator.c',
	'dsp/peripheral/ssc.c',
	'dsp/peripheral/timer1.c',
	'dsp/peripheral/timer2.c',
	'dsp/peripheral/unknown.c',
)

arm_common_ss.add(when: 'CONFIG_PMB887X', if_true: files(
	'board.c',
	'fifo.c',
	'flash.c',
	'flash-blk.c',
	'afc.c',
	'adc.c',
	'ebu.c',
	'dmac.c',
	'stm.c',
	'pll.c',
	'scu.c',
	'sccu.c',
	'gpio.c',
	'usart.c',
	'usb.c',
	'sim.c',
	'mmci.c',
	'mmicif.c',
	'gptu.c',
	'vic.c',
	'tpu.c',
	'rtc.c',
	'i2c_v1.c',
	'i2c_v2.c',
	'capcom.c',
	'dsp.c',
	'gprscu.c',
	'ssc.c',
	'dif_v1.c',
	'dif_v2.c',
	'mod.c',
	'keypad.c',
	'tcm.c',
	'unknown.c',
	'io_bridge.c',
	'regs_dump.c',
	'trace_common.c',

	'gen/brom.c',
	'gen/cpu_meta.c',
	'gen/cpu_modules.c',
	'gen/dsp_rom.c',
	'gen/flash-info.c',

	'board/dsp.c',
	'board/analog.c',
	'board/board.c',
	'board/cpu_module.c',
	'board/devices.c',
	'board/gpio.c',
	'board/keyboard.c',
	'board/memory.c',
	'board/startup.c',

	'utils/regexp.c',
	'utils/strings.c',
	'utils/toml.c',
	'utils/tomlc17.c',

	'ssc/s1d13716.c',
	'ssc/s1d13705.c',
	'ssc/s1d13732.c',
	'ssc/b00b10b.c',
	'ssc/lcd_common.c',
	'ssc/lcd_common_format.c',
	'ssc/lcd_jbt6k71.c',
	'ssc/lcd_ssd1286.c',
	'ssc/lcd_pcf8882.c',
	'ssc/lcd_l2f50333t.c',
	'ssc/lcd_l2f50126t.c',
	'ssc/lcd_l2f50250.c',
	'ssc/lcd_lr38826.c',

	'i2c/d1094xx.c',
	'i2c/pmb6812.c',
	'i2c/tea5761uk.c',
	'sim/apdu.c',
	'sim/gsm_sim.c',
	'sim/sim_fs.c',
	'sim/sim_card.c',
))
arm_common_ss.add(when: 'CONFIG_PMB887X', if_true: dsp_core_sources + dsp_peripheral_sources +
	files('dsp/tcg.c', 'dsp/runtime.c'))

dsp_test_c_args = ['-include', meson.current_source_dir() / 'dsp/tests/compat.h']
host_unit_tests += {
	'pmb887x-dsp-core': {
		'sources': files('dsp/tests/core.c') + dsp_core_sources,
		'c_args': dsp_test_c_args,
		'dependencies': [glib],
	},
	'pmb887x-dsp-peripheral': {
		'sources': files('dsp/tests/peripheral.c') + dsp_peripheral_sources,
		'c_args': dsp_test_c_args,
		'dependencies': [glib],
	},
}

target_unit_tests += {
	'arm-softmmu': {
		'pmb887x-dsp-tcg': {
			'sources': files('dsp/tests/tcg.c'),
			'c_args': dsp_test_c_args,
			'config': 'CONFIG_PMB887X',
		},
	},
}

if cacard.found()
	arm_common_ss.add(when: 'CONFIG_PMB887X', if_true: [cacard, files('sim/cacard_apdu.c')])
endif
