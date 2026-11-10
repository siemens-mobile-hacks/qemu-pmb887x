#include "qemu/osdep.h"
#include "regs.h"

static const DebugRegField ssd1286_oscillation_fields[] = {
    { "OSCEN",  SSD1286_OSCILLATION_OSCEN,  SSD1286_OSCILLATION_OSCEN_SHIFT,    NULL,   0 },
};

static const DebugRegField ssd1286_driver_output_control_fields[] = {
    { "MUX",    SSD1286_DRIVER_OUTPUT_CONTROL_MUX,  SSD1286_DRIVER_OUTPUT_CONTROL_MUX_SHIFT,    NULL,   0 },
    { "RL",     SSD1286_DRIVER_OUTPUT_CONTROL_RL,   SSD1286_DRIVER_OUTPUT_CONTROL_RL_SHIFT,     NULL,   0 },
    { "TB",     SSD1286_DRIVER_OUTPUT_CONTROL_TB,   SSD1286_DRIVER_OUTPUT_CONTROL_TB_SHIFT,     NULL,   0 },
    { "SM",     SSD1286_DRIVER_OUTPUT_CONTROL_SM,   SSD1286_DRIVER_OUTPUT_CONTROL_SM_SHIFT,     NULL,   0 },
    { "BGR",    SSD1286_DRIVER_OUTPUT_CONTROL_BGR,  SSD1286_DRIVER_OUTPUT_CONTROL_BGR_SHIFT,    NULL,   0 },
    { "CAD",    SSD1286_DRIVER_OUTPUT_CONTROL_CAD,  SSD1286_DRIVER_OUTPUT_CONTROL_CAD_SHIFT,    NULL,   0 },
    { "REV",    SSD1286_DRIVER_OUTPUT_CONTROL_REV,  SSD1286_DRIVER_OUTPUT_CONTROL_REV_SHIFT,    NULL,   0 },
};

static const DebugRegField ssd1286_lcd_drive_ac_control_fields[] = {
    { "NW",     SSD1286_LCD_DRIVE_AC_CONTROL_NW,    SSD1286_LCD_DRIVE_AC_CONTROL_NW_SHIFT,      NULL,   0 },
    { "WSMD",   SSD1286_LCD_DRIVE_AC_CONTROL_WSMD,  SSD1286_LCD_DRIVE_AC_CONTROL_WSMD_SHIFT,    NULL,   0 },
    { "EOR",    SSD1286_LCD_DRIVE_AC_CONTROL_EOR,   SSD1286_LCD_DRIVE_AC_CONTROL_EOR_SHIFT,     NULL,   0 },
    { "B_C",    SSD1286_LCD_DRIVE_AC_CONTROL_B_C,   SSD1286_LCD_DRIVE_AC_CONTROL_B_C_SHIFT,     NULL,   0 },
    { "ENWS",   SSD1286_LCD_DRIVE_AC_CONTROL_ENWS,  SSD1286_LCD_DRIVE_AC_CONTROL_ENWS_SHIFT,    NULL,   0 },
    { "FLD",    SSD1286_LCD_DRIVE_AC_CONTROL_FLD,   SSD1286_LCD_DRIVE_AC_CONTROL_FLD_SHIFT,     NULL,   0 },
};

