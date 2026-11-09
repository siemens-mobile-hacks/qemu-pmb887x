#include "qemu/osdep.h"
#include "regs.h"

static const DebugRegValue tea5761uk_intreg_status_fields_blflag_values[] = {
    { "BAND_LIMIT_NOT_REACHED",         TEA5761UK_INTREG_STATUS_BLFLAG_BAND_LIMIT_NOT_REACHED },
    { "BAND_LIMIT_REACHED_OR_TIMEOUT",  TEA5761UK_INTREG_STATUS_BLFLAG_BAND_LIMIT_REACHED_OR_TIMEOUT },
};

static const DebugRegValue tea5761uk_intreg_status_fields_frrflag_values[] = {
    { "TUNER_NOT_READY",    TEA5761UK_INTREG_STATUS_FRRFLAG_TUNER_NOT_READY },
    { "TUNER_READY",        TEA5761UK_INTREG_STATUS_FRRFLAG_TUNER_READY },
};

static const DebugRegValue tea5761uk_intreg_status_fields_levflag_values[] = {
    { "LEVEL_ABOVE_THRESHOLD",  TEA5761UK_INTREG_STATUS_LEVFLAG_LEVEL_ABOVE_THRESHOLD },
    { "LEVEL_BELOW_THRESHOLD",  TEA5761UK_INTREG_STATUS_LEVFLAG_LEVEL_BELOW_THRESHOLD },
};

static const DebugRegValue tea5761uk_intreg_status_fields_ifflag_values[] = {
    { "IF_COUNT_CORRECT",   TEA5761UK_INTREG_STATUS_IFFLAG_IF_COUNT_CORRECT },
    { "IF_COUNT_INCORRECT", TEA5761UK_INTREG_STATUS_IFFLAG_IF_COUNT_INCORRECT },
};

static const DebugRegField tea5761uk_intreg_status_fields[] = {
    { "BLFLAG",     TEA5761UK_INTREG_STATUS_BLFLAG,     TEA5761UK_INTREG_STATUS_BLFLAG_SHIFT,   tea5761uk_intreg_status_fields_blflag_values,   ARRAY_SIZE(tea5761uk_intreg_status_fields_blflag_values) },
    { "FRRFLAG",    TEA5761UK_INTREG_STATUS_FRRFLAG,    TEA5761UK_INTREG_STATUS_FRRFLAG_SHIFT,  tea5761uk_intreg_status_fields_frrflag_values,  ARRAY_SIZE(tea5761uk_intreg_status_fields_frrflag_values) },
    { "LEVFLAG",    TEA5761UK_INTREG_STATUS_LEVFLAG,    TEA5761UK_INTREG_STATUS_LEVFLAG_SHIFT,  tea5761uk_intreg_status_fields_levflag_values,  ARRAY_SIZE(tea5761uk_intreg_status_fields_levflag_values) },
    { "IFFLAG",     TEA5761UK_INTREG_STATUS_IFFLAG,     TEA5761UK_INTREG_STATUS_IFFLAG_SHIFT,   tea5761uk_intreg_status_fields_ifflag_values,   ARRAY_SIZE(tea5761uk_intreg_status_fields_ifflag_values) },
};

static const DebugRegValue tea5761uk_intreg_mask_fields_blmsk_values[] = {
    { "DISABLED",   TEA5761UK_INTREG_MASK_BLMSK_DISABLED },
    { "ENABLED",    TEA5761UK_INTREG_MASK_BLMSK_ENABLED },
};

static const DebugRegValue tea5761uk_intreg_mask_fields_frrmsk_values[] = {
    { "DISABLED",   TEA5761UK_INTREG_MASK_FRRMSK_DISABLED },
    { "ENABLED",    TEA5761UK_INTREG_MASK_FRRMSK_ENABLED },
};

static const DebugRegValue tea5761uk_intreg_mask_fields_levmsk_values[] = {
    { "DISABLED",   TEA5761UK_INTREG_MASK_LEVMSK_DISABLED },
    { "ENABLED",    TEA5761UK_INTREG_MASK_LEVMSK_ENABLED },
};

