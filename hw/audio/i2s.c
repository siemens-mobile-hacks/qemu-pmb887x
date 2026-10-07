/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/audio/i2s.h"
#include "qapi/error.h"

#define TYPE_I2S_SINK_ENDPOINT "i2s-sink-endpoint"
OBJECT_DECLARE_SIMPLE_TYPE(I2SSinkEndpoint, I2S_SINK_ENDPOINT)
#define TYPE_I2S_SOURCE_ENDPOINT "i2s-source-endpoint"
OBJECT_DECLARE_SIMPLE_TYPE(I2SSourceEndpoint, I2S_SOURCE_ENDPOINT)

struct I2STxBus {
    BusState parent_obj;
    I2SFormat format;
    bool configured;
};

struct I2SRxBus {
    BusState parent_obj;
    I2SFormat format;
    bool configured;
};

struct I2SSinkEndpoint {
    DeviceState parent_obj;
    I2SSink *sink;
};

struct I2SSourceEndpoint {
    DeviceState parent_obj;
    I2SSource *source;
};

I2STxBus *i2s_tx_bus_init(DeviceState *parent, const char *name)
{
    return I2S_TX_BUS(qbus_new(TYPE_I2S_TX_BUS, parent, name));
}

I2SRxBus *i2s_rx_bus_init(DeviceState *parent, const char *name)
{
    return I2S_RX_BUS(qbus_new(TYPE_I2S_RX_BUS, parent, name));
}

static DeviceState *i2s_sink_port_create_endpoint(QDevBusPort *base,
                                                  Error **errp)
{
    I2SSinkPort *port = container_of(base, I2SSinkPort, parent);
    DeviceState *dev = qdev_new(TYPE_I2S_SINK_ENDPOINT);
    I2SSinkEndpoint *endpoint = I2S_SINK_ENDPOINT(dev);

    endpoint->sink = port->sink;
    return dev;
}

void i2s_sink_port_init(I2SSinkPort *port, DeviceState *dev,
                        const char *name, I2SSink *sink)
{
    port->sink = sink;
    qdev_init_bus_port(dev, &port->parent, name, TYPE_I2S_TX_BUS,
                       i2s_sink_port_create_endpoint);
}

static DeviceState *i2s_source_port_create_endpoint(QDevBusPort *base,
                                                    Error **errp)
{
    I2SSourcePort *port = container_of(base, I2SSourcePort, parent);
    DeviceState *dev = qdev_new(TYPE_I2S_SOURCE_ENDPOINT);
    I2SSourceEndpoint *endpoint = I2S_SOURCE_ENDPOINT(dev);

    endpoint->source = port->source;
    return dev;
}

void i2s_source_port_init(I2SSourcePort *port, DeviceState *dev,
                          const char *name, I2SSource *source)
{
    port->source = source;
    qdev_init_bus_port(dev, &port->parent, name, TYPE_I2S_RX_BUS,
                       i2s_source_port_create_endpoint);
}

void i2s_tx_configure(I2STxBus *bus, const I2SFormat *format)
{
    BusChild *child = QTAILQ_FIRST(&BUS(bus)->children);

    bus->format = *format;
    bus->configured = format->sample_rate_hz != 0 && format->channels != 0;
    if (child == NULL) {
        return;
    }

    I2SSink *sink = I2S_SINK_ENDPOINT(child->child)->sink;
    I2SSinkClass *klass = I2S_SINK_GET_CLASS(sink);

    if (klass->configure != NULL) {
        klass->configure(sink, format);
    }
}

void i2s_tx_write(I2STxBus *bus, const int16_t *samples,
                  size_t frame_count)
{
    BusChild *child = QTAILQ_FIRST(&BUS(bus)->children);

    if (!bus->configured || child == NULL) {
        return;
    }

    I2SSink *sink = I2S_SINK_ENDPOINT(child->child)->sink;
    I2SSinkClass *klass = I2S_SINK_GET_CLASS(sink);

    if (klass->write != NULL) {
        klass->write(sink, samples, frame_count);
    }
}

void i2s_rx_configure(I2SRxBus *bus, const I2SFormat *format)
{
    BusChild *child = QTAILQ_FIRST(&BUS(bus)->children);

    bus->format = *format;
    bus->configured = format->sample_rate_hz != 0 && format->channels != 0;
    if (child == NULL) {
        return;
    }

    I2SSource *source = I2S_SOURCE_ENDPOINT(child->child)->source;
    I2SSourceClass *klass = I2S_SOURCE_GET_CLASS(source);

    if (klass->configure != NULL) {
        klass->configure(source, format);
    }
}

size_t i2s_rx_read(I2SRxBus *bus, int16_t *samples,
                   size_t frame_capacity)
{
    BusChild *child = QTAILQ_FIRST(&BUS(bus)->children);

    if (!bus->configured || child == NULL) {
        return 0;
    }

    I2SSource *source = I2S_SOURCE_ENDPOINT(child->child)->source;
    I2SSourceClass *klass = I2S_SOURCE_GET_CLASS(source);

    if (klass->read == NULL) {
        return 0;
    }

    return klass->read(source, samples, frame_capacity);
}

static bool i2s_bus_check_address(BusState *bus, DeviceState *dev,
                                  Error **errp)
{
    if (!QTAILQ_EMPTY(&bus->children)) {
        error_setg(errp, "I2S bus already has an endpoint");
        return false;
    }

    return true;
}

static void i2s_bus_class_init(ObjectClass *klass, const void *data)
{
    BusClass *bus_class = BUS_CLASS(klass);

    bus_class->check_address = i2s_bus_check_address;
}

static void i2s_sink_endpoint_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *device_class = DEVICE_CLASS(klass);

    device_class->bus_type = TYPE_I2S_TX_BUS;
}

static void i2s_source_endpoint_class_init(ObjectClass *klass,
                                           const void *data)
{
    DeviceClass *device_class = DEVICE_CLASS(klass);

    device_class->bus_type = TYPE_I2S_RX_BUS;
}

static const TypeInfo i2s_tx_bus_info = {
    .name = TYPE_I2S_TX_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(I2STxBus),
    .class_init = i2s_bus_class_init,
};

static const TypeInfo i2s_rx_bus_info = {
    .name = TYPE_I2S_RX_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(I2SRxBus),
    .class_init = i2s_bus_class_init,
};

static const TypeInfo i2s_sink_endpoint_info = {
    .name = TYPE_I2S_SINK_ENDPOINT,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(I2SSinkEndpoint),
    .class_init = i2s_sink_endpoint_class_init,
};

static const TypeInfo i2s_source_endpoint_info = {
    .name = TYPE_I2S_SOURCE_ENDPOINT,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(I2SSourceEndpoint),
    .class_init = i2s_source_endpoint_class_init,
};

static const TypeInfo i2s_sink_info = {
    .name = TYPE_I2S_SINK,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(I2SSinkClass),
};

static const TypeInfo i2s_source_info = {
    .name = TYPE_I2S_SOURCE,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(I2SSourceClass),
};

static void i2s_register_types(void)
{
    type_register_static(&i2s_tx_bus_info);
    type_register_static(&i2s_rx_bus_info);
    type_register_static(&i2s_sink_endpoint_info);
    type_register_static(&i2s_source_endpoint_info);
    type_register_static(&i2s_sink_info);
    type_register_static(&i2s_source_info);
}

type_init(i2s_register_types)
