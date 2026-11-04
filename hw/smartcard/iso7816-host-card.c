/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"

#include <libcacard.h>

#include "hw/smartcard/gsm11.h"
#include "hw/smartcard/iso7816.h"

#define TYPE_ISO7816_HOST_CARD "iso7816-host-card"
OBJECT_DECLARE_TYPE(ISO7816HostCard, ISO7816HostCardClass, ISO7816_HOST_CARD)

#define ISO7816_HOST_CARD_STATUS_SIZE 2
#define ISO7816_HOST_CARD_RESPONSE_SIZE \
    (ISO7816_MAX_DATA_SIZE + ISO7816_HOST_CARD_STATUS_SIZE)
#define ISO7816_HOST_CARD_INSERT_TIMEOUT_MS 5000

struct ISO7816HostCard {
    ISO7816Card parent_obj;

    char *reader_name;

    VReader *reader;
    QemuThread event_thread;
    QemuMutex reader_mutex;
    QemuCond card_inserted;
    bool event_thread_started;
    bool card_present;
    bool powered;
    uint8_t atr[ISO7816_MAX_ATR_SIZE];
    size_t atr_size;
};

struct ISO7816HostCardClass {
    ISO7816CardClass parent_class;

    DeviceRealize parent_realize;
    DeviceUnrealize parent_unrealize;
};

static bool iso7816_host_card_matches(ISO7816HostCard *card, VReader *reader)
{
    return !card->reader_name ||
           strcmp(card->reader_name, vreader_get_name(reader)) == 0;
}

static void iso7816_host_card_forget_card_locked(ISO7816HostCard *card)
{
    card->card_present = false;
    card->powered = false;
    card->atr_size = 0;
}

static void iso7816_host_card_handle_event_locked(ISO7816HostCard *card,
                                                  VEvent *event)
{
    switch (event->type) {
    case VEVENT_READER_INSERT:
        if (card->reader_name && !card->reader &&
            iso7816_host_card_matches(card, event->reader)) {
            card->reader = vreader_reference(event->reader);
        }
        break;
    case VEVENT_READER_REMOVE:
        if (event->reader == card->reader) {
            iso7816_host_card_forget_card_locked(card);
            vreader_free(card->reader);
            card->reader = NULL;
        }
        break;
    case VEVENT_CARD_INSERT:
        if (!card->reader && iso7816_host_card_matches(card, event->reader)) {
            card->reader = vreader_reference(event->reader);
        }
        if (event->reader == card->reader) {
            card->card_present = true;
            qemu_cond_signal(&card->card_inserted);
        }
        break;
    case VEVENT_CARD_REMOVE:
        if (event->reader == card->reader) {
            iso7816_host_card_forget_card_locked(card);
        }
        break;
    case VEVENT_LAST:
        g_assert_not_reached();
    }
}

static void *iso7816_host_card_event_thread(void *opaque)
{
    ISO7816HostCard *card = opaque;

    while (true) {
        VEvent *event = vevent_wait_next_vevent();

        if (!event) {
            return NULL;
        }
        if (event->type == VEVENT_LAST) {
            vevent_delete(event);
            return NULL;
        }

        qemu_mutex_lock(&card->reader_mutex);
        iso7816_host_card_handle_event_locked(card, event);
        qemu_mutex_unlock(&card->reader_mutex);
        vevent_delete(event);
    }
}

static bool iso7816_host_card_power_on_locked(ISO7816HostCard *card)
{
    int atr_size = sizeof(card->atr);

    if (!card->reader || !card->card_present) {
        return false;
    }
    if (vreader_power_on(card->reader, card->atr, &atr_size) != VREADER_OK) {
        return false;
    }

    card->atr_size = atr_size;
    card->powered = true;
    return true;
}

static void iso7816_host_card_power_off_locked(ISO7816HostCard *card)
{
    if (!card->powered) {
        return;
    }
    vreader_power_off(card->reader);
    card->powered = false;
    card->atr_size = 0;
}