static const DebugRegValue tea5761uk_intreg_mask_fields_ifmsk_values[] = {
    { "DISABLED",   TEA5761UK_INTREG_MASK_IFMSK_DISABLED },
    { "ENABLED",    TEA5761UK_INTREG_MASK_IFMSK_ENABLED },
};

static const DebugRegField tea5761uk_intreg_mask_fields[] = {
    { "BLMSK",  TEA5761UK_INTREG_MASK_BLMSK,    TEA5761UK_INTREG_MASK_BLMSK_SHIFT,  tea5761uk_intreg_mask_fields_blmsk_values,  ARRAY_SIZE(tea5761uk_intreg_mask_fields_blmsk_values) },
    { "FRRMSK", TEA5761UK_INTREG_MASK_FRRMSK,   TEA5761UK_INTREG_MASK_FRRMSK_SHIFT, tea5761uk_intreg_mask_fields_frrmsk_values, ARRAY_SIZE(tea5761uk_intreg_mask_fields_frrmsk_values) },
    { "LEVMSK", TEA5761UK_INTREG_MASK_LEVMSK,   TEA5761UK_INTREG_MASK_LEVMSK_SHIFT, tea5761uk_intreg_mask_fields_levmsk_values, ARRAY_SIZE(tea5761uk_intreg_mask_fields_levmsk_values) },
    { "IFMSK",  TEA5761UK_INTREG_MASK_IFMSK,    TEA5761UK_INTREG_MASK_IFMSK_SHIFT,  tea5761uk_intreg_mask_fields_ifmsk_values,  ARRAY_SIZE(tea5761uk_intreg_mask_fields_ifmsk_values) },
};

static const DebugRegValue tea5761uk_frqset_high_fields_sm_values[] = {
    { "PRESET", TEA5761UK_FRQSET_HIGH_SM_PRESET },
    { "SEARCH", TEA5761UK_FRQSET_HIGH_SM_SEARCH },
};

static const DebugRegValue tea5761uk_frqset_high_fields_sud_values[] = {
    { "SEARCH_DOWN",    TEA5761UK_FRQSET_HIGH_SUD_SEARCH_DOWN },
    { "SEARCH_UP",      TEA5761UK_FRQSET_HIGH_SUD_SEARCH_UP },
};

static const DebugRegField tea5761uk_frqset_high_fields[] = {
    { "FR_13_08",   TEA5761UK_FRQSET_HIGH_FR_13_08, TEA5761UK_FRQSET_HIGH_FR_13_08_SHIFT,   NULL,                                       0 },
    { "SM",         TEA5761UK_FRQSET_HIGH_SM,       TEA5761UK_FRQSET_HIGH_SM_SHIFT,         tea5761uk_frqset_high_fields_sm_values,     ARRAY_SIZE(tea5761uk_frqset_high_fields_sm_values) },
    { "SUD",        TEA5761UK_FRQSET_HIGH_SUD,      TEA5761UK_FRQSET_HIGH_SUD_SHIFT,        tea5761uk_frqset_high_fields_sud_values,    ARRAY_SIZE(tea5761uk_frqset_high_fields_sud_values) },
};

static const DebugRegField tea5761uk_frqset_low_fields[] = {
    { "FR_07_00",   TEA5761UK_FRQSET_LOW_FR_07_00,  TEA5761UK_FRQSET_LOW_FR_07_00_SHIFT,    NULL,   0 },
};

static const DebugRegValue tea5761uk_tnctrl_control_fields_snc_values[] = {
    { "OFF",    TEA5761UK_TNCTRL_CONTROL_SNC_OFF },
    { "ON",     TEA5761UK_TNCTRL_CONTROL_SNC_ON },
};

static const DebugRegValue tea5761uk_tnctrl_control_fields_smute_values[] = {
    { "OFF",    TEA5761UK_TNCTRL_CONTROL_SMUTE_OFF },
    { "ON",     TEA5761UK_TNCTRL_CONTROL_SMUTE_ON },
};

