system_ss.add(when: 'CONFIG_ISO7816', if_true: files('iso7816.c'))
system_ss.add(when: 'CONFIG_GSM_SIM', if_true: files('gsm-sim.c', 'gsm-sim-fs.c', 'gsm-sim-storage.c', 'gsm-test-algorithm.c'))
if cacard.found()
  system_ss.add(when: 'CONFIG_ISO7816', if_true: [cacard, files('iso7816-host-card.c')])
endif

host_unit_tests += {
  'smartcard-gsm-sim-fs': {
    'sources': files('tests/gsm-sim-fs.c', 'gsm-sim-fs.c'),
    'dependencies': [glib],
  },
  'smartcard-gsm-sim-storage': {
    'sources': files('tests/gsm-sim-storage.c', 'gsm-sim-storage.c', 'gsm-sim-fs.c'),
    'dependencies': [glib],
  },
  'smartcard-gsm-test-algorithm': {
    'sources': files('tests/gsm-test-algorithm.c', 'gsm-test-algorithm.c'),
    'dependencies': [glib],
  },
}
