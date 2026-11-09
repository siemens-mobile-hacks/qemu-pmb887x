#include "qemu/osdep.h"
#include "regs.h"

static const DebugRegField lh15kah2_x_start_fields[] = {
    { "ADDR",   LH15KAH2_X_START_ADDR,  LH15KAH2_X_START_ADDR_SHIFT,    NULL,   0 },
};

static const DebugRegField lh15kah2_y_start_fields[] = {
    { "ADDR",   LH15KAH2_Y_START_ADDR,  LH15KAH2_Y_START_ADDR_SHIFT,    NULL,   0 },
};

static const DebugRegField lh15kah2_x_end_fields[] = {
    { "ADDR",   LH15KAH2_X_END_ADDR,    LH15KAH2_X_END_ADDR_SHIFT,  NULL,   0 },
};

static const DebugRegField lh15kah2_y_end_fields[] = {
    { "ADDR",   LH15KAH2_Y_END_ADDR,    LH15KAH2_Y_END_ADDR_SHIFT,  NULL,   0 },
};

static const DebugReg lh15kah2_regs[] = {
    { "X_START",    LH15KAH2_X_START,   lh15kah2_x_start_fields,    ARRAY_SIZE(lh15kah2_x_start_fields) },
    { "Y_START",    LH15KAH2_Y_START,   lh15kah2_y_start_fields,    ARRAY_SIZE(lh15kah2_y_start_fields) },
    { "X_END",      LH15KAH2_X_END,     lh15kah2_x_end_fields,      ARRAY_SIZE(lh15kah2_x_end_fields) },
    { "Y_END",      LH15KAH2_Y_END,     lh15kah2_y_end_fields,      ARRAY_SIZE(lh15kah2_y_end_fields) },
};

const DebugRegs lh15kah2_debug_regs = {
    .regs = lh15kah2_regs,
    .count = ARRAY_SIZE(lh15kah2_regs),
    .reg_size = 1,
    .addr_digits = 2,
};
