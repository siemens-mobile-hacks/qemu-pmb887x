/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_AUDIO_I2S_H
#define HW_AUDIO_I2S_H

#include "hw/core/bus-port.h"
#include "qom/object.h"

/* TX and RX are named relative to the device that owns the bus. */
#define TYPE_I2S_TX_BUS "i2s-tx-bus"
OBJECT_DECLARE_SIMPLE_TYPE(I2STxBus, I2S_TX_BUS)
#define TYPE_I2S_RX_BUS "i2s-rx-bus"
OBJECT_DECLARE_SIMPLE_TYPE(I2SRxBus, I2S_RX_BUS)
#define TYPE_I2S_SINK "i2s-sink"
#define TYPE_I2S_SOURCE "i2s-source"

typedef struct I2SSink I2SSink;
typedef struct I2SSinkClass I2SSinkClass;
typedef struct I2SSinkPort I2SSinkPort;
typedef struct I2SFormat I2SFormat;
typedef struct I2SSource I2SSource;
typedef struct I2SSourceClass I2SSourceClass;
typedef struct I2SSourcePort I2SSourcePort;

DECLARE_CLASS_CHECKERS(I2SSinkClass, I2S_SINK, TYPE_I2S_SINK)
#define I2S_SINK(obj) INTERFACE_CHECK(I2SSink, (obj), TYPE_I2S_SINK)
DECLARE_CLASS_CHECKERS(I2SSourceClass, I2S_SOURCE, TYPE_I2S_SOURCE)
#define I2S_SOURCE(obj) INTERFACE_CHECK(I2SSource, (obj), TYPE_I2S_SOURCE)

struct I2SFormat {
    uint32_t sample_rate_hz;
    uint32_t channels;
};

struct I2SSinkClass {
    InterfaceClass parent_class;
    void (*configure)(I2SSink *sink, const I2SFormat *format);
    void (*write)(I2SSink *sink, const int16_t *samples, size_t frame_count);
};

struct I2SSourceClass {
    InterfaceClass parent_class;
    void (*configure)(I2SSource *source, const I2SFormat *format);
    /* Return the number of frames stored; zero means no frame is available. */
    size_t (*read)(I2SSource *source, int16_t *samples,
                   size_t frame_capacity);
};

struct I2SSinkPort {
    QDevBusPort parent;
    I2SSink *sink;
};

struct I2SSourcePort {
    QDevBusPort parent;
    I2SSource *source;
};

I2STxBus *i2s_tx_bus_init(DeviceState *parent, const char *name);
I2SRxBus *i2s_rx_bus_init(DeviceState *parent, const char *name);
void i2s_sink_port_init(I2SSinkPort *port, DeviceState *dev,
                        const char *name, I2SSink *sink);
void i2s_source_port_init(I2SSourcePort *port, DeviceState *dev,
                          const char *name, I2SSource *source);
void i2s_tx_configure(I2STxBus *bus, const I2SFormat *format);
void i2s_tx_write(I2STxBus *bus, const int16_t *samples, size_t frame_count);
void i2s_rx_configure(I2SRxBus *bus, const I2SFormat *format);
size_t i2s_rx_read(I2SRxBus *bus, int16_t *samples,
                   size_t frame_capacity);

#endif
