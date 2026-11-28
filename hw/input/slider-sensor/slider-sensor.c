/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"

#define TYPE_SLIDER_SENSOR "slider-sensor"
OBJECT_DECLARE_SIMPLE_TYPE(SliderSensorState, SLIDER_SENSOR)

struct SliderSensorState {
    DeviceState parent_obj;
    qemu_irq open_out;
    qemu_irq closed_out;
    QEMUTimer *travel_timer;
    uint32_t travel_ms;
    bool is_open;
    bool is_moving;
};

static void slider_sensor_update(SliderSensorState *s) {
    bool is_at_open = s->is_open && !s->is_moving;
    bool is_at_closed = !s->is_open && !s->is_moving;

    qemu_set_irq(s->open_out, !is_at_open);
    qemu_set_irq(s->closed_out, !is_at_closed);
}

static void slider_sensor_arrive(void *opaque) {
    SliderSensorState *s = opaque;

    s->is_moving = false;
    slider_sensor_update(s);
}

static bool slider_sensor_get_open(Object *obj, Error **_errp) {
    return SLIDER_SENSOR(obj)->is_open;
}

static void slider_sensor_set_open(Object *obj, bool is_open, Error **_errp) {
    SliderSensorState *s = SLIDER_SENSOR(obj);

    if (is_open == s->is_open)
        return;
    s->is_open = is_open;
    if (!DEVICE(obj)->realized)
        return;

    s->is_moving = true;
    slider_sensor_update(s);
    timer_mod(s->travel_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->travel_ms);
}

static void slider_sensor_reset(DeviceState *dev) {
    SliderSensorState *s = SLIDER_SENSOR(dev);

    timer_del(s->travel_timer);
    s->is_moving = false;
    slider_sensor_update(s);
}

static void slider_sensor_realize(DeviceState *dev, Error **_errp) {
    SliderSensorState *s = SLIDER_SENSOR(dev);

    s->travel_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, slider_sensor_arrive, s);
}

static void slider_sensor_unrealize(DeviceState *dev) {
    SliderSensorState *s = SLIDER_SENSOR(dev);

    timer_free(s->travel_timer);
}

static void slider_sensor_init(Object *obj) {
    SliderSensorState *s = SLIDER_SENSOR(obj);

    s->is_open = true;
    qdev_init_gpio_out_named(DEVICE(obj), &s->open_out, "open", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->closed_out, "closed", 1);
}

static const Property slider_sensor_properties[] = {
    DEFINE_PROP_UINT32("travel_ms", SliderSensorState, travel_ms, 200),
};

static void slider_sensor_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = slider_sensor_realize;
    dc->unrealize = slider_sensor_unrealize;
    device_class_set_legacy_reset(dc, slider_sensor_reset);
    device_class_set_props(dc, slider_sensor_properties);
    object_class_property_add_bool(klass, "open", slider_sensor_get_open, slider_sensor_set_open);
}

static const TypeInfo slider_sensor_info = {
    .name = TYPE_SLIDER_SENSOR,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SliderSensorState),
    .instance_init = slider_sensor_init,
    .class_init = slider_sensor_class_init,
};

static void slider_sensor_register_types(void) {
    type_register_static(&slider_sensor_info);
}

type_init(slider_sensor_register_types)
