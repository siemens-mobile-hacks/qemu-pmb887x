/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef HW_CORE_BUS_PORT_H
#define HW_CORE_BUS_PORT_H

#include "hw/core/qdev.h"

/*
 * A named bus port connects one logical device to a bus through a
 * protocol-specific proxy device created by its endpoint callback. The
 * callback returns a new, unrealized device. The port owns the endpoint
 * while connected, and its link property keeps the selected bus alive.
 */
typedef DeviceState *QDevBusPortCreateEndpoint(QDevBusPort *port,
                                               Error **errp);

struct QDevBusPort {
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

#endif
