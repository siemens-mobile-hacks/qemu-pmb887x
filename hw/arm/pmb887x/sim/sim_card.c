/*
 * ISO 7816 character/T=0 bridge between a PMB887x SIM interface and an APDU backend
 */
#define PMB887X_TRACE_ID SIM_CARD
#define PMB887X_TRACE_PREFIX "sim-card"

#include "qemu/osdep.h"
#include "qemu/fifo8.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "chardev/char.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"

#include "hw/arm/pmb887x/sim/apdu.h"
#include "hw/arm/pmb887x/sim/sim_card.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_SIM_CARD_CHARDEV "chardev-sim-card"
OBJECT_DECLARE_SIMPLE_TYPE(pmb887x_sim_card_chardev_t, PMB887X_SIM_CARD_CHARDEV)

#define SIM_CARD_PROCEDURE_BYTE_SIZE 1
#define SIM_CARD_OUTPUT_SIZE (PMB887X_APDU_MAX_RESPONSE_SIZE + 2 * SIM_CARD_PROCEDURE_BYTE_SIZE + PMB887X_APDU_STATUS_SIZE)
#define SIM_CARD_BYTE_TIME_NS SCALE_MS

enum sim_card_state_t {
	SIM_CARD_STATE_HEADER,
	SIM_CARD_STATE_COMMAND_DATA,
	SIM_CARD_STATE_RESPONSE,
};

struct pmb887x_sim_card_t {
	DeviceState parent_obj;
	Chardev *chardev;
	pmb887x_apdu_backend_t *apdu;
	char *apdu_backend;
	char *imsi;
	char *operator_code;
	char *reader_name;
	QEMUTimer *tx_timer;
	Fifo8 output;

	bool reset;
	enum sim_card_state_t state;
	uint8_t command[PMB887X_APDU_MAX_COMMAND_SIZE];
	uint16_t command_size;
	uint16_t expected_command_size;
	uint32_t exchange_id;
};

typedef struct sim_card_exchange_t {
	pmb887x_sim_card_t *card;
	uint32_t id;
} sim_card_exchange_t;

struct pmb887x_sim_card_chardev_t {
	Chardev parent_obj;
	pmb887x_sim_card_t *card;
};

static void sim_card_trace_bytes(const char *name, const uint8_t *data, size_t size) {
	if (!pmb887x_trace_log_enabled(PMB887X_TRACE_SIM_CARD))
		return;

	GString *text = g_string_new(NULL);
	for (size_t i = 0; i < size; i++)
		g_string_append_printf(text, " %02X", data[i]);
	DPRINTF("%s (%zu bytes):%s\n", name, size, text->str);
	g_string_free(text, true);
}

static void sim_card_schedule_output(pmb887x_sim_card_t *card) {
	if (!timer_pending(card->tx_timer) && !fifo8_is_empty(&card->output))
		timer_mod(card->tx_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SIM_CARD_BYTE_TIME_NS);
}

static void sim_card_queue_bytes(pmb887x_sim_card_t *card, const uint8_t *data, size_t size) {
	g_assert(size <= fifo8_num_free(&card->output));
	fifo8_push_all(&card->output, data, size);
	sim_card_schedule_output(card);
}

static void sim_card_queue_response(pmb887x_sim_card_t *card, const pmb887x_apdu_response_t *response) {
	uint8_t output[SIM_CARD_OUTPUT_SIZE];
	size_t output_size = 0;
	if (response->size != 0) {
		output[output_size++] = card->command[PMB887X_APDU_OFFSET_INS];
		memcpy(output + output_size, response->data, response->size);
		output_size += response->size;
	}
	output[output_size++] = response->sw1;
	output[output_size++] = response->sw2;
	sim_card_queue_bytes(card, output, output_size);
}

static void sim_card_tx(void *opaque) {
	pmb887x_sim_card_t *card = opaque;

	if (!card->reset || fifo8_is_empty(&card->output))
		return;
	if (qemu_chr_be_can_write(card->chardev) == 0) {
		sim_card_schedule_output(card);
		return;
	}

	uint8_t value = fifo8_pop(&card->output);
	qemu_chr_be_write(card->chardev, &value, 1);
	sim_card_schedule_output(card);
}

static void sim_card_reset_protocol(pmb887x_sim_card_t *card) {
	card->state = SIM_CARD_STATE_HEADER;
	card->command_size = 0;
	card->expected_command_size = PMB887X_APDU_HEADER_SIZE;
}

static void sim_card_exchange_complete(void *opaque, const pmb887x_apdu_response_t *response) {
	sim_card_exchange_t *exchange = opaque;
	pmb887x_sim_card_t *card = exchange->card;
	if (card->reset && card->state == SIM_CARD_STATE_RESPONSE && exchange->id == card->exchange_id) {
		sim_card_trace_bytes("APDU response data", response->data, response->size);
		DPRINTF("APDU status: %02X%02X\n", response->sw1, response->sw2);
		sim_card_queue_response(card, response);
		sim_card_reset_protocol(card);
	}
	g_free(exchange);
	object_unref(OBJECT(card));
}

