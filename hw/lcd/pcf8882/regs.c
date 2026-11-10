#include "qemu/osdep.h"
#include "regs.h"

static const DebugRegValue pcf8882_rddidif_fields_module_type_values[] = {
    { "MONOCHROME", PCF8882_RDDIDIF_MODULE_TYPE_MONOCHROME },
    { "COLOR",      PCF8882_RDDIDIF_MODULE_TYPE_COLOR },
};

static const DebugRegField pcf8882_rddidif_fields[] = {
    { "MODULE_CODE",    PCF8882_RDDIDIF_MODULE_CODE,    PCF8882_RDDIDIF_MODULE_CODE_SHIFT,  NULL,                                       0 },
    { "VERSION",        PCF8882_RDDIDIF_VERSION,        PCF8882_RDDIDIF_VERSION_SHIFT,      NULL,                                       0 },
    { "MODULE_TYPE",    PCF8882_RDDIDIF_MODULE_TYPE,    PCF8882_RDDIDIF_MODULE_TYPE_SHIFT,  pcf8882_rddidif_fields_module_type_values,  ARRAY_SIZE(pcf8882_rddidif_fields_module_type_values) },
    { "MANID",          PCF8882_RDDIDIF_MANID,          PCF8882_RDDIDIF_MANID_SHIFT,        NULL,                                       0 },
};

static const DebugRegField pcf8882_rddst_fields[] = {
    { "TEARING",        PCF8882_RDDST_TEARING,          PCF8882_RDDST_TEARING_SHIFT,        NULL,   0 },
    { "DISPLAY_ON",     PCF8882_RDDST_DISPLAY_ON,       PCF8882_RDDST_DISPLAY_ON_SHIFT,     NULL,   0 },
    { "ALL_OFF",        PCF8882_RDDST_ALL_OFF,          PCF8882_RDDST_ALL_OFF_SHIFT,        NULL,   0 },
    { "ALL_ON",         PCF8882_RDDST_ALL_ON,           PCF8882_RDDST_ALL_ON_SHIFT,         NULL,   0 },
    { "INVERSION",      PCF8882_RDDST_INVERSION,        PCF8882_RDDST_INVERSION_SHIFT,      NULL,   0 },
    { "SCROLL",         PCF8882_RDDST_SCROLL,           PCF8882_RDDST_SCROLL_SHIFT,         NULL,   0 },
    { "NORMAL_DISPLAY", PCF8882_RDDST_NORMAL_DISPLAY,   PCF8882_RDDST_NORMAL_DISPLAY_SHIFT, NULL,   0 },
    { "SLEEP_OUT",      PCF8882_RDDST_SLEEP_OUT,        PCF8882_RDDST_SLEEP_OUT_SHIFT,      NULL,   0 },
    { "PARTIAL",        PCF8882_RDDST_PARTIAL,          PCF8882_RDDST_PARTIAL_SHIFT,        NULL,   0 },
    { "IDLE",           PCF8882_RDDST_IDLE,             PCF8882_RDDST_IDLE_SHIFT,           NULL,   0 },
    { "PIXEL_FORMAT",   PCF8882_RDDST_PIXEL_FORMAT,     PCF8882_RDDST_PIXEL_FORMAT_SHIFT,   NULL,   0 },
    { "RGB",            PCF8882_RDDST_RGB,              PCF8882_RDDST_RGB_SHIFT,            NULL,   0 },
    { "LAO",            PCF8882_RDDST_LAO,              PCF8882_RDDST_LAO_SHIFT,            NULL,   0 },
    { "V",              PCF8882_RDDST_V,                PCF8882_RDDST_V_SHIFT,              NULL,   0 },
    { "MX",             PCF8882_RDDST_MX,               PCF8882_RDDST_MX_SHIFT,             NULL,   0 },
    { "MY",             PCF8882_RDDST_MY,               PCF8882_RDDST_MY_SHIFT,             NULL,   0 },
};

static const DebugRegField pcf8882_caset_fields[] = {
    { "XE", PCF8882_CASET_XE,   PCF8882_CASET_XE_SHIFT, NULL,   0 },
    { "XS", PCF8882_CASET_XS,   PCF8882_CASET_XS_SHIFT, NULL,   0 },
};

static const DebugRegField pcf8882_paset_fields[] = {
    { "YE", PCF8882_PASET_YE,   PCF8882_PASET_YE_SHIFT, NULL,   0 },
    { "YS", PCF8882_PASET_YS,   PCF8882_PASET_YS_SHIFT, NULL,   0 },
};