static void iso7816_host_card_cold_reset(ISO7816Card *iso_card)
{
    ISO7816HostCard *card = ISO7816_HOST_CARD(iso_card);

    qemu_mutex_lock(&card->reader_mutex);
    iso7816_host_card_power_off_locked(card);
    iso7816_host_card_power_on_locked(card);
    qemu_mutex_unlock(&card->reader_mutex);
}

static const uint8_t *iso7816_host_card_get_atr(ISO7816Card *iso_card,
                                                size_t *size)
{
    ISO7816HostCard *card = ISO7816_HOST_CARD(iso_card);

    qemu_mutex_lock(&card->reader_mutex);
    *size = card->atr_size;
    qemu_mutex_unlock(&card->reader_mutex);
    return card->atr;
}

static bool iso7816_host_card_command_has_data(ISO7816Card *card,
                                               const uint8_t *header)
{
    return gsm11_command_has_data(header[ISO7816_OFFSET_INS]);
}

static uint32_t iso7816_host_card_processing_clocks(ISO7816Card *card,
                                                    const uint8_t *header)
{
    return 0;
}

static bool iso7816_host_card_transmit_locked(ISO7816HostCard *card,
                                              const uint8_t *command,
                                              size_t size,
                                              ISO7816Response *response)
{
    uint8_t send[ISO7816_MAX_COMMAND_SIZE];
    uint8_t receive[ISO7816_HOST_CARD_RESPONSE_SIZE];
    int receive_size = sizeof(receive);

    if (!card->powered) {
        return false;
    }
    memcpy(send, command, size);
    if (vreader_xfr_bytes(card->reader, send, size, receive,
                          &receive_size) != VREADER_OK) {
        return false;
    }
    if (receive_size < ISO7816_HOST_CARD_STATUS_SIZE) {
        return false;
    }

    response->size = receive_size - ISO7816_HOST_CARD_STATUS_SIZE;
    memcpy(response->data, receive, response->size);
    response->sw1 = receive[response->size];
    response->sw2 = receive[response->size + 1];
    return true;
}

static void iso7816_host_card_exchange(ISO7816Card *iso_card,
                                       const uint8_t *command, size_t size,
                                       uint32_t exchange_id)
{
    ISO7816HostCard *card = ISO7816_HOST_CARD(iso_card);
    ISO7816Response response = { 0 };
    bool transmitted;

    qemu_mutex_lock(&card->reader_mutex);
    transmitted = iso7816_host_card_transmit_locked(card, command, size,
                                                    &response);
    qemu_mutex_unlock(&card->reader_mutex);

    if (!transmitted) {
        response.size = 0;
        iso7816_response_set_status(&response,
                                    GSM11_STATUS_TECHNICAL_PROBLEM);
    }
    iso7816_card_complete(iso_card, exchange_id, &response);
}

static void iso7816_host_card_stop_event_thread(ISO7816HostCard *card)
{
    if (!card->event_thread_started) {
        return;
    }
    vevent_queue_vevent(vevent_new(VEVENT_LAST, NULL, NULL));
    qemu_thread_join(&card->event_thread);
    card->event_thread_started = false;
}

static void iso7816_host_card_release(ISO7816HostCard *card)
{
    iso7816_host_card_stop_event_thread(card);
    qemu_mutex_lock(&card->reader_mutex);
    if (card->reader) {
        iso7816_host_card_power_off_locked(card);
        vreader_free(card->reader);
        card->reader = NULL;
    }
    qemu_mutex_unlock(&card->reader_mutex);
    vcard_emul_finalize();
}

static bool iso7816_host_card_start_passthrough(Error **errp)
{
    VCardEmulOptions *options = vcard_emul_options("passthru");
    VCardEmulError status;

    if (!options) {
        error_setg(errp,
                   "iso7816-host-card: libcacard has no host card passthrough");
        return false;
    }

    status = vcard_emul_init(options);
    if (status == VCARD_EMUL_INIT_ALREADY_INITED) {
        error_setg(errp, "iso7816-host-card: libcacard is already in use");
        return false;
    }
    if (status != VCARD_EMUL_OK) {
        error_setg(errp, "iso7816-host-card: failed to initialize libcacard");
        return false;
    }
    return true;
}

