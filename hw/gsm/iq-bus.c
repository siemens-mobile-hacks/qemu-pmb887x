/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/gsm/iq-bus.h"

#define TYPE_IQ_SOURCE_ENDPOINT "iq-source-endpoint"

OBJECT_DECLARE_SIMPLE_TYPE(IQSourceEndpoint, IQ_SOURCE_ENDPOINT)

struct IQBus {
    BusState parent_obj;
};

struct IQSourceEndpoint {
    DeviceState parent_obj;
    IQSource *source;
    IQSourceClass *source_class;
};

static DeviceState *iq_source_port_create_endpoint(QDevBusPort *base, Error **_errp) {
    IQSourcePort *port = container_of(base, IQSourcePort, parent);
    DeviceState *dev = qdev_new(TYPE_IQ_SOURCE_ENDPOINT);
    IQSourceEndpoint *endpoint = IQ_SOURCE_ENDPOINT(dev);
    endpoint->source = port->source;
    endpoint->source_class = IQ_SOURCE_GET_CLASS(port->source);
    return dev;
}

void iq_source_port_init(IQSourcePort *port, DeviceState *dev, const char *name, IQSource *source) {
    port->source = source;
    qdev_init_bus_port(dev, &port->parent, name, TYPE_IQ_BUS, iq_source_port_create_endpoint);
}

bool iq_bus_has_source(const IQBus *bus) {
    return bus != NULL && !QTAILQ_EMPTY(&bus->parent_obj.children);
}

static IQSourceEndpoint *iq_bus_get_endpoint(IQBus *bus) {
    BusChild *child = QTAILQ_FIRST(&bus->parent_obj.children);
    return container_of(child->child, IQSourceEndpoint, parent_obj);
}

IQSample iq_bus_read(IQBus *bus, int64_t timestamp, uint64_t sample_index) {
    if (!iq_bus_has_source(bus))
        return (IQSample) { 0, 0 };

    IQSourceEndpoint *endpoint = iq_bus_get_endpoint(bus);
    return endpoint->source_class->read(endpoint->source, timestamp, sample_index);
}

void iq_bus_transmit(IQBus *bus, int64_t timestamp, bool symbol) {
    if (!iq_bus_has_source(bus))
        return;

    IQSourceEndpoint *endpoint = iq_bus_get_endpoint(bus);
    if (endpoint->source_class->transmit != NULL)
        endpoint->source_class->transmit(endpoint->source, timestamp, symbol);
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
    device_class->bus_type = TYPE_IQ_BUS;
}

static const TypeInfo iq_source_info = {
    .name           = TYPE_IQ_SOURCE,
    .parent         = TYPE_INTERFACE,
    .class_size     = sizeof(IQSourceClass),
};

static const TypeInfo iq_bus_info = {
    .name           = TYPE_IQ_BUS,
    .parent         = TYPE_BUS,
    .instance_size  = sizeof(IQBus),
    .class_init     = iq_bus_class_init,
};

static const TypeInfo iq_source_endpoint_info = {
    .name           = TYPE_IQ_SOURCE_ENDPOINT,
    .parent         = TYPE_DEVICE,
    .instance_size  = sizeof(IQSourceEndpoint),
    .class_init     = iq_source_endpoint_class_init,
};

static void iq_bus_register_types(void) {
    type_register_static(&iq_source_info);
    type_register_static(&iq_bus_info);
    type_register_static(&iq_source_endpoint_info);
}

type_init(iq_bus_register_types)
