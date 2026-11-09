#include "qemu/osdep.h"
#include "regs.h"

static const DebugRegValue lm4946_mode_fields_mc_values[] = {
    { "ALL_SHUTDOWN",       LM4946_MODE_MC_ALL_SHUTDOWN },
    { "MONO_IN_TO_SPK",     LM4946_MODE_MC_MONO_IN_TO_SPK },
    { "MONO_IN_TO_HP",      LM4946_MODE_MC_MONO_IN_TO_HP },
    { "LINE_IN_TO_SPK",     LM4946_MODE_MC_LINE_IN_TO_SPK },
    { "LINE_IN_TO_HP",      LM4946_MODE_MC_LINE_IN_TO_HP },
    { "LINE_MONO_TO_SPK",   LM4946_MODE_MC_LINE_MONO_TO_SPK },
    { "LINE_MONO_TO_HP",    LM4946_MODE_MC_LINE_MONO_TO_HP },
    { "LINE_IN_TO_SPK_HP",  LM4946_MODE_MC_LINE_IN_TO_SPK_HP },
};

static const DebugRegField lm4946_mode_fields[] = {
    { "MC",     LM4946_MODE_MC,     LM4946_MODE_MC_SHIFT,   lm4946_mode_fields_mc_values,   ARRAY_SIZE(lm4946_mode_fields_mc_values) },
    { "OCL",    LM4946_MODE_OCL,    LM4946_MODE_OCL_SHIFT,  NULL,                           0 },
};

static const DebugRegValue lm4946_n3d_fields_narrow_values[] = {
    { "WIDE",   LM4946_N3D_NARROW_WIDE },
    { "NARROW", LM4946_N3D_NARROW_NARROW },
};

static const DebugRegValue lm4946_n3d_fields_level_values[] = {
    { "LOW",    LM4946_N3D_LEVEL_LOW },
    { "MEDIUM", LM4946_N3D_LEVEL_MEDIUM },
    { "HIGH",   LM4946_N3D_LEVEL_HIGH },
    { "MAX",    LM4946_N3D_LEVEL_MAX },
};

static const DebugRegField lm4946_n3d_fields[] = {
    { "EN",     LM4946_N3D_EN,      LM4946_N3D_EN_SHIFT,        NULL,                               0 },
    { "NARROW", LM4946_N3D_NARROW,  LM4946_N3D_NARROW_SHIFT,    lm4946_n3d_fields_narrow_values,    ARRAY_SIZE(lm4946_n3d_fields_narrow_values) },
    { "LEVEL",  LM4946_N3D_LEVEL,   LM4946_N3D_LEVEL_SHIFT,     lm4946_n3d_fields_level_values,     ARRAY_SIZE(lm4946_n3d_fields_level_values) },
};

static const DebugRegField lm4946_mono_vol_fields[] = {
    { "VOL",    LM4946_MONO_VOL_VOL,    LM4946_MONO_VOL_VOL_SHIFT,  NULL,   0 },
};

static const DebugRegField lm4946_left_vol_fields[] = {
    { "VOL",    LM4946_LEFT_VOL_VOL,    LM4946_LEFT_VOL_VOL_SHIFT,  NULL,   0 },
};

static const DebugRegField lm4946_right_vol_fields[] = {
    { "VOL",    LM4946_RIGHT_VOL_VOL,   LM4946_RIGHT_VOL_VOL_SHIFT, NULL,   0 },
};

static const DebugReg lm4946_regs[] = {
    { "MODE",       LM4946_MODE,        lm4946_mode_fields,         ARRAY_SIZE(lm4946_mode_fields) },
    { "N3D",        LM4946_N3D,         lm4946_n3d_fields,          ARRAY_SIZE(lm4946_n3d_fields) },
    { "MONO_VOL",   LM4946_MONO_VOL,    lm4946_mono_vol_fields,     ARRAY_SIZE(lm4946_mono_vol_fields) },
    { "LEFT_VOL",   LM4946_LEFT_VOL,    lm4946_left_vol_fields,     ARRAY_SIZE(lm4946_left_vol_fields) },
    { "RIGHT_VOL",  LM4946_RIGHT_VOL,   lm4946_right_vol_fields,    ARRAY_SIZE(lm4946_right_vol_fields) },
};

const DebugRegs lm4946_debug_regs = {
    .regs = lm4946_regs,
    .count = ARRAY_SIZE(lm4946_regs),
    .reg_size = 1,
    .addr_digits = 2,
};
