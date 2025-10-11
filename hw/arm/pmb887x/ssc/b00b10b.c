/*
* Epson B00B10B (Serial Audio Codec)
 * */
#define PMB887X_TRACE_ID		ACODEC
#define PMB887X_TRACE_PREFIX	"b00b10b"

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/hw.h"
#include "hw/ssi/ssi.h"
#include "hw/irq.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "qom/object.h"
#include "hw/arm/pmb887x/trace.h"

enum SACCState {
	STATE_NONE,
	STATE_ISC_MESSAGE_HEADER,
	STATE_ISC_MESSAGE_BODY,
	STATE_ISC_MESSAGE_PADDING,
	STATE_ISC_MESSAGE_RESPONSE,
};

enum ISCMessageID {
	// Boot mode
	ISC_BOOT_LOAD_REQ	= 0x0080,
	ISC_BOOT_LOAD_RESP	= 0x0081,
	ISC_BOOT_RUN_REQ	= 0x0082,
	ISC_BOOT_RUN_RESP	= 0x0083,
	ISC_VENDOR_REQ		= 0x00A0,
	ISC_VENDOR_RESP		= 0x00A1,

	// Main mode
	ISC_TEST_REQ		= 0x0003,
	ISC_TEST_RESP		= 0x0004,

	ISC_AUDIO_CONFIG_REQ	= 0x0008,
	ISC_AUDIO_CONFIG_RESP	= 0x0009,

	ISC_AUDIO_CONFIG_I2S_REQ	= 0x0010,
	ISC_AUDIO_CONFIG_I2S_RESP	= 0x0011,

	ISC_PMAN_CONFIG_REQ			= 0x0062,
	ISC_PMAN_CONFIG_RESP		= 0x0063,

	ISC_PMAN_STANDBY_ENTRY_REQ	= 0x0064,
	ISC_PMAN_STANDBY_ENTRY_RESP	= 0x0065,
};

typedef struct pmb887x_acodec_t pmb887x_acodec_t;

struct pmb887x_acodec_t {
	SSIPeripheral dev;
	SSIBus *bus;
	uint32_t wcycle;
	enum SACCState state;

	uint32_t prev_byte;

	uint32_t msg_id;
	uint32_t msg_len;
	uint8_t msg_payload[2048];

	uint8_t response[2050];
	uint32_t response_size;
	uint8_t *response_payload;
	bool has_response;

	int padding_count;
	qemu_irq gpio_int;
};

#define TYPE_PMB887X_ACODEC "b00b10b"
#define PMB887X_ACODEC(obj)	OBJECT_CHECK(pmb887x_acodec_t, (obj), TYPE_PMB887X_ACODEC)

static void acodec_answer(pmb887x_acodec_t *p, uint16_t msg_id, uint16_t payload_len) {
	uint16_t frame_size = payload_len + 4;
	p->has_response = true;
	p->response_size = frame_size + 2;

	if (payload_len % 2 != 0)
		p->response_size++;

	p->response[0] = 0x00;
	p->response[1] = 0xAA;
	p->response[2] = (frame_size >> 8) & 0xFF;
	p->response[3] = frame_size & 0xFF;
	p->response[4] = (msg_id >> 8) & 0xFF;
	p->response[5] = msg_id & 0xFF;
}

static void acodec_handle_message(pmb887x_acodec_t *p) {
	qemu_set_irq(p->gpio_int, 1);
	qemu_set_irq(p->gpio_int, 0);

	switch (p->msg_id) {
		case ISC_BOOT_LOAD_REQ:
			DPRINTF("ISC_BOOT_LOAD_REQ [%d bytes]\n", p->msg_len);
			p->response_payload[0] = 0x00;
			p->response_payload[1] = 0x01;
			acodec_answer(p, ISC_BOOT_LOAD_RESP, 1);
			break;
		case ISC_BOOT_RUN_REQ:
			DPRINTF("ISC_BOOT_RUN_REQ\n");
			p->response_payload[0] = 0x00;
			p->response_payload[1] = 0x01;
			acodec_answer(p, ISC_BOOT_RUN_RESP, 1);
			break;
		case ISC_VENDOR_REQ:
			DPRINTF("ISC_VENDOR_REQ\n");
			p->response_payload[0] = 0x00;
			p->response_payload[1] = 0x02;
			p->response_payload[2] = 0x00;
			p->response_payload[3] = 0x00;
			acodec_answer(p, ISC_VENDOR_RESP, 4);
			break;
		case ISC_TEST_REQ:
			DPRINTF("ISC_TEST_REQ\n");
			p->response_payload[0] = 0x00;
			p->response_payload[1] = 0x00;
			acodec_answer(p, ISC_TEST_RESP, 2);
			break;
		case ISC_AUDIO_CONFIG_REQ:
			DPRINTF("ISC_AUDIO_CONFIG_REQ\n");
			p->response_payload[0] = 0x00;
			p->response_payload[1] = 0x00;
			acodec_answer(p, ISC_AUDIO_CONFIG_RESP, 2);
			break;
		case ISC_AUDIO_CONFIG_I2S_REQ:
			DPRINTF("ISC_AUDIO_CONFIG_I2S_REQ\n");
			p->response_payload[0] = 0x00;
			p->response_payload[1] = 0x00;
			acodec_answer(p, ISC_AUDIO_CONFIG_I2S_RESP, 2);
			break;
		case ISC_PMAN_CONFIG_REQ:
			DPRINTF("ISC_PMAN_CONFIG_REQ\n");
			p->response_payload[0] = 0x00;
			p->response_payload[1] = 0x00;
			acodec_answer(p, ISC_PMAN_CONFIG_RESP, 2);
			break;
		case ISC_PMAN_STANDBY_ENTRY_REQ:
			DPRINTF("ISC_PMAN_STANDBY_ENTRY_REQ\n");
			p->response_payload[0] = 0x00;
			p->response_payload[1] = 0x00;
			acodec_answer(p, ISC_PMAN_STANDBY_ENTRY_RESP, 2);
			break;
		default:
			hw_error("UNKNOWN MESSAGE: id=%04X, len=%d\n", p->msg_id, p->msg_len);
			break;
	}
}

