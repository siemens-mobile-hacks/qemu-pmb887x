/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/misc/phone/accessory-connector.h"
#include "hw/core/bus-port.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "system/debug-channel.h"

typedef struct ConnectorGpio {
    bool from_input;
    unsigned input;
    bool level;
} ConnectorGpio;

typedef struct ConnectorPort {
    DeviceState *device;
    char *name;
} ConnectorPort;

typedef struct ConnectorProfile {
    char *name;
    ConnectorGpio *gpio;
    BusState **port_buses;
} ConnectorProfile;

struct AccessoryConnector {
    DeviceState parent_obj;
    DebugChannel dbg;

    char *accessory;
    char *unplugged_name;
    uint32_t replug_delay_ms;

    ConnectorProfile *profiles;
    size_t profile_count;
    ConnectorPort *ports;
    BusState **port_bound;
    size_t port_count;
    unsigned input_count;
    unsigned output_count;
    bool *input_levels;
    qemu_irq *outputs;

    const ConnectorProfile *plugged;
    const ConnectorProfile *pending;
    const ConnectorProfile *unplugged;
    QEMUTimer *replug_timer;
};

static size_t connector_add_port(AccessoryConnector *s,
                                 const AccessoryPortRoute *route)
{
    for (size_t i = 0; i < s->port_count; i++) {
        if (s->ports[i].device == route->device &&
            strcmp(s->ports[i].name, route->port) == 0) {
            return i;
        }
    }
    s->ports = g_renew(ConnectorPort, s->ports, s->port_count + 1);
    s->ports[s->port_count] = (ConnectorPort) {
        .device = route->device,
        .name = g_strdup(route->port),
    };
    return s->port_count++;
}

static void connector_count_lines(AccessoryConnector *s,
                                  const AccessoryProfile *profile)
{
    for (size_t i = 0; i < profile->gpio_count; i++) {
        const AccessoryGpioRoute *route = &profile->gpio[i];

        assert(route->output < UINT_MAX && route->input < UINT_MAX);
        s->output_count = MAX(s->output_count, route->output + 1);
        if (route->from_input) {
            s->input_count = MAX(s->input_count, route->input + 1);
        }
    }
    for (size_t i = 0; i < profile->port_count; i++) {
        connector_add_port(s, &profile->ports[i]);
    }
}

static void connector_copy_profile(AccessoryConnector *s,
                                   ConnectorProfile *copy,
                                   const AccessoryProfile *profile)
{
    g_autofree bool *output_routed = g_new0(bool, s->output_count);
    g_autofree bool *port_routed = g_new0(bool, s->port_count);

    copy->name = g_strdup(profile->name);
    copy->gpio = g_new0(ConnectorGpio, s->output_count);
    copy->port_buses = g_new0(BusState *, s->port_count);

    for (size_t i = 0; i < profile->gpio_count; i++) {
        const AccessoryGpioRoute *route = &profile->gpio[i];

        assert(!output_routed[route->output]);
        output_routed[route->output] = true;
        copy->gpio[route->output] = (ConnectorGpio) {
            .from_input = route->from_input,
            .input = route->input,
            .level = route->level,
        };
    }
    for (size_t i = 0; i < profile->port_count; i++) {
        size_t port = connector_add_port(s, &profile->ports[i]);

        assert(!port_routed[port]);
        port_routed[port] = true;
        copy->port_buses[port] = profile->ports[i].bus;
    }
}

static bool connector_get_level(AccessoryConnector *s,
                                const ConnectorGpio *gpio)
{
    if (!gpio->from_input) {
        return gpio->level;
    }
    return s->input_levels[gpio->input] != gpio->level;
}

static bool connector_bind_port(AccessoryConnector *s, size_t port,
                                BusState *bus, Error **errp)
{
    bool bound = qdev_bus_port_bind(s->ports[port].device,
                                    s->ports[port].name, bus, errp);

    s->port_bound[port] = bound ? bus : NULL;
    return bound;
}

/*
 * Ports leaving a bus are disconnected before any port connects, so a bus
 * that takes one endpoint can move from one device to another. A port that
 * cannot connect is left disconnected; the rest of the profile still applies.
 */
