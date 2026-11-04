/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"

#include "hw/smartcard/iso7816.h"

#define ISO7816_CHARACTER_ETU 12
#define ISO7816_PROCEDURE_BYTE_SIZE 1
#define ISO7816_STATUS_SIZE 2
#define ISO7816_OUTPUT_SIZE \
    (ISO7816_PROCEDURE_BYTE_SIZE + ISO7816_MAX_DATA_SIZE + ISO7816_STATUS_SIZE)
#define ISO7816_DEFAULT_FIDI 0x11
#define ISO7816_FIDI_FI_SHIFT 4
#define ISO7816_FIDI_DI_MASK 0x0F
#define ISO7816_ATR_T0_TA1_PRESENT 0x10
#define ISO7816_ATR_TA1_OFFSET 2
#define ISO7816_PPSS 0xFF
#define ISO7816_PPS0_OFFSET 1
#define ISO7816_PPS1_OFFSET 2
#define ISO7816_PPS0_PPS1_PRESENT 0x10
#define ISO7816_PPS0_PPS2_PRESENT 0x20
#define ISO7816_PPS0_PPS3_PRESENT 0x40
#define ISO7816_PPS0_PROTOCOL_MASK 0x0F
#define ISO7816_PPS_MIN_SIZE 3

static const uint16_t FI_CLOCKS_PER_ETU[16] = {
    372, 372, 558, 744, 1116, 1488, 1860, 0, 0, 512, 768, 1024, 1536, 2048
};
static const uint8_t DI_DIVISORS[16] = { 0, 1, 2, 4, 8, 16, 32, 64, 12, 20 };

static ISO7816Card *iso7816_slot_get_card(ISO7816Slot *slot)
{
    BusChild *child = QTAILQ_FIRST(&slot->bus->children);

    return child ? ISO7816_CARD(child->child) : NULL;
}

static ISO7816Slot *iso7816_card_get_slot(ISO7816Card *card)
{
    return ISO7816_SLOT(qdev_get_parent_bus(DEVICE(card))->parent);
}

static void G_GNUC_PRINTF(2, 3) iso7816_card_trace(ISO7816Card *card,
                                                   const char *format, ...)
{
    g_autofree char *message = NULL;
    va_list args;

    if (!iso7816_card_get_slot(card)->trace) {
        return;
    }

    va_start(args, format);
    message = g_strdup_vprintf(format, args);
    va_end(args);
    qemu_log_mask(LOG_TRACE, "[iso7816]: %s\n", message);
}

static void iso7816_card_trace_bytes(ISO7816Card *card, const char *name,
                                     const uint8_t *data, size_t size)
{
    g_autoptr(GString) text = NULL;

    if (!iso7816_card_get_slot(card)->trace) {
        return;
    }

    text = qemu_hexdump_line(NULL, data, size, 1, 0);
    iso7816_card_trace(card, "%s (%zu bytes): %s", name, size, text->str);
}

static uint32_t iso7816_fidi_to_clocks_per_etu(uint8_t fidi)
{
    uint32_t divisor = DI_DIVISORS[fidi & ISO7816_FIDI_DI_MASK];

    if (divisor == 0) {
        return 0;
    }
    return FI_CLOCKS_PER_ETU[fidi >> ISO7816_FIDI_FI_SHIFT] / divisor;
}

