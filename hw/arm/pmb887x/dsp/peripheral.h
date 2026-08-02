#ifndef HW_ARM_PMB887X_DSP_PERIPHERAL_H
#define HW_ARM_PMB887X_DSP_PERIPHERAL_H

#include <stdbool.h>

#include "hw/arm/pmb887x/dsp/config.h"
#include "hw/arm/pmb887x/dsp/signals.h"

typedef struct dsp_bus_t dsp_bus_t;
typedef struct dsp_host_t dsp_host_t;

struct dsp_host_t {
	void *opaque;
	void *ssc_opaque;
	void (*set_page)(void *opaque, uint16_t value);
	void (*set_core_disabled)(void *opaque, bool disabled);
	void (*set_interrupt_lines)(void *opaque, uint8_t lines);
	void (*comm_cleared)(void *opaque, uint16_t flags);
	uint32_t (*get_pc)(void *opaque);
	uint16_t (*data_read)(void *opaque, uint16_t address);
	void (*data_write)(void *opaque, uint16_t address, uint16_t value);
	uint32_t (*ssc_transfer)(void *opaque, uint32_t value);
};

dsp_bus_t *dsp_bus_create(const pmb887x_dsp_config_t *config, const dsp_host_t *host);
void dsp_bus_destroy(dsp_bus_t *bus);
void dsp_bus_reset(dsp_bus_t *bus);
void dsp_bus_set_clock(dsp_bus_t *bus, bool enabled);
void dsp_bus_set_core_idle(dsp_bus_t *bus, bool idle);
void dsp_bus_advance(dsp_bus_t *bus, size_t cycles);
bool dsp_bus_is_active(const dsp_bus_t *bus);
uint16_t dsp_bus_read(dsp_bus_t *bus, uint16_t address);
void dsp_bus_write(dsp_bus_t *bus, uint16_t address, uint16_t value);
uint16_t dsp_bus_external_read(dsp_bus_t *bus, size_t index);
void dsp_bus_external_write(dsp_bus_t *bus, size_t index, uint16_t value);
uint8_t dsp_bus_get_irq_lines(dsp_bus_t *bus);
void dsp_bus_set_request(dsp_bus_t *bus, size_t index, bool level);
void dsp_bus_set_input(dsp_bus_t *bus, size_t index, bool level);
void dsp_bus_set_gsm_signal(dsp_bus_t *bus, pmb887x_dsp_gsm_signal_t signal, bool level);
uint16_t dsp_bus_get_outputs(dsp_bus_t *bus);
uint16_t dsp_bus_take_output_events(dsp_bus_t *bus);
uint16_t dsp_bus_get_comm(dsp_bus_t *bus);
void dsp_bus_set_comm(dsp_bus_t *bus, uint16_t value);
void dsp_bus_clear_comm(dsp_bus_t *bus, uint16_t value);
uint16_t dsp_bus_take_comm_clear(dsp_bus_t *bus);
uint16_t dsp_bus_take_mcu_irqs(dsp_bus_t *bus);
uint16_t dsp_bus_get_mcu_semaphores(dsp_bus_t *bus);
void dsp_bus_request_mcu_semaphores(dsp_bus_t *bus, uint16_t value);
void dsp_bus_release_mcu_semaphores(dsp_bus_t *bus, uint16_t value);

#endif
