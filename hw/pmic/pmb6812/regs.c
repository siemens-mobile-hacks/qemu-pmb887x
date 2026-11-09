#include "qemu/osdep.h"
#include "regs.h"

static const DebugRegField pmb6812_resctrl_fields[] = {
    { "RES",    PMB6812_RESCTRL_RES,    PMB6812_RESCTRL_RES_SHIFT,      NULL,   0 },
    { "UNLOCK", PMB6812_RESCTRL_UNLOCK, PMB6812_RESCTRL_UNLOCK_SHIFT,   NULL,   0 },
    { "ALLOFF", PMB6812_RESCTRL_ALLOFF, PMB6812_RESCTRL_ALLOFF_SHIFT,   NULL,   0 },
    { "RESDN",  PMB6812_RESCTRL_RESDN,  PMB6812_RESCTRL_RESDN_SHIFT,    NULL,   0 },
};

static const DebugRegValue pmb6812_pwctrl1_fields_lbbmd_values[] = {
    { "OFF",                PMB6812_PWCTRL1_LBBMD_OFF },
    { "LBB1_VCXO_LBB2_ON",  PMB6812_PWCTRL1_LBBMD_LBB1_VCXO_LBB2_ON },
    { "LBB1_OFF_LBB2_ON",   PMB6812_PWCTRL1_LBBMD_LBB1_OFF_LBB2_ON },
    { "ON",                 PMB6812_PWCTRL1_LBBMD_ON },
};

static const DebugRegValue pmb6812_pwctrl1_fields_lbbv_values[] = {
    { "1V5",    PMB6812_PWCTRL1_LBBV_1V5 },
    { "1V65",   PMB6812_PWCTRL1_LBBV_1V65 },
};

static const DebugRegValue pmb6812_pwctrl1_fields_lintmd_values[] = {
    { "STANDBY",    PMB6812_PWCTRL1_LINTMD_STANDBY },
    { "ON",         PMB6812_PWCTRL1_LINTMD_ON },
};

static const DebugRegField pmb6812_pwctrl1_fields[] = {
    { "LANAMD", PMB6812_PWCTRL1_LANAMD, PMB6812_PWCTRL1_LANAMD_SHIFT,   NULL,                                   0 },
    { "LBBMD",  PMB6812_PWCTRL1_LBBMD,  PMB6812_PWCTRL1_LBBMD_SHIFT,    pmb6812_pwctrl1_fields_lbbmd_values,    ARRAY_SIZE(pmb6812_pwctrl1_fields_lbbmd_values) },
    { "LBBV",   PMB6812_PWCTRL1_LBBV,   PMB6812_PWCTRL1_LBBV_SHIFT,     pmb6812_pwctrl1_fields_lbbv_values,     ARRAY_SIZE(pmb6812_pwctrl1_fields_lbbv_values) },
    { "LINTMD", PMB6812_PWCTRL1_LINTMD, PMB6812_PWCTRL1_LINTMD_SHIFT,   pmb6812_pwctrl1_fields_lintmd_values,   ARRAY_SIZE(pmb6812_pwctrl1_fields_lintmd_values) },
};

static const DebugRegValue pmb6812_pwctrl2_fields_vsel_values[] = {
    { "1V5",    PMB6812_PWCTRL2_VSEL_1V5 },
    { "1V8",    PMB6812_PWCTRL2_VSEL_1V8 },
    { "1V86",   PMB6812_PWCTRL2_VSEL_1V86 },
    { "1V92",   PMB6812_PWCTRL2_VSEL_1V92 },
};

static const DebugRegValue pmb6812_pwctrl2_fields_imaxsd_values[] = {
    { "650MA",  PMB6812_PWCTRL2_IMAXSD_650MA },
    { "850MA",  PMB6812_PWCTRL2_IMAXSD_850MA },
};

static const DebugRegValue pmb6812_pwctrl2_fields_sdbbmd_values[] = {
    { "FORCED_PWM", PMB6812_PWCTRL2_SDBBMD_FORCED_PWM },
    { "AUTO_PFM",   PMB6812_PWCTRL2_SDBBMD_AUTO_PFM },
    { "TEST",       PMB6812_PWCTRL2_SDBBMD_TEST },
    { "FORCED_PFM", PMB6812_PWCTRL2_SDBBMD_FORCED_PFM },
};

static const DebugRegField pmb6812_pwctrl2_fields[] = {
    { "VSEL",   PMB6812_PWCTRL2_VSEL,   PMB6812_PWCTRL2_VSEL_SHIFT,     pmb6812_pwctrl2_fields_vsel_values,     ARRAY_SIZE(pmb6812_pwctrl2_fields_vsel_values) },
    { "IMAXSD", PMB6812_PWCTRL2_IMAXSD, PMB6812_PWCTRL2_IMAXSD_SHIFT,   pmb6812_pwctrl2_fields_imaxsd_values,   ARRAY_SIZE(pmb6812_pwctrl2_fields_imaxsd_values) },
    { "LPEN",   PMB6812_PWCTRL2_LPEN,   PMB6812_PWCTRL2_LPEN_SHIFT,     NULL,                                   0 },
    { "ASPWM",  PMB6812_PWCTRL2_ASPWM,  PMB6812_PWCTRL2_ASPWM_SHIFT,    NULL,                                   0 },
    { "SDBBMD", PMB6812_PWCTRL2_SDBBMD, PMB6812_PWCTRL2_SDBBMD_SHIFT,   pmb6812_pwctrl2_fields_sdbbmd_values,   ARRAY_SIZE(pmb6812_pwctrl2_fields_sdbbmd_values) },
};