static const DebugRegField ssd1286_entry_mode_fields[] = {
    { "LG",     SSD1286_ENTRY_MODE_LG,      SSD1286_ENTRY_MODE_LG_SHIFT,        NULL,   0 },
    { "AM",     SSD1286_ENTRY_MODE_AM,      SSD1286_ENTRY_MODE_AM_SHIFT,        NULL,   0 },
    { "ID",     SSD1286_ENTRY_MODE_ID,      SSD1286_ENTRY_MODE_ID_SHIFT,        NULL,   0 },
    { "TY",     SSD1286_ENTRY_MODE_TY,      SSD1286_ENTRY_MODE_TY_SHIFT,        NULL,   0 },
    { "DMODE",  SSD1286_ENTRY_MODE_DMODE,   SSD1286_ENTRY_MODE_DMODE_SHIFT,     NULL,   0 },
    { "WMODE",  SSD1286_ENTRY_MODE_WMODE,   SSD1286_ENTRY_MODE_WMODE_SHIFT,     NULL,   0 },
    { "OEDEF",  SSD1286_ENTRY_MODE_OEDEF,   SSD1286_ENTRY_MODE_OEDEF_SHIFT,     NULL,   0 },
    { "TRANS",  SSD1286_ENTRY_MODE_TRANS,   SSD1286_ENTRY_MODE_TRANS_SHIFT,     NULL,   0 },
    { "DFM",    SSD1286_ENTRY_MODE_DFM,     SSD1286_ENTRY_MODE_DFM_SHIFT,       NULL,   0 },
    { "VSMODE", SSD1286_ENTRY_MODE_VSMODE,  SSD1286_ENTRY_MODE_VSMODE_SHIFT,    NULL,   0 },
};

static const DebugRegField ssd1286_compare_1_fields[] = {
    { "CPG",    SSD1286_COMPARE_1_CPG,  SSD1286_COMPARE_1_CPG_SHIFT,    NULL,   0 },
    { "CPR",    SSD1286_COMPARE_1_CPR,  SSD1286_COMPARE_1_CPR_SHIFT,    NULL,   0 },
};

static const DebugRegField ssd1286_compare_2_fields[] = {
    { "CPB",    SSD1286_COMPARE_2_CPB,  SSD1286_COMPARE_2_CPB_SHIFT,    NULL,   0 },
};

static const DebugRegField ssd1286_display_control_fields[] = {
    { "D",      SSD1286_DISPLAY_CONTROL_D,      SSD1286_DISPLAY_CONTROL_D_SHIFT,    NULL,   0 },
    { "CM",     SSD1286_DISPLAY_CONTROL_CM,     SSD1286_DISPLAY_CONTROL_CM_SHIFT,   NULL,   0 },
    { "DTE",    SSD1286_DISPLAY_CONTROL_DTE,    SSD1286_DISPLAY_CONTROL_DTE_SHIFT,  NULL,   0 },
    { "GON",    SSD1286_DISPLAY_CONTROL_GON,    SSD1286_DISPLAY_CONTROL_GON_SHIFT,  NULL,   0 },
    { "SPT",    SSD1286_DISPLAY_CONTROL_SPT,    SSD1286_DISPLAY_CONTROL_SPT_SHIFT,  NULL,   0 },
    { "VLE",    SSD1286_DISPLAY_CONTROL_VLE,    SSD1286_DISPLAY_CONTROL_VLE_SHIFT,  NULL,   0 },
    { "PT",     SSD1286_DISPLAY_CONTROL_PT,     SSD1286_DISPLAY_CONTROL_PT_SHIFT,   NULL,   0 },
};

static const DebugRegField ssd1286_frame_cycle_control_fields[] = {
    { "RTN",    SSD1286_FRAME_CYCLE_CONTROL_RTN,    SSD1286_FRAME_CYCLE_CONTROL_RTN_SHIFT,  NULL,   0 },
    { "SRTN",   SSD1286_FRAME_CYCLE_CONTROL_SRTN,   SSD1286_FRAME_CYCLE_CONTROL_SRTN_SHIFT, NULL,   0 },
    { "SDIV",   SSD1286_FRAME_CYCLE_CONTROL_SDIV,   SSD1286_FRAME_CYCLE_CONTROL_SDIV_SHIFT, NULL,   0 },
    { "DIV",    SSD1286_FRAME_CYCLE_CONTROL_DIV,    SSD1286_FRAME_CYCLE_CONTROL_DIV_SHIFT,  NULL,   0 },
    { "EQ",     SSD1286_FRAME_CYCLE_CONTROL_EQ,     SSD1286_FRAME_CYCLE_CONTROL_EQ_SHIFT,   NULL,   0 },
    { "SDT",    SSD1286_FRAME_CYCLE_CONTROL_SDT,    SSD1286_FRAME_CYCLE_CONTROL_SDT_SHIFT,  NULL,   0 },
    { "NO",     SSD1286_FRAME_CYCLE_CONTROL_NO,     SSD1286_FRAME_CYCLE_CONTROL_NO_SHIFT,   NULL,   0 },
};