static bool connector_plug(AccessoryConnector *s,
                           const ConnectorProfile *profile, Error **errp)
{
    bool ok = true;

    for (size_t i = 0; i < s->port_count; i++) {
        if (s->port_bound[i] != profile->port_buses[i]) {
            connector_bind_port(s, i, NULL, &error_abort);
        }
    }
    for (size_t i = 0; i < s->port_count; i++) {
        Error *err = NULL;

        if (profile->port_buses[i] == NULL ||
            s->port_bound[i] == profile->port_buses[i]) {
            continue;
        }
        if (!connector_bind_port(s, i, profile->port_buses[i], &err)) {
            error_prepend(&err, "accessory '%s': ", profile->name);
            if (ok) {
                error_propagate(errp, err);
            } else {
                error_report_err(err);
            }
            ok = false;
        }
    }

    s->plugged = profile;
    for (unsigned output = 0; output < s->output_count; output++) {
        qemu_set_irq(s->outputs[output],
                     connector_get_level(s, &profile->gpio[output]));
    }

    DEBUG_LOG(&s->dbg, "accessory %s plugged\n", profile->name);
    return ok;
}

static void connector_replug_timeout(void *opaque)
{
    AccessoryConnector *s = opaque;
    Error *err = NULL;

    if (!connector_plug(s, s->pending, &err)) {
        error_report_err(err);
    }
}

static void connector_input_changed(void *opaque, int input, int level)
{
    AccessoryConnector *s = opaque;

    s->input_levels[input] = level != 0;
    if (!s->plugged) {
        return;
    }

    for (unsigned output = 0; output < s->output_count; output++) {
        const ConnectorGpio *gpio = &s->plugged->gpio[output];

        if (gpio->from_input && gpio->input == (unsigned) input) {
            qemu_set_irq(s->outputs[output], connector_get_level(s, gpio));
        }
    }
}

void accessory_connector_set_profiles(AccessoryConnector *s,
                                      const AccessoryProfile *profiles,
                                      size_t count)
{
    DeviceState *dev = DEVICE(s);

    assert(!dev->realized && s->profiles == NULL && count > 0);

    for (size_t i = 0; i < count; i++) {
        connector_count_lines(s, &profiles[i]);
    }

    s->profile_count = count;
    s->profiles = g_new0(ConnectorProfile, count);
    for (size_t i = 0; i < count; i++) {
        connector_copy_profile(s, &s->profiles[i], &profiles[i]);
    }

    s->port_bound = g_new0(BusState *, s->port_count);
    s->input_levels = g_new0(bool, s->input_count);
    s->outputs = g_new0(qemu_irq, s->output_count);
    qdev_init_gpio_in_named(dev, connector_input_changed, "in",
                            s->input_count);
    qdev_init_gpio_out_named(dev, s->outputs, "out", s->output_count);
}

static const ConnectorProfile *connector_find_profile(AccessoryConnector *s,
                                                      const char *name)
{
    for (size_t i = 0; i < s->profile_count; i++) {
        if (strcmp(s->profiles[i].name, name) == 0) {
            return &s->profiles[i];
        }
    }
    return NULL;
}

static void connector_set_unknown_error(AccessoryConnector *s,
                                        const char *name, Error **errp)
{
    g_autoptr(GString) names = g_string_new(NULL);

    for (size_t i = 0; i < s->profile_count; i++) {
        g_string_append_printf(names, "%s%s", i > 0 ? ", " : "",
                               s->profiles[i].name);
    }
    error_setg(errp, "Unknown accessory '%s', available: %s", name,
               names->str);
}

static char *connector_get_accessory(Object *obj, Error **_errp)
{
    AccessoryConnector *s = ACCESSORY_CONNECTOR(obj);

    if (!DEVICE(obj)->realized) {
        return g_strdup(s->accessory ? s->accessory : "");
    }
    if (timer_pending(s->replug_timer)) {
        return g_strdup(s->pending->name);
    }
    return g_strdup(s->plugged->name);
}

static void connector_set_accessory(Object *obj, const char *name,
                                    Error **errp)
{
    AccessoryConnector *s = ACCESSORY_CONNECTOR(obj);
    const ConnectorProfile *profile;

    if (!DEVICE(obj)->realized) {
        g_free(s->accessory);
        s->accessory = g_strdup(name);
        return;
    }

    profile = connector_find_profile(s, name);
    if (!profile) {
        connector_set_unknown_error(s, name, errp);
        return;
    }

    timer_del(s->replug_timer);
    if (!s->unplugged || profile == s->unplugged || s->replug_delay_ms == 0) {
        connector_plug(s, profile, errp);
        return;
    }

    if (!connector_plug(s, s->unplugged, errp)) {
        return;
    }
    s->pending = profile;
    timer_mod(s->replug_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              s->replug_delay_ms);
}

