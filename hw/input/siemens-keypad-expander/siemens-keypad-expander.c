/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "system/debug-channel.h"
#include "regs.h"

#define TYPE_SIEMENS_KEYPAD_EXPANDER "siemens-keypad-expander"
OBJECT_DECLARE_SIMPLE_TYPE(SiemensKeypadExpanderState, SIEMENS_KEYPAD_EXPANDER)

#define SIEMENS_KEYPAD_EXPANDER_LINES 8
#define SIEMENS_KEYPAD_EXPANDER_GND_COLUMN SIEMENS_KEYPAD_EXPANDER_LINES
#define SIEMENS_KEYPAD_EXPANDER_QUEUE_SIZE 16
#define SIEMENS_KEYPAD_EXPANDER_QUEUE_EVENTS_MAX (SIEMENS_KEYPAD_EXPANDER_QUEUE_SIZE - 1)
#define SIEMENS_KEYPAD_EXPANDER_DEBOUNCE_NS (20 * SCALE_MS)
#define SIEMENS_KEYPAD_EXPANDER_RESCAN_NS (20 * SCALE_MS)
#define SIEMENS_KEYPAD_EXPANDER_ALL_LINES MAKE_64BIT_MASK(0, SIEMENS_KEYPAD_EXPANDER_LINES)

typedef struct SiemensKeypadExpanderInput {
    SiemensKeypadExpanderState *expander;
    uint32_t index;
} SiemensKeypadExpanderInput;

struct SiemensKeypadExpanderState {
    I2CSlave parent_obj;
    DebugChannel dbg;
    SiemensKeypadExpanderInput px_in[SIEMENS_KEYPAD_EXPANDER_LINES];
    qemu_irq px_out[SIEMENS_KEYPAD_EXPANDER_LINES];
    qemu_irq int_out;
    QEMUTimer *scan_timer;

    uint8_t high_inputs;
    bool is_reset_released;
    bool is_scanning;
    uint16_t pressed[SIEMENS_KEYPAD_EXPANDER_LINES];

    uint8_t queue[SIEMENS_KEYPAD_EXPANDER_QUEUE_SIZE];
    uint8_t queue_count;
    uint8_t read_buffer[SIEMENS_KEYPAD_EXPANDER_QUEUE_SIZE];
    uint8_t reg;
    uint8_t read_index;
    uint8_t write_index;
};

static uint8_t siemens_keypad_expander_get_code(uint32_t row, uint32_t column) {
    return (((row + 2) & 7) << 4) | (column + 1);
}

static void siemens_keypad_expander_update_int(SiemensKeypadExpanderState *s) {
    qemu_set_irq(s->int_out, s->queue_count == 0);
}

static void siemens_keypad_expander_drive_columns(SiemensKeypadExpanderState *s, uint32_t low_columns) {
    for (uint32_t i = 0; i < SIEMENS_KEYPAD_EXPANDER_LINES; i++)
        qemu_set_irq(s->px_out[i], (low_columns & BIT(i)) == 0);
}

static void siemens_keypad_expander_queue_event(SiemensKeypadExpanderState *s, uint8_t event) {
    if (s->queue_count == SIEMENS_KEYPAD_EXPANDER_QUEUE_EVENTS_MAX)
        return;
    s->queue[s->queue_count++] = event;
}

static void siemens_keypad_expander_read_keys(SiemensKeypadExpanderState *s, uint16_t *pressed) {
    s->is_scanning = true;
    siemens_keypad_expander_drive_columns(s, 0);
    uint8_t rows_to_gnd = ~s->high_inputs;
    for (uint32_t column = 0; column < SIEMENS_KEYPAD_EXPANDER_LINES; column++) {
        siemens_keypad_expander_drive_columns(s, BIT(column));
        uint8_t rows_to_column = ~s->high_inputs & ~rows_to_gnd;
        for (uint32_t row = 0; row < SIEMENS_KEYPAD_EXPANDER_LINES; row++)
            pressed[row] |= ((rows_to_column >> row) & 1) << column;
    }
    siemens_keypad_expander_drive_columns(s, SIEMENS_KEYPAD_EXPANDER_ALL_LINES);
    s->is_scanning = false;

    for (uint32_t row = 0; row < SIEMENS_KEYPAD_EXPANDER_LINES; row++)
        pressed[row] |= ((rows_to_gnd >> row) & 1) << SIEMENS_KEYPAD_EXPANDER_GND_COLUMN;
}

