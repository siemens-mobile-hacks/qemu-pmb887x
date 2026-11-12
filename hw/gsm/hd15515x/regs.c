#include "qemu/osdep.h"
#include "regs.h"

static const DebugRegValue hd15515x_channel_pll_fields_tx_offset_values[] = {
    { "OFFSET_80MHZ",   HD15515X_CHANNEL_PLL_TX_OFFSET_OFFSET_80MHZ },
    { "OFFSET_82MHZ",   HD15515X_CHANNEL_PLL_TX_OFFSET_OFFSET_82MHZ },
};

static const DebugRegField hd15515x_channel_pll_fields[] = {
    { "BAND",       HD15515X_CHANNEL_PLL_BAND,      HD15515X_CHANNEL_PLL_BAND_SHIFT,        NULL,                                           0 },
    { "DIVIDER",    HD15515X_CHANNEL_PLL_DIVIDER,   HD15515X_CHANNEL_PLL_DIVIDER_SHIFT,     NULL,                                           0 },
    { "RECEIVE",    HD15515X_CHANNEL_PLL_RECEIVE,   HD15515X_CHANNEL_PLL_RECEIVE_SHIFT,     NULL,                                           0 },
    { "TX_OFFSET",  HD15515X_CHANNEL_PLL_TX_OFFSET, HD15515X_CHANNEL_PLL_TX_OFFSET_SHIFT,   hd15515x_channel_pll_fields_tx_offset_values,   ARRAY_SIZE(hd15515x_channel_pll_fields_tx_offset_values) },
    { "MODE",       HD15515X_CHANNEL_PLL_MODE,      HD15515X_CHANNEL_PLL_MODE_SHIFT,        NULL,                                           0 },
};

static const DebugRegField hd15515x_gain_control_fields[] = {
    { "PGA_GAIN",   HD15515X_GAIN_CONTROL_PGA_GAIN,     HD15515X_GAIN_CONTROL_PGA_GAIN_SHIFT,   NULL,   0 },
    { "FRONT_END",  HD15515X_GAIN_CONTROL_FRONT_END,    HD15515X_GAIN_CONTROL_FRONT_END_SHIFT,  NULL,   0 },
    { "DATA",       HD15515X_GAIN_CONTROL_DATA,         HD15515X_GAIN_CONTROL_DATA_SHIFT,       NULL,   0 },
};

static const DebugRegField hd15515x_control_r3_fields[] = {
    { "DATA",   HD15515X_CONTROL_R3_DATA,   HD15515X_CONTROL_R3_DATA_SHIFT, NULL,   0 },
};

static const DebugRegField hd15515x_control_r4_fields[] = {
    { "DATA",   HD15515X_CONTROL_R4_DATA,   HD15515X_CONTROL_R4_DATA_SHIFT, NULL,   0 },
};

static const DebugRegField hd15515x_config_r5_fields[] = {
    { "DATA",   HD15515X_CONFIG_R5_DATA,    HD15515X_CONFIG_R5_DATA_SHIFT,  NULL,   0 },
};

static const DebugRegField hd15515x_calibration_r6_fields[] = {
    { "SLOT",   HD15515X_CALIBRATION_R6_SLOT,   HD15515X_CALIBRATION_R6_SLOT_SHIFT, NULL,   0 },
    { "DATA",   HD15515X_CALIBRATION_R6_DATA,   HD15515X_CALIBRATION_R6_DATA_SHIFT, NULL,   0 },
};

static const DebugRegField hd15515x_control_r7_fields[] = {
    { "DATA",   HD15515X_CONTROL_R7_DATA,   HD15515X_CONTROL_R7_DATA_SHIFT, NULL,   0 },
};

static const DebugReg hd15515x_regs[] = {
    { "CHANNEL_PLL",    HD15515X_CHANNEL_PLL,       hd15515x_channel_pll_fields,    ARRAY_SIZE(hd15515x_channel_pll_fields) },
    { "GAIN_CONTROL",   HD15515X_GAIN_CONTROL,      hd15515x_gain_control_fields,   ARRAY_SIZE(hd15515x_gain_control_fields) },
    { "CONTROL_R3",     HD15515X_CONTROL_R3,        hd15515x_control_r3_fields,     ARRAY_SIZE(hd15515x_control_r3_fields) },
    { "CONTROL_R4",     HD15515X_CONTROL_R4,        hd15515x_control_r4_fields,     ARRAY_SIZE(hd15515x_control_r4_fields) },
    { "CONFIG_R5",      HD15515X_CONFIG_R5,         hd15515x_config_r5_fields,      ARRAY_SIZE(hd15515x_config_r5_fields) },
    { "CALIBRATION_R6", HD15515X_CALIBRATION_R6,    hd15515x_calibration_r6_fields, ARRAY_SIZE(hd15515x_calibration_r6_fields) },
    { "CONTROL_R7",     HD15515X_CONTROL_R7,        hd15515x_control_r7_fields,     ARRAY_SIZE(hd15515x_control_r7_fields) },
};

const DebugRegs hd15515x_debug_regs = {
    .regs = hd15515x_regs,
    .count = ARRAY_SIZE(hd15515x_regs),
    .reg_size = 1,
    .addr_digits = 2,
};