static const DebugRegValue tea5761uk_tnctrl_control_fields_afm_values[] = {
    { "AUDIO_ACTIVE",   TEA5761UK_TNCTRL_CONTROL_AFM_AUDIO_ACTIVE },
    { "AUDIO_MUTED",    TEA5761UK_TNCTRL_CONTROL_AFM_AUDIO_MUTED },
};

static const DebugRegValue tea5761uk_tnctrl_control_fields_ifctc_values[] = {
    { "COUNT_1_953_MS",     TEA5761UK_TNCTRL_CONTROL_IFCTC_COUNT_1_953_MS },
    { "COUNT_15_625_MS",    TEA5761UK_TNCTRL_CONTROL_IFCTC_COUNT_15_625_MS },
};

static const DebugRegValue tea5761uk_tnctrl_control_fields_swpm_values[] = {
    { "SWP_OUTPUT",     TEA5761UK_TNCTRL_CONTROL_SWPM_SWP_OUTPUT },
    { "FRRFLAG_OUTPUT", TEA5761UK_TNCTRL_CONTROL_SWPM_FRRFLAG_OUTPUT },
};

static const DebugRegValue tea5761uk_tnctrl_control_fields_blim_values[] = {
    { "US_EUROPE",  TEA5761UK_TNCTRL_CONTROL_BLIM_US_EUROPE },
    { "JAPAN",      TEA5761UK_TNCTRL_CONTROL_BLIM_JAPAN },
};

static const DebugRegValue tea5761uk_tnctrl_control_fields_pupd_values[] = {
    { "FM_OFF", TEA5761UK_TNCTRL_CONTROL_PUPD_FM_OFF },
    { "FM_ON",  TEA5761UK_TNCTRL_CONTROL_PUPD_FM_ON },
};

static const DebugRegField tea5761uk_tnctrl_control_fields[] = {
    { "SNC",    TEA5761UK_TNCTRL_CONTROL_SNC,   TEA5761UK_TNCTRL_CONTROL_SNC_SHIFT,     tea5761uk_tnctrl_control_fields_snc_values,     ARRAY_SIZE(tea5761uk_tnctrl_control_fields_snc_values) },
    { "SMUTE",  TEA5761UK_TNCTRL_CONTROL_SMUTE, TEA5761UK_TNCTRL_CONTROL_SMUTE_SHIFT,   tea5761uk_tnctrl_control_fields_smute_values,   ARRAY_SIZE(tea5761uk_tnctrl_control_fields_smute_values) },
    { "AFM",    TEA5761UK_TNCTRL_CONTROL_AFM,   TEA5761UK_TNCTRL_CONTROL_AFM_SHIFT,     tea5761uk_tnctrl_control_fields_afm_values,     ARRAY_SIZE(tea5761uk_tnctrl_control_fields_afm_values) },
    { "IFCTC",  TEA5761UK_TNCTRL_CONTROL_IFCTC, TEA5761UK_TNCTRL_CONTROL_IFCTC_SHIFT,   tea5761uk_tnctrl_control_fields_ifctc_values,   ARRAY_SIZE(tea5761uk_tnctrl_control_fields_ifctc_values) },
    { "SWPM",   TEA5761UK_TNCTRL_CONTROL_SWPM,  TEA5761UK_TNCTRL_CONTROL_SWPM_SHIFT,    tea5761uk_tnctrl_control_fields_swpm_values,    ARRAY_SIZE(tea5761uk_tnctrl_control_fields_swpm_values) },
    { "BLIM",   TEA5761UK_TNCTRL_CONTROL_BLIM,  TEA5761UK_TNCTRL_CONTROL_BLIM_SHIFT,    tea5761uk_tnctrl_control_fields_blim_values,    ARRAY_SIZE(tea5761uk_tnctrl_control_fields_blim_values) },
    { "PUPD",   TEA5761UK_TNCTRL_CONTROL_PUPD,  TEA5761UK_TNCTRL_CONTROL_PUPD_SHIFT,    tea5761uk_tnctrl_control_fields_pupd_values,    ARRAY_SIZE(tea5761uk_tnctrl_control_fields_pupd_values) },
};