static void iso7816_card_schedule_output(ISO7816Card *card)
{
    uint32_t clock_hz = clock_get_hz(card->clock);
    uint64_t clocks;
    int64_t delay_ns;

    if (fifo8_is_empty(&card->output) || clock_hz == 0 ||
        timer_pending(card->tx_timer)) {
        return;
    }

    clocks = ISO7816_CHARACTER_ETU * card->clocks_per_etu +
             card->processing_clocks;
    card->processing_clocks = 0;
    delay_ns = muldiv64(clocks, NANOSECONDS_PER_SECOND, clock_hz);
    timer_mod(card->tx_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
}

static void iso7816_card_queue_bytes(ISO7816Card *card, const uint8_t *data,
                                     size_t size)
{
    g_assert(size <= fifo8_num_free(&card->output));
    fifo8_push_all(&card->output, data, size);
    iso7816_card_schedule_output(card);
}

static void iso7816_card_tx(void *opaque)
{
    ISO7816Card *card = opaque;
    ISO7816Slot *slot = iso7816_card_get_slot(card);

    if (!card->reset || fifo8_is_empty(&card->output)) {
        return;
    }
    if (!slot->interface->can_receive(slot->interface_opaque)) {
        iso7816_card_schedule_output(card);
        return;
    }

    slot->interface->receive(slot->interface_opaque,
                             fifo8_pop(&card->output));
    if (fifo8_is_empty(&card->output) && card->pps_clocks_per_etu != 0) {
        card->clocks_per_etu = card->pps_clocks_per_etu;
        card->pps_clocks_per_etu = 0;
    }
    iso7816_card_schedule_output(card);
}

static void iso7816_card_reset_protocol(ISO7816Card *card)
{
    card->state = ISO7816_CARD_STATE_HEADER;
    card->command_size = 0;
    card->expected_command_size = ISO7816_HEADER_SIZE;
}

void iso7816_card_complete(ISO7816Card *card, uint32_t exchange_id,
                           const ISO7816Response *response)
{
    ISO7816CardClass *klass = ISO7816_CARD_GET_CLASS(card);
    uint8_t output[ISO7816_OUTPUT_SIZE];
    size_t output_size = 0;

    if (!card->reset || card->state != ISO7816_CARD_STATE_RESPONSE ||
        exchange_id != card->exchange_id) {
        return;
    }

    iso7816_card_trace_bytes(card, "response data", response->data,
                             response->size);
    iso7816_card_trace(card, "status %02X%02X", response->sw1, response->sw2);

    if (response->size != 0) {
        output[output_size++] = card->command[ISO7816_OFFSET_INS];
        memcpy(output + output_size, response->data, response->size);
        output_size += response->size;
    }
    output[output_size++] = response->sw1;
    output[output_size++] = response->sw2;

    card->processing_clocks = klass->processing_clocks(card, card->command);
    iso7816_card_queue_bytes(card, output, output_size);
    iso7816_card_reset_protocol(card);
}

static void iso7816_card_exchange(ISO7816Card *card)
{
    card->exchange_id++;
    card->state = ISO7816_CARD_STATE_RESPONSE;
    iso7816_card_trace_bytes(card, "command", card->command,
                             card->command_size);
    ISO7816_CARD_GET_CLASS(card)->exchange(card, card->command,
                                           card->command_size,
                                           card->exchange_id);
}

static void iso7816_card_handle_header(ISO7816Card *card)
{
    uint16_t length;

    if (!ISO7816_CARD_GET_CLASS(card)->command_has_data(card, card->command)) {
        iso7816_card_exchange(card);
        return;
    }

    length = card->command[ISO7816_OFFSET_P3];
    if (length == 0) {
        length = ISO7816_MAX_DATA_SIZE;
    }
    iso7816_card_queue_bytes(card, &card->command[ISO7816_OFFSET_INS],
                             ISO7816_PROCEDURE_BYTE_SIZE);
    card->state = ISO7816_CARD_STATE_COMMAND_DATA;
    card->expected_command_size = ISO7816_HEADER_SIZE + length;
}

static uint16_t iso7816_pps_size(uint8_t pps0)
{
    uint16_t size = ISO7816_PPS_MIN_SIZE;

    if (pps0 & ISO7816_PPS0_PPS1_PRESENT) {
        size++;
    }
    if (pps0 & ISO7816_PPS0_PPS2_PRESENT) {
        size++;
    }
    if (pps0 & ISO7816_PPS0_PPS3_PRESENT) {
        size++;
    }
    return size;
}

static void iso7816_card_handle_pps(ISO7816Card *card)
{
    uint8_t pps0 = card->command[ISO7816_PPS0_OFFSET];
    uint8_t fidi = ISO7816_DEFAULT_FIDI;
    uint8_t check = 0;
    uint32_t clocks_per_etu;
    bool supported;
    bool accepted;

    for (uint16_t i = 0; i < card->command_size; i++) {
        check ^= card->command[i];
    }
    if (pps0 & ISO7816_PPS0_PPS1_PRESENT) {
        fidi = card->command[ISO7816_PPS1_OFFSET];
    }
    clocks_per_etu = iso7816_fidi_to_clocks_per_etu(fidi);
    supported = fidi == ISO7816_DEFAULT_FIDI || fidi == card->atr_fidi;
    accepted = check == 0 && (pps0 & ISO7816_PPS0_PROTOCOL_MASK) == 0 &&
               supported && clocks_per_etu != 0;

    iso7816_card_trace_bytes(card, accepted ? "PPS accepted" : "PPS ignored",
                             card->command, card->command_size);
    if (accepted) {
        iso7816_card_queue_bytes(card, card->command, card->command_size);
        card->pps_clocks_per_etu = clocks_per_etu;
    }
    iso7816_card_reset_protocol(card);
}

static void iso7816_card_receive(ISO7816Card *card, uint8_t value)
{
    if (!card->reset || card->command_size >= sizeof(card->command)) {
        return;
    }

    if (card->state == ISO7816_CARD_STATE_HEADER && card->command_size == 0) {
        if (card->pps_allowed && value == ISO7816_PPSS) {
            card->state = ISO7816_CARD_STATE_PPS;
        }
        card->pps_allowed = false;
    }
    card->command[card->command_size++] = value;

    switch (card->state) {
    case ISO7816_CARD_STATE_PPS:
        if (card->command_size == ISO7816_PPS0_OFFSET + 1) {
            card->expected_command_size = iso7816_pps_size(value);
        }
        if (card->command_size == card->expected_command_size) {
            iso7816_card_handle_pps(card);
        }
        break;
    case ISO7816_CARD_STATE_HEADER:
        if (card->command_size == ISO7816_HEADER_SIZE) {
            iso7816_card_handle_header(card);
        }
        break;
    case ISO7816_CARD_STATE_COMMAND_DATA:
        if (card->command_size == card->expected_command_size) {
            iso7816_card_exchange(card);
        }
        break;
    case ISO7816_CARD_STATE_RESPONSE:
        break;
    }
}

static void iso7816_card_stop(ISO7816Card *card)
{
    card->reset = false;
    card->exchange_id++;
    timer_del(card->tx_timer);
    fifo8_reset(&card->output);
    card->processing_clocks = 0;
    iso7816_card_reset_protocol(card);
}

static void iso7816_card_answer_to_reset(ISO7816Card *card)
{
    ISO7816CardClass *klass = ISO7816_CARD_GET_CLASS(card);
    const uint8_t *atr;
    size_t atr_size;
    bool has_ta1;

    card->reset = true;
    card->exchange_id++;
    fifo8_reset(&card->output);
    iso7816_card_reset_protocol(card);
    klass->cold_reset(card);

    atr = klass->get_atr(card, &atr_size);
    g_assert(atr_size <= ISO7816_MAX_ATR_SIZE);
    has_ta1 = atr_size > ISO7816_ATR_TA1_OFFSET &&
              (atr[1] & ISO7816_ATR_T0_TA1_PRESENT);
    card->atr_fidi = has_ta1 ? atr[ISO7816_ATR_TA1_OFFSET]
                             : ISO7816_DEFAULT_FIDI;
    card->pps_allowed = true;
    card->clocks_per_etu = iso7816_fidi_to_clocks_per_etu(ISO7816_DEFAULT_FIDI);
    card->pps_clocks_per_etu = 0;
    card->processing_clocks = klass->atr_delay_clocks;
    iso7816_card_trace_bytes(card, "ATR", atr, atr_size);
    iso7816_card_queue_bytes(card, atr, atr_size);
}

static void iso7816_card_set_reset(ISO7816Card *card, bool active)
{
    if (active == card->reset) {
        return;
    }

    iso7816_card_trace(card, "RST %s", active ? "high" : "low");
    if (active) {
        iso7816_card_answer_to_reset(card);
    } else {
        iso7816_card_stop(card);
    }
}

void iso7816_slot_connect(ISO7816Slot *slot, const ISO7816InterfaceOps *ops,
                          void *opaque)
{
    slot->interface = ops;
    slot->interface_opaque = opaque;
}

void iso7816_slot_transmit(ISO7816Slot *slot, uint8_t value)
{
    ISO7816Card *card = iso7816_slot_get_card(slot);

    if (card) {
        iso7816_card_receive(card, value);
    }
}

void iso7816_slot_accept_input(ISO7816Slot *slot)
{
    ISO7816Card *card = iso7816_slot_get_card(slot);

    if (card) {
        iso7816_card_schedule_output(card);
    }
}

static void iso7816_slot_set_reset(void *opaque, int line, int level)
{
    ISO7816Card *card = iso7816_slot_get_card(opaque);

    if (card) {
        iso7816_card_set_reset(card, level != 0);
    }
}

static void iso7816_slot_init(Object *obj)
{
    ISO7816Slot *slot = ISO7816_SLOT(obj);
    DeviceState *dev = DEVICE(obj);

    qdev_init_gpio_in_named(dev, iso7816_slot_set_reset, "RST_IN", 1);
    slot->clock = qdev_init_clock_in(dev, "CLK", NULL, NULL, 0);
    slot->bus = qbus_new(TYPE_ISO7816_BUS, dev, NULL);
}

static void iso7816_slot_realize(DeviceState *dev, Error **errp)
{
    ISO7816Slot *slot = ISO7816_SLOT(dev);

    if (!slot->interface) {
        error_setg(errp, "iso7816-slot: no card interface is connected");
        return;
    }
    if (slot->trace) {
        qemu_set_log(qemu_loglevel | LOG_TRACE, errp);
    }
}

static void iso7816_card_clock_update(void *opaque, ClockEvent event)
{
    iso7816_card_schedule_output(opaque);
}

static void iso7816_card_init(Object *obj)
{
    ISO7816Card *card = ISO7816_CARD(obj);

    card->clock = qdev_init_clock_in(DEVICE(obj), "clk",
                                     iso7816_card_clock_update, card,
                                     ClockUpdate);
}

static void iso7816_card_realize(DeviceState *dev, Error **errp)
{
    ISO7816Card *card = ISO7816_CARD(dev);

    qdev_connect_clock_in(dev, "clk", iso7816_card_get_slot(card)->clock);
    fifo8_create(&card->output, ISO7816_OUTPUT_SIZE);
    card->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, iso7816_card_tx, card);
    iso7816_card_reset_protocol(card);
}