static bool iso7816_host_card_wait_for_card(ISO7816HostCard *card)
{
    bool ready;

    qemu_mutex_lock(&card->reader_mutex);
    if (!card->card_present) {
        qemu_cond_timedwait(&card->card_inserted, &card->reader_mutex,
                            ISO7816_HOST_CARD_INSERT_TIMEOUT_MS);
    }
    ready = iso7816_host_card_power_on_locked(card);
    qemu_mutex_unlock(&card->reader_mutex);
    return ready;
}

static void iso7816_host_card_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    ISO7816HostCard *card = ISO7816_HOST_CARD(dev);

    if (!iso7816_host_card_start_passthrough(errp)) {
        return;
    }
    qemu_thread_create(&card->event_thread, "iso7816/host-card",
                       iso7816_host_card_event_thread, card,
                       QEMU_THREAD_JOINABLE);
    card->event_thread_started = true;

    if (!iso7816_host_card_wait_for_card(card)) {
        if (card->reader_name) {
            error_setg(errp,
                       "iso7816-host-card: reader '%s' has no available card",
                       card->reader_name);
        } else {
            error_setg(errp,
                       "iso7816-host-card: no smartcard reader with an inserted card is available");
        }
        iso7816_host_card_release(card);
        return;
    }

    ISO7816_HOST_CARD_GET_CLASS(dev)->parent_realize(dev, errp);
    if (*errp) {
        iso7816_host_card_release(card);
    }
}

static void iso7816_host_card_unrealize(DeviceState *dev)
{
    ISO7816_HOST_CARD_GET_CLASS(dev)->parent_unrealize(dev);
    iso7816_host_card_release(ISO7816_HOST_CARD(dev));
}

static void iso7816_host_card_init(Object *obj)
{
    ISO7816HostCard *card = ISO7816_HOST_CARD(obj);

    qemu_mutex_init(&card->reader_mutex);
    qemu_cond_init(&card->card_inserted);
}

static void iso7816_host_card_finalize(Object *obj)
{
    ISO7816HostCard *card = ISO7816_HOST_CARD(obj);

    qemu_cond_destroy(&card->card_inserted);
    qemu_mutex_destroy(&card->reader_mutex);
}

static const Property iso7816_host_card_properties[] = {
    DEFINE_PROP_STRING("reader", ISO7816HostCard, reader_name),
};

static void iso7816_host_card_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ISO7816CardClass *cc = ISO7816_CARD_CLASS(klass);
    ISO7816HostCardClass *hc = ISO7816_HOST_CARD_CLASS(klass);

    device_class_set_parent_realize(dc, iso7816_host_card_realize,
                                    &hc->parent_realize);
    device_class_set_parent_unrealize(dc, iso7816_host_card_unrealize,
                                      &hc->parent_unrealize);
    device_class_set_props(dc, iso7816_host_card_properties);
    cc->get_atr = iso7816_host_card_get_atr;
    cc->cold_reset = iso7816_host_card_cold_reset;
    cc->command_has_data = iso7816_host_card_command_has_data;
    cc->exchange = iso7816_host_card_exchange;
    cc->processing_clocks = iso7816_host_card_processing_clocks;
}

static const TypeInfo iso7816_host_card_info = {
    .name = TYPE_ISO7816_HOST_CARD,
    .parent = TYPE_ISO7816_CARD,
    .instance_size = sizeof(ISO7816HostCard),
    .class_size = sizeof(ISO7816HostCardClass),
    .instance_init = iso7816_host_card_init,
    .instance_finalize = iso7816_host_card_finalize,
    .class_init = iso7816_host_card_class_init,
};

static void iso7816_host_card_register_types(void)
{
    type_register_static(&iso7816_host_card_info);
}

type_init(iso7816_host_card_register_types)