static const DebugRegValue pmb6812_chctrl1_fields_pl_values[] = {
    { "2MS",    PMB6812_CHCTRL1_PL_2MS },
    { "4MS",    PMB6812_CHCTRL1_PL_4MS },
    { "8MS",    PMB6812_CHCTRL1_PL_8MS },
    { "16MS",   PMB6812_CHCTRL1_PL_16MS },
    { "33MS",   PMB6812_CHCTRL1_PL_33MS },
    { "66MS",   PMB6812_CHCTRL1_PL_66MS },
    { "131MS",  PMB6812_CHCTRL1_PL_131MS },
    { "262MS",  PMB6812_CHCTRL1_PL_262MS },
};

static const DebugRegValue pmb6812_chctrl1_fields_vl_values[] = {
    { "SHUTDOWN",       PMB6812_CHCTRL1_VL_SHUTDOWN },
    { "VOLTAGE_LIMIT",  PMB6812_CHCTRL1_VL_VOLTAGE_LIMIT },
};

static const DebugRegValue pmb6812_chctrl1_fields_vmax_values[] = {
    { "VCHMAX1",    PMB6812_CHCTRL1_VMAX_VCHMAX1 },
    { "VCHMAX2",    PMB6812_CHCTRL1_VMAX_VCHMAX2 },
    { "RESERVED",   PMB6812_CHCTRL1_VMAX_RESERVED },
    { "VCHMAX3",    PMB6812_CHCTRL1_VMAX_VCHMAX3 },
};

static const DebugRegValue pmb6812_chctrl1_fields_pch_values[] = {
    { "CONTINUOUS", PMB6812_CHCTRL1_PCH_CONTINUOUS },
    { "PULSE",      PMB6812_CHCTRL1_PCH_PULSE },
};

static const DebugRegField pmb6812_chctrl1_fields[] = {
    { "PL",     PMB6812_CHCTRL1_PL,     PMB6812_CHCTRL1_PL_SHIFT,   pmb6812_chctrl1_fields_pl_values,   ARRAY_SIZE(pmb6812_chctrl1_fields_pl_values) },
    { "VL",     PMB6812_CHCTRL1_VL,     PMB6812_CHCTRL1_VL_SHIFT,   pmb6812_chctrl1_fields_vl_values,   ARRAY_SIZE(pmb6812_chctrl1_fields_vl_values) },
    { "VMAX",   PMB6812_CHCTRL1_VMAX,   PMB6812_CHCTRL1_VMAX_SHIFT, pmb6812_chctrl1_fields_vmax_values, ARRAY_SIZE(pmb6812_chctrl1_fields_vmax_values) },
    { "PCH",    PMB6812_CHCTRL1_PCH,    PMB6812_CHCTRL1_PCH_SHIFT,  pmb6812_chctrl1_fields_pch_values,  ARRAY_SIZE(pmb6812_chctrl1_fields_pch_values) },
    { "ON",     PMB6812_CHCTRL1_ON,     PMB6812_CHCTRL1_ON_SHIFT,   NULL,                               0 },
};

static const DebugRegValue pmb6812_intctrl1_fields_intmd_values[] = {
    { "FALLING",    PMB6812_INTCTRL1_INTMD_FALLING },
    { "RISING",     PMB6812_INTCTRL1_INTMD_RISING },
};

static const DebugRegField pmb6812_intctrl1_fields[] = {
    { "INTMD",      PMB6812_INTCTRL1_INTMD,     PMB6812_INTCTRL1_INTMD_SHIFT,       pmb6812_intctrl1_fields_intmd_values,   ARRAY_SIZE(pmb6812_intctrl1_fields_intmd_values) },
    { "EIORLSIM",   PMB6812_INTCTRL1_EIORLSIM,  PMB6812_INTCTRL1_EIORLSIM_SHIFT,    NULL,                                   0 },
    { "EIORLMMC",   PMB6812_INTCTRL1_EIORLMMC,  PMB6812_INTCTRL1_EIORLMMC_SHIFT,    NULL,                                   0 },
    { "EISPWM",     PMB6812_INTCTRL1_EISPWM,    PMB6812_INTCTRL1_EISPWM_SHIFT,      NULL,                                   0 },
    { "EICHCAL",    PMB6812_INTCTRL1_EICHCAL,   PMB6812_INTCTRL1_EICHCAL_SHIFT,     NULL,                                   0 },
    { "EICHMD",     PMB6812_INTCTRL1_EICHMD,    PMB6812_INTCTRL1_EICHMD_SHIFT,      NULL,                                   0 },
    { "EICHV",      PMB6812_INTCTRL1_EICHV,     PMB6812_INTCTRL1_EICHV_SHIFT,       NULL,                                   0 },
};

static const DebugRegValue pmb6812_intctrl2_fields_debug_values[] = {
    { "CURRENT",    PMB6812_INTCTRL2_DEBUG_CURRENT },
    { "LATCHED",    PMB6812_INTCTRL2_DEBUG_LATCHED },
};

