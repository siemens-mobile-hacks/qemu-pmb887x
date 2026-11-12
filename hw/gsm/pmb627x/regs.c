#include "qemu/osdep.h"
#include "regs.h"

static const DebugRegField pmb627x_channel1_fields[] = {
    { "FRACTION",   PMB627X_CHANNEL1_FRACTION,  PMB627X_CHANNEL1_FRACTION_SHIFT,    NULL,   0 },
};

static const DebugRegValue pmb627x_channel2_fields_power_mode_values[] = {
    { "ALL_OFF",    PMB627X_CHANNEL2_POWER_MODE_ALL_OFF },
    { "UNUSED",     PMB627X_CHANNEL2_POWER_MODE_UNUSED },
    { "PLL_TEST",   PMB627X_CHANNEL2_POWER_MODE_PLL_TEST },
    { "ALL_ON",     PMB627X_CHANNEL2_POWER_MODE_ALL_ON },
};

static const DebugRegValue pmb627x_channel2_fields_trx_values[] = {
    { "RX", PMB627X_CHANNEL2_TRX_RX },
    { "TX", PMB627X_CHANNEL2_TRX_TX },
};

static const DebugRegValue pmb627x_channel2_fields_band_values[] = {
    { "GSM850",     PMB627X_CHANNEL2_BAND_GSM850 },
    { "GSM900",     PMB627X_CHANNEL2_BAND_GSM900 },
    { "DCS1800",    PMB627X_CHANNEL2_BAND_DCS1800 },
    { "PCS1900",    PMB627X_CHANNEL2_BAND_PCS1900 },
};

static const DebugRegField pmb627x_channel2_fields[] = {
    { "FRACTION",   PMB627X_CHANNEL2_FRACTION,      PMB627X_CHANNEL2_FRACTION_SHIFT,    NULL,                                       0 },
    { "INTEGER",    PMB627X_CHANNEL2_INTEGER,       PMB627X_CHANNEL2_INTEGER_SHIFT,     NULL,                                       0 },
    { "POWER_MODE", PMB627X_CHANNEL2_POWER_MODE,    PMB627X_CHANNEL2_POWER_MODE_SHIFT,  pmb627x_channel2_fields_power_mode_values,  ARRAY_SIZE(pmb627x_channel2_fields_power_mode_values) },
    { "TRX",        PMB627X_CHANNEL2_TRX,           PMB627X_CHANNEL2_TRX_SHIFT,         pmb627x_channel2_fields_trx_values,         ARRAY_SIZE(pmb627x_channel2_fields_trx_values) },
    { "BAND",       PMB627X_CHANNEL2_BAND,          PMB627X_CHANNEL2_BAND_SHIFT,        pmb627x_channel2_fields_band_values,        ARRAY_SIZE(pmb627x_channel2_fields_band_values) },
};

static const DebugRegValue pmb627x_rxtx_fields_rxgain_values[] = {
    { "LOW",            PMB627X_RXTX_RXGAIN_LOW },
    { "MEDIUM",         PMB627X_RXTX_RXGAIN_MEDIUM },
    { "UNSUPPORTED",    PMB627X_RXTX_RXGAIN_UNSUPPORTED },
    { "HIGH",           PMB627X_RXTX_RXGAIN_HIGH },
};

static const DebugRegField pmb627x_rxtx_fields[] = {
    { "RXGAIN", PMB627X_RXTX_RXGAIN,    PMB627X_RXTX_RXGAIN_SHIFT,  pmb627x_rxtx_fields_rxgain_values,  ARRAY_SIZE(pmb627x_rxtx_fields_rxgain_values) },
    { "RXCORR", PMB627X_RXTX_RXCORR,    PMB627X_RXTX_RXCORR_SHIFT,  NULL,                               0 },
    { "RXCM",   PMB627X_RXTX_RXCM,      PMB627X_RXTX_RXCM_SHIFT,    NULL,                               0 },
    { "RXGS0",  PMB627X_RXTX_RXGS0,     PMB627X_RXTX_RXGS0_SHIFT,   NULL,                               0 },
    { "RXGS1",  PMB627X_RXTX_RXGS1,     PMB627X_RXTX_RXGS1_SHIFT,   NULL,                               0 },
    { "RXGS2",  PMB627X_RXTX_RXGS2,     PMB627X_RXTX_RXGS2_SHIFT,   NULL,                               0 },
    { "RXGS3",  PMB627X_RXTX_RXGS3,     PMB627X_RXTX_RXGS3_SHIFT,   NULL,                               0 },
    { "OFC",    PMB627X_RXTX_OFC,       PMB627X_RXTX_OFC_SHIFT,     NULL,                               0 },
};

