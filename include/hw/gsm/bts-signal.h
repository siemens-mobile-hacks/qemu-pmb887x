/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "hw/gsm/iq-bus.h"

IQSample bts_signal_read_iq(uint32_t frequency, int64_t timestamp, int32_t amplitude);