static const DebugRegField pmb6812_intctrl2_fields[] = {
    { "DEBUG",      PMB6812_INTCTRL2_DEBUG,     PMB6812_INTCTRL2_DEBUG_SHIFT,   pmb6812_intctrl2_fields_debug_values,   ARRAY_SIZE(pmb6812_intctrl2_fields_debug_values) },
    { "EION",       PMB6812_INTCTRL2_EION,      PMB6812_INTCTRL2_EION_SHIFT,    NULL,                                   0 },
    { "EIVBH",      PMB6812_INTCTRL2_EIVBH,     PMB6812_INTCTRL2_EIVBH_SHIFT,   NULL,                                   0 },
    { "EIVBOUT",    PMB6812_INTCTRL2_EIVBOUT,   PMB6812_INTCTRL2_EIVBOUT_SHIFT, NULL,                                   0 },
    { "OTSEN",      PMB6812_INTCTRL2_OTSEN,     PMB6812_INTCTRL2_OTSEN_SHIFT,   NULL,                                   0 },
    { "RAGOTW",     PMB6812_INTCTRL2_RAGOTW,    PMB6812_INTCTRL2_RAGOTW_SHIFT,  NULL,                                   0 },
    { "EIOTW",      PMB6812_INTCTRL2_EIOTW,     PMB6812_INTCTRL2_EIOTW_SHIFT,   NULL,                                   0 },
};

static const DebugRegValue pmb6812_pwctrl3_fields_lrf2md_values[] = {
    { "OFF",        PMB6812_PWCTRL3_LRF2MD_OFF },
    { "VCXO",       PMB6812_PWCTRL3_LRF2MD_VCXO },
    { "FORCED_OFF", PMB6812_PWCTRL3_LRF2MD_FORCED_OFF },
    { "ON",         PMB6812_PWCTRL3_LRF2MD_ON },
};

static const DebugRegValue pmb6812_pwctrl3_fields_lrf2v_values[] = {
    { "2V5",    PMB6812_PWCTRL3_LRF2V_2V5 },
    { "2V7",    PMB6812_PWCTRL3_LRF2V_2V7 },
};

static const DebugRegValue pmb6812_pwctrl3_fields_lrf3md_values[] = {
    { "OFF",    PMB6812_PWCTRL3_LRF3MD_OFF },
    { "VCXO",   PMB6812_PWCTRL3_LRF3MD_VCXO },
    { "VRF3",   PMB6812_PWCTRL3_LRF3MD_VRF3 },
    { "ON",     PMB6812_PWCTRL3_LRF3MD_ON },
};

static const DebugRegValue pmb6812_pwctrl3_fields_lrf3v_values[] = {
    { "2V65",   PMB6812_PWCTRL3_LRF3V_2V65 },
    { "1V8",    PMB6812_PWCTRL3_LRF3V_1V8 },
};

static const DebugRegField pmb6812_pwctrl3_fields[] = {
    { "LRF2MD", PMB6812_PWCTRL3_LRF2MD, PMB6812_PWCTRL3_LRF2MD_SHIFT,   pmb6812_pwctrl3_fields_lrf2md_values,   ARRAY_SIZE(pmb6812_pwctrl3_fields_lrf2md_values) },
    { "LRF2V",  PMB6812_PWCTRL3_LRF2V,  PMB6812_PWCTRL3_LRF2V_SHIFT,    pmb6812_pwctrl3_fields_lrf2v_values,    ARRAY_SIZE(pmb6812_pwctrl3_fields_lrf2v_values) },
    { "LRF3MD", PMB6812_PWCTRL3_LRF3MD, PMB6812_PWCTRL3_LRF3MD_SHIFT,   pmb6812_pwctrl3_fields_lrf3md_values,   ARRAY_SIZE(pmb6812_pwctrl3_fields_lrf3md_values) },
    { "LRF3V",  PMB6812_PWCTRL3_LRF3V,  PMB6812_PWCTRL3_LRF3V_SHIFT,    pmb6812_pwctrl3_fields_lrf3v_values,    ARRAY_SIZE(pmb6812_pwctrl3_fields_lrf3v_values) },
};

static const DebugRegValue pmb6812_chctrl2_fields_chclim_values[] = {
    { "400MA",  PMB6812_CHCTRL2_CHCLIM_400MA },
    { "500MA",  PMB6812_CHCTRL2_CHCLIM_500MA },
    { "600MA",  PMB6812_CHCTRL2_CHCLIM_600MA },
    { "700MA",  PMB6812_CHCTRL2_CHCLIM_700MA },
    { "800MA",  PMB6812_CHCTRL2_CHCLIM_800MA },
    { "900MA",  PMB6812_CHCTRL2_CHCLIM_900MA },
    { "1000MA", PMB6812_CHCTRL2_CHCLIM_1000MA },
    { "1100MA", PMB6812_CHCTRL2_CHCLIM_1100MA },
};

static const DebugRegValue pmb6812_chctrl2_fields_rvm_values[] = {
    { "X1", PMB6812_CHCTRL2_RVM_X1 },
    { "X2", PMB6812_CHCTRL2_RVM_X2 },
    { "X3", PMB6812_CHCTRL2_RVM_X3 },
    { "X4", PMB6812_CHCTRL2_RVM_X4 },
    { "X5", PMB6812_CHCTRL2_RVM_X5 },
    { "X6", PMB6812_CHCTRL2_RVM_X6 },
    { "X7", PMB6812_CHCTRL2_RVM_X7 },
    { "X8", PMB6812_CHCTRL2_RVM_X8 },
};

