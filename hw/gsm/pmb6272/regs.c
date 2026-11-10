#include "qemu/osdep.h"
#include "regs.h"

static const DebugRegField pmb6272_channel1_fields[] = {
    { "FRACTION",   PMB6272_CHANNEL1_FRACTION,  PMB6272_CHANNEL1_FRACTION_SHIFT,    NULL,   0 },
};

static const DebugRegValue pmb6272_channel2_fields_power_mode_values[] = {
    { "ALL_OFF",    PMB6272_CHANNEL2_POWER_MODE_ALL_OFF },
    { "UNUSED",     PMB6272_CHANNEL2_POWER_MODE_UNUSED },
    { "PLL_TEST",   PMB6272_CHANNEL2_POWER_MODE_PLL_TEST },
    { "ALL_ON",     PMB6272_CHANNEL2_POWER_MODE_ALL_ON },
};

static const DebugRegValue pmb6272_channel2_fields_trx_values[] = {
    { "RX", PMB6272_CHANNEL2_TRX_RX },
    { "TX", PMB6272_CHANNEL2_TRX_TX },
};

static const DebugRegValue pmb6272_channel2_fields_band_values[] = {
    { "GSM850",     PMB6272_CHANNEL2_BAND_GSM850 },
    { "GSM900",     PMB6272_CHANNEL2_BAND_GSM900 },
    { "DCS1800",    PMB6272_CHANNEL2_BAND_DCS1800 },
    { "PCS1900",    PMB6272_CHANNEL2_BAND_PCS1900 },
};

static const DebugRegField pmb6272_channel2_fields[] = {
    { "FRACTION",   PMB6272_CHANNEL2_FRACTION,      PMB6272_CHANNEL2_FRACTION_SHIFT,    NULL,                                       0 },
    { "INTEGER",    PMB6272_CHANNEL2_INTEGER,       PMB6272_CHANNEL2_INTEGER_SHIFT,     NULL,                                       0 },
    { "POWER_MODE", PMB6272_CHANNEL2_POWER_MODE,    PMB6272_CHANNEL2_POWER_MODE_SHIFT,  pmb6272_channel2_fields_power_mode_values,  ARRAY_SIZE(pmb6272_channel2_fields_power_mode_values) },
    { "TRX",        PMB6272_CHANNEL2_TRX,           PMB6272_CHANNEL2_TRX_SHIFT,         pmb6272_channel2_fields_trx_values,         ARRAY_SIZE(pmb6272_channel2_fields_trx_values) },
    { "BAND",       PMB6272_CHANNEL2_BAND,          PMB6272_CHANNEL2_BAND_SHIFT,        pmb6272_channel2_fields_band_values,        ARRAY_SIZE(pmb6272_channel2_fields_band_values) },
};

static const DebugRegValue pmb6272_rxtx_fields_rxgain_values[] = {
    { "LOW",            PMB6272_RXTX_RXGAIN_LOW },
    { "MEDIUM",         PMB6272_RXTX_RXGAIN_MEDIUM },
    { "UNSUPPORTED",    PMB6272_RXTX_RXGAIN_UNSUPPORTED },
    { "HIGH",           PMB6272_RXTX_RXGAIN_HIGH },
};

static const DebugRegField pmb6272_rxtx_fields[] = {
    { "RXGAIN", PMB6272_RXTX_RXGAIN,    PMB6272_RXTX_RXGAIN_SHIFT,  pmb6272_rxtx_fields_rxgain_values,  ARRAY_SIZE(pmb6272_rxtx_fields_rxgain_values) },
    { "RXCORR", PMB6272_RXTX_RXCORR,    PMB6272_RXTX_RXCORR_SHIFT,  NULL,                               0 },
    { "RXCM",   PMB6272_RXTX_RXCM,      PMB6272_RXTX_RXCM_SHIFT,    NULL,                               0 },
    { "RXGS0",  PMB6272_RXTX_RXGS0,     PMB6272_RXTX_RXGS0_SHIFT,   NULL,                               0 },
    { "RXGS1",  PMB6272_RXTX_RXGS1,     PMB6272_RXTX_RXGS1_SHIFT,   NULL,                               0 },
    { "RXGS2",  PMB6272_RXTX_RXGS2,     PMB6272_RXTX_RXGS2_SHIFT,   NULL,                               0 },
    { "RXGS3",  PMB6272_RXTX_RXGS3,     PMB6272_RXTX_RXGS3_SHIFT,   NULL,                               0 },
    { "OFC",    PMB6272_RXTX_OFC,       PMB6272_RXTX_OFC_SHIFT,     NULL,                               0 },
};