static void siemens_keypad_expander_queue_changes(SiemensKeypadExpanderState *s, const uint16_t *pressed) {
    for (uint32_t row = 0; row < SIEMENS_KEYPAD_EXPANDER_LINES; row++) {
        uint32_t changed = s->pressed[row] ^ pressed[row];
        for (uint32_t column = 0; column <= SIEMENS_KEYPAD_EXPANDER_GND_COLUMN; column++) {
            if ((changed & BIT(column)) == 0)
                continue;
            bool is_pressed = (pressed[row] & BIT(column)) != 0;
            uint8_t kind = is_pressed ?
                SIEMENS_KEYPAD_EXPANDER_EVENTS_PRESSED_PRESSED : SIEMENS_KEYPAD_EXPANDER_EVENTS_PRESSED_RELEASED;
            siemens_keypad_expander_queue_event(s, kind | siemens_keypad_expander_get_code(row, column));
        }
        s->pressed[row] = pressed[row];
    }
}

static void siemens_keypad_expander_scan(void *opaque) {
    SiemensKeypadExpanderState *s = opaque;
    uint16_t pressed[SIEMENS_KEYPAD_EXPANDER_LINES] = { 0 };

    siemens_keypad_expander_read_keys(s, pressed);

    bool was_empty = s->queue_count == 0;
    siemens_keypad_expander_queue_changes(s, pressed);
    if (was_empty)
        siemens_keypad_expander_update_int(s);

    bool is_any_pressed = false;
    for (uint32_t row = 0; row < SIEMENS_KEYPAD_EXPANDER_LINES; row++)
        is_any_pressed |= pressed[row] != 0;
    if (is_any_pressed)
        timer_mod(s->scan_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SIEMENS_KEYPAD_EXPANDER_RESCAN_NS);
}

static void siemens_keypad_expander_schedule_scan(SiemensKeypadExpanderState *s) {
    if (!s->is_reset_released || s->is_scanning || timer_pending(s->scan_timer))
        return;
    timer_mod(s->scan_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SIEMENS_KEYPAD_EXPANDER_DEBOUNCE_NS);
}

static void siemens_keypad_expander_handle_px_in(void *opaque, int _n, int level) {
    SiemensKeypadExpanderInput *input = opaque;
    SiemensKeypadExpanderState *s = input->expander;
    uint8_t high_inputs = deposit32(s->high_inputs, input->index, 1, level != 0);

    if (high_inputs == s->high_inputs)
        return;
    s->high_inputs = high_inputs;
    siemens_keypad_expander_schedule_scan(s);
}

static void siemens_keypad_expander_clear(SiemensKeypadExpanderState *s) {
    timer_del(s->scan_timer);
    memset(s->pressed, 0, sizeof(s->pressed));
    s->queue_count = 0;
    siemens_keypad_expander_update_int(s);
}

static void siemens_keypad_expander_start(SiemensKeypadExpanderState *s) {
    siemens_keypad_expander_drive_columns(s, SIEMENS_KEYPAD_EXPANDER_ALL_LINES);
    siemens_keypad_expander_schedule_scan(s);
}

static void siemens_keypad_expander_handle_px_reset(void *opaque, int _n, int level) {
    SiemensKeypadExpanderState *s = opaque;
    bool is_reset_released = level != 0;

    if (is_reset_released == s->is_reset_released)
        return;
    s->is_reset_released = is_reset_released;
    siemens_keypad_expander_clear(s);
    if (is_reset_released) {
        siemens_keypad_expander_start(s);
    } else {
        siemens_keypad_expander_drive_columns(s, 0);
    }
}

static void siemens_keypad_expander_start_read(SiemensKeypadExpanderState *s) {
    memset(s->read_buffer, 0, sizeof(s->read_buffer));
    s->read_index = 0;

    switch (s->reg) {
        case SIEMENS_KEYPAD_EXPANDER_STATUS:
            if (s->queue_count != 0)
                s->read_buffer[0] = SIEMENS_KEYPAD_EXPANDER_STATUS_EVENTS_PENDING;
            break;
        case SIEMENS_KEYPAD_EXPANDER_EVENTS:
            memcpy(s->read_buffer, s->queue, s->queue_count);
            s->queue_count = 0;
            siemens_keypad_expander_update_int(s);
            break;
    }
}