static const DebugRegField pmb6812_chctrl2_fields[] = {
    { "CHCLIM", PMB6812_CHCTRL2_CHCLIM, PMB6812_CHCTRL2_CHCLIM_SHIFT,   pmb6812_chctrl2_fields_chclim_values,   ARRAY_SIZE(pmb6812_chctrl2_fields_chclim_values) },
    { "PREOFF", PMB6812_CHCTRL2_PREOFF, PMB6812_CHCTRL2_PREOFF_SHIFT,   NULL,                                   0 },
    { "RVM",    PMB6812_CHCTRL2_RVM,    PMB6812_CHCTRL2_RVM_SHIFT,      pmb6812_chctrl2_fields_rvm_values,      ARRAY_SIZE(pmb6812_chctrl2_fields_rvm_values) },
};

static const DebugRegField pmb6812_test1_fields[] = {
    { "BGTVAL", PMB6812_TEST1_BGTVAL,   PMB6812_TEST1_BGTVAL_SHIFT, NULL,   0 },
    { "BGTRIM", PMB6812_TEST1_BGTRIM,   PMB6812_TEST1_BGTRIM_SHIFT, NULL,   0 },
    { "OTWSEN", PMB6812_TEST1_OTWSEN,   PMB6812_TEST1_OTWSEN_SHIFT, NULL,   0 },
    { "OTSSEN", PMB6812_TEST1_OTSSEN,   PMB6812_TEST1_OTSSEN_SHIFT, NULL,   0 },
};

static const DebugRegValue pmb6812_ledctrl1_fields_pwmfreq_values[] = {
    { "60HZ",   PMB6812_LEDCTRL1_PWMFREQ_60HZ },
    { "16KHZ",  PMB6812_LEDCTRL1_PWMFREQ_16KHZ },
};

static const DebugRegField pmb6812_ledctrl1_fields[] = {
    { "LEDON",      PMB6812_LEDCTRL1_LEDON,     PMB6812_LEDCTRL1_LEDON_SHIFT,   NULL,                                   0 },
    { "LEDPWM",     PMB6812_LEDCTRL1_LEDPWM,    PMB6812_LEDCTRL1_LEDPWM_SHIFT,  NULL,                                   0 },
    { "PWMFREQ",    PMB6812_LEDCTRL1_PWMFREQ,   PMB6812_LEDCTRL1_PWMFREQ_SHIFT, pmb6812_ledctrl1_fields_pwmfreq_values, ARRAY_SIZE(pmb6812_ledctrl1_fields_pwmfreq_values) },
};

static const DebugRegField pmb6812_drvctrl_fields[] = {
    { "VVIB",   PMB6812_DRVCTRL_VVIB,   PMB6812_DRVCTRL_VVIB_SHIFT, NULL,   0 },
};

static const DebugRegValue pmb6812_usbctrl_fields_lusbmd_values[] = {
    { "OFF",            PMB6812_USBCTRL_LUSBMD_OFF },
    { "STANDBY_VCXO",   PMB6812_USBCTRL_LUSBMD_STANDBY_VCXO },
    { "STANDBY",        PMB6812_USBCTRL_LUSBMD_STANDBY },
    { "ON",             PMB6812_USBCTRL_LUSBMD_ON },
};

static const DebugRegValue pmb6812_usbctrl_fields_pullup_values[] = {
    { "NC",         PMB6812_USBCTRL_PULLUP_NC },
    { "PULLUP",     PMB6812_USBCTRL_PULLUP_PULLUP },
    { "PULLDOWN",   PMB6812_USBCTRL_PULLUP_PULLDOWN },
};

static const DebugRegField pmb6812_usbctrl_fields[] = {
    { "VBUS",   PMB6812_USBCTRL_VBUS,   PMB6812_USBCTRL_VBUS_SHIFT,     NULL,                                   0 },
    { "LUSBMD", PMB6812_USBCTRL_LUSBMD, PMB6812_USBCTRL_LUSBMD_SHIFT,   pmb6812_usbctrl_fields_lusbmd_values,   ARRAY_SIZE(pmb6812_usbctrl_fields_lusbmd_values) },
    { "PULLUP", PMB6812_USBCTRL_PULLUP, PMB6812_USBCTRL_PULLUP_SHIFT,   pmb6812_usbctrl_fields_pullup_values,   ARRAY_SIZE(pmb6812_usbctrl_fields_pullup_values) },
    { "COMP2",  PMB6812_USBCTRL_COMP2,  PMB6812_USBCTRL_COMP2_SHIFT,    NULL,                                   0 },
    { "COMP4",  PMB6812_USBCTRL_COMP4,  PMB6812_USBCTRL_COMP4_SHIFT,    NULL,                                   0 },
    { "DISCH",  PMB6812_USBCTRL_DISCH,  PMB6812_USBCTRL_DISCH_SHIFT,    NULL,                                   0 },
};

static const DebugRegValue pmb6812_audctrl_fields_audgain_values[] = {
    { "MINUS_6DB",      PMB6812_AUDCTRL_AUDGAIN_MINUS_6DB },
    { "MINUS_1_2DB",    PMB6812_AUDCTRL_AUDGAIN_MINUS_1_2DB },
    { "PLUS_2_7DB",     PMB6812_AUDCTRL_AUDGAIN_PLUS_2_7DB },
    { "PLUS_6DB",       PMB6812_AUDCTRL_AUDGAIN_PLUS_6DB },
};

