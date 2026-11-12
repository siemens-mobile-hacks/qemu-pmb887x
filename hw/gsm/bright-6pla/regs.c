#include "qemu/osdep.h"
#include "regs.h"

static const DebugRegField bright_6pla_control_r0_fields[] = {
    { "DATA",   BRIGHT_6PLA_CONTROL_R0_DATA,    BRIGHT_6PLA_CONTROL_R0_DATA_SHIFT,  NULL,   0 },
};

static const DebugRegField bright_6pla_channel_fields[] = {
    { "CONTROL",    BRIGHT_6PLA_CHANNEL_CONTROL,    BRIGHT_6PLA_CHANNEL_CONTROL_SHIFT,      NULL,   0 },
    { "RECEIVE",    BRIGHT_6PLA_CHANNEL_RECEIVE,    BRIGHT_6PLA_CHANNEL_RECEIVE_SHIFT,      NULL,   0 },
    { "FREQUENCY",  BRIGHT_6PLA_CHANNEL_FREQUENCY,  BRIGHT_6PLA_CHANNEL_FREQUENCY_SHIFT,    NULL,   0 },
    { "BAND",       BRIGHT_6PLA_CHANNEL_BAND,       BRIGHT_6PLA_CHANNEL_BAND_SHIFT,         NULL,   0 },
};

static const DebugRegField bright_6pla_gain_fields[] = {
    { "LNA",    BRIGHT_6PLA_GAIN_LNA,   BRIGHT_6PLA_GAIN_LNA_SHIFT, NULL,   0 },
    { "PGA",    BRIGHT_6PLA_GAIN_PGA,   BRIGHT_6PLA_GAIN_PGA_SHIFT, NULL,   0 },
};

static const DebugRegField bright_6pla_control_r4_fields[] = {
    { "DATA",   BRIGHT_6PLA_CONTROL_R4_DATA,    BRIGHT_6PLA_CONTROL_R4_DATA_SHIFT,  NULL,   0 },
};

static const DebugRegField bright_6pla_control_r5_fields[] = {
    { "DATA",   BRIGHT_6PLA_CONTROL_R5_DATA,    BRIGHT_6PLA_CONTROL_R5_DATA_SHIFT,  NULL,   0 },
};

static const DebugRegField bright_6pla_control_r6_fields[] = {
    { "DATA",   BRIGHT_6PLA_CONTROL_R6_DATA,    BRIGHT_6PLA_CONTROL_R6_DATA_SHIFT,  NULL,   0 },
};

static const DebugRegField bright_6pla_control_r7_fields[] = {
    { "DATA",   BRIGHT_6PLA_CONTROL_R7_DATA,    BRIGHT_6PLA_CONTROL_R7_DATA_SHIFT,  NULL,   0 },
};

static const DebugReg bright_6pla_regs[] = {
    { "CONTROL_R0", BRIGHT_6PLA_CONTROL_R0, bright_6pla_control_r0_fields,  ARRAY_SIZE(bright_6pla_control_r0_fields) },
    { "CHANNEL",    BRIGHT_6PLA_CHANNEL,    bright_6pla_channel_fields,     ARRAY_SIZE(bright_6pla_channel_fields) },
    { "GAIN",       BRIGHT_6PLA_GAIN,       bright_6pla_gain_fields,        ARRAY_SIZE(bright_6pla_gain_fields) },
    { "CONTROL_R4", BRIGHT_6PLA_CONTROL_R4, bright_6pla_control_r4_fields,  ARRAY_SIZE(bright_6pla_control_r4_fields) },
    { "CONTROL_R5", BRIGHT_6PLA_CONTROL_R5, bright_6pla_control_r5_fields,  ARRAY_SIZE(bright_6pla_control_r5_fields) },
    { "CONTROL_R6", BRIGHT_6PLA_CONTROL_R6, bright_6pla_control_r6_fields,  ARRAY_SIZE(bright_6pla_control_r6_fields) },
    { "CONTROL_R7", BRIGHT_6PLA_CONTROL_R7, bright_6pla_control_r7_fields,  ARRAY_SIZE(bright_6pla_control_r7_fields) },
};

const DebugRegs bright_6pla_debug_regs = {
    .regs = bright_6pla_regs,
    .count = ARRAY_SIZE(bright_6pla_regs),
    .reg_size = 1,
    .addr_digits = 2,
};
