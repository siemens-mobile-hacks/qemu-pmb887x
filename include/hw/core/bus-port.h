/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef HW_CORE_BUS_PORT_H
#define HW_CORE_BUS_PORT_H

#include "hw/core/qdev.h"

/*
 * A named bus port connects one logical device to a bus through a
 * protocol-specific proxy device created by its endpoint callback. The
 * callback returns a new, unrealized device. The port owns the endpoint
 * while connected, and its link property keeps the selected bus alive.
 * The link property can only be set before realize; qdev_bus_port_bind()
 * also rebinds the port of a realized device.
 */
typedef DeviceState *QDevBusPortCreateEndpoint(QDevBusPort *port,
                                               Error **errp);

struct QDevBusPort {
    char *name;
    const char *bus_type;
    BusState *bus;
    DeviceState *endpoint;
    QDevBusPortCreateEndpoint *create_endpoint;
    QLIST_ENTRY(QDevBusPort) node;
};

void qdev_init_bus_port(DeviceState *dev, QDevBusPort *port,
                        const char *name, const char *bus_type,
                        QDevBusPortCreateEndpoint *create_endpoint);
void qdev_connect_bus_ports(DeviceState *dev, Error **errp);
void qdev_disconnect_bus_ports(DeviceState *dev);
void qdev_finalize_bus_ports(DeviceState *dev);

/*
 * Bind the bus port @name of @dev to @bus, or disconnect it when @bus is
 * NULL. A realized device gets a new endpoint on @bus at once. If that
 * endpoint cannot be connected, the port is left disconnected. Do not call
 * it from a callback of the bus the port is bound to: the old endpoint is
 * destroyed while that bus may still be iterating over it.
 */
bool qdev_bus_port_bind(DeviceState *dev, const char *name, BusState *bus,
                        Error **errp);

#endif