static const DebugRegValue pmb6812_audctrl_fields_audcm_values[] = {
    { "1550MV", PMB6812_AUDCTRL_AUDCM_1550MV },
    { "1650MV", PMB6812_AUDCTRL_AUDCM_1650MV },
    { "1800MV", PMB6812_AUDCTRL_AUDCM_1800MV },
    { "1950MV", PMB6812_AUDCTRL_AUDCM_1950MV },
};

static const DebugRegField pmb6812_audctrl_fields[] = {
    { "AUDON",      PMB6812_AUDCTRL_AUDON,      PMB6812_AUDCTRL_AUDON_SHIFT,    NULL,                                   0 },
    { "MUTE",       PMB6812_AUDCTRL_MUTE,       PMB6812_AUDCTRL_MUTE_SHIFT,     NULL,                                   0 },
    { "AUDGAIN",    PMB6812_AUDCTRL_AUDGAIN,    PMB6812_AUDCTRL_AUDGAIN_SHIFT,  pmb6812_audctrl_fields_audgain_values,  ARRAY_SIZE(pmb6812_audctrl_fields_audgain_values) },
    { "TRISTATE",   PMB6812_AUDCTRL_TRISTATE,   PMB6812_AUDCTRL_TRISTATE_SHIFT, NULL,                                   0 },
    { "AUDBST",     PMB6812_AUDCTRL_AUDBST,     PMB6812_AUDCTRL_AUDBST_SHIFT,   NULL,                                   0 },
    { "AUDCM",      PMB6812_AUDCTRL_AUDCM,      PMB6812_AUDCTRL_AUDCM_SHIFT,    pmb6812_audctrl_fields_audcm_values,    ARRAY_SIZE(pmb6812_audctrl_fields_audcm_values) },
};

static const DebugRegValue pmb6812_pwctrl4_fields_lrf1md_values[] = {
    { "OFF",    PMB6812_PWCTRL4_LRF1MD_OFF },
    { "VCXO",   PMB6812_PWCTRL4_LRF1MD_VCXO },
    { "VRF3",   PMB6812_PWCTRL4_LRF1MD_VRF3 },
    { "ON",     PMB6812_PWCTRL4_LRF1MD_ON },
};

static const DebugRegValue pmb6812_pwctrl4_fields_lrf1v_values[] = {
    { "2V85",       PMB6812_PWCTRL4_LRF1V_2V85 },
    { "2V7",        PMB6812_PWCTRL4_LRF1V_2V7 },
    { "2V5",        PMB6812_PWCTRL4_LRF1V_2V5 },
    { "RESERVED",   PMB6812_PWCTRL4_LRF1V_RESERVED },
};

static const DebugRegValue pmb6812_pwctrl4_fields_lrfcmd_values[] = {
    { "OFF",    PMB6812_PWCTRL4_LRFCMD_OFF },
    { "VCXO",   PMB6812_PWCTRL4_LRFCMD_VCXO },
    { "VRF3",   PMB6812_PWCTRL4_LRFCMD_VRF3 },
    { "ON",     PMB6812_PWCTRL4_LRFCMD_ON },
};

static const DebugRegField pmb6812_pwctrl4_fields[] = {
    { "LRF1MD", PMB6812_PWCTRL4_LRF1MD, PMB6812_PWCTRL4_LRF1MD_SHIFT,   pmb6812_pwctrl4_fields_lrf1md_values,   ARRAY_SIZE(pmb6812_pwctrl4_fields_lrf1md_values) },
    { "LRF1V",  PMB6812_PWCTRL4_LRF1V,  PMB6812_PWCTRL4_LRF1V_SHIFT,    pmb6812_pwctrl4_fields_lrf1v_values,    ARRAY_SIZE(pmb6812_pwctrl4_fields_lrf1v_values) },
    { "LRFCMD", PMB6812_PWCTRL4_LRFCMD, PMB6812_PWCTRL4_LRFCMD_SHIFT,   pmb6812_pwctrl4_fields_lrfcmd_values,   ARRAY_SIZE(pmb6812_pwctrl4_fields_lrfcmd_values) },
};

static const DebugRegValue pmb6812_test2_fields_rds_on_sel_values[] = {
    { "PMOS",   PMB6812_TEST2_RDS_ON_SEL_PMOS },
    { "NMOS",   PMB6812_TEST2_RDS_ON_SEL_NMOS },
};

static const DebugRegValue pmb6812_test2_fields_bandgap_values[] = {
    { "R70K",   PMB6812_TEST2_BANDGAP_R70K },
    { "R7K",    PMB6812_TEST2_BANDGAP_R7K },
};

static const DebugRegField pmb6812_test2_fields[] = {
    { "DISCAD",     PMB6812_TEST2_DISCAD,       PMB6812_TEST2_DISCAD_SHIFT,     NULL,                                   0 },
    { "RDS_ON_EN",  PMB6812_TEST2_RDS_ON_EN,    PMB6812_TEST2_RDS_ON_EN_SHIFT,  NULL,                                   0 },
    { "RDS_ON_SEL", PMB6812_TEST2_RDS_ON_SEL,   PMB6812_TEST2_RDS_ON_SEL_SHIFT, pmb6812_test2_fields_rds_on_sel_values, ARRAY_SIZE(pmb6812_test2_fields_rds_on_sel_values) },
    { "LEAKAGE",    PMB6812_TEST2_LEAKAGE,      PMB6812_TEST2_LEAKAGE_SHIFT,    NULL,                                   0 },
    { "PFMTEST",    PMB6812_TEST2_PFMTEST,      PMB6812_TEST2_PFMTEST_SHIFT,    NULL,                                   0 },
    { "BANDGAP",    PMB6812_TEST2_BANDGAP,      PMB6812_TEST2_BANDGAP_SHIFT,    pmb6812_test2_fields_bandgap_values,    ARRAY_SIZE(pmb6812_test2_fields_bandgap_values) },
    { "OSCOFF",     PMB6812_TEST2_OSCOFF,       PMB6812_TEST2_OSCOFF_SHIFT,     NULL,                                   0 },
    { "USBPROT",    PMB6812_TEST2_USBPROT,      PMB6812_TEST2_USBPROT_SHIFT,    NULL,                                   0 },
};

