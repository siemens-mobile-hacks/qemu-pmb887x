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
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"

#include "hw/arm/pmb887x/sim/apdu.h"
#include "hw/arm/pmb887x/sim/sim_card.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_SIM_CARD_CHARDEV "chardev-sim-card"
OBJECT_DECLARE_SIMPLE_TYPE(pmb887x_sim_card_chardev_t, PMB887X_SIM_CARD_CHARDEV)

#define SIM_CARD_PROCEDURE_BYTE_SIZE 1
#define SIM_CARD_OUTPUT_SIZE (PMB887X_APDU_MAX_RESPONSE_SIZE + 2 * SIM_CARD_PROCEDURE_BYTE_SIZE + PMB887X_APDU_STATUS_SIZE)
#define SIM_CARD_CHARACTER_ETU 12
#define SIM_CARD_DEFAULT_FIDI 0x11
#define SIM_CARD_FIDI_FI_SHIFT 4
#define SIM_CARD_FIDI_DI_MASK 0x0F
#define SIM_CARD_ATR_T0_TA1_PRESENT 0x10
#define SIM_CARD_ATR_TA1_OFFSET 2
#define SIM_CARD_PPSS 0xFF
#define SIM_CARD_PPS0_OFFSET 1
#define SIM_CARD_PPS1_OFFSET 2
#define SIM_CARD_PPS0_PPS1_PRESENT 0x10
#define SIM_CARD_PPS0_PPS2_PRESENT 0x20
#define SIM_CARD_PPS0_PPS3_PRESENT 0x40
#define SIM_CARD_PPS0_PROTOCOL_MASK 0x0F
#define SIM_CARD_PPS_MIN_SIZE 3

enum sim_card_state_t {
	SIM_CARD_STATE_HEADER,
	SIM_CARD_STATE_PPS,
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
	Clock *clock;
	QEMUTimer *tx_timer;
	Fifo8 output;

	bool inserted;
	bool reset;
	enum sim_card_state_t state;
	uint8_t command[PMB887X_APDU_MAX_COMMAND_SIZE];
	uint16_t command_size;
	uint16_t expected_command_size;
	uint32_t exchange_id;
	uint8_t atr_fidi;
	bool pps_allowed;
	uint32_t clocks_per_etu;
	uint32_t pps_clocks_per_etu;
};

typedef struct sim_card_exchange_t {
	pmb887x_sim_card_t *card;
	uint32_t id;
} sim_card_exchange_t;

struct pmb887x_sim_card_chardev_t {
	Chardev parent_obj;
	pmb887x_sim_card_t *card;
};

static const uint16_t FI_CLOCKS_PER_ETU[16] = {
	372, 372, 558, 744, 1116, 1488, 1860, 0, 0, 512, 768, 1024, 1536, 2048
};
static const uint8_t DI_DIVISORS[16] = { 0, 1, 2, 4, 8, 16, 32, 64, 12, 20 };

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
	uint32_t clock_hz = clock_get_hz(card->clock);
	if (fifo8_is_empty(&card->output) || clock_hz == 0 || timer_pending(card->tx_timer))
		return;

	int64_t character_ns = muldiv64(SIM_CARD_CHARACTER_ETU * card->clocks_per_etu, NANOSECONDS_PER_SECOND, clock_hz);
	timer_mod(card->tx_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + character_ns);
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
	if (fifo8_is_empty(&card->output) && card->pps_clocks_per_etu != 0) {
		card->clocks_per_etu = card->pps_clocks_per_etu;
		card->pps_clocks_per_etu = 0;
	}
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

static uint32_t sim_card_fidi_to_clocks_per_etu(uint8_t fidi) {
	uint32_t divisor = DI_DIVISORS[fidi & SIM_CARD_FIDI_DI_MASK];
	return divisor != 0 ? FI_CLOCKS_PER_ETU[fidi >> SIM_CARD_FIDI_FI_SHIFT] / divisor : 0;
}

static uint16_t sim_card_pps_size(uint8_t pps0) {
	uint16_t size = SIM_CARD_PPS_MIN_SIZE;
	if (pps0 & SIM_CARD_PPS0_PPS1_PRESENT)
		size++;
	if (pps0 & SIM_CARD_PPS0_PPS2_PRESENT)
		size++;
	if (pps0 & SIM_CARD_PPS0_PPS3_PRESENT)
		size++;
	return size;
}