static const DebugRegValue pmb6272_xo_init1_fields_crystal_size_values[] = {
    { "10PF",   PMB6272_XO_INIT1_CRYSTAL_SIZE_10PF },
    { "8PF",    PMB6272_XO_INIT1_CRYSTAL_SIZE_8PF },
};

static const DebugRegField pmb6272_xo_init1_fields[] = {
    { "XOMODE0",        PMB6272_XO_INIT1_XOMODE0,       PMB6272_XO_INIT1_XOMODE0_SHIFT,         NULL,                                           0 },
    { "XOMODE1",        PMB6272_XO_INIT1_XOMODE1,       PMB6272_XO_INIT1_XOMODE1_SHIFT,         NULL,                                           0 },
    { "XOMODE3",        PMB6272_XO_INIT1_XOMODE3,       PMB6272_XO_INIT1_XOMODE3_SHIFT,         NULL,                                           0 },
    { "XOSETUP",        PMB6272_XO_INIT1_XOSETUP,       PMB6272_XO_INIT1_XOSETUP_SHIFT,         NULL,                                           0 },
    { "XOCAL",          PMB6272_XO_INIT1_XOCAL,         PMB6272_XO_INIT1_XOCAL_SHIFT,           NULL,                                           0 },
    { "CRYSTAL_SIZE",   PMB6272_XO_INIT1_CRYSTAL_SIZE,  PMB6272_XO_INIT1_CRYSTAL_SIZE_SHIFT,    pmb6272_xo_init1_fields_crystal_size_values,    ARRAY_SIZE(pmb6272_xo_init1_fields_crystal_size_values) },
};

static const DebugRegField pmb6272_xo_init3_fields[] = {
    { "VALUE",  PMB6272_XO_INIT3_VALUE, PMB6272_XO_INIT3_VALUE_SHIFT,   NULL,   0 },
};

static const DebugRegField pmb6272_xo_init2_fields[] = {
    { "ALPHA",  PMB6272_XO_INIT2_ALPHA, PMB6272_XO_INIT2_ALPHA_SHIFT,   NULL,   0 },
    { "GAMMA",  PMB6272_XO_INIT2_GAMMA, PMB6272_XO_INIT2_GAMMA_SHIFT,   NULL,   0 },
};

static const DebugRegField pmb6272_xo_tune_fields[] = {
    { "AFC",    PMB6272_XO_TUNE_AFC,    PMB6272_XO_TUNE_AFC_SHIFT,  NULL,   0 },
};

static const DebugReg pmb6272_regs[] = {
    { "CHANNEL1",   PMB6272_CHANNEL1,   pmb6272_channel1_fields,    ARRAY_SIZE(pmb6272_channel1_fields) },
    { "CHANNEL2",   PMB6272_CHANNEL2,   pmb6272_channel2_fields,    ARRAY_SIZE(pmb6272_channel2_fields) },
    { "RXTX",       PMB6272_RXTX,       pmb6272_rxtx_fields,        ARRAY_SIZE(pmb6272_rxtx_fields) },
    { "XO_INIT1",   PMB6272_XO_INIT1,   pmb6272_xo_init1_fields,    ARRAY_SIZE(pmb6272_xo_init1_fields) },
    { "XO_INIT3",   PMB6272_XO_INIT3,   pmb6272_xo_init3_fields,    ARRAY_SIZE(pmb6272_xo_init3_fields) },
    { "XO_INIT2",   PMB6272_XO_INIT2,   pmb6272_xo_init2_fields,    ARRAY_SIZE(pmb6272_xo_init2_fields) },
    { "XO_TUNE",    PMB6272_XO_TUNE,    pmb6272_xo_tune_fields,     ARRAY_SIZE(pmb6272_xo_tune_fields) },
};

const DebugRegs pmb6272_debug_regs = {
    .regs = pmb6272_regs,
    .count = ARRAY_SIZE(pmb6272_regs),
    .reg_size = 1,
    .addr_digits = 2,
};