static const DebugRegField ssd1286_power_control_1_fields[] = {
    { "SLP",    SSD1286_POWER_CONTROL_1_SLP,    SSD1286_POWER_CONTROL_1_SLP_SHIFT,  NULL,   0 },
    { "AP",     SSD1286_POWER_CONTROL_1_AP,     SSD1286_POWER_CONTROL_1_AP_SHIFT,   NULL,   0 },
    { "DC",     SSD1286_POWER_CONTROL_1_DC,     SSD1286_POWER_CONTROL_1_DC_SHIFT,   NULL,   0 },
    { "BTL",    SSD1286_POWER_CONTROL_1_BTL,    SSD1286_POWER_CONTROL_1_BTL_SHIFT,  NULL,   0 },
    { "BTH",    SSD1286_POWER_CONTROL_1_BTH,    SSD1286_POWER_CONTROL_1_BTH_SHIFT,  NULL,   0 },
    { "DCY",    SSD1286_POWER_CONTROL_1_DCY,    SSD1286_POWER_CONTROL_1_DCY_SHIFT,  NULL,   0 },
};

static const DebugRegField ssd1286_power_control_2_fields[] = {
    { "VRC",    SSD1286_POWER_CONTROL_2_VRC,    SSD1286_POWER_CONTROL_2_VRC_SHIFT,  NULL,   0 },
    { "PU",     SSD1286_POWER_CONTROL_2_PU,     SSD1286_POWER_CONTROL_2_PU_SHIFT,   NULL,   0 },
};

static const DebugRegField ssd1286_power_control_3_fields[] = {
    { "VRH",    SSD1286_POWER_CONTROL_3_VRH,    SSD1286_POWER_CONTROL_3_VRH_SHIFT,  NULL,   0 },
};

static const DebugRegField ssd1286_power_control_4_fields[] = {
    { "VDV",    SSD1286_POWER_CONTROL_4_VDV,    SSD1286_POWER_CONTROL_4_VDV_SHIFT,      NULL,   0 },
    { "VCOMG",  SSD1286_POWER_CONTROL_4_VCOMG,  SSD1286_POWER_CONTROL_4_VCOMG_SHIFT,    NULL,   0 },
};

static const DebugRegField ssd1286_horizontal_porch_fields[] = {
    { "HBP",    SSD1286_HORIZONTAL_PORCH_HBP,   SSD1286_HORIZONTAL_PORCH_HBP_SHIFT, NULL,   0 },
    { "XL",     SSD1286_HORIZONTAL_PORCH_XL,    SSD1286_HORIZONTAL_PORCH_XL_SHIFT,  NULL,   0 },
};

static const DebugRegField ssd1286_vertical_porch_fields[] = {
    { "VBP",    SSD1286_VERTICAL_PORCH_VBP, SSD1286_VERTICAL_PORCH_VBP_SHIFT,   NULL,   0 },
    { "VFP",    SSD1286_VERTICAL_PORCH_VFP, SSD1286_VERTICAL_PORCH_VFP_SHIFT,   NULL,   0 },
};

static const DebugRegField ssd1286_power_control_5_fields[] = {
    { "VCM",    SSD1286_POWER_CONTROL_5_VCM,    SSD1286_POWER_CONTROL_5_VCM_SHIFT,  NULL,   0 },
    { "NOTP",   SSD1286_POWER_CONTROL_5_NOTP,   SSD1286_POWER_CONTROL_5_NOTP_SHIFT, NULL,   0 },
};

static const DebugRegField ssd1286_ram_address_fields[] = {
    { "AD", SSD1286_RAM_ADDRESS_AD, SSD1286_RAM_ADDRESS_AD_SHIFT,   NULL,   0 },
};

