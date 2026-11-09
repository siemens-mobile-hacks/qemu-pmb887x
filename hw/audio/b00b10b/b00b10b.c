/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/ssi/ssi.h"
#include "system/debug-channel.h"

#define TYPE_B00B10B "b00b10b"
OBJECT_DECLARE_SIMPLE_TYPE(B00B10BState, B00B10B)

#define B00B10B_ISC_PADDING             0x00
#define B00B10B_ISC_START               0xAA
#define B00B10B_ISC_PREAMBLE_SIZE       2
#define B00B10B_ISC_HEADER_SIZE         4
#define B00B10B_ISC_MAX_FRAME_SIZE      2048
#define B00B10B_RESPONSE_PADDING_BYTES  32

typedef enum B00B10BTransferState B00B10BTransferState;

enum B00B10BTransferState {
    B00B10B_STATE_NONE,
    B00B10B_STATE_ISC_MESSAGE_HEADER,
    B00B10B_STATE_ISC_MESSAGE_BODY,
    B00B10B_STATE_ISC_MESSAGE_RESPONSE,
};

enum ISCMessageID {
    /* Boot mode */
    ISC_BOOT_LOAD_REQ           = 0x0080,
    ISC_BOOT_LOAD_RESP          = 0x0081,
    ISC_BOOT_RUN_REQ            = 0x0082,
    ISC_BOOT_RUN_RESP           = 0x0083,
    ISC_VENDOR_REQ              = 0x00A0,
    ISC_VENDOR_RESP             = 0x00A1,

    /* Main mode */
    ISC_TEST_REQ                = 0x0003,
    ISC_TEST_RESP               = 0x0004,

    ISC_AUDIO_CONFIG_REQ        = 0x0008,
    ISC_AUDIO_CONFIG_RESP       = 0x0009,

    ISC_AUDIO_CONFIG_I2S_REQ    = 0x0010,
    ISC_AUDIO_CONFIG_I2S_RESP   = 0x0011,

    ISC_PMAN_CONFIG_REQ         = 0x0062,
    ISC_PMAN_CONFIG_RESP        = 0x0063,

    ISC_PMAN_STANDBY_ENTRY_REQ  = 0x0064,
    ISC_PMAN_STANDBY_ENTRY_RESP = 0x0065,
};

struct B00B10BState {
    SSIPeripheral parent_obj;
    DebugChannel dbg;
    uint32_t wcycle;
    B00B10BTransferState state;

    uint32_t prev_byte;

    uint32_t msg_id;
    uint32_t msg_len;
    uint8_t msg_payload[B00B10B_ISC_MAX_FRAME_SIZE];

    uint8_t response[B00B10B_ISC_PREAMBLE_SIZE + B00B10B_ISC_MAX_FRAME_SIZE];
    uint32_t response_size;
    uint8_t *response_payload;
    bool has_response;

    qemu_irq gpio_int;
};

static void b00b10b_publish_response(B00B10BState *p, uint16_t msg_id, uint16_t payload_len) {
    uint16_t frame_size = payload_len + B00B10B_ISC_HEADER_SIZE;

    p->response[0] = B00B10B_ISC_PADDING;
    p->response[1] = B00B10B_ISC_START;
    p->response[2] = (frame_size >> 8) & 0xFF;
    p->response[3] = frame_size & 0xFF;
    p->response[4] = (msg_id >> 8) & 0xFF;
    p->response[5] = msg_id & 0xFF;

    p->response_size = frame_size + B00B10B_ISC_PREAMBLE_SIZE;
    if (payload_len % 2 != 0)
        p->response_size++;
    p->has_response = true;
}

static void b00b10b_handle_message(B00B10BState *p) {
    qemu_set_irq(p->gpio_int, 1);
    qemu_set_irq(p->gpio_int, 0);

    switch (p->msg_id) {
        case ISC_BOOT_LOAD_REQ:
            DEBUG_LOG(&p->dbg, "ISC_BOOT_LOAD_REQ [%u bytes]\n", p->msg_len);
            p->response_payload[0] = 0x00;
            p->response_payload[1] = 0x01;
            b00b10b_publish_response(p, ISC_BOOT_LOAD_RESP, 1);
            break;
        case ISC_BOOT_RUN_REQ:
            DEBUG_LOG(&p->dbg, "ISC_BOOT_RUN_REQ\n");
            p->response_payload[0] = 0x00;
            p->response_payload[1] = 0x01;
            b00b10b_publish_response(p, ISC_BOOT_RUN_RESP, 1);
            break;
        case ISC_VENDOR_REQ:
            DEBUG_LOG(&p->dbg, "ISC_VENDOR_REQ\n");
            p->response_payload[0] = 0x00;
            p->response_payload[1] = 0x02;
            p->response_payload[2] = 0x00;
            p->response_payload[3] = 0x00;
            b00b10b_publish_response(p, ISC_VENDOR_RESP, 4);
            break;
        case ISC_TEST_REQ:
            DEBUG_LOG(&p->dbg, "ISC_TEST_REQ\n");
            p->response_payload[0] = 0x00;
            p->response_payload[1] = 0x00;
            b00b10b_publish_response(p, ISC_TEST_RESP, 2);
            break;
        case ISC_AUDIO_CONFIG_REQ:
            DEBUG_LOG(&p->dbg, "ISC_AUDIO_CONFIG_REQ\n");
            p->response_payload[0] = 0x00;
            p->response_payload[1] = 0x00;
            b00b10b_publish_response(p, ISC_AUDIO_CONFIG_RESP, 2);
            break;
        case ISC_AUDIO_CONFIG_I2S_REQ:
            DEBUG_LOG(&p->dbg, "ISC_AUDIO_CONFIG_I2S_REQ\n");
            p->response_payload[0] = 0x00;
            p->response_payload[1] = 0x00;
            b00b10b_publish_response(p, ISC_AUDIO_CONFIG_I2S_RESP, 2);
            break;
        case ISC_PMAN_CONFIG_REQ:
            DEBUG_LOG(&p->dbg, "ISC_PMAN_CONFIG_REQ\n");
            p->response_payload[0] = 0x00;
            p->response_payload[1] = 0x00;
            b00b10b_publish_response(p, ISC_PMAN_CONFIG_RESP, 2);
            break;
        case ISC_PMAN_STANDBY_ENTRY_REQ:
            DEBUG_LOG(&p->dbg, "ISC_PMAN_STANDBY_ENTRY_REQ\n");
            p->response_payload[0] = 0x00;
            p->response_payload[1] = 0x00;
            b00b10b_publish_response(p, ISC_PMAN_STANDBY_ENTRY_RESP, 2);
            break;
        default:
            DEBUG_UNIMP(&p->dbg, "unknown message: id=%04X, len=%u\n", p->msg_id, p->msg_len);
            break;
    }
}

