/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_WIRED_AND "wired-and"
#define WIRED_AND_MAX_LINES 32

OBJECT_DECLARE_SIMPLE_TYPE(WiredAndState, WIRED_AND)

struct WiredAndState {
    DeviceState parent_obj;
    uint32_t num_lines;
    uint32_t low_lines;
    qemu_irq out;
};

static void wired_and_handle_input(void *opaque, int n, int level) {
    WiredAndState *s = opaque;

    s->low_lines = deposit32(s->low_lines, n, 1, level == 0);
    qemu_set_irq(s->out, s->low_lines == 0);
}

static void wired_and_realize(DeviceState *dev, Error **errp) {
    WiredAndState *s = WIRED_AND(dev);

    if (s->num_lines == 0 || s->num_lines > WIRED_AND_MAX_LINES) {
        error_setg(errp, "wired-and: num-lines must be 1 to %d", WIRED_AND_MAX_LINES);
        return;
    }
    qdev_init_gpio_in(dev, wired_and_handle_input, s->num_lines);
}

static void wired_and_init(Object *obj) {
    WiredAndState *s = WIRED_AND(obj);

    qdev_init_gpio_out(DEVICE(obj), &s->out, 1);
}

static const Property wired_and_properties[] = {
    DEFINE_PROP_UINT32("num-lines", WiredAndState, num_lines, 0),
};

static void wired_and_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = wired_and_realize;
    device_class_set_props(dc, wired_and_properties);
}

static const TypeInfo wired_and_info = {
    .name = TYPE_WIRED_AND,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(WiredAndState),
    .instance_init = wired_and_init,
    .class_init = wired_and_class_init,
};

static void wired_and_register_types(void) {
    type_register_static(&wired_and_info);
}

type_init(wired_and_register_types)
