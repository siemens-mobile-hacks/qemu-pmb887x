/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SMARTCARD_GSM_SIM_TOOLKIT_H
#define HW_SMARTCARD_GSM_SIM_TOOLKIT_H

#include "hw/smartcard/iso7816.h"

#define GSM_SIM_TOOLKIT_COMMAND_MAX_SIZE 255
#define GSM_SIM_TOOLKIT_INFO_ITEM 0x01

typedef struct GSMSimToolkit {
    uint8_t command[GSM_SIM_TOOLKIT_COMMAND_MAX_SIZE];
    uint8_t command_size;
    bool command_pending;
    uint8_t command_number;
} GSMSimToolkit;

void gsm_sim_toolkit_reset(GSMSimToolkit *toolkit);
void gsm_sim_toolkit_terminal_profile(GSMSimToolkit *toolkit,
                                      ISO7816Response *response);
void gsm_sim_toolkit_fetch(GSMSimToolkit *toolkit, uint8_t length,
                           ISO7816Response *response);
uint8_t gsm_sim_toolkit_envelope(const uint8_t *data, uint16_t length,
                                 ISO7816Response *response);
void gsm_sim_toolkit_display_text(GSMSimToolkit *toolkit, const char *text);
void gsm_sim_toolkit_announce(const GSMSimToolkit *toolkit,
                              ISO7816Response *response);

#endif