static uint32_t b00b10b_transfer(SSIPeripheral *dev, uint32_t in) {
    B00B10BState *p = B00B10B(dev);
    uint32_t out = 0;
    if (p->state == B00B10B_STATE_NONE) {
        if (p->prev_byte == B00B10B_ISC_PADDING && in == B00B10B_ISC_START) {
            p->state = B00B10B_STATE_ISC_MESSAGE_HEADER;
            p->wcycle = 0;
        } else if (in == B00B10B_ISC_PADDING) {
            p->wcycle++;

            if (p->wcycle >= B00B10B_RESPONSE_PADDING_BYTES && p->has_response) {
                p->state = B00B10B_STATE_ISC_MESSAGE_RESPONSE;
                p->wcycle = 0;
            }
        } else {
            DEBUG_GUEST_ERROR(&p->dbg, "received %02X, but only padding (00) or message start (AA) allowed\n", in);
        }
    } else if (p->state == B00B10B_STATE_ISC_MESSAGE_HEADER) {
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
            p->wcycle = 0;

            if (p->msg_len == B00B10B_ISC_HEADER_SIZE) {
                b00b10b_handle_message(p);
                p->state = B00B10B_STATE_NONE;
            } else if (p->msg_len == 0 || p->msg_len > sizeof(p->msg_payload)) {
                DEBUG_GUEST_ERROR(&p->dbg, "invalid ISC message len (%u)\n", p->msg_len);
                p->state = B00B10B_STATE_NONE;
            } else {
                p->state = B00B10B_STATE_ISC_MESSAGE_BODY;
            }
        }
    } else if (p->state == B00B10B_STATE_ISC_MESSAGE_BODY) {
        p->msg_payload[p->wcycle++] = in;
        if (p->wcycle == p->msg_len) {
            b00b10b_handle_message(p);
            p->state = B00B10B_STATE_NONE;
            p->wcycle = 0;
        }
    } else if (p->state == B00B10B_STATE_ISC_MESSAGE_RESPONSE) {
        out = p->response[p->wcycle++];
        if (p->wcycle == p->response_size) {
            p->state = B00B10B_STATE_NONE;
            p->wcycle = 0;
            p->has_response = false;
        }
    }

    p->prev_byte = in & 0xFF;

    return out;
}

static void b00b10b_handle_reset(void *opaque, int _n, int level) {
    B00B10BState *p = B00B10B(opaque);
    if (level == 0) {
        DEBUG_LOG(&p->dbg, "reset!\n");
        p->wcycle = 0;
        p->state = B00B10B_STATE_NONE;
        p->prev_byte = 0;
        p->has_response = false;
        memset(p->response, 0, sizeof(p->response));
    }
}

static void b00b10b_realize(SSIPeripheral *d, Error **_errp) {
    B00B10BState *p = B00B10B(d);
    debug_channel_init_device(&p->dbg, DEVICE(d));
    qdev_init_gpio_in_named(DEVICE(d), b00b10b_handle_reset, "RESET", 1);
    qdev_init_gpio_out_named(DEVICE(d), &p->gpio_int, "INT", 1);
    p->response_payload = &p->response[B00B10B_ISC_PREAMBLE_SIZE + B00B10B_ISC_HEADER_SIZE];
}

static void b00b10b_class_init(ObjectClass *klass, const void *_data) {
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    k->realize = b00b10b_realize;
    k->transfer = b00b10b_transfer;
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo b00b10b_info = {
    .name          = TYPE_B00B10B,
    .parent        = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(B00B10BState),
    .class_init    = b00b10b_class_init,
};

static void b00b10b_register_types(void) {
    type_register_static(&b00b10b_info);
}

type_init(b00b10b_register_types)