static const DebugRegField pcf8882_ptlar_fields[] = {
    { "AREA_END",   PCF8882_PTLAR_AREA_END,     PCF8882_PTLAR_AREA_END_SHIFT,   NULL,   0 },
    { "AREA_START", PCF8882_PTLAR_AREA_START,   PCF8882_PTLAR_AREA_START_SHIFT, NULL,   0 },
};

static const DebugRegField pcf8882_vscrdef_fields[] = {
    { "BOTTOM_FIXED",   PCF8882_VSCRDEF_BOTTOM_FIXED,   PCF8882_VSCRDEF_BOTTOM_FIXED_SHIFT, NULL,   0 },
    { "SCROLL_AREA",    PCF8882_VSCRDEF_SCROLL_AREA,    PCF8882_VSCRDEF_SCROLL_AREA_SHIFT,  NULL,   0 },
    { "TOP_FIXED",      PCF8882_VSCRDEF_TOP_FIXED,      PCF8882_VSCRDEF_TOP_FIXED_SHIFT,    NULL,   0 },
};

static const DebugRegValue pcf8882_madctl_fields_rgb_values[] = {
    { "RGB_ORDER",  PCF8882_MADCTL_RGB_RGB_ORDER },
    { "BGR_ORDER",  PCF8882_MADCTL_RGB_BGR_ORDER },
};

static const DebugRegValue pcf8882_madctl_fields_lao_values[] = {
    { "TOP_TO_BOTTOM",  PCF8882_MADCTL_LAO_TOP_TO_BOTTOM },
    { "BOTTOM_TO_TOP",  PCF8882_MADCTL_LAO_BOTTOM_TO_TOP },
};

static const DebugRegValue pcf8882_madctl_fields_v_values[] = {
    { "WRITE_X",    PCF8882_MADCTL_V_WRITE_X },
    { "WRITE_Y",    PCF8882_MADCTL_V_WRITE_Y },
};

static const DebugRegValue pcf8882_madctl_fields_mx_values[] = {
    { "NORMAL",     PCF8882_MADCTL_MX_NORMAL },
    { "MIRRORED",   PCF8882_MADCTL_MX_MIRRORED },
};

static const DebugRegValue pcf8882_madctl_fields_my_values[] = {
    { "NORMAL",     PCF8882_MADCTL_MY_NORMAL },
    { "MIRRORED",   PCF8882_MADCTL_MY_MIRRORED },
};

static const DebugRegField pcf8882_madctl_fields[] = {
    { "RGB",    PCF8882_MADCTL_RGB, PCF8882_MADCTL_RGB_SHIFT,   pcf8882_madctl_fields_rgb_values,   ARRAY_SIZE(pcf8882_madctl_fields_rgb_values) },
    { "LAO",    PCF8882_MADCTL_LAO, PCF8882_MADCTL_LAO_SHIFT,   pcf8882_madctl_fields_lao_values,   ARRAY_SIZE(pcf8882_madctl_fields_lao_values) },
    { "V",      PCF8882_MADCTL_V,   PCF8882_MADCTL_V_SHIFT,     pcf8882_madctl_fields_v_values,     ARRAY_SIZE(pcf8882_madctl_fields_v_values) },
    { "MX",     PCF8882_MADCTL_MX,  PCF8882_MADCTL_MX_SHIFT,    pcf8882_madctl_fields_mx_values,    ARRAY_SIZE(pcf8882_madctl_fields_mx_values) },
    { "MY",     PCF8882_MADCTL_MY,  PCF8882_MADCTL_MY_SHIFT,    pcf8882_madctl_fields_my_values,    ARRAY_SIZE(pcf8882_madctl_fields_my_values) },
};

static const DebugRegField pcf8882_sep_fields[] = {
    { "VALUE",  PCF8882_SEP_VALUE,  PCF8882_SEP_VALUE_SHIFT,    NULL,   0 },
};

static const DebugRegValue pcf8882_colmod_fields_p_values[] = {
    { "PIXEL_8_BIT",    PCF8882_COLMOD_P_PIXEL_8_BIT },
    { "PIXEL_12_BIT",   PCF8882_COLMOD_P_PIXEL_12_BIT },
    { "PIXEL_16_BIT",   PCF8882_COLMOD_P_PIXEL_16_BIT },
    { "PIXEL_18_BIT",   PCF8882_COLMOD_P_PIXEL_18_BIT },
};