static void iso7816_card_unrealize(DeviceState *dev)
{
    ISO7816Card *card = ISO7816_CARD(dev);

    iso7816_card_stop(card);
    timer_free(card->tx_timer);
    fifo8_destroy(&card->output);
}

static void iso7816_card_device_reset(DeviceState *dev)
{
    iso7816_card_stop(ISO7816_CARD(dev));
}

/*
 * TODO: temporary switch; replace it with a QEMU log category, the build
 * has no trace backend for speed.
 */
static const Property iso7816_slot_properties[] = {
    DEFINE_PROP_BOOL("trace", ISO7816Slot, trace, false),
};

static void iso7816_bus_class_init(ObjectClass *klass, const void *data)
{
    BUS_CLASS(klass)->max_dev = 1;
}

static void iso7816_slot_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = iso7816_slot_realize;
    dc->user_creatable = false;
    device_class_set_props(dc, iso7816_slot_properties);
}

static void iso7816_card_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->bus_type = TYPE_ISO7816_BUS;
    dc->realize = iso7816_card_realize;
    dc->unrealize = iso7816_card_unrealize;
    device_class_set_legacy_reset(dc, iso7816_card_device_reset);
}

static const TypeInfo iso7816_types[] = {
    {
        .name = TYPE_ISO7816_BUS,
        .parent = TYPE_BUS,
        .class_init = iso7816_bus_class_init,
    },
    {
        .name = TYPE_ISO7816_SLOT,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(ISO7816Slot),
        .instance_init = iso7816_slot_init,
        .class_init = iso7816_slot_class_init,
    },
    {
        .name = TYPE_ISO7816_CARD,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(ISO7816Card),
        .class_size = sizeof(ISO7816CardClass),
        .instance_init = iso7816_card_init,
        .class_init = iso7816_card_class_init,
        .abstract = true,
    },
};

DEFINE_TYPES(iso7816_types)
