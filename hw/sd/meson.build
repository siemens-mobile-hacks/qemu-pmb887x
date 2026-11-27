system_ss.add(when: 'CONFIG_PL181', if_true: files('pl181.c'))
system_ss.add(when: 'CONFIG_SD', if_true: files('sd.c', 'core.c'))
if host_os == 'emscripten'   # PMB887X-WEB WASM PORT
  system_ss.add(when: 'CONFIG_SD', if_true: files('sd-wasm.c'))
endif
system_ss.add(when: 'CONFIG_SD_SLOT', if_true: files('sd-slot.c'))
system_ss.add(when: 'CONFIG_SDHCI', if_true: files('sdhci.c'))
system_ss.add(when: 'CONFIG_SDHCI_PCI', if_true: files('sdhci-pci.c'))
system_ss.add(when: 'CONFIG_SSI_SD', if_true: files('ssi-sd.c'))

system_ss.add(when: 'CONFIG_OMAP', if_true: files('omap_mmc.c'))
system_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2835_sdhost.c'))
system_ss.add(when: 'CONFIG_ASPEED_SOC', if_true: files('aspeed_sdhci.c'))
system_ss.add(when: 'CONFIG_ALLWINNER_H3', if_true: files('allwinner-sdhost.c'))
system_ss.add(when: 'CONFIG_NPCM7XX', if_true: files('npcm7xx_sdhci.c'))
system_ss.add(when: 'CONFIG_CADENCE_SDHCI', if_true: files('cadence_sdhci.c'))
