#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"

#include <libcacard.h>

#include "hw/arm/pmb887x/sim/apdu.h"

OBJECT_DECLARE_SIMPLE_TYPE(pmb887x_cacard_apdu_t, PMB887X_CACARD_APDU)

#define CACARD_RESPONSE_MAX_SIZE (PMB887X_APDU_MAX_RESPONSE_SIZE + PMB887X_APDU_STATUS_SIZE)
#define CACARD_READER_WAIT_TIMEOUT_MS 5000

struct pmb887x_cacard_apdu_t {
	pmb887x_apdu_backend_t parent_obj;
	char *reader_name;
	VReader *reader;
	QemuThread event_thread;
	QemuMutex reader_mutex;
	QemuCond card_inserted_cond;
	bool event_thread_started;
	bool card_present;
	bool powered;
	uint8_t atr[PMB887X_APDU_MAX_ATR_SIZE];
	size_t atr_size;
};

static VCardEmulError cacard_initialize(Error **errp) {
	VCardEmulOptions *options = vcard_emul_options("passthru");
	if (!options) {
		error_setg(errp, "cacard-apdu: failed to enable libcacard hardware passthrough");
		return VCARD_EMUL_FAIL;
	}
	return vcard_emul_init(options);
}

static bool cacard_reader_matches(pmb887x_cacard_apdu_t *card, VReader *reader) {
	return !card->reader_name || strcmp(card->reader_name, vreader_get_name(reader)) == 0;
}

static void *cacard_event_thread(void *opaque) {
	pmb887x_cacard_apdu_t *card = opaque;
	while (true) {
		VEvent *event = vevent_wait_next_vevent();
		if (!event || event->type == VEVENT_LAST) {
			if (event)
				vevent_delete(event);
			break;
		}

		qemu_mutex_lock(&card->reader_mutex);
		switch (event->type) {
			case VEVENT_READER_INSERT:
				if (card->reader_name && !card->reader && cacard_reader_matches(card, event->reader))
					card->reader = vreader_reference(event->reader);
				break;
			case VEVENT_READER_REMOVE:
				if (event->reader == card->reader) {
					card->card_present = false;
					card->powered = false;
					card->atr_size = 0;
					vreader_free(card->reader);
					card->reader = NULL;
				}
				break;
			case VEVENT_CARD_INSERT:
				if (!card->reader && cacard_reader_matches(card, event->reader))
					card->reader = vreader_reference(event->reader);
				if (event->reader == card->reader) {
					card->card_present = true;
					qemu_cond_signal(&card->card_inserted_cond);
				}
				break;
			case VEVENT_CARD_REMOVE:
				if (event->reader == card->reader) {
					card->card_present = false;
					card->powered = false;
					card->atr_size = 0;
				}
				break;
			case VEVENT_LAST:
				g_assert_not_reached();
		}
		qemu_mutex_unlock(&card->reader_mutex);
		vevent_delete(event);
	}
	return NULL;
}

static bool cacard_power_on_locked(pmb887x_cacard_apdu_t *card) {
	int atr_size = sizeof(card->atr);
	if (!card->reader || !card->card_present || vreader_power_on(card->reader, card->atr, &atr_size) != VREADER_OK) {
		card->atr_size = 0;
		return false;
	}
	card->atr_size = atr_size;
	card->powered = true;
	return true;
}

static void cacard_power_off_locked(pmb887x_cacard_apdu_t *card) {
	if (!card->powered)
		return;
	vreader_power_off(card->reader);
	card->powered = false;
	card->atr_size = 0;
}

static void cacard_card_reset(pmb887x_apdu_backend_t *backend) {
	pmb887x_cacard_apdu_t *card = PMB887X_CACARD_APDU(backend);
	qemu_mutex_lock(&card->reader_mutex);
	cacard_power_off_locked(card);
	cacard_power_on_locked(card);
	qemu_mutex_unlock(&card->reader_mutex);
}

static const uint8_t *cacard_get_atr(pmb887x_apdu_backend_t *backend, size_t *size) {
	pmb887x_cacard_apdu_t *card = PMB887X_CACARD_APDU(backend);
	qemu_mutex_lock(&card->reader_mutex);
	*size = card->atr_size;
	qemu_mutex_unlock(&card->reader_mutex);
	return card->atr;
}