static const DebugRegField pmb6812_test3_fields[] = {
    { "LDDIS",      PMB6812_TEST3_LDDIS,        PMB6812_TEST3_LDDIS_SHIFT,      NULL,   0 },
    { "VCHMAXCAL",  PMB6812_TEST3_VCHMAXCAL,    PMB6812_TEST3_VCHMAXCAL_SHIFT,  NULL,   0 },
    { "CHALEN",     PMB6812_TEST3_CHALEN,       PMB6812_TEST3_CHALEN_SHIFT,     NULL,   0 },
    { "OSCOFF",     PMB6812_TEST3_OSCOFF,       PMB6812_TEST3_OSCOFF_SHIFT,     NULL,   0 },
};

static const DebugRegValue pmb6812_pwctrl5_fields_lsimmd_values[] = {
    { "OFF",            PMB6812_PWCTRL5_LSIMMD_OFF },
    { "VCXO_STANDBY",   PMB6812_PWCTRL5_LSIMMD_VCXO_STANDBY },
    { "STANDBY",        PMB6812_PWCTRL5_LSIMMD_STANDBY },
    { "ON",             PMB6812_PWCTRL5_LSIMMD_ON },
};

static const DebugRegValue pmb6812_pwctrl5_fields_lsimv_values[] = {
    { "2V85",   PMB6812_PWCTRL5_LSIMV_2V85 },
    { "1V8",    PMB6812_PWCTRL5_LSIMV_1V8 },
};

static const DebugRegValue pmb6812_pwctrl5_fields_lsim2md_values[] = {
    { "OFF",                PMB6812_PWCTRL5_LSIM2MD_OFF },
    { "VCXO_STANDBY",       PMB6812_PWCTRL5_LSIM2MD_VCXO_STANDBY },
    { "ON",                 PMB6812_PWCTRL5_LSIM2MD_ON },
    { "VCXO_STANDBY_ALT",   PMB6812_PWCTRL5_LSIM2MD_VCXO_STANDBY_ALT },
    { "STANDBY",            PMB6812_PWCTRL5_LSIM2MD_STANDBY },
    { "ON_ALT1",            PMB6812_PWCTRL5_LSIM2MD_ON_ALT1 },
    { "OFF_ALT",            PMB6812_PWCTRL5_LSIM2MD_OFF_ALT },
    { "ON_ALT2",            PMB6812_PWCTRL5_LSIM2MD_ON_ALT2 },
};

static const DebugRegValue pmb6812_pwctrl5_fields_lsim2v_values[] = {
    { "2V85",   PMB6812_PWCTRL5_LSIM2V_2V85 },
    { "1V8",    PMB6812_PWCTRL5_LSIM2V_1V8 },
};

static const DebugRegField pmb6812_pwctrl5_fields[] = {
    { "LSIMMD",     PMB6812_PWCTRL5_LSIMMD,     PMB6812_PWCTRL5_LSIMMD_SHIFT,   pmb6812_pwctrl5_fields_lsimmd_values,   ARRAY_SIZE(pmb6812_pwctrl5_fields_lsimmd_values) },
    { "LSIMV",      PMB6812_PWCTRL5_LSIMV,      PMB6812_PWCTRL5_LSIMV_SHIFT,    pmb6812_pwctrl5_fields_lsimv_values,    ARRAY_SIZE(pmb6812_pwctrl5_fields_lsimv_values) },
    { "LSIM2MD",    PMB6812_PWCTRL5_LSIM2MD,    PMB6812_PWCTRL5_LSIM2MD_SHIFT,  pmb6812_pwctrl5_fields_lsim2md_values,  ARRAY_SIZE(pmb6812_pwctrl5_fields_lsim2md_values) },
    { "LSIM2V",     PMB6812_PWCTRL5_LSIM2V,     PMB6812_PWCTRL5_LSIM2V_SHIFT,   pmb6812_pwctrl5_fields_lsim2v_values,   ARRAY_SIZE(pmb6812_pwctrl5_fields_lsim2v_values) },
};

static const DebugRegValue pmb6812_pwctrl6_fields_lmmcmd_values[] = {
    { "OFF",            PMB6812_PWCTRL6_LMMCMD_OFF },
    { "VCXO_STANDBY",   PMB6812_PWCTRL6_LMMCMD_VCXO_STANDBY },
    { "STANDBY",        PMB6812_PWCTRL6_LMMCMD_STANDBY },
    { "ON",             PMB6812_PWCTRL6_LMMCMD_ON },
};

static const DebugRegValue pmb6812_pwctrl6_fields_lmmcv_values[] = {
    { "1V8",    PMB6812_PWCTRL6_LMMCV_1V8 },
    { "2V85",   PMB6812_PWCTRL6_LMMCV_2V85 },
};