static const DebugRegValue tea5761uk_tnctrl_audio_fields_ahlsi_values[] = {
    { "CONTINUOUS_SEARCH",          TEA5761UK_TNCTRL_AUDIO_AHLSI_CONTINUOUS_SEARCH },
    { "STOP_ON_FAILED_IF_COUNT",    TEA5761UK_TNCTRL_AUDIO_AHLSI_STOP_ON_FAILED_IF_COUNT },
};

static const DebugRegValue tea5761uk_tnctrl_audio_fields_dtc_values[] = {
    { "DEEMPHASIS_75_US",   TEA5761UK_TNCTRL_AUDIO_DTC_DEEMPHASIS_75_US },
    { "DEEMPHASIS_50_US",   TEA5761UK_TNCTRL_AUDIO_DTC_DEEMPHASIS_50_US },
};

static const DebugRegValue tea5761uk_tnctrl_audio_fields_swp_values[] = {
    { "LOW",    TEA5761UK_TNCTRL_AUDIO_SWP_LOW },
    { "HIGH",   TEA5761UK_TNCTRL_AUDIO_SWP_HIGH },
};

static const DebugRegValue tea5761uk_tnctrl_audio_fields_mst_values[] = {
    { "STEREO",         TEA5761UK_TNCTRL_AUDIO_MST_STEREO },
    { "FORCED_MONO",    TEA5761UK_TNCTRL_AUDIO_MST_FORCED_MONO },
};

static const DebugRegValue tea5761uk_tnctrl_audio_fields_hlsi_values[] = {
    { "LOW_SIDE",   TEA5761UK_TNCTRL_AUDIO_HLSI_LOW_SIDE },
    { "HIGH_SIDE",  TEA5761UK_TNCTRL_AUDIO_HLSI_HIGH_SIDE },
};

static const DebugRegValue tea5761uk_tnctrl_audio_fields_ssl_values[] = {
    { "ADC3",   TEA5761UK_TNCTRL_AUDIO_SSL_ADC3 },
    { "ADC5",   TEA5761UK_TNCTRL_AUDIO_SSL_ADC5 },
    { "ADC7",   TEA5761UK_TNCTRL_AUDIO_SSL_ADC7 },
    { "ADC10",  TEA5761UK_TNCTRL_AUDIO_SSL_ADC10 },
};

static const DebugRegValue tea5761uk_tnctrl_audio_fields_mu_values[] = {
    { "NOT_MUTED",  TEA5761UK_TNCTRL_AUDIO_MU_NOT_MUTED },
    { "HARD_MUTED", TEA5761UK_TNCTRL_AUDIO_MU_HARD_MUTED },
};

