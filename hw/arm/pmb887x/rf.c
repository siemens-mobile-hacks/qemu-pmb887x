#include "qemu/osdep.h"
#include "hw/arm/pmb887x/rf.h"
#include "qapi/error.h"

struct pmb887x_rfssc_bus_t {
	BusState parent_obj;
};

void pmb887x_rfssc_transfer(pmb887x_rfssc_bus_t *bus, uint32_t value, uint8_t bits) {
	BusChild *child = QTAILQ_FIRST(&BUS(bus)->children);

	if (child == NULL)
		return;

	pmb887x_rfssc_device_t *device = PMB887X_RFSSC_DEVICE(child->child);
	pmb887x_rfssc_device_class_t *klass = PMB887X_RFSSC_DEVICE_GET_CLASS(device);

	klass->transfer(device, value, bits);
}

static bool rfssc_bus_check_address(BusState *bus, DeviceState *device, Error **errp) {
	(void) device;

	if (!QTAILQ_EMPTY(&bus->children)) {
		error_setg(errp, "RFSSC bus already has a device");
		return false;
	}

	return true;
}

static void rfssc_bus_class_init(ObjectClass *klass, const void *data) {
	BusClass *bus_class = BUS_CLASS(klass);

	(void) data;

	bus_class->check_address = rfssc_bus_check_address;
}

static void rfssc_device_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *device_class = DEVICE_CLASS(klass);

	(void) data;

	device_class->bus_type = TYPE_PMB887X_RFSSC_BUS;
}

pmb887x_rfssc_bus_t *pmb887x_rfssc_create_bus(DeviceState *parent, const char *name) {
	return PMB887X_RFSSC_BUS(qbus_new(TYPE_PMB887X_RFSSC_BUS, parent, name));
}

static const TypeInfo rfssc_bus_info = {
	.name = TYPE_PMB887X_RFSSC_BUS,
	.parent = TYPE_BUS,
	.instance_size = sizeof(pmb887x_rfssc_bus_t),
	.class_init = rfssc_bus_class_init,
};

static const TypeInfo rfssc_device_info = {
	.name = TYPE_PMB887X_RFSSC_DEVICE,
	.parent = TYPE_DEVICE,
	.instance_size = sizeof(pmb887x_rfssc_device_t),
	.class_size = sizeof(pmb887x_rfssc_device_class_t),
	.class_init = rfssc_device_class_init,
	.abstract = true,
};

static const TypeInfo rf_iq_source_info = {
	.name = TYPE_PMB887X_RF_IQ_SOURCE,
	.parent = TYPE_INTERFACE,
	.class_size = sizeof(pmb887x_rf_iq_source_class_t),
};

static void rf_register_types(void) {
	type_register_static(&rfssc_bus_info);
	type_register_static(&rfssc_device_info);
	type_register_static(&rf_iq_source_info);
}

type_init(rf_register_types)
