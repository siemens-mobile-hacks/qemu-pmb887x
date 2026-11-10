#include "qemu/osdep.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"

static void unknown_destroy(dsp_device_t *device) {
	(void) device;
}

static void unknown_reset(dsp_device_t *device) {
	(void) device;
}

static bool unknown_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	*value = 0;

	dsp_device_debug_io(device, offset, *value, false, pc);
	return true;
}

static bool unknown_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	dsp_device_debug_io(device, offset, value, true, pc);
	return true;
}


static const dsp_device_ops_t unknown_ops = {
	.destroy = unknown_destroy,
	.reset = unknown_reset,
	.read = unknown_read,
	.write = unknown_write,
};

dsp_device_t *unknown_create(const pmb887x_dsp_peripheral_config_t *config, const DebugChannel *parent) {
	return dsp_device_create(config, parent, &unknown_ops, NULL);
}