static const DebugRegField tea5761uk_tnctrl_audio_fields[] = {
    { "AHLSI",  TEA5761UK_TNCTRL_AUDIO_AHLSI,   TEA5761UK_TNCTRL_AUDIO_AHLSI_SHIFT, tea5761uk_tnctrl_audio_fields_ahlsi_values, ARRAY_SIZE(tea5761uk_tnctrl_audio_fields_ahlsi_values) },
    { "DTC",    TEA5761UK_TNCTRL_AUDIO_DTC,     TEA5761UK_TNCTRL_AUDIO_DTC_SHIFT,   tea5761uk_tnctrl_audio_fields_dtc_values,   ARRAY_SIZE(tea5761uk_tnctrl_audio_fields_dtc_values) },
    { "SWP",    TEA5761UK_TNCTRL_AUDIO_SWP,     TEA5761UK_TNCTRL_AUDIO_SWP_SHIFT,   tea5761uk_tnctrl_audio_fields_swp_values,   ARRAY_SIZE(tea5761uk_tnctrl_audio_fields_swp_values) },
    { "MST",    TEA5761UK_TNCTRL_AUDIO_MST,     TEA5761UK_TNCTRL_AUDIO_MST_SHIFT,   tea5761uk_tnctrl_audio_fields_mst_values,   ARRAY_SIZE(tea5761uk_tnctrl_audio_fields_mst_values) },
    { "HLSI",   TEA5761UK_TNCTRL_AUDIO_HLSI,    TEA5761UK_TNCTRL_AUDIO_HLSI_SHIFT,  tea5761uk_tnctrl_audio_fields_hlsi_values,  ARRAY_SIZE(tea5761uk_tnctrl_audio_fields_hlsi_values) },
    { "SSL",    TEA5761UK_TNCTRL_AUDIO_SSL,     TEA5761UK_TNCTRL_AUDIO_SSL_SHIFT,   tea5761uk_tnctrl_audio_fields_ssl_values,   ARRAY_SIZE(tea5761uk_tnctrl_audio_fields_ssl_values) },
    { "MU",     TEA5761UK_TNCTRL_AUDIO_MU,      TEA5761UK_TNCTRL_AUDIO_MU_SHIFT,    tea5761uk_tnctrl_audio_fields_mu_values,    ARRAY_SIZE(tea5761uk_tnctrl_audio_fields_mu_values) },
};

static const DebugRegField tea5761uk_frqchk_high_fields[] = {
    { "PLL_13_08",  TEA5761UK_FRQCHK_HIGH_PLL_13_08,    TEA5761UK_FRQCHK_HIGH_PLL_13_08_SHIFT,  NULL,   0 },
};

static const DebugRegField tea5761uk_frqchk_low_fields[] = {
    { "PLL_07_00",  TEA5761UK_FRQCHK_LOW_PLL_07_00, TEA5761UK_FRQCHK_LOW_PLL_07_00_SHIFT,   NULL,   0 },
};

static const DebugRegValue tea5761uk_tunchk_if_fields_tunto_values[] = {
    { "PLL_SETTLED",    TEA5761UK_TUNCHK_IF_TUNTO_PLL_SETTLED },
    { "TUNING_TIMEOUT", TEA5761UK_TUNCHK_IF_TUNTO_TUNING_TIMEOUT },
};

static const DebugRegField tea5761uk_tunchk_if_fields[] = {
    { "TUNTO",  TEA5761UK_TUNCHK_IF_TUNTO,  TEA5761UK_TUNCHK_IF_TUNTO_SHIFT,    tea5761uk_tunchk_if_fields_tunto_values,    ARRAY_SIZE(tea5761uk_tunchk_if_fields_tunto_values) },
    { "IF_6_0", TEA5761UK_TUNCHK_IF_IF_6_0, TEA5761UK_TUNCHK_IF_IF_6_0_SHIFT,   NULL,                                       0 },
};

static const DebugRegValue tea5761uk_tunchk_status_fields_stereo_values[] = {
    { "NO_PILOT",       TEA5761UK_TUNCHK_STATUS_STEREO_NO_PILOT },
    { "PILOT_DETECTED", TEA5761UK_TUNCHK_STATUS_STEREO_PILOT_DETECTED },
};

static const DebugRegValue tea5761uk_tunchk_status_fields_ld_values[] = {
    { "PLL_NOT_LOCKED", TEA5761UK_TUNCHK_STATUS_LD_PLL_NOT_LOCKED },
    { "PLL_LOCKED",     TEA5761UK_TUNCHK_STATUS_LD_PLL_LOCKED },
};

static const DebugRegField tea5761uk_tunchk_status_fields[] = {
    { "STEREO",     TEA5761UK_TUNCHK_STATUS_STEREO,     TEA5761UK_TUNCHK_STATUS_STEREO_SHIFT,   tea5761uk_tunchk_status_fields_stereo_values,   ARRAY_SIZE(tea5761uk_tunchk_status_fields_stereo_values) },
    { "LD",         TEA5761UK_TUNCHK_STATUS_LD,         TEA5761UK_TUNCHK_STATUS_LD_SHIFT,       tea5761uk_tunchk_status_fields_ld_values,       ARRAY_SIZE(tea5761uk_tunchk_status_fields_ld_values) },
    { "LEV_3_0",    TEA5761UK_TUNCHK_STATUS_LEV_3_0,    TEA5761UK_TUNCHK_STATUS_LEV_3_0_SHIFT,  NULL,                                           0 },
};

