#ifndef HW_ARM_PMB887X_DSP_HLE_H
#define HW_ARM_PMB887X_DSP_HLE_H

#include <stdint.h>
#include <stdbool.h>

/* Command ID definitions */
enum {
	DSP_CMD_FC_INIT            = 1,
	DSP_CMD_MODU_INIT          = 2,
	DSP_CMD_DEC_INIT           = 5,
	DSP_CMD_CIPH_KEY           = 6,
	DSP_CMD_CCH_RX             = 7,
	DSP_CMD_CCH_TX             = 8,
	DSP_CMD_TCH_26             = 9,
	DSP_CMD_PDCH               = 11,
	DSP_CMD_BB_TIMESLOT_CONFIG = 12,
	DSP_CMD_BB_OFF             = 13,
	DSP_CMD_IDLE               = 14,
	DSP_CMD_VB_ON              = 15,
	DSP_CMD_VB_SET_BIQUAD      = 16,
	DSP_CMD_VB_SET_GAIN        = 17,
	DSP_CMD_VB_START_TONE      = 18,
	DSP_CMD_VB_STOP_TONE       = 19,
	DSP_CMD_VB_READ_DURATION   = 20,
	DSP_CMD_VB_RESET           = 21,
	DSP_CMD_VB_DAI             = 22,
	DSP_CMD_HF_SET_PAR         = 23,
	DSP_CMD_HF_ON              = 24,
	DSP_CMD_VM_CMD             = 25,
	DSP_CMD_VB_SET_CBUF_GAIN   = 26,
	DSP_CMD_BB_CTRL            = 27,
	DSP_CMD_PCM_SUBMIT         = 28,
	DSP_CMD_PDCH_OFF           = 29,
	DSP_CMD_DTX_ON             = 30,
	DSP_CMD_PW_DOWN            = 31,
	DSP_CMD_AUDIOPOSTPROC      = 44,
	DSP_CMD_PCMPLAY            = 45,
	DSP_CMD_RF_ADAPT           = 67,
};

static const char *dsp_cmd_name(uint16_t id) {
	switch (id) {
		case DSP_CMD_FC_INIT:            return "FC_INIT";
		case DSP_CMD_MODU_INIT:          return "MODU_INIT";
		case DSP_CMD_DEC_INIT:           return "DEC_INIT";
		case DSP_CMD_CIPH_KEY:           return "CIPH_KEY";
		case DSP_CMD_CCH_RX:             return "CCH_RX";
		case DSP_CMD_CCH_TX:             return "CCH_TX";
		case DSP_CMD_TCH_26:             return "TCH_26";
		case DSP_CMD_PDCH:               return "PDCH";
		case DSP_CMD_BB_TIMESLOT_CONFIG: return "BB_TIMESLOT_CONFIG";
		case DSP_CMD_BB_OFF:             return "BB_OFF";
		case DSP_CMD_IDLE:               return "IDLE";
		case DSP_CMD_VB_ON:              return "VB_ON";
		case DSP_CMD_VB_SET_BIQUAD:      return "VB_SET_BIQUAD";
		case DSP_CMD_VB_SET_GAIN:        return "VB_SET_GAIN";
		case DSP_CMD_VB_START_TONE:      return "VB_START_TONE";
		case DSP_CMD_VB_STOP_TONE:       return "VB_STOP_TONE";
		case DSP_CMD_VB_READ_DURATION:   return "VB_READ_DURATION";
		case DSP_CMD_VB_RESET:           return "VB_RESET";
		case DSP_CMD_VB_DAI:             return "VB_DAI";
		case DSP_CMD_HF_SET_PAR:         return "HF_SET_PAR";
		case DSP_CMD_HF_ON:              return "HF_ON";
		case DSP_CMD_VM_CMD:             return "VM_CMD";
		case DSP_CMD_VB_SET_CBUF_GAIN:   return "VB_SET_CBUF_GAIN";
		case DSP_CMD_BB_CTRL:            return "BB_CTRL";
		case DSP_CMD_PCM_SUBMIT:         return "PCM_SUBMIT";
		case DSP_CMD_PDCH_OFF:           return "PDCH_OFF";
		case DSP_CMD_DTX_ON:             return "DTX_ON";
		case DSP_CMD_PW_DOWN:            return "PW_DOWN";
		case DSP_CMD_AUDIOPOSTPROC:      return "AUDIOPOSTPROC";
		case DSP_CMD_PCMPLAY:            return "PCMPLAY";
		case DSP_CMD_RF_ADAPT:           return "RF_ADAPT";
		default:                         return "UNKNOWN";
	}
}

#endif
