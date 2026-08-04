#include "qemu/osdep.h"
#include "qemu/bitops.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"

dsp_device_t *dsp_device_create(const pmb887x_dsp_peripheral_config_t *config, const dsp_device_ops_t *ops, void *state) {
	dsp_device_t *device = g_new0(dsp_device_t, 1);
	device->config = config;
	device->ops = ops;
	device->state = state;
	return device;
}

static dsp_device_t *dsp_bus_create_device(dsp_bus_t *bus, const pmb887x_dsp_peripheral_config_t *config, const dsp_host_t *host) {
	dsp_device_t *device;

	switch (config->type) {
		case PMB887X_DSP_PERIPHERAL_AFE:
			g_assert(bus->interrupt != NULL);

			device = afe_create(config, bus->interrupt, host);
			bus->afe = device;
			return device;

		case PMB887X_DSP_PERIPHERAL_BASEBAND:
			g_assert(bus->interrupt != NULL);

			device = baseband_create(config, bus->interrupt, host);
			bus->baseband = device;
			return device;

		case PMB887X_DSP_PERIPHERAL_DSP:
			device = control_create(config, host);
			bus->dsp = device;
			return device;

		case PMB887X_DSP_PERIPHERAL_CIPHER:
			g_assert(bus->interrupt != NULL);

			device = cipher_create(config, bus->interrupt, host);
			bus->cipher = device;
			return device;

		case PMB887X_DSP_PERIPHERAL_CHANNEL_DECODER:
			g_assert(bus->interrupt != NULL);

			device = chdec_create(config, bus->interrupt);
			bus->channel_decoder = device;
			return device;

		case PMB887X_DSP_PERIPHERAL_EQUALIZER:
			g_assert(bus->interrupt != NULL);

			device = equalizer_create(config, bus->interrupt);
			bus->equalizer = device;
			return device;

		case PMB887X_DSP_PERIPHERAL_INTERRUPT:
			device = dsp_int_create(config, host);
			bus->interrupt = device;
			return device;

		case PMB887X_DSP_PERIPHERAL_I2S:
			g_assert(bus->interrupt != NULL);
			g_assert(bus->i2s_count < ARRAY_SIZE(bus->i2s));

			device = i2s_create(config, bus->interrupt, (uint16_t) BIT(bus->i2s_count * 2));
			bus->i2s[bus->i2s_count++] = device;
			return device;

		case PMB887X_DSP_PERIPHERAL_I2S_TX:
			g_assert(bus->interrupt != NULL);

			device = i2s_tx_create(config, bus->interrupt);
			bus->i2s_tx = device;
			return device;

		case PMB887X_DSP_PERIPHERAL_MCS:
			device = mcs_create(config, host);
			bus->mcs = device;
			return device;

		case PMB887X_DSP_PERIPHERAL_MODULATOR:
			g_assert(bus->interrupt != NULL);

			device = modulator_create(config, bus->interrupt);
			bus->modulator = device;
			return device;

		case PMB887X_DSP_PERIPHERAL_SSC:
			g_assert(bus->interrupt != NULL);

			device = ssc_create(config, bus->interrupt, host);
			bus->ssc = device;
			return device;

		case PMB887X_DSP_PERIPHERAL_TIMER1:
			g_assert(bus->interrupt != NULL);

			device = timer1_create(config, bus->interrupt);
			bus->timer1 = device;
			return device;

		case PMB887X_DSP_PERIPHERAL_TIMER2:
			g_assert(bus->interrupt != NULL);

			device = timer2_create(config, bus->interrupt);
			bus->timer2 = device;
			return device;

		default:
			return unknown_create(config);
	}
}

dsp_bus_t *dsp_bus_create(const pmb887x_dsp_config_t *config, const dsp_host_t *host) {
	dsp_bus_t *bus = g_new0(dsp_bus_t, 1);
	bus->device_count = config->peripheral_count;
	bus->host = *host;
	bus->devices = g_new0(dsp_device_t *, bus->device_count);
	for (size_t i = 0; i < bus->device_count; i++)
		bus->devices[i] = dsp_bus_create_device(bus, &config->peripherals[i], host);

	bus->fallback_config = (pmb887x_dsp_peripheral_config_t) {
		.name = "unknown",
		.base = config->mmio_base,
		.size = config->mmio_size,
	};
	bus->fallback = unknown_create(&bus->fallback_config);

	bus->routes = g_new(dsp_route_t, config->mmio_size);
	for (size_t offset = 0; offset < config->mmio_size; offset++) {
		bus->routes[offset].device = bus->fallback;
		bus->routes[offset].offset = offset;
	}

	for (size_t i = 0; i < bus->device_count; i++) {
		size_t base_offset = config->peripherals[i].base - config->mmio_base;
		for (size_t offset = 0; offset < config->peripherals[i].size; offset++) {
			bus->routes[base_offset + offset].device = bus->devices[i];
			bus->routes[base_offset + offset].offset = offset;
		}
	}

	g_assert(bus->interrupt != NULL);
	g_assert(bus->mcs != NULL);
	g_assert(bus->dsp != NULL);
	return bus;
}