static const DebugRegValue tea5761uk_testreg_control_fields_intctrl_values[] = {
    { "DISABLED",   TEA5761UK_TESTREG_CONTROL_INTCTRL_DISABLED },
    { "ENABLED",    TEA5761UK_TESTREG_CONTROL_INTCTRL_ENABLED },
};

static const DebugRegValue tea5761uk_testreg_control_fields_rfagc_values[] = {
    { "ON",     TEA5761UK_TESTREG_CONTROL_RFAGC_ON },
    { "OFF",    TEA5761UK_TESTREG_CONTROL_RFAGC_OFF },
};

static const DebugRegValue tea5761uk_testreg_control_fields_ldx_values[] = {
    { "NORMAL_GAIN",        TEA5761UK_TESTREG_CONTROL_LDX_NORMAL_GAIN },
    { "REDUCED_GAIN_6_DB",  TEA5761UK_TESTREG_CONTROL_LDX_REDUCED_GAIN_6_DB },
};

static const DebugRegValue tea5761uk_testreg_control_fields_trigfr_values[] = {
    { "XTAL",   TEA5761UK_TESTREG_CONTROL_TRIGFR_XTAL },
    { "FREQIN", TEA5761UK_TESTREG_CONTROL_TRIGFR_FREQIN },
};

static const DebugRegValue tea5761uk_testreg_control_fields_lhsw_values[] = {
    { "SMALL",  TEA5761UK_TESTREG_CONTROL_LHSW_SMALL },
    { "LARGE",  TEA5761UK_TESTREG_CONTROL_LHSW_LARGE },
};

static const DebugRegValue tea5761uk_testreg_control_fields_rhm_values[] = {
    { "NOT_MUTED",  TEA5761UK_TESTREG_CONTROL_RHM_NOT_MUTED },
    { "HARD_MUTED", TEA5761UK_TESTREG_CONTROL_RHM_HARD_MUTED },
};

static const DebugRegValue tea5761uk_testreg_control_fields_lhm_values[] = {
    { "NOT_MUTED",  TEA5761UK_TESTREG_CONTROL_LHM_NOT_MUTED },
    { "HARD_MUTED", TEA5761UK_TESTREG_CONTROL_LHM_HARD_MUTED },
};

static const DebugRegField tea5761uk_testreg_control_fields[] = {
    { "INTCTRL",    TEA5761UK_TESTREG_CONTROL_INTCTRL,  TEA5761UK_TESTREG_CONTROL_INTCTRL_SHIFT,    tea5761uk_testreg_control_fields_intctrl_values,    ARRAY_SIZE(tea5761uk_testreg_control_fields_intctrl_values) },
    { "RFAGC",      TEA5761UK_TESTREG_CONTROL_RFAGC,    TEA5761UK_TESTREG_CONTROL_RFAGC_SHIFT,      tea5761uk_testreg_control_fields_rfagc_values,      ARRAY_SIZE(tea5761uk_testreg_control_fields_rfagc_values) },
    { "LDX",        TEA5761UK_TESTREG_CONTROL_LDX,      TEA5761UK_TESTREG_CONTROL_LDX_SHIFT,        tea5761uk_testreg_control_fields_ldx_values,        ARRAY_SIZE(tea5761uk_testreg_control_fields_ldx_values) },
    { "TRIGFR",     TEA5761UK_TESTREG_CONTROL_TRIGFR,   TEA5761UK_TESTREG_CONTROL_TRIGFR_SHIFT,     tea5761uk_testreg_control_fields_trigfr_values,     ARRAY_SIZE(tea5761uk_testreg_control_fields_trigfr_values) },
    { "LHSW",       TEA5761UK_TESTREG_CONTROL_LHSW,     TEA5761UK_TESTREG_CONTROL_LHSW_SHIFT,       tea5761uk_testreg_control_fields_lhsw_values,       ARRAY_SIZE(tea5761uk_testreg_control_fields_lhsw_values) },
    { "RHM",        TEA5761UK_TESTREG_CONTROL_RHM,      TEA5761UK_TESTREG_CONTROL_RHM_SHIFT,        tea5761uk_testreg_control_fields_rhm_values,        ARRAY_SIZE(tea5761uk_testreg_control_fields_rhm_values) },
    { "LHM",        TEA5761UK_TESTREG_CONTROL_LHM,      TEA5761UK_TESTREG_CONTROL_LHM_SHIFT,        tea5761uk_testreg_control_fields_lhm_values,        ARRAY_SIZE(tea5761uk_testreg_control_fields_lhm_values) },
};

