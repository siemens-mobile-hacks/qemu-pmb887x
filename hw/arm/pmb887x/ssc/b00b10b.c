/*
* Epson B00B10B (Serial Audio Codec)
 * */
#define PMB887X_TRACE_ID		ACODEC
#define PMB887X_TRACE_PREFIX	"b00b10b"

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "qom/object.h"
#include "hw/arm/pmb887x/trace.h"

enum SACCState {
	STATE_NONE,
	STATE_ISC_MESSAGE_HEADER,
	STATE_ISC_MESSAGE_BODY,
	STATE_ISC_MESSAGE_RESPONSE,
};

enum ISCMessageID {
	ISC_BOOT_LOAD_REQ	= 0x0080,
	ISC_BOOT_LOAD_RESP	= 0x0081,
};

typedef struct pmb887x_acodec_t pmb887x_acodec_t;

struct pmb887x_acodec_t {
	SSIPeripheral dev;
	SSIBus *bus;
	uint32_t wcycle;
	enum SACCState state;
	uint32_t msg_id;
	uint32_t msg_len;
	uint32_t msg_recv_len;
	uint8_t msg_payload[2048];
	uint8_t response[2050];
	uint32_t response_cursor;
	uint32_t response_size;
	uint8_t *response_payload;
	int response_wait_cycles;
};

#define TYPE_PMB887X_ACODEC "b00b10b"
#define PMB887X_ACODEC(obj)	OBJECT_CHECK(pmb887x_acodec_t, (obj), TYPE_PMB887X_ACODEC)

static void acodec_answer(pmb887x_acodec_t *p, uint16_t msg_id, uint16_t len) {
	uint16_t frame_size = len + 4;
	p->response_size = len + 6;
	p->response[0] = 0x00;
	p->response[1] = 0xAA;
	p->response[2] = (frame_size >> 8) & 0xFF;
	p->response[3] = frame_size & 0xFF;
	p->response[4] = (msg_id >> 8) & 0xFF;
	p->response[5] = msg_id & 0xFF;
	p->response_cursor = 0;
	p->response_wait_cycles = 16;
	p->state = STATE_ISC_MESSAGE_RESPONSE;
}

static void acodec_handle_message(pmb887x_acodec_t *p) {
	switch (p->msg_id) {
		case ISC_BOOT_LOAD_REQ:
			DPRINTF("ISC_BOOT_LOAD_REQ\n");
			p->response_payload[0] = 0x00;
			p->response_payload[1] = 0x01;
			acodec_answer(p, ISC_BOOT_LOAD_RESP, 2);
			break;
		default:
			EPRINTF("UNKNOWN MESSAGE: id=%04X, len=%d\n", p->msg_id, p->msg_len);
			p->state = STATE_NONE;
			p->wcycle = 0;
			break;
	}
}

static uint32_t acodec_transfer(SSIPeripheral *dev, uint32_t in) {
	pmb887x_acodec_t *p = PMB887X_ACODEC(dev);
	uint32_t out = 0;
	if (p->state == STATE_NONE) {
		if (in == 0xAA) {
			p->state = STATE_ISC_MESSAGE_HEADER;
			p->wcycle = 0;
		} else if (in != 0x00) {
			EPRINTF("received %02X, but only padding (00) or message start (AA) allowed!\n", in);
		}
	} else if (p->state == STATE_ISC_MESSAGE_HEADER) {
		p->state = STATE_ISC_MESSAGE_HEADER;
		if (p->wcycle == 0) {
			p->msg_len = in << 8;
		} else if (p->wcycle == 1) {
			p->msg_len |= in;
		} else if (p->wcycle == 2) {
			p->msg_id = in << 8;
		} else if (p->wcycle == 3) {
			p->msg_id |= in;
			p->state = STATE_ISC_MESSAGE_BODY;

			if (p->msg_len > sizeof(p->msg_payload)) {
				EPRINTF("invalid ISC message len (%d)\n", p->msg_len);
				p->state = STATE_NONE;
				p->wcycle = 0;
				return 0;
			}
		}
		p->wcycle++;
	} else if (p->state == STATE_ISC_MESSAGE_BODY) {
		p->msg_payload[p->wcycle - 4] = in;
		p->wcycle++;
		if (p->wcycle == p->msg_len) {
			acodec_handle_message(p);
		}
	} else if (p->state == STATE_ISC_MESSAGE_RESPONSE) {
		if (p->response_wait_cycles > 0) {
			if (in != 0x00)
				EPRINTF("received %02X, but only padding (00) allowed!\n", in);
			DPRINTF("p->response_wait_cycles=%d\n", p->response_wait_cycles);
			p->response_wait_cycles--;
		} else {
			out = p->response[p->response_cursor++];
			DPRINTF("out=%02X\n", out);
			if (p->response_cursor == p->response_size) {
				DPRINTF("response done\n");
				p->state = STATE_NONE;
			}
		}
	}
	return out;
}

static int acodec_set_cs(SSIPeripheral *dev, bool select) {
	DPRINTF("set_cs=%d\n", select);
	return 0;
}

static void acodec_handle_reset(void *opaque, int n, int level) {
	pmb887x_acodec_t *p = PMB887X_ACODEC(opaque);
	if (level == 0) {
		DPRINTF("reset!\n");
		p->wcycle = 0;
	}
}

static void acodec_handle_int(void *opaque, int n, int level) {

}

static void acodec_realize(SSIPeripheral *d, Error **errp) {
	pmb887x_acodec_t *p = PMB887X_ACODEC(d);
	qdev_init_gpio_in_named(DEVICE(d), acodec_handle_reset, "reset", 1);
	qdev_init_gpio_in_named(DEVICE(d), acodec_handle_int, "int", 1);
	p->response_payload = &p->response[6];
}

static const Property acodec_properties[] = {
	DEFINE_PROP_LINK("bus", pmb887x_acodec_t, bus, TYPE_PMB887X_ACODEC, SSIBus *)
};

static void acodec_class_init(ObjectClass *klass, void *data) {
	SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, acodec_properties);
	k->realize = acodec_realize;
	k->transfer = acodec_transfer;
	k->cs_polarity = SSI_CS_LOW;
	k->set_cs = acodec_set_cs;
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
