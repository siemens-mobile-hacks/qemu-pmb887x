/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/ssi/three-wire-serial.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#define TYPE_THREE_WIRE_SERIAL_ENDPOINT "three-wire-serial-endpoint"
OBJECT_DECLARE_SIMPLE_TYPE(ThreeWireSerialEndpoint,
                           THREE_WIRE_SERIAL_ENDPOINT)

struct ThreeWireSerialBus {
    BusState parent_obj;
};

struct ThreeWireSerialEndpoint {
    DeviceState parent_obj;
    ThreeWireSerialTarget *target;
};

ThreeWireSerialBus *three_wire_serial_bus_init(DeviceState *parent,
                                                const char *name)
{
    return THREE_WIRE_SERIAL_BUS(qbus_new(TYPE_THREE_WIRE_SERIAL_BUS,
                                          parent, name));
}

static DeviceState *three_wire_serial_target_port_create_endpoint(
    QDevBusPort *base, Error **_errp)
{
    ThreeWireSerialTargetPort *port =
        container_of(base, ThreeWireSerialTargetPort, parent);
    DeviceState *dev = qdev_new(TYPE_THREE_WIRE_SERIAL_ENDPOINT);
    ThreeWireSerialEndpoint *endpoint = THREE_WIRE_SERIAL_ENDPOINT(dev);

    endpoint->target = port->target;
    return dev;
}

void three_wire_serial_target_port_init(ThreeWireSerialTargetPort *port,
                                        DeviceState *dev, const char *name,
                                        ThreeWireSerialTarget *target)
{
    port->target = target;
    qdev_init_bus_port(dev, &port->parent, name, TYPE_THREE_WIRE_SERIAL_BUS,
                       three_wire_serial_target_port_create_endpoint);
}

void three_wire_serial_set_strobe(ThreeWireSerialBus *bus, bool asserted)
{
    BusChild *child;

    QTAILQ_FOREACH(child, &BUS(bus)->children, sibling) {
        ThreeWireSerialTarget *target =
            THREE_WIRE_SERIAL_ENDPOINT(child->child)->target;
        ThreeWireSerialTargetClass *klass =
            THREE_WIRE_SERIAL_TARGET_GET_CLASS(target);

        if (klass->set_strobe != NULL) {
            klass->set_strobe(target, asserted);
        }
    }
}

void three_wire_serial_write(ThreeWireSerialBus *bus, uint32_t value,
                             unsigned int bits)
{
    BusChild *child;

    assert(bits >= 1 && bits <= 32);
    value &= MAKE_64BIT_MASK(0, bits);

    QTAILQ_FOREACH(child, &BUS(bus)->children, sibling) {
        ThreeWireSerialTarget *target =
            THREE_WIRE_SERIAL_ENDPOINT(child->child)->target;
        ThreeWireSerialTargetClass *klass =
            THREE_WIRE_SERIAL_TARGET_GET_CLASS(target);

        if (klass->write != NULL) {
            klass->write(target, value, bits);
        }
    }
}

static void three_wire_serial_endpoint_class_init(ObjectClass *klass,
                                                  const void *_data)
{
    DeviceClass *device_class = DEVICE_CLASS(klass);

    device_class->bus_type = TYPE_THREE_WIRE_SERIAL_BUS;
}

static const TypeInfo three_wire_serial_bus_info = {
    .name = TYPE_THREE_WIRE_SERIAL_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(ThreeWireSerialBus),
};

static const TypeInfo three_wire_serial_target_info = {
    .name = TYPE_THREE_WIRE_SERIAL_TARGET,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(ThreeWireSerialTargetClass),
};

static const TypeInfo three_wire_serial_endpoint_info = {
    .name = TYPE_THREE_WIRE_SERIAL_ENDPOINT,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(ThreeWireSerialEndpoint),
    .class_init = three_wire_serial_endpoint_class_init,
};

static void three_wire_serial_register_types(void)
{
    type_register_static(&three_wire_serial_bus_info);
    type_register_static(&three_wire_serial_target_info);
    type_register_static(&three_wire_serial_endpoint_info);
}

type_init(three_wire_serial_register_types)