static const DebugRegValue tea5761uk_testreg_config_fields_tm_values[] = {
    { "NORMAL",     TEA5761UK_TESTREG_CONFIG_TM_NORMAL },
    { "TEST_MODE",  TEA5761UK_TESTREG_CONFIG_TM_TEST_MODE },
};

static const DebugRegValue tea5761uk_testreg_config_fields_dett_values[] = {
    { "SLOW_8_MS",  TEA5761UK_TESTREG_CONFIG_DETT_SLOW_8_MS },
    { "FAST_4_MS",  TEA5761UK_TESTREG_CONFIG_DETT_FAST_4_MS },
};

static const DebugRegField tea5761uk_testreg_config_fields[] = {
    { "TB_3_0", TEA5761UK_TESTREG_CONFIG_TB_3_0,    TEA5761UK_TESTREG_CONFIG_TB_3_0_SHIFT,  NULL,                                           0 },
    { "TM",     TEA5761UK_TESTREG_CONFIG_TM,        TEA5761UK_TESTREG_CONFIG_TM_SHIFT,      tea5761uk_testreg_config_fields_tm_values,      ARRAY_SIZE(tea5761uk_testreg_config_fields_tm_values) },
    { "DETT",   TEA5761UK_TESTREG_CONFIG_DETT,      TEA5761UK_TESTREG_CONFIG_DETT_SHIFT,    tea5761uk_testreg_config_fields_dett_values,    ARRAY_SIZE(tea5761uk_testreg_config_fields_dett_values) },
};

static const DebugRegField tea5761uk_manid_high_fields[] = {
    { "MAN_ID_10_07",   TEA5761UK_MANID_HIGH_MAN_ID_10_07,  TEA5761UK_MANID_HIGH_MAN_ID_10_07_SHIFT,    NULL,   0 },
    { "VERSION",        TEA5761UK_MANID_HIGH_VERSION,       TEA5761UK_MANID_HIGH_VERSION_SHIFT,         NULL,   0 },
};

static const DebugRegValue tea5761uk_manid_low_fields_idav_values[] = {
    { "NOT_AVAILABLE",  TEA5761UK_MANID_LOW_IDAV_NOT_AVAILABLE },
    { "AVAILABLE",      TEA5761UK_MANID_LOW_IDAV_AVAILABLE },
};

static const DebugRegField tea5761uk_manid_low_fields[] = {
    { "IDAV",           TEA5761UK_MANID_LOW_IDAV,           TEA5761UK_MANID_LOW_IDAV_SHIFT,         tea5761uk_manid_low_fields_idav_values, ARRAY_SIZE(tea5761uk_manid_low_fields_idav_values) },
    { "MAN_ID_06_00",   TEA5761UK_MANID_LOW_MAN_ID_06_00,   TEA5761UK_MANID_LOW_MAN_ID_06_00_SHIFT, NULL,                                   0 },
};

static const DebugRegField tea5761uk_chipid_high_fields[] = {
    { "CHIP_ID_15_08",  TEA5761UK_CHIPID_HIGH_CHIP_ID_15_08,    TEA5761UK_CHIPID_HIGH_CHIP_ID_15_08_SHIFT,  NULL,   0 },
};

