#ifndef HW_ARM_PMB887X_DSP_PERIPHERAL_INTERNAL_H
#define HW_ARM_PMB887X_DSP_PERIPHERAL_INTERNAL_H

#include "hw/arm/pmb887x/dsp/peripheral.h"

#define DSP_I2S_COUNT	2

typedef struct dsp_device_t dsp_device_t;
typedef struct dsp_device_ops_t dsp_device_ops_t;
typedef struct dsp_route_t dsp_route_t;

struct dsp_device_ops_t {
	void (*destroy)(dsp_device_t *device);
	void (*reset)(dsp_device_t *device);
	bool (*read)(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value);
	bool (*write)(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value);
};

struct dsp_device_t {
	const pmb887x_dsp_peripheral_config_t *config;
	const dsp_device_ops_t *ops;
	void *state;
};

struct dsp_route_t {
	dsp_device_t *device;
	uint16_t offset;
};

struct dsp_bus_t {
	dsp_device_t **devices;
	dsp_route_t *routes;
	size_t device_count;
	dsp_device_t *fallback;
	pmb887x_dsp_peripheral_config_t fallback_config;
	dsp_host_t host;
	uint16_t gsm_signals;
	dsp_device_t *afe;
	dsp_device_t *baseband;
	dsp_device_t *channel_decoder;
	dsp_device_t *cipher;
	dsp_device_t *equalizer;
	dsp_device_t *interrupt;
	dsp_device_t *i2s[DSP_I2S_COUNT];
	dsp_device_t *i2s_tx;
	size_t i2s_count;
	dsp_device_t *mcs;
	dsp_device_t *modulator;
	dsp_device_t *dsp;
	dsp_device_t *ssc;
	dsp_device_t *timer1;
	dsp_device_t *timer2;
};

static inline uint16_t dsp_bus_read_at(dsp_bus_t *bus, uint16_t address, uint32_t pc) {
	dsp_route_t *route = &bus->routes[address - bus->fallback_config.base];
	dsp_device_t *device = route->device;
	uint16_t value;

	if (!device->ops->read(device, route->offset, pc, &value))
		bus->fallback->ops->read(bus->fallback, address - bus->fallback->config->base, pc, &value);
	return value;
}

static inline void dsp_bus_write_at(dsp_bus_t *bus, uint16_t address, uint16_t value, uint32_t pc) {
	dsp_route_t *route = &bus->routes[address - bus->fallback_config.base];
	dsp_device_t *device = route->device;

	if (!device->ops->write(device, route->offset, pc, value))
		bus->fallback->ops->write(bus->fallback, address - bus->fallback->config->base, pc, value);
}

dsp_device_t *dsp_device_create(const pmb887x_dsp_peripheral_config_t *config, const dsp_device_ops_t *ops, void *state);

dsp_device_t *afe_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt, const dsp_host_t *host);
void afe_advance(dsp_device_t *device, size_t cycles);
bool afe_is_active(const dsp_device_t *device);

dsp_device_t *baseband_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt, const dsp_host_t *host);
void baseband_set_signal(dsp_device_t *device, pmb887x_dsp_gsm_signal_t signal, bool level);
void baseband_set_core_idle(dsp_device_t *device, bool idle);
void baseband_advance(dsp_device_t *device, size_t cycles);
bool baseband_is_active(const dsp_device_t *device);

dsp_device_t *chdec_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt);
void chdec_advance(dsp_device_t *device, size_t cycles);
bool chdec_is_active(const dsp_device_t *device);
uint16_t chdec_external_read(dsp_device_t *device);
void chdec_external_write(dsp_device_t *device, uint16_t value);

dsp_device_t *cipher_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt, const dsp_host_t *host);
void cipher_advance(dsp_device_t *device, size_t cycles);
bool cipher_is_active(const dsp_device_t *device);

dsp_device_t *control_create(const pmb887x_dsp_peripheral_config_t *config, const dsp_host_t *host);
int control_set_input(dsp_device_t *device, size_t index, bool level);
uint16_t control_get_outputs(dsp_device_t *device);
uint16_t control_take_output_events(dsp_device_t *device);

dsp_device_t *equalizer_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt);
void equalizer_advance(dsp_device_t *device, size_t cycles);
bool equalizer_is_active(const dsp_device_t *device);
uint16_t equalizer_external_read(dsp_device_t *device);
void equalizer_external_write(dsp_device_t *device, uint16_t value);

dsp_device_t *i2s_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt, uint16_t interrupt_flag);
void i2s_advance(dsp_device_t *device, size_t cycles);
bool i2s_is_active(const dsp_device_t *device);

dsp_device_t *i2s_tx_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt);
void i2s_tx_advance(dsp_device_t *device, size_t cycles);
bool i2s_tx_is_active(const dsp_device_t *device);

dsp_device_t *dsp_int_create(const pmb887x_dsp_peripheral_config_t *config, const dsp_host_t *host);
uint8_t dsp_int_get_lines(dsp_device_t *device);
uint16_t dsp_int_get_flags(dsp_device_t *device, size_t group);
void dsp_int_set_request(dsp_device_t *device, size_t index, bool level);
void dsp_int_set_flags(dsp_device_t *device, size_t group, uint16_t flags);
uint16_t dsp_int_take_mcu_events(dsp_device_t *device);

dsp_device_t *mcs_create(const pmb887x_dsp_peripheral_config_t *config, const dsp_host_t *host);
uint16_t mcs_get_flags(dsp_device_t *device);
void mcs_set_flags(dsp_device_t *device, uint16_t value);
void mcs_clear_flags(dsp_device_t *device, uint16_t value);
uint16_t mcs_take_clear(dsp_device_t *device);
uint16_t mcs_get_mcu_semaphore_status(dsp_device_t *device);
void mcs_request_mcu_semaphores(dsp_device_t *device, uint16_t value);
void mcs_release_mcu_semaphores(dsp_device_t *device, uint16_t value);

dsp_device_t *modulator_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt);
void modulator_advance(dsp_device_t *device, size_t cycles);
bool modulator_is_active(const dsp_device_t *device);

dsp_device_t *ssc_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt, const dsp_host_t *host);
void ssc_advance(dsp_device_t *device, size_t cycles);
bool ssc_is_active(const dsp_device_t *device);

dsp_device_t *timer1_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt);
void timer1_advance(dsp_device_t *device, size_t cycles);
bool timer1_is_active(const dsp_device_t *device);

dsp_device_t *timer2_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt);
void timer2_set_clock_enabled(dsp_device_t *device, bool enabled);
void timer2_set_core_idle(dsp_device_t *device, bool idle);
void timer2_advance(dsp_device_t *device, size_t cycles);
bool timer2_is_active(dsp_device_t *device);

dsp_device_t *unknown_create(const pmb887x_dsp_peripheral_config_t *config);

#endif
