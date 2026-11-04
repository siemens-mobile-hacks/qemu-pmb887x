/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SMARTCARD_GSM_SIM_STORAGE_H
#define HW_SMARTCARD_GSM_SIM_STORAGE_H

#include "gsm-sim-fs.h"

GString *gsm_sim_storage_save(GSMSimFS *fs);
bool gsm_sim_storage_load(GSMSimFS *fs, const char *json, Error **errp);

#endif