static uint32_t acodec_transfer(SSIPeripheral *dev, uint32_t in) {
	pmb887x_acodec_t *p = PMB887X_ACODEC(dev);
	uint32_t out = 0;
	if (p->state == STATE_NONE) {
		if (p->prev_byte == 0x00 && in == 0xAA) {
			p->state = STATE_ISC_MESSAGE_HEADER;
			p->wcycle = 0;
		} else if (in == 0x00) {
			p->wcycle++;

			if (p->wcycle >= 32 && p->has_response) {
				p->state = STATE_ISC_MESSAGE_RESPONSE;
				p->wcycle = 0;
			}
		} else {
			hw_error("received %02X, but only padding (00) or message start (AA) allowed!\n", in);
		}
	} else if (p->state == STATE_ISC_MESSAGE_HEADER) {
		if (p->wcycle == 0) {
			p->msg_len = in << 8;
			p->wcycle++;
		} else if (p->wcycle == 1) {
			p->msg_len |= in;
			p->wcycle++;
		} else if (p->wcycle == 2) {
			p->msg_id = in << 8;
			p->wcycle++;
		} else if (p->wcycle == 3) {
			p->msg_id |= in;

			if (p->msg_len == 4) {
				acodec_handle_message(p);
				p->state = STATE_NONE;
				p->wcycle = 0;
			} else {
				p->state = STATE_ISC_MESSAGE_BODY;
				p->wcycle = 0;
			}

			if (p->msg_len > sizeof(p->msg_payload))
				hw_error("invalid ISC message len (%d)\n", p->msg_len);
		}
	} else if (p->state == STATE_ISC_MESSAGE_BODY) {
		p->msg_payload[p->wcycle++] = in;
		if (p->wcycle == p->msg_len) {
			acodec_handle_message(p);
			p->state = STATE_NONE;
			p->wcycle = 0;
		}
	} else if (p->state == STATE_ISC_MESSAGE_RESPONSE) {
		out = p->response[p->wcycle++];
		if (p->wcycle == p->response_size) {
			p->state = STATE_NONE;
			p->wcycle = 0;
			p->has_response = false;
		}
	}

	p->prev_byte = in & 0xFF;

	return out;
}


static void acodec_handle_reset(void *opaque, int n, int level) {
	pmb887x_acodec_t *p = PMB887X_ACODEC(opaque);
	if (level == 0) {
		DPRINTF("reset!\n");
		p->wcycle = 0;
		p->state = STATE_NONE;
		p->prev_byte = 0;
		p->has_response = false;
		memset(p->response, 0, sizeof(p->response));
	}
}

static void acodec_realize(SSIPeripheral *d, Error **errp) {
	pmb887x_acodec_t *p = PMB887X_ACODEC(d);
	qdev_init_gpio_in_named(DEVICE(d), acodec_handle_reset, "RESET_IN", 1);
	qdev_init_gpio_out_named(DEVICE(d), &p->gpio_int, "INT_OUT", 1);
	p->response_payload = &p->response[6];
}

static const Property acodec_properties[] = {
	DEFINE_PROP_LINK("bus", pmb887x_acodec_t, bus, TYPE_PMB887X_ACODEC, SSIBus *),
};

static void acodec_class_init(ObjectClass *klass, void *data) {
	SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, acodec_properties);
	k->realize = acodec_realize;
	k->transfer = acodec_transfer;
	k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo acodec_info = {
	.name          = TYPE_PMB887X_ACODEC,
	.parent        = TYPE_SSI_PERIPHERAL,
	.instance_size = sizeof(pmb887x_acodec_t),
	.class_init    = acodec_class_init,
};

static void acodec_register_types(void) {
	type_register_static(&acodec_info);
}

type_init(acodec_register_types)
