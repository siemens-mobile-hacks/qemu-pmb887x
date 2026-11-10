#include "qemu/osdep.h"
#include "regs.h"

static const DebugRegField hd155153np_channel_pll_fields[] = {
    { "SYNTHESIZER",    HD155153NP_CHANNEL_PLL_SYNTHESIZER, HD155153NP_CHANNEL_PLL_SYNTHESIZER_SHIFT,   NULL,   0 },
    { "MODE",           HD155153NP_CHANNEL_PLL_MODE,        HD155153NP_CHANNEL_PLL_MODE_SHIFT,          NULL,   0 },
};

static const DebugRegField hd155153np_gain_control_fields[] = {
    { "PGA_GAIN",           HD155153NP_GAIN_CONTROL_PGA_GAIN,           HD155153NP_GAIN_CONTROL_PGA_GAIN_SHIFT,         NULL,   0 },
    { "MIXER2_LOW_GAIN",    HD155153NP_GAIN_CONTROL_MIXER2_LOW_GAIN,    HD155153NP_GAIN_CONTROL_MIXER2_LOW_GAIN_SHIFT,  NULL,   0 },
    { "MIXER1_LOW_GAIN",    HD155153NP_GAIN_CONTROL_MIXER1_LOW_GAIN,    HD155153NP_GAIN_CONTROL_MIXER1_LOW_GAIN_SHIFT,  NULL,   0 },
};

static const DebugRegField hd155153np_control_r4_fields[] = {
    { "DATA",   HD155153NP_CONTROL_R4_DATA, HD155153NP_CONTROL_R4_DATA_SHIFT,   NULL,   0 },
};

static const DebugRegField hd155153np_config_r5_fields[] = {
    { "DATA",   HD155153NP_CONFIG_R5_DATA,  HD155153NP_CONFIG_R5_DATA_SHIFT,    NULL,   0 },
};

static const DebugRegField hd155153np_calibration_r6_fields[] = {
    { "SLOT",   HD155153NP_CALIBRATION_R6_SLOT, HD155153NP_CALIBRATION_R6_SLOT_SHIFT,   NULL,   0 },
    { "DATA",   HD155153NP_CALIBRATION_R6_DATA, HD155153NP_CALIBRATION_R6_DATA_SHIFT,   NULL,   0 },
};

static const DebugRegField hd155153np_control_r7_fields[] = {
    { "DATA",   HD155153NP_CONTROL_R7_DATA, HD155153NP_CONTROL_R7_DATA_SHIFT,   NULL,   0 },
};

static const DebugReg hd155153np_regs[] = {
    { "CHANNEL_PLL",    HD155153NP_CHANNEL_PLL,     hd155153np_channel_pll_fields,      ARRAY_SIZE(hd155153np_channel_pll_fields) },
    { "GAIN_CONTROL",   HD155153NP_GAIN_CONTROL,    hd155153np_gain_control_fields,     ARRAY_SIZE(hd155153np_gain_control_fields) },
    { "CONTROL_R4",     HD155153NP_CONTROL_R4,      hd155153np_control_r4_fields,       ARRAY_SIZE(hd155153np_control_r4_fields) },
    { "CONFIG_R5",      HD155153NP_CONFIG_R5,       hd155153np_config_r5_fields,        ARRAY_SIZE(hd155153np_config_r5_fields) },
    { "CALIBRATION_R6", HD155153NP_CALIBRATION_R6,  hd155153np_calibration_r6_fields,   ARRAY_SIZE(hd155153np_calibration_r6_fields) },
    { "CONTROL_R7",     HD155153NP_CONTROL_R7,      hd155153np_control_r7_fields,       ARRAY_SIZE(hd155153np_control_r7_fields) },
};

const DebugRegs hd155153np_debug_regs = {
    .regs = hd155153np_regs,
    .count = ARRAY_SIZE(hd155153np_regs),
    .reg_size = 1,
    .addr_digits = 2,
};
