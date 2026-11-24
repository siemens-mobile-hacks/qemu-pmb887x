/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "ui/input.h"

#define TYPE_KEY_SWITCH "key-switch"

OBJECT_DECLARE_SIMPLE_TYPE(KeySwitchState, KEY_SWITCH)

struct KeySwitchState {
    DeviceState parent_obj;
    uint32_t *key_codes;
    uint32_t key_codes_count;
    bool level;
    bool pressed;
    qemu_irq out;
    QemuInputHandlerState *input;
};

static void key_switch_update(KeySwitchState *s) {
    qemu_set_irq(s->out, s->pressed ? s->level : !s->level);
}

static void key_switch_handle_event(DeviceState *dev, QemuConsole *_src, QemuInputEvent *evt) {
    KeySwitchState *s = KEY_SWITCH(dev);
    int qcode = qemu_input_linux_to_qcode(evt->key.key);
    bool down = evt->key.down;

    for (uint32_t i = 0; i < s->key_codes_count; i++) {
        if (s->key_codes[i] == qcode && s->pressed != down) {
            s->pressed = down;
            key_switch_update(s);
        }
    }
}

static const QemuInputHandler key_switch_input_handler = {
    .name = "Key switch",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = key_switch_handle_event,
    .shared = true,
};

static void key_switch_realize(DeviceState *dev, Error **_errp) {
    KeySwitchState *s = KEY_SWITCH(dev);

    s->input = qemu_input_handler_register(dev, &key_switch_input_handler);
}

static void key_switch_unrealize(DeviceState *dev) {
    KeySwitchState *s = KEY_SWITCH(dev);

    qemu_input_handler_unregister(s->input);
}

static void key_switch_reset(DeviceState *dev) {
    key_switch_update(KEY_SWITCH(dev));
}

static void key_switch_init(Object *obj) {
    KeySwitchState *s = KEY_SWITCH(obj);

    qdev_init_gpio_out_named(DEVICE(obj), &s->out, "out", 1);
}

static const Property key_switch_properties[] = {
    DEFINE_PROP_ARRAY("key-codes", KeySwitchState, key_codes_count, key_codes, qdev_prop_uint32, uint32_t),
    DEFINE_PROP_BOOL("level", KeySwitchState, level, false),
};

static void key_switch_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = key_switch_realize;
    dc->unrealize = key_switch_unrealize;
    device_class_set_legacy_reset(dc, key_switch_reset);
    device_class_set_props(dc, key_switch_properties);
}

static const TypeInfo key_switch_info = {
    .name = TYPE_KEY_SWITCH,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(KeySwitchState),
    .instance_init = key_switch_init,
    .class_init = key_switch_class_init,
};

static void key_switch_register_types(void) {
    type_register_static(&key_switch_info);
}

type_init(key_switch_register_types)