void dsp_bus_destroy(dsp_bus_t *bus) {
	if (bus == NULL)
		return;

	bus->fallback->ops->destroy(bus->fallback);
	g_free(bus->fallback);

	for (size_t i = 0; i < bus->device_count; i++) {
		bus->devices[i]->ops->destroy(bus->devices[i]);
		g_free(bus->devices[i]);
	}

	g_free(bus->routes);
	g_free(bus->devices);
	g_free(bus);
}

void dsp_bus_reset(dsp_bus_t *bus) {
	qatomic_set(&bus->gsm_signals, 0);
	bus->fallback->ops->reset(bus->fallback);
	for (size_t i = 0; i < bus->device_count; i++)
		bus->devices[i]->ops->reset(bus->devices[i]);
}

void dsp_bus_set_clock(dsp_bus_t *bus, bool enabled) {
	if (bus->timer2 != NULL)
		timer2_set_clock_enabled(bus->timer2, enabled);
}

void dsp_bus_set_core_idle(dsp_bus_t *bus, bool idle) {
	if (bus->timer2 != NULL)
		timer2_set_core_idle(bus->timer2, idle);
}

void dsp_bus_advance(dsp_bus_t *bus, size_t cycles) {
	if (bus->afe != NULL && afe_is_active(bus->afe))
		afe_advance(bus->afe, cycles);
	if (bus->channel_decoder != NULL && chdec_is_active(bus->channel_decoder))
		chdec_advance(bus->channel_decoder, cycles);
	if (bus->cipher != NULL && cipher_is_active(bus->cipher))
		cipher_advance(bus->cipher, cycles);
	if (bus->equalizer != NULL && equalizer_is_active(bus->equalizer))
		equalizer_advance(bus->equalizer, cycles);
	for (size_t i = 0; i < bus->i2s_count; i++)
		if (i2s_is_active(bus->i2s[i]))
			i2s_advance(bus->i2s[i], cycles);
	if (bus->i2s_tx != NULL && i2s_tx_is_active(bus->i2s_tx))
		i2s_tx_advance(bus->i2s_tx, cycles);
	if (bus->modulator != NULL && modulator_is_active(bus->modulator))
		modulator_advance(bus->modulator, cycles);
	if (bus->ssc != NULL && ssc_is_active(bus->ssc))
		ssc_advance(bus->ssc, cycles);
	if (bus->timer1 != NULL && timer1_is_active(bus->timer1))
		timer1_advance(bus->timer1, cycles);
	if (bus->timer2 != NULL && timer2_is_active(bus->timer2))
		timer2_advance(bus->timer2, cycles);
}

bool dsp_bus_is_active(const dsp_bus_t *bus) {
	if (bus->afe != NULL && afe_is_active(bus->afe))
		return true;
	if (bus->channel_decoder != NULL && chdec_is_active(bus->channel_decoder))
		return true;
	if (bus->cipher != NULL && cipher_is_active(bus->cipher))
		return true;
	if (bus->equalizer != NULL && equalizer_is_active(bus->equalizer))
		return true;
	for (size_t i = 0; i < bus->i2s_count; i++)
		if (i2s_is_active(bus->i2s[i]))
			return true;
	if (bus->i2s_tx != NULL && i2s_tx_is_active(bus->i2s_tx))
		return true;
	if (bus->modulator != NULL && modulator_is_active(bus->modulator))
		return true;
	if (bus->ssc != NULL && ssc_is_active(bus->ssc))
		return true;
	if (bus->timer1 != NULL && timer1_is_active(bus->timer1))
		return true;
	if (bus->timer2 != NULL && timer2_is_active(bus->timer2))
		return true;
	return false;
}

uint16_t dsp_bus_read(dsp_bus_t *bus, uint16_t address) {
	size_t offset = address - bus->fallback_config.base;
	uint32_t pc = bus->host.get_pc(bus->host.opaque);
	uint16_t value;

	if (offset < bus->fallback_config.size)
		return dsp_bus_read_at(bus, address, pc);
	bus->fallback->ops->read(bus->fallback, offset, pc, &value);
	return value;
}

void dsp_bus_write(dsp_bus_t *bus, uint16_t address, uint16_t value) {
	size_t offset = address - bus->fallback_config.base;
	uint32_t pc = bus->host.get_pc(bus->host.opaque);

	if (offset < bus->fallback_config.size) {
		dsp_bus_write_at(bus, address, value, pc);
		return;
	}
	bus->fallback->ops->write(bus->fallback, offset, pc, value);
}

