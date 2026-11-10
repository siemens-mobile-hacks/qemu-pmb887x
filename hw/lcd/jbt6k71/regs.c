#include "qemu/osdep.h"
#include "regs.h"

static const DebugRegField jbt6k71_oscillation_fields[] = {
    { "OSC",    JBT6K71_OSCILLATION_OSC,    JBT6K71_OSCILLATION_OSC_SHIFT,  NULL,   0 },
};

static const DebugRegField jbt6k71_driver_output_control_fields[] = {
    { "NL", JBT6K71_DRIVER_OUTPUT_CONTROL_NL,   JBT6K71_DRIVER_OUTPUT_CONTROL_NL_SHIFT, NULL,   0 },
    { "SS", JBT6K71_DRIVER_OUTPUT_CONTROL_SS,   JBT6K71_DRIVER_OUTPUT_CONTROL_SS_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_lcd_driving_signal_fields[] = {
    { "B_C",    JBT6K71_LCD_DRIVING_SIGNAL_B_C, JBT6K71_LCD_DRIVING_SIGNAL_B_C_SHIFT,   NULL,   0 },
};

static const DebugRegField jbt6k71_entry_mode_fields[] = {
    { "AM",     JBT6K71_ENTRY_MODE_AM,      JBT6K71_ENTRY_MODE_AM_SHIFT,    NULL,   0 },
    { "ID",     JBT6K71_ENTRY_MODE_ID,      JBT6K71_ENTRY_MODE_ID_SHIFT,    NULL,   0 },
    { "HWM",    JBT6K71_ENTRY_MODE_HWM,     JBT6K71_ENTRY_MODE_HWM_SHIFT,   NULL,   0 },
    { "IF18",   JBT6K71_ENTRY_MODE_IF18,    JBT6K71_ENTRY_MODE_IF18_SHIFT,  NULL,   0 },
    { "BGR",    JBT6K71_ENTRY_MODE_BGR,     JBT6K71_ENTRY_MODE_BGR_SHIFT,   NULL,   0 },
    { "DFM",    JBT6K71_ENTRY_MODE_DFM,     JBT6K71_ENTRY_MODE_DFM_SHIFT,   NULL,   0 },
    { "TRI",    JBT6K71_ENTRY_MODE_TRI,     JBT6K71_ENTRY_MODE_TRI_SHIFT,   NULL,   0 },
};

static const DebugRegField jbt6k71_horizontal_valid_width_fields[] = {
    { "HBP",    JBT6K71_HORIZONTAL_VALID_WIDTH_HBP, JBT6K71_HORIZONTAL_VALID_WIDTH_HBP_SHIFT,   NULL,   0 },
    { "HWS",    JBT6K71_HORIZONTAL_VALID_WIDTH_HWS, JBT6K71_HORIZONTAL_VALID_WIDTH_HWS_SHIFT,   NULL,   0 },
};

static const DebugRegField jbt6k71_display_mode_1_fields[] = {
    { "REV",    JBT6K71_DISPLAY_MODE_1_REV, JBT6K71_DISPLAY_MODE_1_REV_SHIFT,   NULL,   0 },
    { "NBW",    JBT6K71_DISPLAY_MODE_1_NBW, JBT6K71_DISPLAY_MODE_1_NBW_SHIFT,   NULL,   0 },
    { "PT",     JBT6K71_DISPLAY_MODE_1_PT,  JBT6K71_DISPLAY_MODE_1_PT_SHIFT,    NULL,   0 },
    { "SPT",    JBT6K71_DISPLAY_MODE_1_SPT, JBT6K71_DISPLAY_MODE_1_SPT_SHIFT,   NULL,   0 },
    { "VLE",    JBT6K71_DISPLAY_MODE_1_VLE, JBT6K71_DISPLAY_MODE_1_VLE_SHIFT,   NULL,   0 },
    { "COL",    JBT6K71_DISPLAY_MODE_1_COL, JBT6K71_DISPLAY_MODE_1_COL_SHIFT,   NULL,   0 },
};

static const DebugRegField jbt6k71_display_mode_2_fields[] = {
    { "BP", JBT6K71_DISPLAY_MODE_2_BP,  JBT6K71_DISPLAY_MODE_2_BP_SHIFT,    NULL,   0 },
    { "FP", JBT6K71_DISPLAY_MODE_2_FP,  JBT6K71_DISPLAY_MODE_2_FP_SHIFT,    NULL,   0 },
};

static const DebugRegField jbt6k71_display_mode_3_fields[] = {
    { "RSH",    JBT6K71_DISPLAY_MODE_3_RSH, JBT6K71_DISPLAY_MODE_3_RSH_SHIFT,   NULL,   0 },
    { "RSB",    JBT6K71_DISPLAY_MODE_3_RSB, JBT6K71_DISPLAY_MODE_3_RSB_SHIFT,   NULL,   0 },
    { "RSE",    JBT6K71_DISPLAY_MODE_3_RSE, JBT6K71_DISPLAY_MODE_3_RSE_SHIFT,   NULL,   0 },
};

static const DebugRegField jbt6k71_display_mode_4_fields[] = {
    { "SIP",    JBT6K71_DISPLAY_MODE_4_SIP,     JBT6K71_DISPLAY_MODE_4_SIP_SHIFT,   NULL,   0 },
    { "Y2",     JBT6K71_DISPLAY_MODE_4_Y2,      JBT6K71_DISPLAY_MODE_4_Y2_SHIFT,    NULL,   0 },
    { "X2",     JBT6K71_DISPLAY_MODE_4_X2,      JBT6K71_DISPLAY_MODE_4_X2_SHIFT,    NULL,   0 },
    { "XYON",   JBT6K71_DISPLAY_MODE_4_XYON,    JBT6K71_DISPLAY_MODE_4_XYON_SHIFT,  NULL,   0 },
};

static const DebugRegField jbt6k71_external_display_1_fields[] = {
    { "RIM",    JBT6K71_EXTERNAL_DISPLAY_1_RIM, JBT6K71_EXTERNAL_DISPLAY_1_RIM_SHIFT,   NULL,   0 },
    { "DM",     JBT6K71_EXTERNAL_DISPLAY_1_DM,  JBT6K71_EXTERNAL_DISPLAY_1_DM_SHIFT,    NULL,   0 },
    { "RM",     JBT6K71_EXTERNAL_DISPLAY_1_RM,  JBT6K71_EXTERNAL_DISPLAY_1_RM_SHIFT,    NULL,   0 },
};

static const DebugRegField jbt6k71_fr_frequency_adjustment_fields[] = {
    { "RTNI",   JBT6K71_FR_FREQUENCY_ADJUSTMENT_RTNI,   JBT6K71_FR_FREQUENCY_ADJUSTMENT_RTNI_SHIFT, NULL,   0 },
    { "DIVI",   JBT6K71_FR_FREQUENCY_ADJUSTMENT_DIVI,   JBT6K71_FR_FREQUENCY_ADJUSTMENT_DIVI_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_external_display_2_fields[] = {
    { "RTNE",   JBT6K71_EXTERNAL_DISPLAY_2_RTNE,    JBT6K71_EXTERNAL_DISPLAY_2_RTNE_SHIFT,  NULL,   0 },
    { "DIVE",   JBT6K71_EXTERNAL_DISPLAY_2_DIVE,    JBT6K71_EXTERNAL_DISPLAY_2_DIVE_SHIFT,  NULL,   0 },
};

static const DebugRegField jbt6k71_external_display_3_fields[] = {
    { "DPL",    JBT6K71_EXTERNAL_DISPLAY_3_DPL,     JBT6K71_EXTERNAL_DISPLAY_3_DPL_SHIFT,   NULL,   0 },
    { "EPL",    JBT6K71_EXTERNAL_DISPLAY_3_EPL,     JBT6K71_EXTERNAL_DISPLAY_3_EPL_SHIFT,   NULL,   0 },
    { "VPL",    JBT6K71_EXTERNAL_DISPLAY_3_VPL,     JBT6K71_EXTERNAL_DISPLAY_3_VPL_SHIFT,   NULL,   0 },
    { "HSPL",   JBT6K71_EXTERNAL_DISPLAY_3_HSPL,    JBT6K71_EXTERNAL_DISPLAY_3_HSPL_SHIFT,  NULL,   0 },
    { "VSPL",   JBT6K71_EXTERNAL_DISPLAY_3_VSPL,    JBT6K71_EXTERNAL_DISPLAY_3_VSPL_SHIFT,  NULL,   0 },
};

static const DebugRegField jbt6k71_ltps_control_1_fields[] = {
    { "CLTI",   JBT6K71_LTPS_CONTROL_1_CLTI,    JBT6K71_LTPS_CONTROL_1_CLTI_SHIFT,  NULL,   0 },
    { "CLWI",   JBT6K71_LTPS_CONTROL_1_CLWI,    JBT6K71_LTPS_CONTROL_1_CLWI_SHIFT,  NULL,   0 },
};

static const DebugRegField jbt6k71_ltps_control_2_fields[] = {
    { "OEVFI",  JBT6K71_LTPS_CONTROL_2_OEVFI,   JBT6K71_LTPS_CONTROL_2_OEVFI_SHIFT, NULL,   0 },
    { "OEVBI",  JBT6K71_LTPS_CONTROL_2_OEVBI,   JBT6K71_LTPS_CONTROL_2_OEVBI_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_ltps_control_3_fields[] = {
    { "SHI",    JBT6K71_LTPS_CONTROL_3_SHI, JBT6K71_LTPS_CONTROL_3_SHI_SHIFT,   NULL,   0 },
};

static const DebugRegField jbt6k71_ltps_control_4_fields[] = {
    { "CKFI",   JBT6K71_LTPS_CONTROL_4_CKFI,    JBT6K71_LTPS_CONTROL_4_CKFI_SHIFT,  NULL,   0 },
    { "CKBI",   JBT6K71_LTPS_CONTROL_4_CKBI,    JBT6K71_LTPS_CONTROL_4_CKBI_SHIFT,  NULL,   0 },
};

static const DebugRegField jbt6k71_ltps_control_5_fields[] = {
    { "CLTE",   JBT6K71_LTPS_CONTROL_5_CLTE,    JBT6K71_LTPS_CONTROL_5_CLTE_SHIFT,  NULL,   0 },
    { "CLWE",   JBT6K71_LTPS_CONTROL_5_CLWE,    JBT6K71_LTPS_CONTROL_5_CLWE_SHIFT,  NULL,   0 },
};

static const DebugRegField jbt6k71_ltps_control_6_fields[] = {
    { "OEVFE",  JBT6K71_LTPS_CONTROL_6_OEVFE,   JBT6K71_LTPS_CONTROL_6_OEVFE_SHIFT, NULL,   0 },
    { "OEVBE",  JBT6K71_LTPS_CONTROL_6_OEVBE,   JBT6K71_LTPS_CONTROL_6_OEVBE_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_ltps_control_7_fields[] = {
    { "SHE",    JBT6K71_LTPS_CONTROL_7_SHE, JBT6K71_LTPS_CONTROL_7_SHE_SHIFT,   NULL,   0 },
};

static const DebugRegField jbt6k71_ltps_control_8_fields[] = {
    { "CKFE",   JBT6K71_LTPS_CONTROL_8_CKFE,    JBT6K71_LTPS_CONTROL_8_CKFE_SHIFT,  NULL,   0 },
    { "CKBE",   JBT6K71_LTPS_CONTROL_8_CKBE,    JBT6K71_LTPS_CONTROL_8_CKBE_SHIFT,  NULL,   0 },
};

static const DebugRegField jbt6k71_amplifier_capability_fields[] = {
    { "ABSW",   JBT6K71_AMPLIFIER_CAPABILITY_ABSW,  JBT6K71_AMPLIFIER_CAPABILITY_ABSW_SHIFT,    NULL,   0 },
};

static const DebugRegField jbt6k71_mode_fields[] = {
    { "STB",    JBT6K71_MODE_STB,   JBT6K71_MODE_STB_SHIFT,     NULL,   0 },
    { "DSTB",   JBT6K71_MODE_DSTB,  JBT6K71_MODE_DSTB_SHIFT,    NULL,   0 },
};

static const DebugRegField jbt6k71_power_off_line_count_fields[] = {
    { "POFH",   JBT6K71_POWER_OFF_LINE_COUNT_POFH,  JBT6K71_POWER_OFF_LINE_COUNT_POFH_SHIFT,    NULL,   0 },
};

static const DebugRegField jbt6k71_display_control_fields[] = {
    { "DCG",    JBT6K71_DISPLAY_CONTROL_DCG,    JBT6K71_DISPLAY_CONTROL_DCG_SHIFT,  NULL,   0 },
    { "VGAM",   JBT6K71_DISPLAY_CONTROL_VGAM,   JBT6K71_DISPLAY_CONTROL_VGAM_SHIFT, NULL,   0 },
    { "FDON",   JBT6K71_DISPLAY_CONTROL_FDON,   JBT6K71_DISPLAY_CONTROL_FDON_SHIFT, NULL,   0 },
    { "FR",     JBT6K71_DISPLAY_CONTROL_FR,     JBT6K71_DISPLAY_CONTROL_FR_SHIFT,   NULL,   0 },
    { "D",      JBT6K71_DISPLAY_CONTROL_D,      JBT6K71_DISPLAY_CONTROL_D_SHIFT,    NULL,   0 },
    { "ASW",    JBT6K71_DISPLAY_CONTROL_ASW,    JBT6K71_DISPLAY_CONTROL_ASW_SHIFT,  NULL,   0 },
    { "VCS",    JBT6K71_DISPLAY_CONTROL_VCS,    JBT6K71_DISPLAY_CONTROL_VCS_SHIFT,  NULL,   0 },
    { "OEV",    JBT6K71_DISPLAY_CONTROL_OEV,    JBT6K71_DISPLAY_CONTROL_OEV_SHIFT,  NULL,   0 },
    { "CON",    JBT6K71_DISPLAY_CONTROL_CON,    JBT6K71_DISPLAY_CONTROL_CON_SHIFT,  NULL,   0 },
    { "UD",     JBT6K71_DISPLAY_CONTROL_UD,     JBT6K71_DISPLAY_CONTROL_UD_SHIFT,   NULL,   0 },
    { "DCEV",   JBT6K71_DISPLAY_CONTROL_DCEV,   JBT6K71_DISPLAY_CONTROL_DCEV_SHIFT, NULL,   0 },
    { "PEV",    JBT6K71_DISPLAY_CONTROL_PEV,    JBT6K71_DISPLAY_CONTROL_PEV_SHIFT,  NULL,   0 },
    { "CONT",   JBT6K71_DISPLAY_CONTROL_CONT,   JBT6K71_DISPLAY_CONTROL_CONT_SHIFT, NULL,   0 },
    { "PO",     JBT6K71_DISPLAY_CONTROL_PO,     JBT6K71_DISPLAY_CONTROL_PO_SHIFT,   NULL,   0 },
};

static const DebugRegField jbt6k71_auto_management_control_fields[] = {
    { "AUTO",   JBT6K71_AUTO_MANAGEMENT_CONTROL_AUTO,   JBT6K71_AUTO_MANAGEMENT_CONTROL_AUTO_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_power_supply_control_1_fields[] = {
    { "VGM",    JBT6K71_POWER_SUPPLY_CONTROL_1_VGM, JBT6K71_POWER_SUPPLY_CONTROL_1_VGM_SHIFT,   NULL,   0 },
    { "VCS",    JBT6K71_POWER_SUPPLY_CONTROL_1_VCS, JBT6K71_POWER_SUPPLY_CONTROL_1_VCS_SHIFT,   NULL,   0 },
    { "VCD",    JBT6K71_POWER_SUPPLY_CONTROL_1_VCD, JBT6K71_POWER_SUPPLY_CONTROL_1_VCD_SHIFT,   NULL,   0 },
};

static const DebugRegField jbt6k71_power_supply_control_2_fields[] = {
    { "XVD",    JBT6K71_POWER_SUPPLY_CONTROL_2_XVD,     JBT6K71_POWER_SUPPLY_CONTROL_2_XVD_SHIFT,   NULL,   0 },
    { "WSEL1",  JBT6K71_POWER_SUPPLY_CONTROL_2_WSEL1,   JBT6K71_POWER_SUPPLY_CONTROL_2_WSEL1_SHIFT, NULL,   0 },
    { "WSEL2",  JBT6K71_POWER_SUPPLY_CONTROL_2_WSEL2,   JBT6K71_POWER_SUPPLY_CONTROL_2_WSEL2_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_power_supply_control_3_fields[] = {
    { "BAV",    JBT6K71_POWER_SUPPLY_CONTROL_3_BAV, JBT6K71_POWER_SUPPLY_CONTROL_3_BAV_SHIFT,   NULL,   0 },
    { "BXV",    JBT6K71_POWER_SUPPLY_CONTROL_3_BXV, JBT6K71_POWER_SUPPLY_CONTROL_3_BXV_SHIFT,   NULL,   0 },
};

static const DebugRegField jbt6k71_power_supply_control_4_fields[] = {
    { "CK1",    JBT6K71_POWER_SUPPLY_CONTROL_4_CK1, JBT6K71_POWER_SUPPLY_CONTROL_4_CK1_SHIFT,   NULL,   0 },
    { "CK2",    JBT6K71_POWER_SUPPLY_CONTROL_4_CK2, JBT6K71_POWER_SUPPLY_CONTROL_4_CK2_SHIFT,   NULL,   0 },
    { "CK3",    JBT6K71_POWER_SUPPLY_CONTROL_4_CK3, JBT6K71_POWER_SUPPLY_CONTROL_4_CK3_SHIFT,   NULL,   0 },
    { "DCW",    JBT6K71_POWER_SUPPLY_CONTROL_4_DCW, JBT6K71_POWER_SUPPLY_CONTROL_4_DCW_SHIFT,   NULL,   0 },
};

static const DebugRegField jbt6k71_external_polarity_fields[] = {
    { "EXTC1",  JBT6K71_EXTERNAL_POLARITY_EXTC1,    JBT6K71_EXTERNAL_POLARITY_EXTC1_SHIFT,  NULL,   0 },
    { "EXTC2",  JBT6K71_EXTERNAL_POLARITY_EXTC2,    JBT6K71_EXTERNAL_POLARITY_EXTC2_SHIFT,  NULL,   0 },
};

static const DebugRegField jbt6k71_ram_address_low_fields[] = {
    { "AD_07_00",   JBT6K71_RAM_ADDRESS_LOW_AD_07_00,   JBT6K71_RAM_ADDRESS_LOW_AD_07_00_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_ram_address_high_fields[] = {
    { "AD_16_08",   JBT6K71_RAM_ADDRESS_HIGH_AD_16_08,  JBT6K71_RAM_ADDRESS_HIGH_AD_16_08_SHIFT,    NULL,   0 },
};

static const DebugRegField jbt6k71_gram_data_fields[] = {
    { "DATA",   JBT6K71_GRAM_DATA_DATA, JBT6K71_GRAM_DATA_DATA_SHIFT,   NULL,   0 },
};

static const DebugRegField jbt6k71_graphic_operation_1_fields[] = {
    { "WM_05_00",   JBT6K71_GRAPHIC_OPERATION_1_WM_05_00,   JBT6K71_GRAPHIC_OPERATION_1_WM_05_00_SHIFT, NULL,   0 },
    { "WM_11_06",   JBT6K71_GRAPHIC_OPERATION_1_WM_11_06,   JBT6K71_GRAPHIC_OPERATION_1_WM_11_06_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_graphic_operation_2_fields[] = {
    { "WM_17_12",   JBT6K71_GRAPHIC_OPERATION_2_WM_17_12,   JBT6K71_GRAPHIC_OPERATION_2_WM_17_12_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_gray_scale_1_fields[] = {
    { "PK0",    JBT6K71_GRAY_SCALE_1_PK0,   JBT6K71_GRAY_SCALE_1_PK0_SHIFT, NULL,   0 },
    { "PK1",    JBT6K71_GRAY_SCALE_1_PK1,   JBT6K71_GRAY_SCALE_1_PK1_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_gray_scale_2_fields[] = {
    { "PK2",    JBT6K71_GRAY_SCALE_2_PK2,   JBT6K71_GRAY_SCALE_2_PK2_SHIFT, NULL,   0 },
    { "PK3",    JBT6K71_GRAY_SCALE_2_PK3,   JBT6K71_GRAY_SCALE_2_PK3_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_gray_scale_3_fields[] = {
    { "PK4",    JBT6K71_GRAY_SCALE_3_PK4,   JBT6K71_GRAY_SCALE_3_PK4_SHIFT, NULL,   0 },
    { "PK5",    JBT6K71_GRAY_SCALE_3_PK5,   JBT6K71_GRAY_SCALE_3_PK5_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_gray_scale_4_fields[] = {
    { "PR0",    JBT6K71_GRAY_SCALE_4_PR0,   JBT6K71_GRAY_SCALE_4_PR0_SHIFT, NULL,   0 },
    { "PR1",    JBT6K71_GRAY_SCALE_4_PR1,   JBT6K71_GRAY_SCALE_4_PR1_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_gray_scale_5_fields[] = {
    { "VR0",    JBT6K71_GRAY_SCALE_5_VR0,   JBT6K71_GRAY_SCALE_5_VR0_SHIFT, NULL,   0 },
    { "VR1",    JBT6K71_GRAY_SCALE_5_VR1,   JBT6K71_GRAY_SCALE_5_VR1_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_blue_offset_fields[] = {
    { "BOFS",   JBT6K71_BLUE_OFFSET_BOFS,   JBT6K71_BLUE_OFFSET_BOFS_SHIFT, NULL,   0 },
    { "BUP",    JBT6K71_BLUE_OFFSET_BUP,    JBT6K71_BLUE_OFFSET_BUP_SHIFT,  NULL,   0 },
    { "BLON",   JBT6K71_BLUE_OFFSET_BLON,   JBT6K71_BLUE_OFFSET_BLON_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_vertical_scroll_1_fields[] = {
    { "VL1",    JBT6K71_VERTICAL_SCROLL_1_VL1,  JBT6K71_VERTICAL_SCROLL_1_VL1_SHIFT,    NULL,   0 },
};

static const DebugRegField jbt6k71_vertical_scroll_2_fields[] = {
    { "VL2",    JBT6K71_VERTICAL_SCROLL_2_VL2,  JBT6K71_VERTICAL_SCROLL_2_VL2_SHIFT,    NULL,   0 },
};

static const DebugRegField jbt6k71_screen1_drive_start_fields[] = {
    { "SS1",    JBT6K71_SCREEN1_DRIVE_START_SS1,    JBT6K71_SCREEN1_DRIVE_START_SS1_SHIFT,  NULL,   0 },
};

static const DebugRegField jbt6k71_screen1_drive_end_fields[] = {
    { "SE1",    JBT6K71_SCREEN1_DRIVE_END_SE1,  JBT6K71_SCREEN1_DRIVE_END_SE1_SHIFT,    NULL,   0 },
};

static const DebugRegField jbt6k71_screen2_drive_start_fields[] = {
    { "SS2",    JBT6K71_SCREEN2_DRIVE_START_SS2,    JBT6K71_SCREEN2_DRIVE_START_SS2_SHIFT,  NULL,   0 },
};

static const DebugRegField jbt6k71_screen2_drive_end_fields[] = {
    { "SE2",    JBT6K71_SCREEN2_DRIVE_END_SE2,  JBT6K71_SCREEN2_DRIVE_END_SE2_SHIFT,    NULL,   0 },
};

static const DebugRegField jbt6k71_horizontal_ram_start_fields[] = {
    { "HSA",    JBT6K71_HORIZONTAL_RAM_START_HSA,   JBT6K71_HORIZONTAL_RAM_START_HSA_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_horizontal_ram_end_fields[] = {
    { "HEA",    JBT6K71_HORIZONTAL_RAM_END_HEA, JBT6K71_HORIZONTAL_RAM_END_HEA_SHIFT,   NULL,   0 },
};

static const DebugRegField jbt6k71_vertical_ram_start_fields[] = {
    { "VSA",    JBT6K71_VERTICAL_RAM_START_VSA, JBT6K71_VERTICAL_RAM_START_VSA_SHIFT,   NULL,   0 },
};

static const DebugRegField jbt6k71_vertical_ram_end_fields[] = {
    { "VEA",    JBT6K71_VERTICAL_RAM_END_VEA,   JBT6K71_VERTICAL_RAM_END_VEA_SHIFT, NULL,   0 },
};

static const DebugRegField jbt6k71_osd_control_fields[] = {
    { "OSDON",  JBT6K71_OSD_CONTROL_OSDON,  JBT6K71_OSD_CONTROL_OSDON_SHIFT,    NULL,   0 },
    { "OSDW",   JBT6K71_OSD_CONTROL_OSDW,   JBT6K71_OSD_CONTROL_OSDW_SHIFT,     NULL,   0 },
};

static const DebugRegField jbt6k71_osd_screen1_start_fields[] = {
    { "ST1",    JBT6K71_OSD_SCREEN1_START_ST1,  JBT6K71_OSD_SCREEN1_START_ST1_SHIFT,    NULL,   0 },
};

static const DebugRegField jbt6k71_osd_screen2_start_fields[] = {
    { "ST2",    JBT6K71_OSD_SCREEN2_START_ST2,  JBT6K71_OSD_SCREEN2_START_ST2_SHIFT,    NULL,   0 },
};

static const DebugReg jbt6k71_regs[] = {
    { "OSCILLATION",                JBT6K71_OSCILLATION,                jbt6k71_oscillation_fields,             ARRAY_SIZE(jbt6k71_oscillation_fields) },
    { "DRIVER_OUTPUT_CONTROL",      JBT6K71_DRIVER_OUTPUT_CONTROL,      jbt6k71_driver_output_control_fields,   ARRAY_SIZE(jbt6k71_driver_output_control_fields) },
    { "LCD_DRIVING_SIGNAL",         JBT6K71_LCD_DRIVING_SIGNAL,         jbt6k71_lcd_driving_signal_fields,      ARRAY_SIZE(jbt6k71_lcd_driving_signal_fields) },
    { "ENTRY_MODE",                 JBT6K71_ENTRY_MODE,                 jbt6k71_entry_mode_fields,              ARRAY_SIZE(jbt6k71_entry_mode_fields) },
    { "HORIZONTAL_VALID_WIDTH",     JBT6K71_HORIZONTAL_VALID_WIDTH,     jbt6k71_horizontal_valid_width_fields,  ARRAY_SIZE(jbt6k71_horizontal_valid_width_fields) },
    { "DISPLAY_MODE_1",             JBT6K71_DISPLAY_MODE_1,             jbt6k71_display_mode_1_fields,          ARRAY_SIZE(jbt6k71_display_mode_1_fields) },
    { "DISPLAY_MODE_2",             JBT6K71_DISPLAY_MODE_2,             jbt6k71_display_mode_2_fields,          ARRAY_SIZE(jbt6k71_display_mode_2_fields) },
    { "DISPLAY_MODE_3",             JBT6K71_DISPLAY_MODE_3,             jbt6k71_display_mode_3_fields,          ARRAY_SIZE(jbt6k71_display_mode_3_fields) },
    { "DISPLAY_MODE_4",             JBT6K71_DISPLAY_MODE_4,             jbt6k71_display_mode_4_fields,          ARRAY_SIZE(jbt6k71_display_mode_4_fields) },
    { "EXTERNAL_DISPLAY_1",         JBT6K71_EXTERNAL_DISPLAY_1,         jbt6k71_external_display_1_fields,      ARRAY_SIZE(jbt6k71_external_display_1_fields) },
    { "FR_FREQUENCY_ADJUSTMENT",    JBT6K71_FR_FREQUENCY_ADJUSTMENT,    jbt6k71_fr_frequency_adjustment_fields, ARRAY_SIZE(jbt6k71_fr_frequency_adjustment_fields) },
    { "EXTERNAL_DISPLAY_2",         JBT6K71_EXTERNAL_DISPLAY_2,         jbt6k71_external_display_2_fields,      ARRAY_SIZE(jbt6k71_external_display_2_fields) },
    { "EXTERNAL_DISPLAY_3",         JBT6K71_EXTERNAL_DISPLAY_3,         jbt6k71_external_display_3_fields,      ARRAY_SIZE(jbt6k71_external_display_3_fields) },
    { "LTPS_CONTROL_1",             JBT6K71_LTPS_CONTROL_1,             jbt6k71_ltps_control_1_fields,          ARRAY_SIZE(jbt6k71_ltps_control_1_fields) },
    { "LTPS_CONTROL_2",             JBT6K71_LTPS_CONTROL_2,             jbt6k71_ltps_control_2_fields,          ARRAY_SIZE(jbt6k71_ltps_control_2_fields) },
    { "LTPS_CONTROL_3",             JBT6K71_LTPS_CONTROL_3,             jbt6k71_ltps_control_3_fields,          ARRAY_SIZE(jbt6k71_ltps_control_3_fields) },
    { "LTPS_CONTROL_4",             JBT6K71_LTPS_CONTROL_4,             jbt6k71_ltps_control_4_fields,          ARRAY_SIZE(jbt6k71_ltps_control_4_fields) },
    { "LTPS_CONTROL_5",             JBT6K71_LTPS_CONTROL_5,             jbt6k71_ltps_control_5_fields,          ARRAY_SIZE(jbt6k71_ltps_control_5_fields) },
    { "LTPS_CONTROL_6",             JBT6K71_LTPS_CONTROL_6,             jbt6k71_ltps_control_6_fields,          ARRAY_SIZE(jbt6k71_ltps_control_6_fields) },
    { "LTPS_CONTROL_7",             JBT6K71_LTPS_CONTROL_7,             jbt6k71_ltps_control_7_fields,          ARRAY_SIZE(jbt6k71_ltps_control_7_fields) },
    { "LTPS_CONTROL_8",             JBT6K71_LTPS_CONTROL_8,             jbt6k71_ltps_control_8_fields,          ARRAY_SIZE(jbt6k71_ltps_control_8_fields) },
    { "AMPLIFIER_CAPABILITY",       JBT6K71_AMPLIFIER_CAPABILITY,       jbt6k71_amplifier_capability_fields,    ARRAY_SIZE(jbt6k71_amplifier_capability_fields) },
    { "MODE",                       JBT6K71_MODE,                       jbt6k71_mode_fields,                    ARRAY_SIZE(jbt6k71_mode_fields) },
    { "POWER_OFF_LINE_COUNT",       JBT6K71_POWER_OFF_LINE_COUNT,       jbt6k71_power_off_line_count_fields,    ARRAY_SIZE(jbt6k71_power_off_line_count_fields) },
    { "DISPLAY_CONTROL",            JBT6K71_DISPLAY_CONTROL,            jbt6k71_display_control_fields,         ARRAY_SIZE(jbt6k71_display_control_fields) },
    { "AUTO_MANAGEMENT_CONTROL",    JBT6K71_AUTO_MANAGEMENT_CONTROL,    jbt6k71_auto_management_control_fields, ARRAY_SIZE(jbt6k71_auto_management_control_fields) },
    { "POWER_SUPPLY_CONTROL_1",     JBT6K71_POWER_SUPPLY_CONTROL_1,     jbt6k71_power_supply_control_1_fields,  ARRAY_SIZE(jbt6k71_power_supply_control_1_fields) },
    { "POWER_SUPPLY_CONTROL_2",     JBT6K71_POWER_SUPPLY_CONTROL_2,     jbt6k71_power_supply_control_2_fields,  ARRAY_SIZE(jbt6k71_power_supply_control_2_fields) },
    { "POWER_SUPPLY_CONTROL_3",     JBT6K71_POWER_SUPPLY_CONTROL_3,     jbt6k71_power_supply_control_3_fields,  ARRAY_SIZE(jbt6k71_power_supply_control_3_fields) },
    { "POWER_SUPPLY_CONTROL_4",     JBT6K71_POWER_SUPPLY_CONTROL_4,     jbt6k71_power_supply_control_4_fields,  ARRAY_SIZE(jbt6k71_power_supply_control_4_fields) },
    { "EXTERNAL_POLARITY",          JBT6K71_EXTERNAL_POLARITY,          jbt6k71_external_polarity_fields,       ARRAY_SIZE(jbt6k71_external_polarity_fields) },
    { "RAM_ADDRESS_LOW",            JBT6K71_RAM_ADDRESS_LOW,            jbt6k71_ram_address_low_fields,         ARRAY_SIZE(jbt6k71_ram_address_low_fields) },
    { "RAM_ADDRESS_HIGH",           JBT6K71_RAM_ADDRESS_HIGH,           jbt6k71_ram_address_high_fields,        ARRAY_SIZE(jbt6k71_ram_address_high_fields) },
    { "GRAM_DATA",                  JBT6K71_GRAM_DATA,                  jbt6k71_gram_data_fields,               ARRAY_SIZE(jbt6k71_gram_data_fields) },
    { "GRAPHIC_OPERATION_1",        JBT6K71_GRAPHIC_OPERATION_1,        jbt6k71_graphic_operation_1_fields,     ARRAY_SIZE(jbt6k71_graphic_operation_1_fields) },
    { "GRAPHIC_OPERATION_2",        JBT6K71_GRAPHIC_OPERATION_2,        jbt6k71_graphic_operation_2_fields,     ARRAY_SIZE(jbt6k71_graphic_operation_2_fields) },
    { "GRAY_SCALE_1",               JBT6K71_GRAY_SCALE_1,               jbt6k71_gray_scale_1_fields,            ARRAY_SIZE(jbt6k71_gray_scale_1_fields) },
    { "GRAY_SCALE_2",               JBT6K71_GRAY_SCALE_2,               jbt6k71_gray_scale_2_fields,            ARRAY_SIZE(jbt6k71_gray_scale_2_fields) },
    { "GRAY_SCALE_3",               JBT6K71_GRAY_SCALE_3,               jbt6k71_gray_scale_3_fields,            ARRAY_SIZE(jbt6k71_gray_scale_3_fields) },
    { "GRAY_SCALE_4",               JBT6K71_GRAY_SCALE_4,               jbt6k71_gray_scale_4_fields,            ARRAY_SIZE(jbt6k71_gray_scale_4_fields) },
    { "GRAY_SCALE_5",               JBT6K71_GRAY_SCALE_5,               jbt6k71_gray_scale_5_fields,            ARRAY_SIZE(jbt6k71_gray_scale_5_fields) },
    { "BLUE_OFFSET",                JBT6K71_BLUE_OFFSET,                jbt6k71_blue_offset_fields,             ARRAY_SIZE(jbt6k71_blue_offset_fields) },
    { "VERTICAL_SCROLL_1",          JBT6K71_VERTICAL_SCROLL_1,          jbt6k71_vertical_scroll_1_fields,       ARRAY_SIZE(jbt6k71_vertical_scroll_1_fields) },
    { "VERTICAL_SCROLL_2",          JBT6K71_VERTICAL_SCROLL_2,          jbt6k71_vertical_scroll_2_fields,       ARRAY_SIZE(jbt6k71_vertical_scroll_2_fields) },
    { "SCREEN1_DRIVE_START",        JBT6K71_SCREEN1_DRIVE_START,        jbt6k71_screen1_drive_start_fields,     ARRAY_SIZE(jbt6k71_screen1_drive_start_fields) },
    { "SCREEN1_DRIVE_END",          JBT6K71_SCREEN1_DRIVE_END,          jbt6k71_screen1_drive_end_fields,       ARRAY_SIZE(jbt6k71_screen1_drive_end_fields) },
    { "SCREEN2_DRIVE_START",        JBT6K71_SCREEN2_DRIVE_START,        jbt6k71_screen2_drive_start_fields,     ARRAY_SIZE(jbt6k71_screen2_drive_start_fields) },
    { "SCREEN2_DRIVE_END",          JBT6K71_SCREEN2_DRIVE_END,          jbt6k71_screen2_drive_end_fields,       ARRAY_SIZE(jbt6k71_screen2_drive_end_fields) },
    { "HORIZONTAL_RAM_START",       JBT6K71_HORIZONTAL_RAM_START,       jbt6k71_horizontal_ram_start_fields,    ARRAY_SIZE(jbt6k71_horizontal_ram_start_fields) },
    { "HORIZONTAL_RAM_END",         JBT6K71_HORIZONTAL_RAM_END,         jbt6k71_horizontal_ram_end_fields,      ARRAY_SIZE(jbt6k71_horizontal_ram_end_fields) },
    { "VERTICAL_RAM_START",         JBT6K71_VERTICAL_RAM_START,         jbt6k71_vertical_ram_start_fields,      ARRAY_SIZE(jbt6k71_vertical_ram_start_fields) },
    { "VERTICAL_RAM_END",           JBT6K71_VERTICAL_RAM_END,           jbt6k71_vertical_ram_end_fields,        ARRAY_SIZE(jbt6k71_vertical_ram_end_fields) },
    { "OSD_CONTROL",                JBT6K71_OSD_CONTROL,                jbt6k71_osd_control_fields,             ARRAY_SIZE(jbt6k71_osd_control_fields) },
    { "OSD_SCREEN1_START",          JBT6K71_OSD_SCREEN1_START,          jbt6k71_osd_screen1_start_fields,       ARRAY_SIZE(jbt6k71_osd_screen1_start_fields) },
    { "OSD_SCREEN2_START",          JBT6K71_OSD_SCREEN2_START,          jbt6k71_osd_screen2_start_fields,       ARRAY_SIZE(jbt6k71_osd_screen2_start_fields) },
};

const DebugRegs jbt6k71_debug_regs = {
    .regs = jbt6k71_regs,
    .count = ARRAY_SIZE(jbt6k71_regs),
    .reg_size = 1,
    .addr_digits = 4,
};
