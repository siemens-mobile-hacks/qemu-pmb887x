/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "migration/vmstate.h"
#include "hw/audio/i2s.h"
#include "hw/core/bus-port.h"
#include "hw/i2c/i2c.h"
#include "hw/ssi/three-wire-serial.h"
#include "qapi/error.h"
#include "qemu/module.h"

#define TYPE_BUS_PORT_TEST_BUS "bus-port-test-bus"
#define TYPE_BUS_PORT_TEST_ENDPOINT "bus-port-test-endpoint"
#define TYPE_BUS_PORT_TEST_DEVICE "bus-port-test-device"
#define TYPE_PROTOCOL_TEST_DEVICE "protocol-test-device"
#define TYPE_EMPTY_I2C_TARGET "empty-i2c-target"
#define TYPE_TEST_BUS_OWNER "test-bus-owner"

const VMStateInfo vmstate_info_uint8;

OBJECT_DECLARE_SIMPLE_TYPE(BusPortTestDevice, BUS_PORT_TEST_DEVICE)
OBJECT_DECLARE_SIMPLE_TYPE(ProtocolTestDevice, PROTOCOL_TEST_DEVICE)
OBJECT_DECLARE_SIMPLE_TYPE(EmptyI2CTarget, EMPTY_I2C_TARGET)

struct BusPortTestDevice {
    DeviceState parent_obj;
    QDevBusPort failing_port;
    QDevBusPort port;
};

struct ProtocolTestDevice {
    DeviceState parent_obj;
    I2CTargetPort i2c_port;
    I2SSinkPort i2s_sink_port;
    I2SSourcePort i2s_source_port;
    ThreeWireSerialTargetPort serial_port;
    uint32_t i2c_address;
    enum i2c_event i2c_event;
    uint8_t i2c_sent;
    uint8_t i2c_receive;
    uint32_t i2s_sample_rate_hz;
    uint32_t i2s_channels;
    size_t i2s_configure_count;
    int16_t i2s_sink_samples[2];
    int16_t i2s_source_samples[2];
    size_t i2s_frames;
    bool serial_strobe_asserted;
    uint32_t serial_value[2];
    unsigned int serial_bits[2];
    size_t serial_write_count;
};

struct EmptyI2CTarget {
    DeviceState parent_obj;
    I2CTargetPort port;
    uint32_t address;
};

static unsigned test_sequence;
static unsigned endpoint_unrealize_sequence;
static unsigned device_unrealize_sequence;
static bool endpoint_realize_fails;

static bool test_bus_check_address(BusState *bus, DeviceState *dev,
                                   Error **errp)
{
    if (!QTAILQ_EMPTY(&bus->children)) {
        error_setg(errp, "test bus already has an endpoint");
        return false;
    }
    return true;
}

static void test_bus_class_init(ObjectClass *oc, const void *data)
{
    BusClass *bc = BUS_CLASS(oc);

    bc->check_address = test_bus_check_address;
}

static void test_endpoint_unrealize(DeviceState *dev)
{
    endpoint_unrealize_sequence = ++test_sequence;
}

static void test_endpoint_realize(DeviceState *dev, Error **errp)
{
    if (endpoint_realize_fails) {
        error_setg(errp, "requested endpoint realization failure");
    }
}

static void test_endpoint_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->bus_type = TYPE_BUS_PORT_TEST_BUS;
    dc->realize = test_endpoint_realize;
    dc->unrealize = test_endpoint_unrealize;
}

static DeviceState *test_port_create_endpoint(QDevBusPort *port, Error **errp)
{
    return qdev_new(TYPE_BUS_PORT_TEST_ENDPOINT);
}

static DeviceState *test_port_fail(QDevBusPort *port, Error **errp)
{
    error_setg(errp, "requested port connection failure");
    return NULL;
}

static void test_device_init(Object *obj)
{
    BusPortTestDevice *dev = BUS_PORT_TEST_DEVICE(obj);

    qdev_init_bus_port(DEVICE(dev), &dev->failing_port, "FAIL",
                       TYPE_BUS_PORT_TEST_BUS, test_port_fail);
    qdev_init_bus_port(DEVICE(dev), &dev->port, "TEST",
                       TYPE_BUS_PORT_TEST_BUS, test_port_create_endpoint);
}

static void test_device_unrealize(DeviceState *dev)
{
    device_unrealize_sequence = ++test_sequence;
}

