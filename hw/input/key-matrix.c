/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "ui/input.h"

#define TYPE_KEY_MATRIX "key-matrix"
#define KEY_MATRIX_LINES 32

OBJECT_DECLARE_SIMPLE_TYPE(KeyMatrixState, KEY_MATRIX)

typedef struct KeyMatrixColumn {
    KeyMatrixState *matrix;
    uint32_t index;
} KeyMatrixColumn;

struct KeyMatrixState {
    DeviceState parent_obj;

    uint32_t *key_codes;
    uint32_t key_codes_count;
    uint32_t *key_rows;
    uint32_t key_rows_count;
    uint32_t *key_columns;
    uint32_t key_columns_count;

    uint32_t high_columns;
    bool *pressed;
    KeyMatrixColumn columns[KEY_MATRIX_LINES];
    qemu_irq rows[KEY_MATRIX_LINES];
    QemuInputHandlerState *input;
};

static uint32_t key_matrix_find_root(uint32_t *parent, uint32_t node) {
    while (parent[node] != node)
        node = parent[node];
    return node;
}

static void key_matrix_update(KeyMatrixState *s) {
    uint32_t parent[KEY_MATRIX_LINES * 2];
    bool low[KEY_MATRIX_LINES * 2] = { 0 };

    for (uint32_t i = 0; i < ARRAY_SIZE(parent); i++)
        parent[i] = i;
    for (uint32_t i = 0; i < s->key_codes_count; i++) {
        if (!s->pressed[i])
            continue;
        uint32_t row = key_matrix_find_root(parent, s->key_rows[i]);
        uint32_t column = key_matrix_find_root(parent, KEY_MATRIX_LINES + s->key_columns[i]);
        parent[row] = column;
    }

    for (uint32_t column = 0; column < KEY_MATRIX_LINES; column++) {
        uint32_t root = key_matrix_find_root(parent, KEY_MATRIX_LINES + column);
        low[root] |= (s->high_columns & BIT(column)) == 0;
    }

    for (uint32_t row = 0; row < KEY_MATRIX_LINES; row++)
        qemu_set_irq(s->rows[row], !low[key_matrix_find_root(parent, row)]);
}

static void key_matrix_handle_column(void *opaque, int _n, int level) {
    KeyMatrixColumn *column = opaque;
    KeyMatrixState *s = column->matrix;
    uint32_t high_columns = deposit32(s->high_columns, column->index, 1, level != 0);

    if (high_columns == s->high_columns)
        return;
    s->high_columns = high_columns;
    key_matrix_update(s);
}

static void key_matrix_handle_event(DeviceState *dev, QemuConsole *_src, QemuInputEvent *evt) {
    KeyMatrixState *s = KEY_MATRIX(dev);
    int qcode = qemu_input_linux_to_qcode(evt->key.key);
    bool down = evt->key.down;
    bool is_changed = false;

    for (uint32_t i = 0; i < s->key_codes_count; i++) {
        if (s->key_codes[i] == qcode && s->pressed[i] != down) {
            s->pressed[i] = down;
            is_changed = true;
        }
    }
    if (is_changed)
        key_matrix_update(s);
}

static const QemuInputHandler key_matrix_input_handler = {
    .name = "Key matrix",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = key_matrix_handle_event,
    .shared = true,
};

static bool key_matrix_validate(KeyMatrixState *s, Error **errp) {
    if (s->key_rows_count != s->key_codes_count || s->key_columns_count != s->key_codes_count) {
        error_setg(errp, "key-matrix: key-codes, key-rows and key-columns differ in length");
        return false;
    }
    for (uint32_t i = 0; i < s->key_codes_count; i++) {
        if (s->key_rows[i] >= KEY_MATRIX_LINES || s->key_columns[i] >= KEY_MATRIX_LINES) {
            error_setg(errp, "key-matrix: key %u is outside the matrix", i);
            return false;
        }
    }
    return true;
}

static void key_matrix_realize(DeviceState *dev, Error **errp) {
    KeyMatrixState *s = KEY_MATRIX(dev);

    if (!key_matrix_validate(s, errp))
        return;
    s->pressed = g_new0(bool, s->key_codes_count);
    s->input = qemu_input_handler_register(dev, &key_matrix_input_handler);
}

static void key_matrix_unrealize(DeviceState *dev) {
    KeyMatrixState *s = KEY_MATRIX(dev);

    qemu_input_handler_unregister(s->input);
    g_free(s->pressed);
}

static void key_matrix_reset(DeviceState *dev) {
    key_matrix_update(KEY_MATRIX(dev));
}

static void key_matrix_init(Object *obj) {
    KeyMatrixState *s = KEY_MATRIX(obj);
    DeviceState *dev = DEVICE(obj);

    s->high_columns = UINT32_MAX;
    for (uint32_t i = 0; i < KEY_MATRIX_LINES; i++) {
        g_autofree char *in_name = g_strdup_printf("in%u", i);
        g_autofree char *out_name = g_strdup_printf("out%u", i);

        s->columns[i] = (KeyMatrixColumn) { s, i };
        qdev_init_gpio_in_named_with_opaque(dev, key_matrix_handle_column, &s->columns[i], in_name, 1);
        qdev_init_gpio_out_named(dev, &s->rows[i], out_name, 1);
    }
}

static const Property key_matrix_properties[] = {
    DEFINE_PROP_ARRAY("key-codes", KeyMatrixState, key_codes_count, key_codes, qdev_prop_uint32, uint32_t),
    DEFINE_PROP_ARRAY("key-rows", KeyMatrixState, key_rows_count, key_rows, qdev_prop_uint32, uint32_t),
    DEFINE_PROP_ARRAY("key-columns", KeyMatrixState, key_columns_count, key_columns, qdev_prop_uint32, uint32_t),
};

static void key_matrix_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = key_matrix_realize;
    dc->unrealize = key_matrix_unrealize;
    device_class_set_legacy_reset(dc, key_matrix_reset);
    device_class_set_props(dc, key_matrix_properties);
}

static const TypeInfo key_matrix_info = {
    .name = TYPE_KEY_MATRIX,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(KeyMatrixState),
    .instance_init = key_matrix_init,
    .class_init = key_matrix_class_init,
};

static void key_matrix_register_types(void) {
    type_register_static(&key_matrix_info);
}

type_init(key_matrix_register_types)
