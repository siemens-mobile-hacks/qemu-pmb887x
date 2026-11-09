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
	'afc.c',
	'adc.c',
	'ebu.c',
	'dmac.c',
	'stm.c',
	'cgu.c',
	'scu.c',
	'sccu.c',
	'gpio.c',
	'usart.c',
	'usif.c',
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

	'gen/brom.c',
	'gen/cpu_meta.c',
	'gen/cpu_modules.c',
	'gen/dsp_rom.c',

	'board/accessory.c',
	'board/dsp.c',
	'board/analog.c',
	'board/board.c',
	'board/cpu_module.c',
	'board/devices.c',
	'board/gpio.c',
	'board/keyboard.c',
	'board/memory.c',
	'board/startup.c',
	'board/timings/bus.c',
	'board/timings/memory.c',

	'utils/toml.c',

	'ssc/b00b10b.c',

))
arm_common_ss.add(when: 'CONFIG_PMB887X', if_true: dsp_core_sources + dsp_peripheral_sources +
	files('dsp/tcg.c', 'dsp/runtime.c'))
arm_common_ss.add(when: 'CONFIG_PMB887X', if_true: tomlc17)

host_unit_tests += {
	'pmb887x-dsp-core': {
		'sources': files('dsp/tests/core.c') + dsp_core_sources,
		'dependencies': [glib],
	},
	'pmb887x-dsp-peripheral': {
		'sources': files('dsp/tests/peripheral.c') + dsp_peripheral_sources,
		'dependencies': [glib],
	},
}

target_unit_tests += {
	'arm-softmmu': {
		'pmb887x-dsp-tcg': {
			'sources': files('dsp/tests/tcg.c'),
			'config': 'CONFIG_PMB887X',
		},
	},
}