static void test_device_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->unrealize = test_device_unrealize;
}

static int protocol_i2c_event(I2CTarget *target, enum i2c_event event)
{
    ProtocolTestDevice *dev = PROTOCOL_TEST_DEVICE(OBJECT(target));

    dev->i2c_event = event;
    return 0;
}

static int protocol_i2c_send(I2CTarget *target, uint8_t data)
{
    ProtocolTestDevice *dev = PROTOCOL_TEST_DEVICE(OBJECT(target));

    dev->i2c_sent = data;
    return 0;
}

static uint8_t protocol_i2c_recv(I2CTarget *target)
{
    return PROTOCOL_TEST_DEVICE(OBJECT(target))->i2c_receive;
}

static void protocol_i2s_configure(DeviceState *device,
                                   const I2SFormat *format)
{
    ProtocolTestDevice *dev = PROTOCOL_TEST_DEVICE(device);

    dev->i2s_sample_rate_hz = format->sample_rate_hz;
    dev->i2s_channels = format->channels;
    dev->i2s_configure_count++;
}

static void protocol_i2s_sink_configure(I2SSink *sink,
                                        const I2SFormat *format)
{
    protocol_i2s_configure(DEVICE(sink), format);
}

static void protocol_i2s_source_configure(I2SSource *source,
                                          const I2SFormat *format)
{
    protocol_i2s_configure(DEVICE(source), format);
}

static void protocol_i2s_write(I2SSink *sink, const int16_t *samples,
                               size_t frame_count)
{
    ProtocolTestDevice *dev = PROTOCOL_TEST_DEVICE(OBJECT(sink));

    dev->i2s_frames = frame_count;
    memcpy(dev->i2s_sink_samples, samples,
           dev->i2s_channels * sizeof(*samples));
}

static size_t protocol_i2s_read(I2SSource *source, int16_t *samples,
                                size_t frame_capacity)
{
    ProtocolTestDevice *dev = PROTOCOL_TEST_DEVICE(OBJECT(source));
    size_t frames = MIN(frame_capacity, (size_t) 1);

    memcpy(samples, dev->i2s_source_samples,
           frames * dev->i2s_channels * sizeof(*samples));
    return frames;
}

static void protocol_serial_set_strobe(ThreeWireSerialTarget *target,
                                       bool asserted)
{
    ProtocolTestDevice *dev = PROTOCOL_TEST_DEVICE(OBJECT(target));

    dev->serial_strobe_asserted = asserted;
}

static void protocol_serial_write(ThreeWireSerialTarget *target,
                                  uint32_t value, unsigned int bits)
{
    ProtocolTestDevice *dev = PROTOCOL_TEST_DEVICE(OBJECT(target));
    size_t index = dev->serial_write_count++;

    g_assert_cmpuint(index, <, ARRAY_SIZE(dev->serial_value));
    dev->serial_value[index] = value;
    dev->serial_bits[index] = bits;
}

static void protocol_test_device_init(Object *obj)
{
    ProtocolTestDevice *dev = PROTOCOL_TEST_DEVICE(obj);

    dev->i2c_address = 0x31;
    i2c_target_port_init(&dev->i2c_port, DEVICE(dev), "I2C",
                         I2C_TARGET(dev), &dev->i2c_address);
    i2s_sink_port_init(&dev->i2s_sink_port, DEVICE(dev), "I2S_RX",
                       I2S_SINK(dev));
    i2s_source_port_init(&dev->i2s_source_port, DEVICE(dev), "I2S_TX",
                         I2S_SOURCE(dev));
    three_wire_serial_target_port_init(&dev->serial_port, DEVICE(dev),
                                       "SERIAL", THREE_WIRE_SERIAL_TARGET(dev));
}

static void protocol_test_device_class_init(ObjectClass *oc, const void *_data)
{
    I2CTargetClass *tc = I2C_TARGET_CLASS(oc);
    I2SSinkClass *sc = I2S_SINK_CLASS(oc);
    I2SSourceClass *source_class = I2S_SOURCE_CLASS(oc);
    ThreeWireSerialTargetClass *serial_class =
        THREE_WIRE_SERIAL_TARGET_CLASS(oc);

    tc->event = protocol_i2c_event;
    tc->send = protocol_i2c_send;
    tc->recv = protocol_i2c_recv;
    sc->configure = protocol_i2s_sink_configure;
    sc->write = protocol_i2s_write;
    source_class->configure = protocol_i2s_source_configure;
    source_class->read = protocol_i2s_read;
    serial_class->set_strobe = protocol_serial_set_strobe;
    serial_class->write = protocol_serial_write;
}