static void sim_card_exchange(pmb887x_sim_card_t *card) {
	sim_card_exchange_t *exchange = g_new(sim_card_exchange_t, 1);
	exchange->card = card;
	object_ref(OBJECT(card));
	exchange->id = ++card->exchange_id;
	card->state = SIM_CARD_STATE_RESPONSE;
	sim_card_trace_bytes("APDU command", card->command, card->command_size);
	pmb887x_apdu_backend_exchange(
		card->apdu,
		card->command,
		card->command_size,
		sim_card_exchange_complete,
		exchange
	);
}

static bool sim_card_instruction_has_command_data(uint8_t instruction) {
	switch (instruction) {
		case PMB887X_APDU_INS_TERMINAL_PROFILE:
		case PMB887X_APDU_INS_TERMINAL_RESPONSE:
		case PMB887X_APDU_INS_VERIFY_PIN:
		case PMB887X_APDU_INS_CHANGE_PIN:
		case PMB887X_APDU_INS_DISABLE_PIN:
		case PMB887X_APDU_INS_ENABLE_PIN:
		case PMB887X_APDU_INS_UNBLOCK_PIN:
		case PMB887X_APDU_INS_INCREASE:
		case PMB887X_APDU_INS_RUN_GSM_ALGORITHM:
		case PMB887X_APDU_INS_SEEK:
		case PMB887X_APDU_INS_SELECT:
		case PMB887X_APDU_INS_ENVELOPE:
		case PMB887X_APDU_INS_UPDATE_BINARY:
		case PMB887X_APDU_INS_UPDATE_RECORD:
			return true;
		default:
			return false;
	}
}

static void sim_card_handle_header(pmb887x_sim_card_t *card) {
	uint8_t instruction = card->command[PMB887X_APDU_OFFSET_INS];
	uint16_t length = card->command[PMB887X_APDU_OFFSET_P3];

	if (sim_card_instruction_has_command_data(instruction)) {
		if (length == 0)
			length = PMB887X_APDU_MAX_DATA_SIZE;
		sim_card_queue_bytes(card, &instruction, SIM_CARD_PROCEDURE_BYTE_SIZE);
		card->state = SIM_CARD_STATE_COMMAND_DATA;
		card->expected_command_size = PMB887X_APDU_HEADER_SIZE + length;
	} else {
		sim_card_exchange(card);
	}
}

static void sim_card_receive(pmb887x_sim_card_t *card, uint8_t value) {
	if (!card->reset || card->command_size >= sizeof(card->command))
		return;

	card->command[card->command_size++] = value;
	if (card->state == SIM_CARD_STATE_HEADER && card->command_size == PMB887X_APDU_HEADER_SIZE) {
		sim_card_handle_header(card);
	} else if (card->state == SIM_CARD_STATE_COMMAND_DATA && card->command_size == card->expected_command_size) {
		sim_card_exchange(card);
	}
}

static void sim_card_handle_reset(void *opaque, int id, int level) {
	pmb887x_sim_card_t *card = opaque;
	bool reset = level != 0;

	if (!reset) {
		if (!card->reset)
			return;
		DPRINTF("RST low\n");
		card->reset = false;
		card->exchange_id++;
		timer_del(card->tx_timer);
		fifo8_reset(&card->output);
		sim_card_reset_protocol(card);
		return;
	}

	if (!card->reset) {
		DPRINTF("RST high\n");
		card->reset = true;
		card->exchange_id++;
		fifo8_reset(&card->output);
		sim_card_reset_protocol(card);
		pmb887x_apdu_backend_card_reset(card->apdu);
		size_t atr_size;
		const uint8_t *atr = pmb887x_apdu_backend_get_atr(card->apdu, &atr_size);
		g_assert(atr_size <= PMB887X_APDU_MAX_ATR_SIZE);
		sim_card_trace_bytes("ATR", atr, atr_size);
		sim_card_queue_bytes(card, atr, atr_size);
	}
}

static void sim_card_init(Object *obj) {
	qdev_init_gpio_in_named(DEVICE(obj), sim_card_handle_reset, "RST_IN", 1);
}