static const DebugRegField pcf8882_colmod_fields[] = {
    { "P",  PCF8882_COLMOD_P,   PCF8882_COLMOD_P_SHIFT, pcf8882_colmod_fields_p_values, ARRAY_SIZE(pcf8882_colmod_fields_p_values) },
};

static const DebugRegField pcf8882_rdid1_fields[] = {
    { "MANID",  PCF8882_RDID1_MANID,    PCF8882_RDID1_MANID_SHIFT,  NULL,   0 },
};

static const DebugRegField pcf8882_rdid2_fields[] = {
    { "VERSION",    PCF8882_RDID2_VERSION,  PCF8882_RDID2_VERSION_SHIFT,    NULL,   0 },
};

static const DebugRegField pcf8882_rdid3_fields[] = {
    { "MODULE_ID",  PCF8882_RDID3_MODULE_ID,    PCF8882_RDID3_MODULE_ID_SHIFT,  NULL,   0 },
};

static const DebugReg pcf8882_regs[] = {
    { "NOP",        PCF8882_NOP,        NULL,                   0 },
    { "SWRESET",    PCF8882_SWRESET,    NULL,                   0 },
    { "RDDIDIF",    PCF8882_RDDIDIF,    pcf8882_rddidif_fields, ARRAY_SIZE(pcf8882_rddidif_fields) },
    { "RDDST",      PCF8882_RDDST,      pcf8882_rddst_fields,   ARRAY_SIZE(pcf8882_rddst_fields) },
    { "SLPIN",      PCF8882_SLPIN,      NULL,                   0 },
    { "SLPOUT",     PCF8882_SLPOUT,     NULL,                   0 },
    { "PTLON",      PCF8882_PTLON,      NULL,                   0 },
    { "NORON",      PCF8882_NORON,      NULL,                   0 },
    { "INVOFF",     PCF8882_INVOFF,     NULL,                   0 },
    { "INVON",      PCF8882_INVON,      NULL,                   0 },
    { "DALO",       PCF8882_DALO,       NULL,                   0 },
    { "DAL",        PCF8882_DAL,        NULL,                   0 },
    { "GAMSET",     PCF8882_GAMSET,     NULL,                   0 },
    { "DISPOFF",    PCF8882_DISPOFF,    NULL,                   0 },
    { "DISPON",     PCF8882_DISPON,     NULL,                   0 },
    { "CASET",      PCF8882_CASET,      pcf8882_caset_fields,   ARRAY_SIZE(pcf8882_caset_fields) },
    { "PASET",      PCF8882_PASET,      pcf8882_paset_fields,   ARRAY_SIZE(pcf8882_paset_fields) },
    { "RAMWR",      PCF8882_RAMWR,      NULL,                   0 },
    { "RGBSET",     PCF8882_RGBSET,     NULL,                   0 },
    { "PTLAR",      PCF8882_PTLAR,      pcf8882_ptlar_fields,   ARRAY_SIZE(pcf8882_ptlar_fields) },
    { "VSCRDEF",    PCF8882_VSCRDEF,    pcf8882_vscrdef_fields, ARRAY_SIZE(pcf8882_vscrdef_fields) },
    { "TEOFF",      PCF8882_TEOFF,      NULL,                   0 },
    { "TEON",       PCF8882_TEON,       NULL,                   0 },
    { "MADCTL",     PCF8882_MADCTL,     pcf8882_madctl_fields,  ARRAY_SIZE(pcf8882_madctl_fields) },
    { "SEP",        PCF8882_SEP,        pcf8882_sep_fields,     ARRAY_SIZE(pcf8882_sep_fields) },
    { "IDMOFF",     PCF8882_IDMOFF,     NULL,                   0 },
    { "IDMON",      PCF8882_IDMON,      NULL,                   0 },
    { "COLMOD",     PCF8882_COLMOD,     pcf8882_colmod_fields,  ARRAY_SIZE(pcf8882_colmod_fields) },
    { "RDID1",      PCF8882_RDID1,      pcf8882_rdid1_fields,   ARRAY_SIZE(pcf8882_rdid1_fields) },
    { "RDID2",      PCF8882_RDID2,      pcf8882_rdid2_fields,   ARRAY_SIZE(pcf8882_rdid2_fields) },
    { "RDID3",      PCF8882_RDID3,      pcf8882_rdid3_fields,   ARRAY_SIZE(pcf8882_rdid3_fields) },
};

const DebugRegs pcf8882_debug_regs = {
    .regs = pcf8882_regs,
    .count = ARRAY_SIZE(pcf8882_regs),
    .reg_size = 1,
    .addr_digits = 2,
};
