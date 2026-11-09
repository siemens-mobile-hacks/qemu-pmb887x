/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "hw/core/bus-port.h"
#include "hw/core/qdev.h"
#include "qom/object.h"

#define TYPE_IQ_BUS "iq-bus"
#define TYPE_IQ_SOURCE "iq-source"

typedef struct IQSample IQSample;
typedef struct IQSource IQSource;
typedef struct IQSourceClass IQSourceClass;
typedef struct IQSourcePort IQSourcePort;

OBJECT_DECLARE_SIMPLE_TYPE(IQBus, IQ_BUS)

#define IQ_SOURCE(obj) INTERFACE_CHECK(IQSource, (obj), TYPE_IQ_SOURCE)
DECLARE_CLASS_CHECKERS(IQSourceClass, IQ_SOURCE, TYPE_IQ_SOURCE)

struct IQSample {
    int16_t i;
    int16_t q;
};

struct IQSourceClass {
    InterfaceClass parent_class;
    IQSample (*read)(IQSource *source, int64_t timestamp, uint64_t sample_index);
};

struct IQSourcePort {
    QDevBusPort parent;
    IQSource *source;
};

void iq_source_port_init(IQSourcePort *port, DeviceState *dev, const char *name, IQSource *source);

bool iq_bus_has_source(const IQBus *bus);
/* timestamp is the virtual time of the requested sample. */
IQSample iq_bus_read(IQBus *bus, int64_t timestamp, uint64_t sample_index);
