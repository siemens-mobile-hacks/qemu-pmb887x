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
    port->create_endpoint = create_endpoint;
    QLIST_INSERT_HEAD(&dev->bus_ports, port, node);

    object_property_add_link(OBJECT(dev), property_name, bus_type,
                             (Object **)&port->bus,
                             qdev_prop_allow_set_link_before_realize,
                             OBJ_PROP_LINK_STRONG);
}

void qdev_connect_bus_ports(DeviceState *dev, Error **errp)
{
    QDevBusPort *port;

    ERRP_GUARD();

    QLIST_FOREACH(port, &dev->bus_ports, node) {
        DeviceState *endpoint;

        if (!port->bus || port->endpoint) {
            continue;
        }

        endpoint = port->create_endpoint(port, errp);
        if (*errp) {
            if (endpoint) {
                object_unref(OBJECT(endpoint));
            }
            qdev_disconnect_bus_ports(dev);
            return;
        }
        assert(endpoint);
        if (!qdev_realize(endpoint, port->bus, errp)) {
            if (endpoint->parent_bus) {
                object_unparent(OBJECT(endpoint));
            }
            object_unref(OBJECT(endpoint));
            qdev_disconnect_bus_ports(dev);
            return;
        }
        port->endpoint = endpoint;
    }
}

void qdev_disconnect_bus_ports(DeviceState *dev)
{
    QDevBusPort *port;

    QLIST_FOREACH(port, &dev->bus_ports, node) {
        DeviceState *endpoint = port->endpoint;

        if (!endpoint) {
            continue;
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
}