uint16_t dsp_bus_external_read(dsp_bus_t *bus, size_t index) {
	if (index == 0 && bus->channel_decoder != NULL)
		return chdec_external_read(bus->channel_decoder);
	if (index == 1 && bus->equalizer != NULL)
		return equalizer_external_read(bus->equalizer);
	return 0;
}

void dsp_bus_external_write(dsp_bus_t *bus, size_t index, uint16_t value) {
	if (index == 0 && bus->channel_decoder != NULL)
		chdec_external_write(bus->channel_decoder, value);
	if (index == 1 && bus->equalizer != NULL)
		equalizer_external_write(bus->equalizer, value);
}

uint8_t dsp_bus_get_irq_lines(dsp_bus_t *bus) {
	return dsp_int_get_lines(bus->interrupt);
}

uint16_t dsp_bus_get_irq_flags(dsp_bus_t *bus, size_t group) {
	return dsp_int_get_flags(bus->interrupt, group);
}

uint16_t dsp_bus_get_irq_pending_flags(dsp_bus_t *bus, size_t group) {
	return dsp_int_get_pending_flags(bus->interrupt, group);
}

void dsp_bus_set_request(dsp_bus_t *bus, size_t index, bool level) {
	dsp_int_set_request(bus->interrupt, index, level);
}

void dsp_bus_set_input(dsp_bus_t *bus, size_t index, bool level) {
	int edge = control_set_input(bus->dsp, index, level);
	uint16_t flag;

	if (edge == 0)
		return;
	flag = (uint16_t) BIT(4 + index * 2 + (edge < 0));
	dsp_int_set_flags(bus->interrupt, 2, flag);
}

void dsp_bus_set_gsm_clock(dsp_bus_t *bus, uint32_t frequency) {
	if (bus->baseband != NULL)
		baseband_set_clock(bus->baseband, frequency);
}

void dsp_bus_set_gsm_signal(dsp_bus_t *bus, pmb887x_dsp_gsm_signal_t signal, bool level) {
	uint16_t mask;
	uint16_t old_signals;

	g_assert(signal < PMB887X_DSP_GSM_SIGNAL_COUNT);

	mask = (uint16_t) BIT(signal);
	old_signals = qatomic_read(&bus->gsm_signals);
	if (((old_signals & mask) != 0) == level)
		return;
	if (level) {
		qatomic_or(&bus->gsm_signals, mask);
	} else {
		qatomic_and(&bus->gsm_signals, (uint16_t) ~mask);
	}

	if (signal <= PMB887X_DSP_GSM_SIGNAL_RXON) {
		if (bus->baseband != NULL)
			baseband_set_signal(bus->baseband, signal, level);
		return;
	}

	if (signal == PMB887X_DSP_GSM_SIGNAL_CODON) {
		if (bus->modulator != NULL)
			modulator_set_codon(bus->modulator, level);

		uint16_t flag = level ? TEAK_INT_FINTA0_CODONHI : TEAK_INT_FINTA0_CODONLO;
		dsp_int_set_flags(bus->interrupt, 0, flag);
		return;
	}

	if (signal == PMB887X_DSP_GSM_SIGNAL_FRAME && level)
		dsp_int_set_flags(bus->interrupt, 0, TEAK_INT_FINTA0_FRAME);

	if (signal == PMB887X_DSP_GSM_SIGNAL_SYSMCU && level)
		dsp_int_set_flags(bus->interrupt, 1, TEAK_INT_FINTB0_SYSMCU);
}

uint16_t dsp_bus_get_outputs(dsp_bus_t *bus) {
	return control_get_outputs(bus->dsp);
}

uint16_t dsp_bus_take_output_events(dsp_bus_t *bus) {
	return control_take_output_events(bus->dsp);
}

uint16_t dsp_bus_get_comm(dsp_bus_t *bus) {
	return mcs_get_flags(bus->mcs);
}

void dsp_bus_set_comm(dsp_bus_t *bus, uint16_t value) {
	mcs_set_flags(bus->mcs, value);
}

void dsp_bus_clear_comm(dsp_bus_t *bus, uint16_t value) {
	mcs_clear_flags(bus->mcs, value);
}

uint16_t dsp_bus_take_comm_clear(dsp_bus_t *bus) {
	return mcs_take_clear(bus->mcs);
}

uint16_t dsp_bus_take_mcu_irqs(dsp_bus_t *bus) {
	return dsp_int_take_mcu_events(bus->interrupt);
}

uint16_t dsp_bus_get_mcu_semaphores(dsp_bus_t *bus) {
	return mcs_get_mcu_semaphore_status(bus->mcs);
}

void dsp_bus_request_mcu_semaphores(dsp_bus_t *bus, uint16_t value) {
	mcs_request_mcu_semaphores(bus->mcs, value);
}

void dsp_bus_release_mcu_semaphores(dsp_bus_t *bus, uint16_t value) {
	mcs_release_mcu_semaphores(bus->mcs, value);
}
