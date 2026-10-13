#include "qemu/osdep.h"
#include "hw/arm/pmb887x/rf.h"
#include "qapi/error.h"

struct pmb887x_iq_bus_t {
	BusState parent_obj;
};

#define TYPE_PMB887X_IQ_ENDPOINT "pmb887x-iq-source-endpoint"
#define PMB887X_IQ_ENDPOINT(obj) OBJECT_CHECK(pmb887x_iq_endpoint_t, (obj), TYPE_PMB887X_IQ_ENDPOINT)
typedef struct pmb887x_iq_endpoint_t {
	DeviceState parent_obj;
	pmb887x_rf_iq_source_t *source;
} pmb887x_iq_endpoint_t;

pmb887x_iq_bus_t *pmb887x_iq_bus_create(DeviceState *parent, const char *name) {
	return PMB887X_IQ_BUS(qbus_new(TYPE_PMB887X_IQ_BUS, parent, name));
}

static DeviceState *iq_source_port_create_endpoint(QDevBusPort *base, Error **_errp) {
	pmb887x_iq_source_port_t *port = container_of(base, pmb887x_iq_source_port_t, parent);
	DeviceState *dev = qdev_new(TYPE_PMB887X_IQ_ENDPOINT);
	pmb887x_iq_endpoint_t *endpoint = PMB887X_IQ_ENDPOINT(dev);
	endpoint->source = port->source;
	return dev;
}

void pmb887x_iq_source_port_init(
	pmb887x_iq_source_port_t *port,
	DeviceState *dev,
	const char *name,
	pmb887x_rf_iq_source_t *source
) {
	port->source = source;
	qdev_init_bus_port(dev, &port->parent, name, TYPE_PMB887X_IQ_BUS, iq_source_port_create_endpoint);
}

bool pmb887x_iq_bus_has_source(const pmb887x_iq_bus_t *bus) {
	return bus != NULL && !QTAILQ_EMPTY(&BUS(bus)->children);
}

pmb887x_iq_sample_t pmb887x_iq_bus_read(pmb887x_iq_bus_t *bus, int64_t timestamp, uint64_t sample_index) {
	if (!pmb887x_iq_bus_has_source(bus))
		return (pmb887x_iq_sample_t) { 0, 0 };

	BusChild *child = QTAILQ_FIRST(&BUS(bus)->children);
	pmb887x_rf_iq_source_t *source = PMB887X_IQ_ENDPOINT(child->child)->source;
	pmb887x_rf_iq_source_class_t *klass = PMB887X_RF_IQ_SOURCE_GET_CLASS(source);
	return klass->read_iq(source, timestamp, sample_index);
}

static bool iq_bus_check_address(BusState *bus, DeviceState *_dev, Error **errp) {
	if (!QTAILQ_EMPTY(&bus->children)) {
		error_setg(errp, "I/Q bus already has a source");
		return false;
	}
	return true;
}

static void iq_bus_class_init(ObjectClass *klass, const void *_data) {
	BusClass *bus_class = BUS_CLASS(klass);
	bus_class->check_address = iq_bus_check_address;
}

static void iq_source_endpoint_class_init(ObjectClass *klass, const void *_data) {
	DeviceClass *device_class = DEVICE_CLASS(klass);
	device_class->bus_type = TYPE_PMB887X_IQ_BUS;
}

static const TypeInfo rf_iq_source_info = {
	.name = TYPE_PMB887X_RF_IQ_SOURCE,
	.parent = TYPE_INTERFACE,
	.class_size = sizeof(pmb887x_rf_iq_source_class_t),
};

static const TypeInfo iq_bus_info = {
	.name = TYPE_PMB887X_IQ_BUS,
	.parent = TYPE_BUS,
	.instance_size = sizeof(pmb887x_iq_bus_t),
	.class_init = iq_bus_class_init,
};

static const TypeInfo iq_source_endpoint_info = {
	.name = TYPE_PMB887X_IQ_ENDPOINT,
	.parent = TYPE_DEVICE,
	.instance_size = sizeof(pmb887x_iq_endpoint_t),
	.class_init = iq_source_endpoint_class_init,
};

static void rf_register_types(void) {
	type_register_static(&rf_iq_source_info);
	type_register_static(&iq_bus_info);
	type_register_static(&iq_source_endpoint_info);
}

type_init(rf_register_types)