static const DebugRegValue pmb627x_xo_init1_fields_crystal_size_values[] = {
    { "10PF",   PMB627X_XO_INIT1_CRYSTAL_SIZE_10PF },
    { "8PF",    PMB627X_XO_INIT1_CRYSTAL_SIZE_8PF },
};

static const DebugRegField pmb627x_xo_init1_fields[] = {
    { "XOMODE0",        PMB627X_XO_INIT1_XOMODE0,       PMB627X_XO_INIT1_XOMODE0_SHIFT,         NULL,                                           0 },
    { "XOMODE1",        PMB627X_XO_INIT1_XOMODE1,       PMB627X_XO_INIT1_XOMODE1_SHIFT,         NULL,                                           0 },
    { "XOMODE3",        PMB627X_XO_INIT1_XOMODE3,       PMB627X_XO_INIT1_XOMODE3_SHIFT,         NULL,                                           0 },
    { "XOSETUP",        PMB627X_XO_INIT1_XOSETUP,       PMB627X_XO_INIT1_XOSETUP_SHIFT,         NULL,                                           0 },
    { "XOCAL",          PMB627X_XO_INIT1_XOCAL,         PMB627X_XO_INIT1_XOCAL_SHIFT,           NULL,                                           0 },
    { "CRYSTAL_SIZE",   PMB627X_XO_INIT1_CRYSTAL_SIZE,  PMB627X_XO_INIT1_CRYSTAL_SIZE_SHIFT,    pmb627x_xo_init1_fields_crystal_size_values,    ARRAY_SIZE(pmb627x_xo_init1_fields_crystal_size_values) },
};

static const DebugRegField pmb627x_xo_init3_fields[] = {
    { "VALUE",  PMB627X_XO_INIT3_VALUE, PMB627X_XO_INIT3_VALUE_SHIFT,   NULL,   0 },
};

static const DebugRegField pmb627x_xo_init2_fields[] = {
    { "ALPHA",  PMB627X_XO_INIT2_ALPHA, PMB627X_XO_INIT2_ALPHA_SHIFT,   NULL,   0 },
    { "GAMMA",  PMB627X_XO_INIT2_GAMMA, PMB627X_XO_INIT2_GAMMA_SHIFT,   NULL,   0 },
};

static const DebugRegField pmb627x_xo_tune_fields[] = {
    { "AFC",    PMB627X_XO_TUNE_AFC,    PMB627X_XO_TUNE_AFC_SHIFT,  NULL,   0 },
};

static const DebugReg pmb627x_regs[] = {
    { "CHANNEL1",   PMB627X_CHANNEL1,   pmb627x_channel1_fields,    ARRAY_SIZE(pmb627x_channel1_fields) },
    { "CHANNEL2",   PMB627X_CHANNEL2,   pmb627x_channel2_fields,    ARRAY_SIZE(pmb627x_channel2_fields) },
    { "RXTX",       PMB627X_RXTX,       pmb627x_rxtx_fields,        ARRAY_SIZE(pmb627x_rxtx_fields) },
    { "XO_INIT1",   PMB627X_XO_INIT1,   pmb627x_xo_init1_fields,    ARRAY_SIZE(pmb627x_xo_init1_fields) },
    { "XO_INIT3",   PMB627X_XO_INIT3,   pmb627x_xo_init3_fields,    ARRAY_SIZE(pmb627x_xo_init3_fields) },
    { "XO_INIT2",   PMB627X_XO_INIT2,   pmb627x_xo_init2_fields,    ARRAY_SIZE(pmb627x_xo_init2_fields) },
    { "XO_TUNE",    PMB627X_XO_TUNE,    pmb627x_xo_tune_fields,     ARRAY_SIZE(pmb627x_xo_tune_fields) },
};

const DebugRegs pmb627x_debug_regs = {
    .regs = pmb627x_regs,
    .count = ARRAY_SIZE(pmb627x_regs),
    .reg_size = 1,
    .addr_digits = 2,
};
