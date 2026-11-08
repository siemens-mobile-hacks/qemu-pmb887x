/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"

#include "hw/core/bus-port.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"

void qdev_init_bus_port(DeviceState *dev, QDevBusPort *port,
                        const char *name, const char *bus_type,
                        QDevBusPortCreateEndpoint *create_endpoint)
{
    g_autofree char *property_name = g_strdup_printf("port.%s", name);

    assert(create_endpoint);
    port->dev = dev;
    port->name = g_strdup(name);
    port->bus_type = bus_type;
    port->create_endpoint = create_endpoint;
    QLIST_INSERT_HEAD(&dev->bus_ports, port, node);

    object_property_add_link(OBJECT(dev), property_name, bus_type,
                             (Object **)&port->bus,
                             qdev_prop_allow_set_link_before_realize,
                             OBJ_PROP_LINK_STRONG);
}

static bool bus_port_connect(QDevBusPort *port, Error **errp)
{
    g_autofree char *child_name = g_strdup_printf("port.%s.endpoint",
                                                  port->name);
    DeviceState *endpoint;

    ERRP_GUARD();

    endpoint = port->create_endpoint(port, errp);
    if (*errp) {
        if (endpoint) {
            object_unref(OBJECT(endpoint));
        }
        return false;
    }
    assert(endpoint);
    object_property_add_child(OBJECT(port->dev), child_name, OBJECT(endpoint));
    if (!qdev_realize(endpoint, port->bus, errp)) {
        object_unparent(OBJECT(endpoint));
        object_unref(OBJECT(endpoint));
        return false;
    }
    port->endpoint = endpoint;
    return true;
}

static void bus_port_disconnect(QDevBusPort *port)
{
    DeviceState *endpoint = port->endpoint;

    if (!endpoint) {
        return;
    }

    port->endpoint = NULL;
    if (endpoint->realized) {
        qdev_unrealize(endpoint);
    }
    if (OBJECT(endpoint)->parent) {
        object_unparent(OBJECT(endpoint));
    }
    object_unref(OBJECT(endpoint));
}

void qdev_connect_bus_ports(DeviceState *dev, Error **errp)
{
    QDevBusPort *port;

    QLIST_FOREACH(port, &dev->bus_ports, node) {
        if (!port->bus || port->endpoint) {
            continue;
        }
        if (!bus_port_connect(port, errp)) {
            qdev_disconnect_bus_ports(dev);
            return;
        }
    }
}

void qdev_disconnect_bus_ports(DeviceState *dev)
{
    QDevBusPort *port;

    QLIST_FOREACH(port, &dev->bus_ports, node) {
        bus_port_disconnect(port);
    }
}

void qdev_finalize_bus_ports(DeviceState *dev)
{
    QDevBusPort *port;

    QLIST_FOREACH(port, &dev->bus_ports, node) {
        g_free(port->name);
    }
}

static QDevBusPort *bus_port_find(DeviceState *dev, const char *name)
{
    QDevBusPort *port;

    QLIST_FOREACH(port, &dev->bus_ports, node) {
        if (strcmp(port->name, name) == 0) {
            return port;
        }
    }
    return NULL;
}

static void bus_port_set_bus(QDevBusPort *port, BusState *bus)
{
    BusState *old_bus = port->bus;

    if (bus) {
        object_ref(OBJECT(bus));
    }
    port->bus = bus;
    if (old_bus) {
        object_unref(OBJECT(old_bus));
    }
}

static QDevBusPort *bus_port_find_compatible(DeviceState *dev,
                                             const char *name, BusState *bus,
                                             Error **errp)
{
    QDevBusPort *port = bus_port_find(dev, name);

    if (!port) {
        error_setg(errp, "Device '%s' has no bus port '%s'",
                   object_get_typename(OBJECT(dev)), name);
        return NULL;
    }
    if (bus && !object_dynamic_cast(OBJECT(bus), port->bus_type)) {
        error_setg(errp, "Bus port '%s' needs a '%s' bus, got '%s'",
                   name, port->bus_type, object_get_typename(OBJECT(bus)));
        return NULL;
    }
    return port;
}

bool qdev_bus_port_check(DeviceState *dev, const char *name, BusState *bus,
                         Error **errp)
{
    return bus_port_find_compatible(dev, name, bus, errp) != NULL;
}

bool qdev_bus_port_bind(DeviceState *dev, const char *name, BusState *bus,
                        Error **errp)
{
    QDevBusPort *port = bus_port_find_compatible(dev, name, bus, errp);

    if (!port) {
        return false;
    }
    if (port->bus == bus) {
        return true;
    }

    bus_port_disconnect(port);
    bus_port_set_bus(port, bus);
    if (!bus || !dev->realized) {
        return true;
    }
    if (!bus_port_connect(port, errp)) {
        bus_port_set_bus(port, NULL);
        return false;
    }
    return true;
}