static const DebugRegField tea5761uk_chipid_low_fields[] = {
    { "CHIP_ID_07_00",  TEA5761UK_CHIPID_LOW_CHIP_ID_07_00, TEA5761UK_CHIPID_LOW_CHIP_ID_07_00_SHIFT,   NULL,   0 },
};

static const DebugReg tea5761uk_regs[] = {
    { "INTREG_STATUS",      TEA5761UK_INTREG_STATUS,    tea5761uk_intreg_status_fields,     ARRAY_SIZE(tea5761uk_intreg_status_fields) },
    { "INTREG_MASK",        TEA5761UK_INTREG_MASK,      tea5761uk_intreg_mask_fields,       ARRAY_SIZE(tea5761uk_intreg_mask_fields) },
    { "FRQSET_HIGH",        TEA5761UK_FRQSET_HIGH,      tea5761uk_frqset_high_fields,       ARRAY_SIZE(tea5761uk_frqset_high_fields) },
    { "FRQSET_LOW",         TEA5761UK_FRQSET_LOW,       tea5761uk_frqset_low_fields,        ARRAY_SIZE(tea5761uk_frqset_low_fields) },
    { "TNCTRL_CONTROL",     TEA5761UK_TNCTRL_CONTROL,   tea5761uk_tnctrl_control_fields,    ARRAY_SIZE(tea5761uk_tnctrl_control_fields) },
    { "TNCTRL_AUDIO",       TEA5761UK_TNCTRL_AUDIO,     tea5761uk_tnctrl_audio_fields,      ARRAY_SIZE(tea5761uk_tnctrl_audio_fields) },
    { "FRQCHK_HIGH",        TEA5761UK_FRQCHK_HIGH,      tea5761uk_frqchk_high_fields,       ARRAY_SIZE(tea5761uk_frqchk_high_fields) },
    { "FRQCHK_LOW",         TEA5761UK_FRQCHK_LOW,       tea5761uk_frqchk_low_fields,        ARRAY_SIZE(tea5761uk_frqchk_low_fields) },
    { "TUNCHK_IF",          TEA5761UK_TUNCHK_IF,        tea5761uk_tunchk_if_fields,         ARRAY_SIZE(tea5761uk_tunchk_if_fields) },
    { "TUNCHK_STATUS",      TEA5761UK_TUNCHK_STATUS,    tea5761uk_tunchk_status_fields,     ARRAY_SIZE(tea5761uk_tunchk_status_fields) },
    { "TESTREG_CONTROL",    TEA5761UK_TESTREG_CONTROL,  tea5761uk_testreg_control_fields,   ARRAY_SIZE(tea5761uk_testreg_control_fields) },
    { "TESTREG_CONFIG",     TEA5761UK_TESTREG_CONFIG,   tea5761uk_testreg_config_fields,    ARRAY_SIZE(tea5761uk_testreg_config_fields) },
    { "MANID_HIGH",         TEA5761UK_MANID_HIGH,       tea5761uk_manid_high_fields,        ARRAY_SIZE(tea5761uk_manid_high_fields) },
    { "MANID_LOW",          TEA5761UK_MANID_LOW,        tea5761uk_manid_low_fields,         ARRAY_SIZE(tea5761uk_manid_low_fields) },
    { "CHIPID_HIGH",        TEA5761UK_CHIPID_HIGH,      tea5761uk_chipid_high_fields,       ARRAY_SIZE(tea5761uk_chipid_high_fields) },
    { "CHIPID_LOW",         TEA5761UK_CHIPID_LOW,       tea5761uk_chipid_low_fields,        ARRAY_SIZE(tea5761uk_chipid_low_fields) },
};

const DebugRegs tea5761uk_debug_regs = {
    .regs = tea5761uk_regs,
    .count = ARRAY_SIZE(tea5761uk_regs),
    .reg_size = 1,
    .addr_digits = 2,
};