static void empty_i2c_target_init(Object *obj)
{
    EmptyI2CTarget *target = EMPTY_I2C_TARGET(obj);

    target->address = 0x32;
    i2c_target_port_init(&target->port, DEVICE(target), "I2C",
                         I2C_TARGET(target), &target->address);
}

static const TypeInfo test_bus_info = {
    .name = TYPE_BUS_PORT_TEST_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(BusState),
    .class_init = test_bus_class_init,
};

static const TypeInfo test_endpoint_info = {
    .name = TYPE_BUS_PORT_TEST_ENDPOINT,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(DeviceState),
    .class_init = test_endpoint_class_init,
};

static const TypeInfo test_device_info = {
    .name = TYPE_BUS_PORT_TEST_DEVICE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(BusPortTestDevice),
    .instance_init = test_device_init,
    .class_init = test_device_class_init,
};

static const TypeInfo test_bus_owner_info = {
    .name = TYPE_TEST_BUS_OWNER,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(DeviceState),
};

static const TypeInfo protocol_test_device_info = {
    .name = TYPE_PROTOCOL_TEST_DEVICE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(ProtocolTestDevice),
    .instance_init = protocol_test_device_init,
    .class_init = protocol_test_device_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_I2C_TARGET },
        { TYPE_I2S_SINK },
        { TYPE_I2S_SOURCE },
        { TYPE_THREE_WIRE_SERIAL_TARGET },
        { NULL },
    },
};

static const TypeInfo empty_i2c_target_info = {
    .name = TYPE_EMPTY_I2C_TARGET,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(EmptyI2CTarget),
    .instance_init = empty_i2c_target_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_I2C_TARGET },
        { NULL },
    },
};

static void object_destroy(Object *obj)
{
    object_unparent(obj);
    object_unref(obj);
}

static DeviceState *test_bus_owner_create(void)
{
    DeviceState *owner = qdev_new(TYPE_TEST_BUS_OWNER);

    qdev_realize(owner, NULL, &error_fatal);
    return owner;
}

static void test_lifecycle(void)
{
    DeviceState *bus_owner = test_bus_owner_create();
    BusState *first_bus = qbus_new(TYPE_BUS_PORT_TEST_BUS, bus_owner, "first");
    BusState *second_bus = qbus_new(TYPE_BUS_PORT_TEST_BUS, bus_owner, "second");
    BusPortTestDevice *dev = BUS_PORT_TEST_DEVICE(object_new(TYPE_BUS_PORT_TEST_DEVICE));
    Error *err = NULL;

    object_property_set_link(OBJECT(dev), "port.TEST",
                             OBJECT(first_bus), &error_fatal);
    qdev_realize(DEVICE(dev), NULL, &error_fatal);
    g_assert_nonnull(dev->port.endpoint);
    g_assert_cmpint(first_bus->num_children, ==, 1);

    object_property_set_link(OBJECT(dev), "port.TEST",
                             OBJECT(second_bus), &err);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_true(dev->port.bus == first_bus);

    test_sequence = 0;
    endpoint_unrealize_sequence = 0;
    device_unrealize_sequence = 0;
    qdev_unrealize(DEVICE(dev));
    g_assert_null(dev->port.endpoint);
    g_assert_cmpint(first_bus->num_children, ==, 0);
    g_assert_cmpuint(endpoint_unrealize_sequence, ==, 1);
    g_assert_cmpuint(device_unrealize_sequence, ==, 2);

    qdev_realize(DEVICE(dev), NULL, &error_fatal);
    g_assert_nonnull(dev->port.endpoint);
    object_destroy(OBJECT(dev));
    object_destroy(OBJECT(bus_owner));
}