static void cacard_exchange(
	pmb887x_apdu_backend_t *backend,
	const uint8_t *command,
	size_t command_size,
	pmb887x_apdu_complete_fn complete,
	void *opaque
) {
	pmb887x_cacard_apdu_t *card = PMB887X_CACARD_APDU(backend);
	pmb887x_apdu_response_t response = { 0 };
	uint8_t send[PMB887X_APDU_MAX_COMMAND_SIZE];
	uint8_t receive[CACARD_RESPONSE_MAX_SIZE];
	int receive_size = sizeof(receive);
	pmb887x_apdu_response_set_status(&response, PMB887X_APDU_STATUS_TECHNICAL_PROBLEM);

	qemu_mutex_lock(&card->reader_mutex);
	if (!card->reader || !card->card_present || !card->powered || command_size > sizeof(send))
		goto unlock;
	memcpy(send, command, command_size);
	if (vreader_xfr_bytes(card->reader, send, command_size, receive, &receive_size) != VREADER_OK ||
		receive_size < PMB887X_APDU_STATUS_SIZE)
		goto unlock;

	response.size = MIN((size_t) receive_size - PMB887X_APDU_STATUS_SIZE, sizeof(response.data));
	memcpy(response.data, receive, response.size);
	response.sw1 = receive[receive_size - PMB887X_APDU_STATUS_SIZE];
	response.sw2 = receive[receive_size - 1];

unlock:
	qemu_mutex_unlock(&card->reader_mutex);
	complete(opaque, &response);
}

static void cacard_stop_event_thread(pmb887x_cacard_apdu_t *card) {
	if (!card->event_thread_started)
		return;
	vevent_queue_vevent(vevent_new(VEVENT_LAST, NULL, NULL));
	qemu_thread_join(&card->event_thread);
	card->event_thread_started = false;
}

static void cacard_cleanup(pmb887x_cacard_apdu_t *card) {
	cacard_stop_event_thread(card);
	qemu_mutex_lock(&card->reader_mutex);
	if (card->reader) {
		cacard_power_off_locked(card);
		vreader_free(card->reader);
		card->reader = NULL;
	}
	qemu_mutex_unlock(&card->reader_mutex);
	vcard_emul_finalize();
	qemu_cond_destroy(&card->card_inserted_cond);
	qemu_mutex_destroy(&card->reader_mutex);
}

static void cacard_realize(DeviceState *dev, Error **errp) {
	pmb887x_cacard_apdu_t *card = PMB887X_CACARD_APDU(dev);
	qemu_mutex_init(&card->reader_mutex);
	qemu_cond_init(&card->card_inserted_cond);
	VCardEmulError status = cacard_initialize(errp);
	if (status != VCARD_EMUL_OK) {
		if (!*errp)
			error_setg(errp, status == VCARD_EMUL_INIT_ALREADY_INITED ?
				"cacard-apdu: libcacard is already in use" : "cacard-apdu: failed to initialize libcacard");
		qemu_cond_destroy(&card->card_inserted_cond);
		qemu_mutex_destroy(&card->reader_mutex);
		return;
	}

	qemu_thread_create(&card->event_thread, "sim/cacard-event", cacard_event_thread, card, QEMU_THREAD_JOINABLE);
	card->event_thread_started = true;
	qemu_mutex_lock(&card->reader_mutex);
	if (!card->card_present)
		qemu_cond_timedwait(&card->card_inserted_cond, &card->reader_mutex, CACARD_READER_WAIT_TIMEOUT_MS);
	bool card_ready = card->reader && card->card_present && cacard_power_on_locked(card);
	qemu_mutex_unlock(&card->reader_mutex);
	if (!card_ready) {
		if (card->reader_name) {
			error_setg(errp, "cacard-apdu: reader '%s' has no available card", card->reader_name);
		} else {
			error_setg(errp, "cacard-apdu: no smartcard reader with an inserted card is available");
		}
		cacard_cleanup(card);
		return;
	}
}

static void cacard_unrealize(DeviceState *dev) {
	cacard_cleanup(PMB887X_CACARD_APDU(dev));
}

static const Property cacard_properties[] = {
	DEFINE_PROP_STRING("reader", pmb887x_cacard_apdu_t, reader_name),
};

static void cacard_class_init(ObjectClass *klass, const void *data) {
	pmb887x_apdu_backend_class_t *apdu_class = PMB887X_APDU_BACKEND_CLASS(klass);
	DeviceClass *device_class = DEVICE_CLASS(klass);

	apdu_class->card_reset = cacard_card_reset;
	apdu_class->get_atr = cacard_get_atr;
	apdu_class->exchange = cacard_exchange;
	device_class->realize = cacard_realize;
	device_class->unrealize = cacard_unrealize;
	device_class_set_props(device_class, cacard_properties);
}

static const TypeInfo cacard_info = {
	.name = TYPE_PMB887X_CACARD_APDU,
	.parent = TYPE_PMB887X_APDU_BACKEND,
	.instance_size = sizeof(pmb887x_cacard_apdu_t),
	.class_init = cacard_class_init,
};

static void cacard_register_types(void) {
	type_register_static(&cacard_info);
}
type_init(cacard_register_types)
