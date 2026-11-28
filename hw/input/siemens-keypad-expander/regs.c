#include "qemu/osdep.h"
#include "regs.h"

static const DebugRegValue siemens_keypad_expander_events_fields_pressed_values[] = {
    { "RELEASED",   SIEMENS_KEYPAD_EXPANDER_EVENTS_PRESSED_RELEASED },
    { "PRESSED",    SIEMENS_KEYPAD_EXPANDER_EVENTS_PRESSED_PRESSED },
};

static const DebugRegField siemens_keypad_expander_events_fields[] = {
    { "CODE",       SIEMENS_KEYPAD_EXPANDER_EVENTS_CODE,    SIEMENS_KEYPAD_EXPANDER_EVENTS_CODE_SHIFT,      NULL,                                                   0 },
    { "PRESSED",    SIEMENS_KEYPAD_EXPANDER_EVENTS_PRESSED, SIEMENS_KEYPAD_EXPANDER_EVENTS_PRESSED_SHIFT,   siemens_keypad_expander_events_fields_pressed_values,   ARRAY_SIZE(siemens_keypad_expander_events_fields_pressed_values) },
};

static const DebugRegValue siemens_keypad_expander_status_fields_events_values[] = {
    { "EMPTY",      SIEMENS_KEYPAD_EXPANDER_STATUS_EVENTS_EMPTY },
    { "PENDING",    SIEMENS_KEYPAD_EXPANDER_STATUS_EVENTS_PENDING },
};

static const DebugRegField siemens_keypad_expander_status_fields[] = {
    { "EVENTS", SIEMENS_KEYPAD_EXPANDER_STATUS_EVENTS,  SIEMENS_KEYPAD_EXPANDER_STATUS_EVENTS_SHIFT,    siemens_keypad_expander_status_fields_events_values,    ARRAY_SIZE(siemens_keypad_expander_status_fields_events_values) },
};

static const DebugReg siemens_keypad_expander_regs[] = {
    { "EVENTS", SIEMENS_KEYPAD_EXPANDER_EVENTS, siemens_keypad_expander_events_fields,  ARRAY_SIZE(siemens_keypad_expander_events_fields) },
    { "STATUS", SIEMENS_KEYPAD_EXPANDER_STATUS, siemens_keypad_expander_status_fields,  ARRAY_SIZE(siemens_keypad_expander_status_fields) },
    { "REG_F0", SIEMENS_KEYPAD_EXPANDER_REG_F0, NULL,                                   0 },
};

const DebugRegs siemens_keypad_expander_debug_regs = {
    .regs = siemens_keypad_expander_regs,
    .count = ARRAY_SIZE(siemens_keypad_expander_regs),
    .reg_size = 1,
    .addr_digits = 2,
};
