#ifndef HW_ARM_PMB887X_DSP_TCG_H
#define HW_ARM_PMB887X_DSP_TCG_H

#include "hw/arm/pmb887x/dsp/core.h"

void teak_tcg_request_interrupt(teak_tcg_core_t *core, uint8_t interrupt);
void teak_tcg_set_interrupt(teak_tcg_core_t *core, uint8_t interrupt, bool level);
void teak_tcg_update_irq_lines(teak_tcg_core_t *core, uint8_t lines);
void teak_tcg_request_exit(teak_tcg_core_t *core);
bool teak_tcg_service_interrupt(teak_tcg_core_t *core);
void teak_tcg_invalidate_program(teak_tcg_core_t *core, uint32_t address);
void teak_tcg_invalidate_all(teak_tcg_core_t *core);
bool teak_tcg_execute_block(teak_tcg_core_t *core);
bool teak_tcg_execute_slice(teak_tcg_core_t *core, size_t max_cycles);

#endif
