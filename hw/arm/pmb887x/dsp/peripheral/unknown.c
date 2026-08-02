#define PMB887X_TRACE_ID		DSP_UNKNOWN
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-unknown"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/trace.h"

static void unknown_destroy(dsp_device_t *device) {
	(void) device;
}

static void unknown_reset(dsp_device_t *device) {
	(void) device;
}

static bool unknown_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	*value = 0;

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool unknown_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t unknown_ops = {
	.destroy = unknown_destroy,
	.reset = unknown_reset,
	.read = unknown_read,
	.write = unknown_write,
};

dsp_device_t *unknown_create(const pmb887x_dsp_peripheral_config_t *config) {
	return dsp_device_create(config, &unknown_ops, NULL);
}