static const DebugRegField ssd1286_gram_data_fields[] = {
    { "DATA",   SSD1286_GRAM_DATA_DATA, SSD1286_GRAM_DATA_DATA_SHIFT,   NULL,   0 },
};

static const DebugRegField ssd1286_ram_write_mask_1_fields[] = {
    { "WMG",    SSD1286_RAM_WRITE_MASK_1_WMG,   SSD1286_RAM_WRITE_MASK_1_WMG_SHIFT, NULL,   0 },
    { "WMR",    SSD1286_RAM_WRITE_MASK_1_WMR,   SSD1286_RAM_WRITE_MASK_1_WMR_SHIFT, NULL,   0 },
};

static const DebugRegField ssd1286_ram_write_mask_2_fields[] = {
    { "WMB",    SSD1286_RAM_WRITE_MASK_2_WMB,   SSD1286_RAM_WRITE_MASK_2_WMB_SHIFT, NULL,   0 },
};

static const DebugRegField ssd1286_vcom_otp_1_fields[] = {
    { "VALUE",  SSD1286_VCOM_OTP_1_VALUE,   SSD1286_VCOM_OTP_1_VALUE_SHIFT, NULL,   0 },
};

static const DebugRegField ssd1286_vcom_otp_2_fields[] = {
    { "VALUE",  SSD1286_VCOM_OTP_2_VALUE,   SSD1286_VCOM_OTP_2_VALUE_SHIFT, NULL,   0 },
};

static const DebugRegField ssd1286_gamma_1_fields[] = {
    { "PKP0",   SSD1286_GAMMA_1_PKP0,   SSD1286_GAMMA_1_PKP0_SHIFT, NULL,   0 },
    { "PKP1",   SSD1286_GAMMA_1_PKP1,   SSD1286_GAMMA_1_PKP1_SHIFT, NULL,   0 },
};

static const DebugRegField ssd1286_gamma_2_fields[] = {
    { "PKP2",   SSD1286_GAMMA_2_PKP2,   SSD1286_GAMMA_2_PKP2_SHIFT, NULL,   0 },
    { "PKP3",   SSD1286_GAMMA_2_PKP3,   SSD1286_GAMMA_2_PKP3_SHIFT, NULL,   0 },
};

static const DebugRegField ssd1286_gamma_3_fields[] = {
    { "PKP4",   SSD1286_GAMMA_3_PKP4,   SSD1286_GAMMA_3_PKP4_SHIFT, NULL,   0 },
    { "PKP5",   SSD1286_GAMMA_3_PKP5,   SSD1286_GAMMA_3_PKP5_SHIFT, NULL,   0 },
};

static const DebugRegField ssd1286_gamma_4_fields[] = {
    { "PRP0",   SSD1286_GAMMA_4_PRP0,   SSD1286_GAMMA_4_PRP0_SHIFT, NULL,   0 },
    { "PRP1",   SSD1286_GAMMA_4_PRP1,   SSD1286_GAMMA_4_PRP1_SHIFT, NULL,   0 },
};

static const DebugRegField ssd1286_gamma_5_fields[] = {
    { "PKN0",   SSD1286_GAMMA_5_PKN0,   SSD1286_GAMMA_5_PKN0_SHIFT, NULL,   0 },
    { "PKN1",   SSD1286_GAMMA_5_PKN1,   SSD1286_GAMMA_5_PKN1_SHIFT, NULL,   0 },
};

static const DebugRegField ssd1286_gamma_6_fields[] = {
    { "PKN2",   SSD1286_GAMMA_6_PKN2,   SSD1286_GAMMA_6_PKN2_SHIFT, NULL,   0 },
    { "PKN3",   SSD1286_GAMMA_6_PKN3,   SSD1286_GAMMA_6_PKN3_SHIFT, NULL,   0 },
};