static void test_realize_rollback(void)
{
    DeviceState *bus_owner = test_bus_owner_create();
    BusState *bus = qbus_new(TYPE_BUS_PORT_TEST_BUS, bus_owner, "bus");
    BusPortTestDevice *dev = BUS_PORT_TEST_DEVICE(object_new(TYPE_BUS_PORT_TEST_DEVICE));
    Error *err = NULL;

    object_property_set_link(OBJECT(dev), "port.TEST", OBJECT(bus),
                             &error_fatal);
    object_property_set_link(OBJECT(dev), "port.FAIL", OBJECT(bus),
                             &error_fatal);
    g_assert_false(qdev_realize(DEVICE(dev), NULL, &err));
    g_assert_nonnull(err);
    error_free(err);
    g_assert_null(dev->port.endpoint);
    g_assert_null(dev->failing_port.endpoint);
    g_assert_cmpint(bus->num_children, ==, 0);

    object_property_set_link(OBJECT(dev), "port.FAIL", NULL, &error_fatal);
    qdev_realize(DEVICE(dev), NULL, &error_fatal);
    g_assert_nonnull(dev->port.endpoint);
    object_destroy(OBJECT(dev));
    object_destroy(OBJECT(bus_owner));
}

static void test_endpoint_realize_failure(void)
{
    DeviceState *bus_owner = test_bus_owner_create();
    BusState *bus = qbus_new(TYPE_BUS_PORT_TEST_BUS, bus_owner, "bus");
    BusPortTestDevice *dev = BUS_PORT_TEST_DEVICE(object_new(TYPE_BUS_PORT_TEST_DEVICE));
    Error *err = NULL;

    object_property_set_link(OBJECT(dev), "port.TEST", OBJECT(bus),
                             &error_fatal);
    endpoint_realize_fails = true;
    g_assert_false(qdev_realize(DEVICE(dev), NULL, &err));
    g_assert_nonnull(err);
    error_free(err);
    g_assert_null(dev->port.endpoint);
    g_assert_cmpint(bus->num_children, ==, 0);

    endpoint_realize_fails = false;
    qdev_realize(DEVICE(dev), NULL, &error_fatal);
    g_assert_nonnull(dev->port.endpoint);
    object_destroy(OBJECT(dev));
    object_destroy(OBJECT(bus_owner));
}

static void test_bus_owner_destroyed_first(void)
{
    DeviceState *bus_owner = test_bus_owner_create();
    BusState *bus = qbus_new(TYPE_BUS_PORT_TEST_BUS, bus_owner, "bus");
    BusPortTestDevice *dev = BUS_PORT_TEST_DEVICE(object_new(TYPE_BUS_PORT_TEST_DEVICE));

    object_property_set_link(OBJECT(dev), "port.TEST", OBJECT(bus),
                             &error_fatal);
    qdev_realize(DEVICE(dev), NULL, &error_fatal);
    object_destroy(OBJECT(bus_owner));
    g_assert_nonnull(dev->port.bus);
    g_assert_nonnull(dev->port.endpoint);

    qdev_unrealize(DEVICE(dev));
    g_assert_null(dev->port.endpoint);
    object_destroy(OBJECT(dev));
}