static int siemens_keypad_expander_event(I2CSlave *i2c, enum i2c_event event) {
    SiemensKeypadExpanderState *s = SIEMENS_KEYPAD_EXPANDER(i2c);

    switch (event) {
        case I2C_START_SEND:
        case I2C_START_SEND_ASYNC:
            s->write_index = 0;
            break;
        case I2C_START_RECV:
            siemens_keypad_expander_start_read(s);
            break;
        case I2C_FINISH:
        case I2C_NACK:
            break;
    }
    return 0;
}

static uint8_t siemens_keypad_expander_recv(I2CSlave *i2c) {
    SiemensKeypadExpanderState *s = SIEMENS_KEYPAD_EXPANDER(i2c);
    uint8_t data = s->read_index < ARRAY_SIZE(s->read_buffer) ? s->read_buffer[s->read_index] : 0;

    DEBUG_IO_READ(&s->dbg, s->reg, 1, data);
    s->read_index++;
    return data;
}

static int siemens_keypad_expander_send(I2CSlave *i2c, uint8_t data) {
    SiemensKeypadExpanderState *s = SIEMENS_KEYPAD_EXPANDER(i2c);

    if (s->write_index == 0) {
        s->reg = data;
    } else {
        DEBUG_IO_WRITE(&s->dbg, s->reg, 1, data);
    }
    s->write_index++;
    return 0;
}

static void siemens_keypad_expander_reset(DeviceState *dev) {
    SiemensKeypadExpanderState *s = SIEMENS_KEYPAD_EXPANDER(dev);

    s->reg = 0;
    s->read_index = 0;
    s->write_index = 0;
    siemens_keypad_expander_clear(s);
    if (s->is_reset_released)
        siemens_keypad_expander_start(s);
}

static void siemens_keypad_expander_realize(DeviceState *dev, Error **_errp) {
    SiemensKeypadExpanderState *s = SIEMENS_KEYPAD_EXPANDER(dev);

    debug_channel_init_device_regs(&s->dbg, dev, &siemens_keypad_expander_debug_regs);
    s->scan_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, siemens_keypad_expander_scan, s);
}

static void siemens_keypad_expander_unrealize(DeviceState *dev) {
    SiemensKeypadExpanderState *s = SIEMENS_KEYPAD_EXPANDER(dev);

    timer_free(s->scan_timer);
    debug_channel_cleanup(&s->dbg);
}

static void siemens_keypad_expander_init(Object *obj) {
    SiemensKeypadExpanderState *s = SIEMENS_KEYPAD_EXPANDER(obj);
    DeviceState *dev = DEVICE(obj);

    s->high_inputs = SIEMENS_KEYPAD_EXPANDER_ALL_LINES;
    s->is_reset_released = true;
    for (uint32_t i = 0; i < SIEMENS_KEYPAD_EXPANDER_LINES; i++) {
        g_autofree char *in_name = g_strdup_printf("PX_IN%u", i);
        g_autofree char *out_name = g_strdup_printf("PX_OUT%u", i);

        s->px_in[i] = (SiemensKeypadExpanderInput) { s, i };
        qdev_init_gpio_in_named_with_opaque(dev, siemens_keypad_expander_handle_px_in, &s->px_in[i], in_name, 1);
        qdev_init_gpio_out_named(dev, &s->px_out[i], out_name, 1);
    }
    qdev_init_gpio_in_named(dev, siemens_keypad_expander_handle_px_reset, "PX_RESET", 1);
    qdev_init_gpio_out_named(dev, &s->int_out, "PX_INT_OUT", 1);
}

static void siemens_keypad_expander_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    device_class_set_legacy_reset(dc, siemens_keypad_expander_reset);
    dc->realize = siemens_keypad_expander_realize;
    dc->unrealize = siemens_keypad_expander_unrealize;
    k->event = siemens_keypad_expander_event;
    k->recv = siemens_keypad_expander_recv;
    k->send = siemens_keypad_expander_send;
}

static const TypeInfo siemens_keypad_expander_info = {
    .name = TYPE_SIEMENS_KEYPAD_EXPANDER,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(SiemensKeypadExpanderState),
    .instance_init = siemens_keypad_expander_init,
    .class_init = siemens_keypad_expander_class_init,
};

static void siemens_keypad_expander_register_types(void) {
    type_register_static(&siemens_keypad_expander_info);
}

type_init(siemens_keypad_expander_register_types)