static const DebugRegField ssd1286_gamma_7_fields[] = {
    { "PKN4",   SSD1286_GAMMA_7_PKN4,   SSD1286_GAMMA_7_PKN4_SHIFT, NULL,   0 },
    { "PKN5",   SSD1286_GAMMA_7_PKN5,   SSD1286_GAMMA_7_PKN5_SHIFT, NULL,   0 },
};

static const DebugRegField ssd1286_gamma_8_fields[] = {
    { "PRN0",   SSD1286_GAMMA_8_PRN0,   SSD1286_GAMMA_8_PRN0_SHIFT, NULL,   0 },
    { "PRN1",   SSD1286_GAMMA_8_PRN1,   SSD1286_GAMMA_8_PRN1_SHIFT, NULL,   0 },
};

static const DebugRegField ssd1286_gamma_9_fields[] = {
    { "VRP_03_00",  SSD1286_GAMMA_9_VRP_03_00,  SSD1286_GAMMA_9_VRP_03_00_SHIFT,    NULL,   0 },
    { "VRP_14_10",  SSD1286_GAMMA_9_VRP_14_10,  SSD1286_GAMMA_9_VRP_14_10_SHIFT,    NULL,   0 },
};

static const DebugRegField ssd1286_gamma_10_fields[] = {
    { "VRN_03_00",  SSD1286_GAMMA_10_VRN_03_00, SSD1286_GAMMA_10_VRN_03_00_SHIFT,   NULL,   0 },
    { "VRN_14_10",  SSD1286_GAMMA_10_VRN_14_10, SSD1286_GAMMA_10_VRN_14_10_SHIFT,   NULL,   0 },
};

static const DebugRegField ssd1286_gate_scan_start_fields[] = {
    { "SCN",    SSD1286_GATE_SCAN_START_SCN,    SSD1286_GATE_SCAN_START_SCN_SHIFT,  NULL,   0 },
};

static const DebugRegField ssd1286_vertical_scroll_fields[] = {
    { "VL1",    SSD1286_VERTICAL_SCROLL_VL1,    SSD1286_VERTICAL_SCROLL_VL1_SHIFT,  NULL,   0 },
    { "VL2",    SSD1286_VERTICAL_SCROLL_VL2,    SSD1286_VERTICAL_SCROLL_VL2_SHIFT,  NULL,   0 },
};

static const DebugRegField ssd1286_first_display_drive_fields[] = {
    { "SS1",    SSD1286_FIRST_DISPLAY_DRIVE_SS1,    SSD1286_FIRST_DISPLAY_DRIVE_SS1_SHIFT,  NULL,   0 },
    { "SE1",    SSD1286_FIRST_DISPLAY_DRIVE_SE1,    SSD1286_FIRST_DISPLAY_DRIVE_SE1_SHIFT,  NULL,   0 },
};

static const DebugRegField ssd1286_second_display_drive_fields[] = {
    { "SS2",    SSD1286_SECOND_DISPLAY_DRIVE_SS2,   SSD1286_SECOND_DISPLAY_DRIVE_SS2_SHIFT, NULL,   0 },
    { "SE2",    SSD1286_SECOND_DISPLAY_DRIVE_SE2,   SSD1286_SECOND_DISPLAY_DRIVE_SE2_SHIFT, NULL,   0 },
};

static const DebugRegField ssd1286_horizontal_ram_address_fields[] = {
    { "HSA",    SSD1286_HORIZONTAL_RAM_ADDRESS_HSA, SSD1286_HORIZONTAL_RAM_ADDRESS_HSA_SHIFT,   NULL,   0 },
    { "HEA",    SSD1286_HORIZONTAL_RAM_ADDRESS_HEA, SSD1286_HORIZONTAL_RAM_ADDRESS_HEA_SHIFT,   NULL,   0 },
};

static const DebugRegField ssd1286_vertical_ram_address_fields[] = {
    { "VSA",    SSD1286_VERTICAL_RAM_ADDRESS_VSA,   SSD1286_VERTICAL_RAM_ADDRESS_VSA_SHIFT, NULL,   0 },
    { "VEA",    SSD1286_VERTICAL_RAM_ADDRESS_VEA,   SSD1286_VERTICAL_RAM_ADDRESS_VEA_SHIFT, NULL,   0 },
};