static void test_i2s_adapter(void)
{
    static const int16_t OUTPUT[] = { 0x1234, -0x1234 };
    static const I2SFormat TX_FORMAT = { 48000, 2 };
    static const I2SFormat RX_FORMAT = { 44100, 2 };
    DeviceState *bus_owner = test_bus_owner_create();
    I2STxBus *tx_bus = i2s_tx_bus_init(bus_owner, "tx");
    I2SRxBus *rx_bus = i2s_rx_bus_init(bus_owner, "rx");
    ProtocolTestDevice *dev = PROTOCOL_TEST_DEVICE(object_new(TYPE_PROTOCOL_TEST_DEVICE));
    ProtocolTestDevice *second = PROTOCOL_TEST_DEVICE(object_new(TYPE_PROTOCOL_TEST_DEVICE));
    int16_t input[2] = {};
    Error *err = NULL;

    dev->i2s_source_samples[0] = 0x5678;
    dev->i2s_source_samples[1] = -0x5678;
    object_property_set_link(OBJECT(dev), "port.I2S_RX", OBJECT(tx_bus),
                             &error_fatal);
    object_property_set_link(OBJECT(dev), "port.I2S_TX", OBJECT(rx_bus),
                             &error_fatal);
    qdev_realize(DEVICE(dev), NULL, &error_fatal);

    i2s_tx_configure(tx_bus, &TX_FORMAT);
    g_assert_cmpuint(dev->i2s_configure_count, ==, 1);
    i2s_tx_write(tx_bus, OUTPUT, 1);
    g_assert_cmpuint(dev->i2s_sample_rate_hz, ==, 48000);
    g_assert_cmpuint(dev->i2s_channels, ==, 2);
    g_assert_cmpuint(dev->i2s_frames, ==, 1);
    g_assert_cmpmem(dev->i2s_sink_samples, sizeof(OUTPUT), OUTPUT,
                    sizeof(OUTPUT));
    i2s_tx_write(tx_bus, OUTPUT, 1);
    g_assert_cmpuint(dev->i2s_configure_count, ==, 1);

    i2s_rx_configure(rx_bus, &RX_FORMAT);
    g_assert_cmpuint(dev->i2s_configure_count, ==, 2);
    g_assert_cmpuint(i2s_rx_read(rx_bus, input, 1), ==, 1);
    g_assert_cmpuint(dev->i2s_sample_rate_hz, ==, 44100);
    g_assert_cmpmem(input, sizeof(input), dev->i2s_source_samples,
                    sizeof(dev->i2s_source_samples));

    object_property_set_link(OBJECT(second), "port.I2S_RX", OBJECT(tx_bus),
                             &error_fatal);
    g_assert_false(qdev_realize(DEVICE(second), NULL, &err));
    g_assert_nonnull(err);
    error_free(err);
    g_assert_null(second->i2s_sink_port.parent.endpoint);
    g_assert_cmpint(BUS(tx_bus)->num_children, ==, 1);

    object_destroy(OBJECT(second));
    object_destroy(OBJECT(dev));
    object_destroy(OBJECT(bus_owner));
}

static void test_i2c_adapter(void)
{
    DeviceState *bus_owner = test_bus_owner_create();
    I2CBus *bus = i2c_init_bus(bus_owner, "i2c");
    ProtocolTestDevice *dev = PROTOCOL_TEST_DEVICE(object_new(TYPE_PROTOCOL_TEST_DEVICE));
    DeviceState *endpoint;

    dev->i2c_receive = 0x5a;
    object_property_set_link(OBJECT(dev), "port.I2C", OBJECT(bus),
                             &error_fatal);
    qdev_realize(DEVICE(dev), NULL, &error_fatal);
    endpoint = QTAILQ_FIRST(&BUS(bus)->children)->child;
    g_assert_true(qdev_get_vmsd(endpoint) == &vmstate_i2c_slave);

    g_assert_cmpint(i2c_start_send(bus, 0x31), ==, 0);
    g_assert_cmpint(dev->i2c_event, ==, I2C_START_SEND);
    g_assert_cmpint(i2c_send(bus, 0xa5), ==, 0);
    g_assert_cmphex(dev->i2c_sent, ==, 0xa5);
    i2c_end_transfer(bus);
    g_assert_cmpint(dev->i2c_event, ==, I2C_FINISH);

    g_assert_cmpint(i2c_start_recv(bus, 0x31), ==, 0);
    g_assert_cmphex(i2c_recv(bus), ==, 0x5a);
    i2c_nack(bus);
    g_assert_cmpint(dev->i2c_event, ==, I2C_NACK);
    i2c_end_transfer(bus);

    bus->saved_address = 0x31;
    g_assert_cmpint(qdev_get_vmsd(endpoint)->post_load(endpoint, 1), ==, 0);
    g_assert_false(QLIST_EMPTY(&bus->current_devs));
    i2c_end_transfer(bus);

    object_destroy(OBJECT(dev));
    object_destroy(OBJECT(bus_owner));
}