static void sim_card_realize(DeviceState *dev, Error **errp) {
	pmb887x_sim_card_t *card = PMB887X_SIM_CARD(dev);

	if (!card->apdu) {
		const char *backend_type = TYPE_PMB887X_GSM_SIM;
		if (card->apdu_backend) {
			if (strcmp(card->apdu_backend, "cacard") != 0) {
				error_setg(errp, "sim-card: unknown APDU backend '%s'", card->apdu_backend);
				return;
			}
			backend_type = TYPE_PMB887X_CACARD_APDU;
		}
		if (!object_class_by_name(backend_type)) {
			error_setg(errp, "sim-card: APDU backend '%s' is not available in this build", backend_type);
			return;
		}

		DeviceState *backend = qdev_new(backend_type);
		object_property_add_child(OBJECT(card), "apdu-backend", OBJECT(backend));
		if (strcmp(backend_type, TYPE_PMB887X_CACARD_APDU) == 0) {
			if (card->reader_name)
				qdev_prop_set_string(backend, "reader", card->reader_name);
		} else {
			if (card->imsi)
				qdev_prop_set_string(backend, "imsi", card->imsi);
			if (card->operator_code)
				qdev_prop_set_string(backend, "operator_code", card->operator_code);
		}
		if (!qdev_realize(backend, NULL, errp)) {
			object_unref(OBJECT(backend));
			return;
		}
		card->apdu = PMB887X_APDU_BACKEND(backend);
		object_unref(OBJECT(backend));
	}
	DPRINTF("APDU backend: %s\n", object_get_typename(OBJECT(card->apdu)));

	fifo8_create(&card->output, SIM_CARD_OUTPUT_SIZE);
	card->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sim_card_tx, card);
	sim_card_reset_protocol(card);

	card->chardev = qemu_chardev_new(dev->id, TYPE_PMB887X_SIM_CARD_CHARDEV, NULL, NULL, errp);
	if (!card->chardev)
		return;
	PMB887X_SIM_CARD_CHARDEV(card->chardev)->card = card;
}

Chardev *pmb887x_sim_card_get_chardev(pmb887x_sim_card_t *card) {
	return card->chardev;
}

static void sim_card_reset(DeviceState *dev) {
	pmb887x_sim_card_t *card = PMB887X_SIM_CARD(dev);
	if (card->tx_timer)
		timer_del(card->tx_timer);
	if (card->output.data)
		fifo8_reset(&card->output);
	card->reset = false;
	card->exchange_id++;
	sim_card_reset_protocol(card);
	if (card->apdu)
		pmb887x_apdu_backend_card_reset(card->apdu);
}

static void sim_card_unrealize(DeviceState *dev) {
	pmb887x_sim_card_t *card = PMB887X_SIM_CARD(dev);
	card->reset = false;
	card->exchange_id++;
	if (card->tx_timer)
		timer_del(card->tx_timer);
	if (card->chardev) {
		PMB887X_SIM_CARD_CHARDEV(card->chardev)->card = NULL;
		object_unparent(OBJECT(card->chardev));
		card->chardev = NULL;
	}
}

static void sim_card_finalize(Object *obj) {
	pmb887x_sim_card_t *card = PMB887X_SIM_CARD(obj);
	if (card->tx_timer)
		timer_free(card->tx_timer);
	if (card->output.data)
		fifo8_destroy(&card->output);
}

static int sim_card_chr_write(Chardev *chardev, const uint8_t *buffer, int size) {
	pmb887x_sim_card_t *card = PMB887X_SIM_CARD_CHARDEV(chardev)->card;
	if (!card)
		return 0;
	for (int i = 0; i < size; i++)
		sim_card_receive(card, buffer[i]);
	return size;
}

static const Property sim_card_properties[] = {
	DEFINE_PROP_LINK("apdu", pmb887x_sim_card_t, apdu, TYPE_PMB887X_APDU_BACKEND, pmb887x_apdu_backend_t *),
	DEFINE_PROP_STRING("apdu_backend", pmb887x_sim_card_t, apdu_backend),
	DEFINE_PROP_STRING("imsi", pmb887x_sim_card_t, imsi),
	DEFINE_PROP_STRING("operator_code", pmb887x_sim_card_t, operator_code),
	DEFINE_PROP_STRING("reader", pmb887x_sim_card_t, reader_name),
};

static void sim_card_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *device_class = DEVICE_CLASS(klass);

	device_class->realize = sim_card_realize;
	device_class->unrealize = sim_card_unrealize;
	device_class_set_legacy_reset(device_class, sim_card_reset);
	device_class_set_props(device_class, sim_card_properties);
}

static void sim_card_chardev_class_init(ObjectClass *klass, const void *data) {
	CHARDEV_CLASS(klass)->chr_write = sim_card_chr_write;
}

static const TypeInfo sim_card_info = {
	.name = TYPE_PMB887X_SIM_CARD,
	.parent = TYPE_DEVICE,
	.instance_size = sizeof(pmb887x_sim_card_t),
	.instance_init = sim_card_init,
	.instance_finalize = sim_card_finalize,
	.class_init = sim_card_class_init,
};

static const TypeInfo sim_card_chardev_info = {
	.name = TYPE_PMB887X_SIM_CARD_CHARDEV,
	.parent = TYPE_CHARDEV,
	.instance_size = sizeof(pmb887x_sim_card_chardev_t),
	.class_init = sim_card_chardev_class_init,
};

static void sim_card_register_types(void) {
	type_register_static(&sim_card_chardev_info);
	type_register_static(&sim_card_info);
}
type_init(sim_card_register_types)
