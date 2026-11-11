/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SMARTCARD_ISO7816_H
#define HW_SMARTCARD_ISO7816_H

#include "qemu/debug-channel.h"
#include "qemu/fifo8.h"
#include "hw/core/clock.h"
#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"

#define ISO7816_HEADER_SIZE 5
#define ISO7816_MAX_DATA_SIZE 256
#define ISO7816_MAX_COMMAND_SIZE (ISO7816_HEADER_SIZE + ISO7816_MAX_DATA_SIZE)
#define ISO7816_MAX_ATR_SIZE 33
#define ISO7816_T0_NULL_BYTE 0x60

enum {
    ISO7816_OFFSET_CLA,
    ISO7816_OFFSET_INS,
    ISO7816_OFFSET_P1,
    ISO7816_OFFSET_P2,
    ISO7816_OFFSET_P3,
};

typedef struct ISO7816Response {
    uint8_t data[ISO7816_MAX_DATA_SIZE];
    size_t size;
    uint8_t sw1;
    uint8_t sw2;
} ISO7816Response;

static inline void iso7816_response_set_status(ISO7816Response *response,
                                               uint16_t status)
{
    response->sw1 = (uint8_t)(status >> 8);
    response->sw2 = (uint8_t)status;
}

typedef struct ISO7816InterfaceOps {
    bool (*can_receive)(void *opaque);
    void (*receive)(void *opaque, uint8_t value);
} ISO7816InterfaceOps;

#define TYPE_ISO7816_BUS "iso7816-bus"

#define TYPE_ISO7816_SLOT "iso7816-slot"
OBJECT_DECLARE_SIMPLE_TYPE(ISO7816Slot, ISO7816_SLOT)

struct ISO7816Slot {
    SysBusDevice parent_obj;

    DebugChannel dbg;
    BusState *bus;
    Clock *clock;
    const ISO7816InterfaceOps *interface;
    void *interface_opaque;
};

void iso7816_slot_connect(ISO7816Slot *slot, const ISO7816InterfaceOps *ops,
                          void *opaque);
void iso7816_slot_transmit(ISO7816Slot *slot, uint8_t value);
void iso7816_slot_accept_input(ISO7816Slot *slot);

typedef enum ISO7816CardState {
    ISO7816_CARD_STATE_HEADER,
    ISO7816_CARD_STATE_PPS,
    ISO7816_CARD_STATE_COMMAND_DATA,
    ISO7816_CARD_STATE_RESPONSE,
} ISO7816CardState;

#define TYPE_ISO7816_CARD "iso7816-card"
OBJECT_DECLARE_TYPE(ISO7816Card, ISO7816CardClass, ISO7816_CARD)

struct ISO7816Card {
    DeviceState parent_obj;

    Clock *clock;
    QEMUTimer *tx_timer;
    Fifo8 output;

    bool reset;
    ISO7816CardState state;
    uint8_t command[ISO7816_MAX_COMMAND_SIZE];
    uint16_t command_size;
    uint16_t expected_command_size;
    uint32_t exchange_id;
    uint8_t atr_fidi;
    bool pps_allowed;
    uint32_t clocks_per_etu;
    uint32_t pps_clocks_per_etu;
    int64_t processing_ns;
};

struct ISO7816CardClass {
    DeviceClass parent_class;

    const uint8_t *(*get_atr)(ISO7816Card *card, size_t *size);
    void (*cold_reset)(ISO7816Card *card);
    bool (*command_has_data)(ISO7816Card *card, const uint8_t *header);
    void (*exchange)(ISO7816Card *card, const uint8_t *command, size_t size,
                     uint32_t exchange_id);
    int64_t (*processing_ns)(ISO7816Card *card, const uint8_t *header);
    int64_t atr_delay_ns;
};

void iso7816_card_complete(ISO7816Card *card, uint32_t exchange_id,
                           const ISO7816Response *response);

#endif