static const DebugReg ssd1286_regs[] = {
    { "OSCILLATION",            SSD1286_OSCILLATION,            ssd1286_oscillation_fields,             ARRAY_SIZE(ssd1286_oscillation_fields) },
    { "DRIVER_OUTPUT_CONTROL",  SSD1286_DRIVER_OUTPUT_CONTROL,  ssd1286_driver_output_control_fields,   ARRAY_SIZE(ssd1286_driver_output_control_fields) },
    { "LCD_DRIVE_AC_CONTROL",   SSD1286_LCD_DRIVE_AC_CONTROL,   ssd1286_lcd_drive_ac_control_fields,    ARRAY_SIZE(ssd1286_lcd_drive_ac_control_fields) },
    { "ENTRY_MODE",             SSD1286_ENTRY_MODE,             ssd1286_entry_mode_fields,              ARRAY_SIZE(ssd1286_entry_mode_fields) },
    { "COMPARE_1",              SSD1286_COMPARE_1,              ssd1286_compare_1_fields,               ARRAY_SIZE(ssd1286_compare_1_fields) },
    { "COMPARE_2",              SSD1286_COMPARE_2,              ssd1286_compare_2_fields,               ARRAY_SIZE(ssd1286_compare_2_fields) },
    { "DISPLAY_CONTROL",        SSD1286_DISPLAY_CONTROL,        ssd1286_display_control_fields,         ARRAY_SIZE(ssd1286_display_control_fields) },
    { "FRAME_CYCLE_CONTROL",    SSD1286_FRAME_CYCLE_CONTROL,    ssd1286_frame_cycle_control_fields,     ARRAY_SIZE(ssd1286_frame_cycle_control_fields) },
    { "POWER_CONTROL_1",        SSD1286_POWER_CONTROL_1,        ssd1286_power_control_1_fields,         ARRAY_SIZE(ssd1286_power_control_1_fields) },
    { "POWER_CONTROL_2",        SSD1286_POWER_CONTROL_2,        ssd1286_power_control_2_fields,         ARRAY_SIZE(ssd1286_power_control_2_fields) },
    { "POWER_CONTROL_3",        SSD1286_POWER_CONTROL_3,        ssd1286_power_control_3_fields,         ARRAY_SIZE(ssd1286_power_control_3_fields) },
    { "POWER_CONTROL_4",        SSD1286_POWER_CONTROL_4,        ssd1286_power_control_4_fields,         ARRAY_SIZE(ssd1286_power_control_4_fields) },
    { "HORIZONTAL_PORCH",       SSD1286_HORIZONTAL_PORCH,       ssd1286_horizontal_porch_fields,        ARRAY_SIZE(ssd1286_horizontal_porch_fields) },
    { "VERTICAL_PORCH",         SSD1286_VERTICAL_PORCH,         ssd1286_vertical_porch_fields,          ARRAY_SIZE(ssd1286_vertical_porch_fields) },
    { "POWER_CONTROL_5",        SSD1286_POWER_CONTROL_5,        ssd1286_power_control_5_fields,         ARRAY_SIZE(ssd1286_power_control_5_fields) },
    { "RAM_ADDRESS",            SSD1286_RAM_ADDRESS,            ssd1286_ram_address_fields,             ARRAY_SIZE(ssd1286_ram_address_fields) },
    { "GRAM_DATA",              SSD1286_GRAM_DATA,              ssd1286_gram_data_fields,               ARRAY_SIZE(ssd1286_gram_data_fields) },
    { "RAM_WRITE_MASK_1",       SSD1286_RAM_WRITE_MASK_1,       ssd1286_ram_write_mask_1_fields,        ARRAY_SIZE(ssd1286_ram_write_mask_1_fields) },
    { "RAM_WRITE_MASK_2",       SSD1286_RAM_WRITE_MASK_2,       ssd1286_ram_write_mask_2_fields,        ARRAY_SIZE(ssd1286_ram_write_mask_2_fields) },
    { "VCOM_OTP_1",             SSD1286_VCOM_OTP_1,             ssd1286_vcom_otp_1_fields,              ARRAY_SIZE(ssd1286_vcom_otp_1_fields) },
    { "VCOM_OTP_2",             SSD1286_VCOM_OTP_2,             ssd1286_vcom_otp_2_fields,              ARRAY_SIZE(ssd1286_vcom_otp_2_fields) },
    { "GAMMA_1",                SSD1286_GAMMA_1,                ssd1286_gamma_1_fields,                 ARRAY_SIZE(ssd1286_gamma_1_fields) },
    { "GAMMA_2",                SSD1286_GAMMA_2,                ssd1286_gamma_2_fields,                 ARRAY_SIZE(ssd1286_gamma_2_fields) },
    { "GAMMA_3",                SSD1286_GAMMA_3,                ssd1286_gamma_3_fields,                 ARRAY_SIZE(ssd1286_gamma_3_fields) },
    { "GAMMA_4",                SSD1286_GAMMA_4,                ssd1286_gamma_4_fields,                 ARRAY_SIZE(ssd1286_gamma_4_fields) },
    { "GAMMA_5",                SSD1286_GAMMA_5,                ssd1286_gamma_5_fields,                 ARRAY_SIZE(ssd1286_gamma_5_fields) },
    { "GAMMA_6",                SSD1286_GAMMA_6,                ssd1286_gamma_6_fields,                 ARRAY_SIZE(ssd1286_gamma_6_fields) },
    { "GAMMA_7",                SSD1286_GAMMA_7,                ssd1286_gamma_7_fields,                 ARRAY_SIZE(ssd1286_gamma_7_fields) },
    { "GAMMA_8",                SSD1286_GAMMA_8,                ssd1286_gamma_8_fields,                 ARRAY_SIZE(ssd1286_gamma_8_fields) },
    { "GAMMA_9",                SSD1286_GAMMA_9,                ssd1286_gamma_9_fields,                 ARRAY_SIZE(ssd1286_gamma_9_fields) },
    { "GAMMA_10",               SSD1286_GAMMA_10,               ssd1286_gamma_10_fields,                ARRAY_SIZE(ssd1286_gamma_10_fields) },
    { "GATE_SCAN_START",        SSD1286_GATE_SCAN_START,        ssd1286_gate_scan_start_fields,         ARRAY_SIZE(ssd1286_gate_scan_start_fields) },
    { "VERTICAL_SCROLL",        SSD1286_VERTICAL_SCROLL,        ssd1286_vertical_scroll_fields,         ARRAY_SIZE(ssd1286_vertical_scroll_fields) },
    { "FIRST_DISPLAY_DRIVE",    SSD1286_FIRST_DISPLAY_DRIVE,    ssd1286_first_display_drive_fields,     ARRAY_SIZE(ssd1286_first_display_drive_fields) },
    { "SECOND_DISPLAY_DRIVE",   SSD1286_SECOND_DISPLAY_DRIVE,   ssd1286_second_display_drive_fields,    ARRAY_SIZE(ssd1286_second_display_drive_fields) },
    { "HORIZONTAL_RAM_ADDRESS", SSD1286_HORIZONTAL_RAM_ADDRESS, ssd1286_horizontal_ram_address_fields,  ARRAY_SIZE(ssd1286_horizontal_ram_address_fields) },
    { "VERTICAL_RAM_ADDRESS",   SSD1286_VERTICAL_RAM_ADDRESS,   ssd1286_vertical_ram_address_fields,    ARRAY_SIZE(ssd1286_vertical_ram_address_fields) },
};

const DebugRegs ssd1286_debug_regs = {
    .regs = ssd1286_regs,
    .count = ARRAY_SIZE(ssd1286_regs),
    .reg_size = 1,
    .addr_digits = 2,
};
