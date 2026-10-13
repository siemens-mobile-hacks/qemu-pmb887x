/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SSI_THREE_WIRE_SERIAL_H
#define HW_SSI_THREE_WIRE_SERIAL_H

#include "hw/core/bus-port.h"
#include "qom/object.h"

/*
 * Transmit-only synchronous serial control bus with clock, data, and strobe
 * signals.  Strobe polarity and clock timing belong to the controller; this
 * interface exposes logical strobe state and the resulting wire-order bit
 * stream.
 */
#define TYPE_THREE_WIRE_SERIAL_BUS "three-wire-serial-bus"
OBJECT_DECLARE_SIMPLE_TYPE(ThreeWireSerialBus, THREE_WIRE_SERIAL_BUS)

#define TYPE_THREE_WIRE_SERIAL_TARGET "three-wire-serial-target"
typedef struct ThreeWireSerialTarget ThreeWireSerialTarget;
typedef struct ThreeWireSerialTargetClass ThreeWireSerialTargetClass;
typedef struct ThreeWireSerialTargetPort ThreeWireSerialTargetPort;

DECLARE_CLASS_CHECKERS(ThreeWireSerialTargetClass, THREE_WIRE_SERIAL_TARGET,
                       TYPE_THREE_WIRE_SERIAL_TARGET)
#define THREE_WIRE_SERIAL_TARGET(obj) \
    INTERFACE_CHECK(ThreeWireSerialTarget, (obj), \
                    TYPE_THREE_WIRE_SERIAL_TARGET)

struct ThreeWireSerialTargetClass {
    InterfaceClass parent_class;

    void (*set_strobe)(ThreeWireSerialTarget *target, bool asserted);
    /* Of the low @bits bits, bit @bits - 1 is first on the wire. */
    void (*write)(ThreeWireSerialTarget *target, uint32_t value,
                  unsigned int bits);
};

struct ThreeWireSerialTargetPort {
    QDevBusPort parent;
    ThreeWireSerialTarget *target;
};

ThreeWireSerialBus *three_wire_serial_bus_init(DeviceState *parent,
                                                const char *name);
void three_wire_serial_target_port_init(ThreeWireSerialTargetPort *port,
                                        DeviceState *dev, const char *name,
                                        ThreeWireSerialTarget *target);
void three_wire_serial_set_strobe(ThreeWireSerialBus *bus, bool asserted);
void three_wire_serial_write(ThreeWireSerialBus *bus, uint32_t value,
                             unsigned int bits);

#endif
