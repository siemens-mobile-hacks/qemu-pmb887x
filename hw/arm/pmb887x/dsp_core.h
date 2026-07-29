#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct pmb887x_dsp_core_t pmb887x_dsp_core_t;

#ifdef __cplusplus
extern "C" {
#endif

pmb887x_dsp_core_t *pmb887x_dsp_core_create(uint32_t revision, uint16_t rom_version, const uint8_t *image, size_t size);
uint32_t pmb887x_dsp_core_revision_family(uint32_t revision);
uint16_t pmb887x_dsp_core_default_rom_version(uint32_t revision);
void pmb887x_dsp_core_destroy(pmb887x_dsp_core_t *core);
void pmb887x_dsp_core_reset(pmb887x_dsp_core_t *core);
void pmb887x_dsp_core_run(pmb887x_dsp_core_t *core, size_t cycles);
uint16_t pmb887x_dsp_core_shared_read(pmb887x_dsp_core_t *core, uint16_t offset);
void pmb887x_dsp_core_shared_write(pmb887x_dsp_core_t *core, uint16_t offset, uint16_t value);
void pmb887x_dsp_core_set_request(pmb887x_dsp_core_t *core, size_t index, bool level);
void pmb887x_dsp_core_set_boot_mode(pmb887x_dsp_core_t *core, bool boot_mode);
uint16_t pmb887x_dsp_core_take_com_clear(pmb887x_dsp_core_t *core);
uint16_t pmb887x_dsp_core_get_mcu_interrupts(pmb887x_dsp_core_t *core);
uint32_t pmb887x_dsp_core_get_pc(pmb887x_dsp_core_t *core);

#ifdef __cplusplus
}
#endif