static bool connector_check_ports(AccessoryConnector *s, Error **errp)
{
    ERRP_GUARD();

    for (size_t port = 0; port < s->port_count; port++) {
        if (!qdev_bus_port_check(s->ports[port].device, s->ports[port].name,
                                 NULL, errp)) {
            return false;
        }
    }
    for (size_t i = 0; i < s->profile_count; i++) {
        const ConnectorProfile *profile = &s->profiles[i];

        for (size_t port = 0; port < s->port_count; port++) {
            if (!qdev_bus_port_check(s->ports[port].device,
                                     s->ports[port].name,
                                     profile->port_buses[port], errp)) {
                error_prepend(errp, "accessory '%s': ", profile->name);
                return false;
            }
        }
    }
    return true;
}

static void connector_realize(DeviceState *dev, Error **errp)
{
    AccessoryConnector *s = ACCESSORY_CONNECTOR(dev);
    const ConnectorProfile *initial;

    if (s->profile_count == 0) {
        error_setg(errp, "accessory connector has no profiles");
        return;
    }
    if (!s->accessory) {
        error_setg(errp, "accessory connector has no accessory selected");
        return;
    }
    initial = connector_find_profile(s, s->accessory);
    if (!initial) {
        connector_set_unknown_error(s, s->accessory, errp);
        return;
    }
    s->unplugged = NULL;
    if (s->unplugged_name) {
        s->unplugged = connector_find_profile(s, s->unplugged_name);
        if (!s->unplugged) {
            connector_set_unknown_error(s, s->unplugged_name, errp);
            return;
        }
    }
    if (!connector_check_ports(s, errp)) {
        return;
    }

    debug_channel_init_device(&s->dbg, dev);
    s->replug_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                   connector_replug_timeout, s);
    for (size_t i = 0; i < s->port_count; i++) {
        connector_bind_port(s, i, NULL, &error_abort);
    }
    if (!connector_plug(s, initial, errp)) {
        s->plugged = NULL;
        timer_free(g_steal_pointer(&s->replug_timer));
        debug_channel_cleanup(&s->dbg);
    }
}

static void connector_unrealize(DeviceState *dev)
{
    AccessoryConnector *s = ACCESSORY_CONNECTOR(dev);

    timer_free(s->replug_timer);
    s->replug_timer = NULL;
    s->plugged = NULL;
    debug_channel_cleanup(&s->dbg);
}

static void connector_finalize(Object *obj)
{
    AccessoryConnector *s = ACCESSORY_CONNECTOR(obj);

    for (size_t i = 0; i < s->profile_count; i++) {
        g_free(s->profiles[i].name);
        g_free(s->profiles[i].gpio);
        g_free(s->profiles[i].port_buses);
    }
    for (size_t i = 0; i < s->port_count; i++) {
        g_free(s->ports[i].name);
    }
    g_free(s->profiles);
    g_free(s->ports);
    g_free(s->port_bound);
    g_free(s->input_levels);
    g_free(s->outputs);
    g_free(s->accessory);
}

static const Property connector_properties[] = {
    DEFINE_PROP_STRING("unplugged", AccessoryConnector, unplugged_name),
    DEFINE_PROP_UINT32("replug-delay-ms", AccessoryConnector,
                       replug_delay_ms, 500),
};

static void connector_class_init(ObjectClass *klass, const void *_data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = connector_realize;
    dc->unrealize = connector_unrealize;
    dc->user_creatable = false;
    dc->hotpluggable = false;
    device_class_set_props(dc, connector_properties);
    object_class_property_add_str(klass, "accessory", connector_get_accessory,
                                  connector_set_accessory);
}

static const TypeInfo connector_types[] = {
    {
        .name = TYPE_ACCESSORY_CONNECTOR,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(AccessoryConnector),
        .instance_finalize = connector_finalize,
        .class_init = connector_class_init,
    },
};

DEFINE_TYPES(connector_types)