static void sim_card_handle_pps(pmb887x_sim_card_t *card) {
	uint8_t check = 0;
	for (uint16_t i = 0; i < card->command_size; i++)
		check ^= card->command[i];
	uint8_t pps0 = card->command[SIM_CARD_PPS0_OFFSET];
	uint8_t fidi = (pps0 & SIM_CARD_PPS0_PPS1_PRESENT) ? card->command[SIM_CARD_PPS1_OFFSET] : SIM_CARD_DEFAULT_FIDI;
	uint32_t clocks_per_etu = sim_card_fidi_to_clocks_per_etu(fidi);
	bool supported = fidi == SIM_CARD_DEFAULT_FIDI || fidi == card->atr_fidi;
	bool accepted = check == 0 && (pps0 & SIM_CARD_PPS0_PROTOCOL_MASK) == 0 && supported && clocks_per_etu != 0;

	sim_card_trace_bytes(accepted ? "PPS accepted" : "PPS ignored", card->command, card->command_size);
	if (accepted) {
		sim_card_queue_bytes(card, card->command, card->command_size);
		card->pps_clocks_per_etu = clocks_per_etu;
	}
	sim_card_reset_protocol(card);
}

static void sim_card_receive(pmb887x_sim_card_t *card, uint8_t value) {
	if (!card->reset || card->command_size >= sizeof(card->command))
		return;

	if (card->state == SIM_CARD_STATE_HEADER && card->command_size == 0) {
		if (card->pps_allowed && value == SIM_CARD_PPSS)
			card->state = SIM_CARD_STATE_PPS;
		card->pps_allowed = false;
	}
	card->command[card->command_size++] = value;
	if (card->state == SIM_CARD_STATE_PPS) {
		if (card->command_size == SIM_CARD_PPS0_OFFSET + 1)
			card->expected_command_size = sim_card_pps_size(value);
		if (card->command_size == card->expected_command_size)
			sim_card_handle_pps(card);
	} else if (card->state == SIM_CARD_STATE_HEADER && card->command_size == PMB887X_APDU_HEADER_SIZE) {
		sim_card_handle_header(card);
	} else if (card->state == SIM_CARD_STATE_COMMAND_DATA && card->command_size == card->expected_command_size) {
		sim_card_exchange(card);
	}
}

static void sim_card_handle_reset(void *opaque, int _id, int level) {
	pmb887x_sim_card_t *card = opaque;
	bool reset = level != 0;
	if (!card->inserted)
		return;

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
		bool has_ta1 = atr_size > SIM_CARD_ATR_TA1_OFFSET && (atr[1] & SIM_CARD_ATR_T0_TA1_PRESENT);
		card->atr_fidi = has_ta1 ? atr[SIM_CARD_ATR_TA1_OFFSET] : SIM_CARD_DEFAULT_FIDI;
		card->pps_allowed = true;
		card->clocks_per_etu = sim_card_fidi_to_clocks_per_etu(SIM_CARD_DEFAULT_FIDI);
		card->pps_clocks_per_etu = 0;
		sim_card_trace_bytes("ATR", atr, atr_size);
		sim_card_queue_bytes(card, atr, atr_size);
	}
}

static void sim_card_clock_update(void *opaque, ClockEvent _event) {
	sim_card_schedule_output(opaque);
}

static void sim_card_init(Object *obj) {
	pmb887x_sim_card_t *card = PMB887X_SIM_CARD(obj);
	qdev_init_gpio_in_named(DEVICE(obj), sim_card_handle_reset, "RST_IN", 1);
	card->clock = qdev_init_clock_in(DEVICE(obj), "clk", sim_card_clock_update, card, ClockUpdate);
}

static void sim_card_realize(DeviceState *dev, Error **errp) {
	pmb887x_sim_card_t *card = PMB887X_SIM_CARD(dev);

	if (card->inserted && !card->apdu) {
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
	if (card->apdu)
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
	DEFINE_PROP_BOOL("inserted", pmb887x_sim_card_t, inserted, true),
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