static const DebugRegField pmb6812_pwctrl6_fields[] = {
    { "LMMCMD", PMB6812_PWCTRL6_LMMCMD, PMB6812_PWCTRL6_LMMCMD_SHIFT,   pmb6812_pwctrl6_fields_lmmcmd_values,   ARRAY_SIZE(pmb6812_pwctrl6_fields_lmmcmd_values) },
    { "LMMCV",  PMB6812_PWCTRL6_LMMCV,  PMB6812_PWCTRL6_LMMCV_SHIFT,    pmb6812_pwctrl6_fields_lmmcv_values,    ARRAY_SIZE(pmb6812_pwctrl6_fields_lmmcv_values) },
};

static const DebugRegValue pmb6812_ledctrl2_fields_ledcur_values[] = {
    { "POWER_DOWN", PMB6812_LEDCTRL2_LEDCUR_POWER_DOWN },
    { "20MA",       PMB6812_LEDCTRL2_LEDCUR_20MA },
    { "40MA",       PMB6812_LEDCTRL2_LEDCUR_40MA },
    { "60MA",       PMB6812_LEDCTRL2_LEDCUR_60MA },
    { "80MA",       PMB6812_LEDCTRL2_LEDCUR_80MA },
    { "100MA",      PMB6812_LEDCTRL2_LEDCUR_100MA },
    { "120MA",      PMB6812_LEDCTRL2_LEDCUR_120MA },
    { "140MA",      PMB6812_LEDCTRL2_LEDCUR_140MA },
};

static const DebugRegField pmb6812_ledctrl2_fields[] = {
    { "LEDCUR",     PMB6812_LEDCTRL2_LEDCUR,    PMB6812_LEDCTRL2_LEDCUR_SHIFT,  pmb6812_ledctrl2_fields_ledcur_values,  ARRAY_SIZE(pmb6812_ledctrl2_fields_ledcur_values) },
    { "SLED1ON",    PMB6812_LEDCTRL2_SLED1ON,   PMB6812_LEDCTRL2_SLED1ON_SHIFT, NULL,                                   0 },
    { "SLED2ON",    PMB6812_LEDCTRL2_SLED2ON,   PMB6812_LEDCTRL2_SLED2ON_SHIFT, NULL,                                   0 },
};

static const DebugRegField pmb6812_test4_fields[] = {
    { "PROGV",  PMB6812_TEST4_PROGV,    PMB6812_TEST4_PROGV_SHIFT,  NULL,   0 },
};

static const DebugRegField pmb6812_gef1_fields[] = {
    { "LINT",   PMB6812_GEF1_LINT,  PMB6812_GEF1_LINT_SHIFT,    NULL,   0 },
    { "LANA",   PMB6812_GEF1_LANA,  PMB6812_GEF1_LANA_SHIFT,    NULL,   0 },
    { "LRF2",   PMB6812_GEF1_LRF2,  PMB6812_GEF1_LRF2_SHIFT,    NULL,   0 },
    { "LRF1",   PMB6812_GEF1_LRF1,  PMB6812_GEF1_LRF1_SHIFT,    NULL,   0 },
    { "LRFC",   PMB6812_GEF1_LRFC,  PMB6812_GEF1_LRFC_SHIFT,    NULL,   0 },
    { "LRF3",   PMB6812_GEF1_LRF3,  PMB6812_GEF1_LRF3_SHIFT,    NULL,   0 },
    { "LUSB",   PMB6812_GEF1_LUSB,  PMB6812_GEF1_LUSB_SHIFT,    NULL,   0 },
};

static const DebugRegField pmb6812_isf_fields[] = {
    { "LON",    PMB6812_ISF_LON,    PMB6812_ISF_LON_SHIFT,      NULL,   0 },
    { "OTS",    PMB6812_ISF_OTS,    PMB6812_ISF_OTS_SHIFT,      NULL,   0 },
    { "SPWM",   PMB6812_ISF_SPWM,   PMB6812_ISF_SPWM_SHIFT,     NULL,   0 },
    { "LSIM",   PMB6812_ISF_LSIM,   PMB6812_ISF_LSIM_SHIFT,     NULL,   0 },
    { "LMMC",   PMB6812_ISF_LMMC,   PMB6812_ISF_LMMC_SHIFT,     NULL,   0 },
    { "VBH",    PMB6812_ISF_VBH,    PMB6812_ISF_VBH_SHIFT,      NULL,   0 },
    { "BVOUT",  PMB6812_ISF_BVOUT,  PMB6812_ISF_BVOUT_SHIFT,    NULL,   0 },
    { "OTW",    PMB6812_ISF_OTW,    PMB6812_ISF_OTW_SHIFT,      NULL,   0 },
};

static const DebugRegValue pmb6812_chst_fields_pmuid_values[] = {
    { "SM_POWER",       PMB6812_CHST_PMUID_SM_POWER },
    { "E_POWERLITE",    PMB6812_CHST_PMUID_E_POWERLITE },
};

static const DebugRegField pmb6812_chst_fields[] = {
    { "PMUID",  PMB6812_CHST_PMUID, PMB6812_CHST_PMUID_SHIFT,   pmb6812_chst_fields_pmuid_values,   ARRAY_SIZE(pmb6812_chst_fields_pmuid_values) },
    { "CCAL",   PMB6812_CHST_CCAL,  PMB6812_CHST_CCAL_SHIFT,    NULL,                               0 },
    { "CHMD",   PMB6812_CHST_CHMD,  PMB6812_CHST_CHMD_SHIFT,    NULL,                               0 },
    { "CHV",    PMB6812_CHST_CHV,   PMB6812_CHST_CHV_SHIFT,     NULL,                               0 },
};