static void test_three_wire_serial_adapter(void)
{
    DeviceState *bus_owner = test_bus_owner_create();
    ThreeWireSerialBus *bus = three_wire_serial_bus_init(bus_owner, "serial");
    ProtocolTestDevice *dev =
        PROTOCOL_TEST_DEVICE(object_new(TYPE_PROTOCOL_TEST_DEVICE));
    ProtocolTestDevice *second =
        PROTOCOL_TEST_DEVICE(object_new(TYPE_PROTOCOL_TEST_DEVICE));

    object_property_set_link(OBJECT(dev), "port.SERIAL", OBJECT(bus),
                             &error_fatal);
    qdev_realize(DEVICE(dev), NULL, &error_fatal);
    object_property_set_link(OBJECT(second), "port.SERIAL", OBJECT(bus),
                             &error_fatal);
    qdev_realize(DEVICE(second), NULL, &error_fatal);

    three_wire_serial_set_strobe(bus, true);
    g_assert_true(dev->serial_strobe_asserted);

    three_wire_serial_write(bus, 0xfffff868, 12);
    three_wire_serial_write(bus, 0x00e, 12);
    g_assert_cmpuint(dev->serial_write_count, ==, 2);
    g_assert_cmphex(dev->serial_value[0], ==, 0x868);
    g_assert_cmpuint(dev->serial_bits[0], ==, 12);
    g_assert_cmphex(dev->serial_value[1], ==, 0x00e);
    g_assert_cmpuint(dev->serial_bits[1], ==, 12);
    g_assert_cmpuint(second->serial_write_count, ==, 2);
    g_assert_cmphex(second->serial_value[0], ==, 0x868);
    g_assert_cmphex(second->serial_value[1], ==, 0x00e);
    g_assert_true(second->serial_strobe_asserted);

    three_wire_serial_set_strobe(bus, false);
    g_assert_false(dev->serial_strobe_asserted);
    g_assert_false(second->serial_strobe_asserted);

    object_destroy(OBJECT(second));
    object_destroy(OBJECT(dev));
    object_destroy(OBJECT(bus_owner));
}

static void test_i2c_adapter_defaults_and_address(void)
{
    DeviceState *bus_owner = test_bus_owner_create();
    I2CBus *bus = i2c_init_bus(bus_owner, "i2c");
    EmptyI2CTarget *empty = EMPTY_I2C_TARGET(object_new(TYPE_EMPTY_I2C_TARGET));
    EmptyI2CTarget *invalid = EMPTY_I2C_TARGET(object_new(TYPE_EMPTY_I2C_TARGET));
    Error *err = NULL;

    object_property_set_link(OBJECT(empty), "port.I2C", OBJECT(bus),
                             &error_fatal);
    qdev_realize(DEVICE(empty), NULL, &error_fatal);
    g_assert_cmpint(i2c_start_send(bus, 0x32), ==, 0);
    g_assert_cmpint(i2c_send(bus, 0xa5), ==, -1);
    i2c_end_transfer(bus);
    g_assert_cmpint(i2c_start_recv(bus, 0x32), ==, 0);
    g_assert_cmphex(i2c_recv(bus), ==, 0xff);
    i2c_end_transfer(bus);

    invalid->address = 0x100;
    object_property_set_link(OBJECT(invalid), "port.I2C", OBJECT(bus),
                             &error_fatal);
    g_assert_false(qdev_realize(DEVICE(invalid), NULL, &err));
    g_assert_nonnull(err);
    error_free(err);
    g_assert_null(invalid->port.parent.endpoint);
    g_assert_cmpint(BUS(bus)->num_children, ==, 1);

    object_destroy(OBJECT(invalid));
    object_destroy(OBJECT(empty));
    object_destroy(OBJECT(bus_owner));
}

static void register_test_types(void)
{
    type_register_static(&test_bus_info);
    type_register_static(&test_endpoint_info);
    type_register_static(&test_device_info);
    type_register_static(&test_bus_owner_info);
    type_register_static(&protocol_test_device_info);
    type_register_static(&empty_i2c_target_info);
}

static void test_machine_init(void)
{
    Object *machine = object_property_add_new_container(
        object_get_root(), "machine");

    object_property_add_new_container(machine, "unattached");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    register_test_types();
    test_machine_init();

    g_test_add_func("/qdev/bus-port/lifecycle", test_lifecycle);
    g_test_add_func("/qdev/bus-port/realize-rollback",
                    test_realize_rollback);
    g_test_add_func("/qdev/bus-port/endpoint-realize-failure",
                    test_endpoint_realize_failure);
    g_test_add_func("/qdev/bus-port/bus-owner-destroyed-first",
                    test_bus_owner_destroyed_first);
    g_test_add_func("/qdev/bus-port/i2s-adapter", test_i2s_adapter);
    g_test_add_func("/qdev/bus-port/i2c-adapter", test_i2c_adapter);
    g_test_add_func("/qdev/bus-port/three-wire-serial-adapter",
                    test_three_wire_serial_adapter);
    g_test_add_func("/qdev/bus-port/i2c-defaults-and-address",
                    test_i2c_adapter_defaults_and_address);
    return g_test_run();
}
