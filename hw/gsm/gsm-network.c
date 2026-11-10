/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/gsm/gsm-network.h"

static const TypeInfo gsm_network_info = {
    .name           = TYPE_GSM_NETWORK,
    .parent         = TYPE_INTERFACE,
    .class_size     = sizeof(GSMNetworkClass),
};

static void gsm_network_register_types(void) {
    type_register_static(&gsm_network_info);
}

type_init(gsm_network_register_types)