static const DebugRegField pmb6812_gef2_fields[] = {
    { "SDBB",   PMB6812_GEF2_SDBB,  PMB6812_GEF2_SDBB_SHIFT,    NULL,   0 },
    { "LBB1",   PMB6812_GEF2_LBB1,  PMB6812_GEF2_LBB1_SHIFT,    NULL,   0 },
    { "LBB2",   PMB6812_GEF2_LBB2,  PMB6812_GEF2_LBB2_SHIFT,    NULL,   0 },
    { "LSIM2",  PMB6812_GEF2_LSIM2, PMB6812_GEF2_LSIM2_SHIFT,   NULL,   0 },
    { "OTWD",   PMB6812_GEF2_OTWD,  PMB6812_GEF2_OTWD_SHIFT,    NULL,   0 },
};

static const DebugReg pmb6812_regs[] = {
    { "RESCTRL",    PMB6812_RESCTRL,    pmb6812_resctrl_fields,     ARRAY_SIZE(pmb6812_resctrl_fields) },
    { "PWCTRL1",    PMB6812_PWCTRL1,    pmb6812_pwctrl1_fields,     ARRAY_SIZE(pmb6812_pwctrl1_fields) },
    { "PWCTRL2",    PMB6812_PWCTRL2,    pmb6812_pwctrl2_fields,     ARRAY_SIZE(pmb6812_pwctrl2_fields) },
    { "CHCTRL1",    PMB6812_CHCTRL1,    pmb6812_chctrl1_fields,     ARRAY_SIZE(pmb6812_chctrl1_fields) },
    { "INTCTRL1",   PMB6812_INTCTRL1,   pmb6812_intctrl1_fields,    ARRAY_SIZE(pmb6812_intctrl1_fields) },
    { "INTCTRL2",   PMB6812_INTCTRL2,   pmb6812_intctrl2_fields,    ARRAY_SIZE(pmb6812_intctrl2_fields) },
    { "PWCTRL3",    PMB6812_PWCTRL3,    pmb6812_pwctrl3_fields,     ARRAY_SIZE(pmb6812_pwctrl3_fields) },
    { "CHCTRL2",    PMB6812_CHCTRL2,    pmb6812_chctrl2_fields,     ARRAY_SIZE(pmb6812_chctrl2_fields) },
    { "TEST1",      PMB6812_TEST1,      pmb6812_test1_fields,       ARRAY_SIZE(pmb6812_test1_fields) },
    { "LEDCTRL1",   PMB6812_LEDCTRL1,   pmb6812_ledctrl1_fields,    ARRAY_SIZE(pmb6812_ledctrl1_fields) },
    { "DRVCTRL",    PMB6812_DRVCTRL,    pmb6812_drvctrl_fields,     ARRAY_SIZE(pmb6812_drvctrl_fields) },
    { "USBCTRL",    PMB6812_USBCTRL,    pmb6812_usbctrl_fields,     ARRAY_SIZE(pmb6812_usbctrl_fields) },
    { "AUDCTRL",    PMB6812_AUDCTRL,    pmb6812_audctrl_fields,     ARRAY_SIZE(pmb6812_audctrl_fields) },
    { "PWCTRL4",    PMB6812_PWCTRL4,    pmb6812_pwctrl4_fields,     ARRAY_SIZE(pmb6812_pwctrl4_fields) },
    { "TEST2",      PMB6812_TEST2,      pmb6812_test2_fields,       ARRAY_SIZE(pmb6812_test2_fields) },
    { "TEST3",      PMB6812_TEST3,      pmb6812_test3_fields,       ARRAY_SIZE(pmb6812_test3_fields) },
    { "PWCTRL5",    PMB6812_PWCTRL5,    pmb6812_pwctrl5_fields,     ARRAY_SIZE(pmb6812_pwctrl5_fields) },
    { "PWCTRL6",    PMB6812_PWCTRL6,    pmb6812_pwctrl6_fields,     ARRAY_SIZE(pmb6812_pwctrl6_fields) },
    { "LEDCTRL2",   PMB6812_LEDCTRL2,   pmb6812_ledctrl2_fields,    ARRAY_SIZE(pmb6812_ledctrl2_fields) },
    { "TEST4",      PMB6812_TEST4,      pmb6812_test4_fields,       ARRAY_SIZE(pmb6812_test4_fields) },
    { "GEF1",       PMB6812_GEF1,       pmb6812_gef1_fields,        ARRAY_SIZE(pmb6812_gef1_fields) },
    { "ISF",        PMB6812_ISF,        pmb6812_isf_fields,         ARRAY_SIZE(pmb6812_isf_fields) },
    { "CHST",       PMB6812_CHST,       pmb6812_chst_fields,        ARRAY_SIZE(pmb6812_chst_fields) },
    { "GEF2",       PMB6812_GEF2,       pmb6812_gef2_fields,        ARRAY_SIZE(pmb6812_gef2_fields) },
};

const DebugRegs pmb6812_debug_regs = {
    .regs = pmb6812_regs,
    .count = ARRAY_SIZE(pmb6812_regs),
    .reg_size = 1,
    .addr_digits = 2,
};
