#ifndef HW_ARM_PMB887X_DSP_RUNTIME_H
#define HW_ARM_PMB887X_DSP_RUNTIME_H

#include <stdbool.h>

#include "hw/arm/pmb887x/dsp/config.h"
#include "hw/arm/pmb887x/dsp/signals.h"

typedef struct dsp_runtime_t dsp_runtime_t;

dsp_runtime_t *dsp_runtime_create(const pmb887x_dsp_config_t *config,
	uint16_t rom_version, const uint8_t *program_rom, const uint8_t *data_rom, void *device_opaque,
	void (*notify_activity)(void *opaque), void (*notify_comm)(void *opaque, uint16_t flags),
	uint32_t (*ssc_transfer)(void *opaque, uint32_t value));
void dsp_runtime_destroy(dsp_runtime_t *runtime);
void dsp_runtime_reset(dsp_runtime_t *runtime);
void dsp_runtime_set_clock(dsp_runtime_t *runtime, bool enabled);
bool dsp_runtime_run(dsp_runtime_t *runtime);
bool dsp_runtime_is_idle(const dsp_runtime_t *runtime);
void dsp_runtime_wake(dsp_runtime_t *runtime);
void dsp_runtime_kick(dsp_runtime_t *runtime);
bool dsp_runtime_take_program_start(dsp_runtime_t *runtime, uint32_t *pc);
bool dsp_runtime_is_program_warming(const dsp_runtime_t *runtime);
void dsp_runtime_finish_program_warmup(dsp_runtime_t *runtime);
void dsp_runtime_thread_enter(void);
void dsp_runtime_thread_exit(void);
uint16_t dsp_runtime_shared_read(dsp_runtime_t *runtime, uint16_t offset);
void dsp_runtime_shared_write(dsp_runtime_t *runtime, uint16_t offset, uint16_t value);
uint64_t dsp_runtime_shared_read_bytes(dsp_runtime_t *runtime, size_t offset, size_t size);
void dsp_runtime_shared_write_bytes(dsp_runtime_t *runtime, size_t offset, uint64_t value, size_t size);
void dsp_runtime_set_request(dsp_runtime_t *runtime, size_t index, bool level);
void dsp_runtime_set_input(dsp_runtime_t *runtime, size_t index, bool level);
void dsp_runtime_set_gsm_signal(dsp_runtime_t *runtime, pmb887x_dsp_gsm_signal_t signal, bool level);
uint16_t dsp_runtime_get_outputs(dsp_runtime_t *runtime);
uint64_t dsp_runtime_get_cache_compiles(const dsp_runtime_t *runtime);
uint16_t dsp_runtime_take_output_events(dsp_runtime_t *runtime);
uint16_t dsp_runtime_get_comm(dsp_runtime_t *runtime);
void dsp_runtime_set_comm(dsp_runtime_t *runtime, uint16_t value);
void dsp_runtime_clear_comm(dsp_runtime_t *runtime, uint16_t value);
uint16_t dsp_runtime_take_comm_clear(dsp_runtime_t *runtime);
uint16_t dsp_runtime_take_mcu_irqs(dsp_runtime_t *runtime);
uint16_t dsp_runtime_get_mcu_semaphores(dsp_runtime_t *runtime);
void dsp_runtime_request_mcu_semaphores(dsp_runtime_t *runtime, uint16_t value);
void dsp_runtime_release_mcu_semaphores(dsp_runtime_t *runtime, uint16_t value);

#endif
