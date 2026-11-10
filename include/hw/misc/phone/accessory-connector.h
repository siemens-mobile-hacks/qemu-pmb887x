/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_PHONE_ACCESSORY_CONNECTOR_H
#define HW_MISC_PHONE_ACCESSORY_CONNECTOR_H

#include "hw/core/qdev.h"

/*
 * A connector that one of several accessories is plugged into. Each
 * accessory profile drives the connector GPIO outputs (to constant levels
 * or to follow an input) and binds bus ports of other devices to buses.
 * An output a profile does not name is driven low, and a port it does not
 * name is disconnected: the connector owns every port its profiles name.
 * GPIO inputs "in" and outputs "out" exist once the profiles are set.
 * Selecting another profile at run time first applies the "unplugged"
 * profile for "replug-delay-ms" of virtual time.
 */
#define TYPE_ACCESSORY_CONNECTOR "accessory-connector"
OBJECT_DECLARE_SIMPLE_TYPE(AccessoryConnector, ACCESSORY_CONNECTOR)

typedef struct AccessoryGpioRoute {
    unsigned output;
    bool from_input;
    unsigned input;
    bool level;          /* constant level, or inversion of the input */
} AccessoryGpioRoute;

typedef struct AccessoryPortRoute {
    DeviceState *device;
    const char *port;
    BusState *bus;
} AccessoryPortRoute;

typedef struct AccessoryProfile {
    const char *name;
    const AccessoryGpioRoute *gpio;
    size_t gpio_count;
    const AccessoryPortRoute *ports;
    size_t port_count;
} AccessoryProfile;

/* Before realize; the connector keeps its own copy. */
void accessory_connector_set_profiles(AccessoryConnector *s,
                                      const AccessoryProfile *profiles,
                                      size_t count);

#endif
