/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "qemu/bswap.h"
#include "system/debug-channel.h"

#define TYPE_BCM2141 "bcm2141"
OBJECT_DECLARE_SIMPLE_TYPE(Bcm2141, BCM2141)

#define BCM2141_IO_SIZE             0x20000

#define BCM2141_SHM_QB_START        0x0001C
#define BCM2141_SHM_QB_END          0x00020
#define BCM2141_SHM_QB_RD           0x00024
#define BCM2141_SHM_QB_WR           0x00028
#define BCM2141_SHM_QD_START        0x00044
#define BCM2141_SHM_QD_END          0x00048
#define BCM2141_SHM_QD_WR           0x00050
#define BCM2141_HIF_EVENTS          0x0F000
#define BCM2141_HIF_EVENT_ACK       0x0F014
#define BCM2141_HIF_STATUS          0x0F040
#define BCM2141_HIF_PARAM           0x0F048
#define BCM2141_HIF_SEMA_REQ        0x0F064
#define BCM2141_HIF_SEMA_STATUS     0x0F068
#define BCM2141_CPU_RUN             0x1F070
#define BCM2141_RESET_CTRL          0x1F078

#define BCM2141_RESET_ASSERT        0xDEAD
#define BCM2141_HIF_STATUS_AWAKE    0x2000
#define BCM2141_HIF_SEMA_GRANTED    BIT(1)
#define BCM2141_EVENT_FRAME         BIT(2)
#define BCM2141_ADDR_MASK           0xFFFF
#define BCM2141_MAILBOX_TOGGLE      0x80

typedef enum Bcm2141State Bcm2141State;

enum Bcm2141State {
    BCM2141_STATE_OFF,
    BCM2141_STATE_HANDSHAKE,
    BCM2141_STATE_LOADER,
    BCM2141_STATE_STARTING,
    BCM2141_STATE_RUNNING,
};

enum Bcm2141HostCommand {
    BCM2141_HOST_GO             = 0,
    BCM2141_HOST_CHUNK_REQUEST  = 1,
    BCM2141_HOST_CHUNK_WRITTEN  = 2,
    BCM2141_HOST_DOWNLOAD_DONE  = 5,
    BCM2141_HOST_START          = 8,
};

enum Bcm2141ChipReply {
    BCM2141_CHIP_STARTING       = 0,
    BCM2141_CHIP_CHUNK_READY    = 3,
    BCM2141_CHIP_CHUNK_COPIED   = 4,
    BCM2141_CHIP_CHECKSUM_OK    = 6,
};

enum Bcm2141Message {
    BCM2141_MSG_AFC_SETUP       = 0x38,
    BCM2141_MSG_AFC_REQUEST     = 0x39,
};

enum Bcm2141Indication {
    BCM2141_IND_AFC_SETUP_DONE  = 'J',
    BCM2141_IND_AFC_DONE        = 'K',
};

/*
 * HLE without a datasheet: answers the host driver protocol of the bootstrap,
 * the image download, the firmware start handshake and the AFC request.
 */
struct Bcm2141 {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    DebugChannel dbg;
    qemu_irq irq;
    Bcm2141State state;
    uint8_t memory[BCM2141_IO_SIZE];
};

static void bcm2141_raise_event(Bcm2141 *s, uint32_t events) {
    stl_le_p(&s->memory[BCM2141_HIF_EVENTS], ldl_le_p(&s->memory[BCM2141_HIF_EVENTS]) | events);
    qemu_set_irq(s->irq, 0);
    qemu_set_irq(s->irq, 1);
}

static void bcm2141_push_indication(Bcm2141 *s, uint8_t type, uint8_t arg, uint16_t value) {
    uint32_t wr = ldl_le_p(&s->memory[BCM2141_SHM_QD_WR]);
    uint8_t *entry = &s->memory[wr & BCM2141_ADDR_MASK];

    entry[0] = type;
    entry[1] = arg;
    stw_le_p(entry + 2, value);
    stl_le_p(entry + 4, 0);
    wr += 8;
    if (wr >= ldl_le_p(&s->memory[BCM2141_SHM_QD_END]))
        wr = ldl_le_p(&s->memory[BCM2141_SHM_QD_START]);
    stl_le_p(&s->memory[BCM2141_SHM_QD_WR], wr);
    DEBUG_LOG(&s->dbg, "indication %02X %02X %04X\n", type, arg, value);
    bcm2141_raise_event(s, BCM2141_EVENT_FRAME);
}

static void bcm2141_handle_message(Bcm2141 *s, const uint8_t *msg) {
    DEBUG_LOG(&s->dbg, "message %02X %02X %02X %02X\n", msg[0], msg[1], msg[2], msg[3]);
    if (msg[0] == BCM2141_MSG_AFC_SETUP) {
        bcm2141_push_indication(s, BCM2141_IND_AFC_SETUP_DONE, 0, 0);
    } else if (msg[0] == BCM2141_MSG_AFC_REQUEST) {
        bcm2141_push_indication(s, BCM2141_IND_AFC_DONE, 0, ldl_le_p(&s->memory[BCM2141_HIF_PARAM]));
    }
}

static void bcm2141_consume_queue_b(Bcm2141 *s) {
    uint32_t rd = ldl_le_p(&s->memory[BCM2141_SHM_QB_RD]);
    uint32_t wr = ldl_le_p(&s->memory[BCM2141_SHM_QB_WR]);

    while (rd != wr) {
        uint32_t msg = ldl_le_p(&s->memory[rd & BCM2141_ADDR_MASK]);
        bcm2141_handle_message(s, &s->memory[msg & BCM2141_ADDR_MASK]);
        rd += 4;
        if (rd >= ldl_le_p(&s->memory[BCM2141_SHM_QB_END]))
            rd = ldl_le_p(&s->memory[BCM2141_SHM_QB_START]);
    }
    stl_le_p(&s->memory[BCM2141_SHM_QB_RD], rd);
}

static void bcm2141_start_firmware(Bcm2141 *s) {
    DEBUG_LOG(&s->dbg, "firmware started\n");
    s->state = BCM2141_STATE_RUNNING;
    stl_le_p(&s->memory[BCM2141_HIF_STATUS], BCM2141_HIF_STATUS_AWAKE);
    qemu_set_irq(s->irq, 1);
}

static void bcm2141_handle_host_command(Bcm2141 *s, uint8_t command) {
    uint8_t *mailbox = s->memory;

    switch (s->state) {
        case BCM2141_STATE_HANDSHAKE:
            mailbox[1] = command;
            break;

        case BCM2141_STATE_LOADER:
            if (command == BCM2141_HOST_CHUNK_REQUEST) {
                mailbox[0] = BCM2141_CHIP_CHUNK_READY;
            } else if (command == BCM2141_HOST_CHUNK_WRITTEN) {
                mailbox[0] = BCM2141_CHIP_CHUNK_COPIED;
            } else if (command == BCM2141_HOST_DOWNLOAD_DONE) {
                mailbox[0] = BCM2141_CHIP_CHECKSUM_OK;
            } else if (command == BCM2141_HOST_START) {
                mailbox[0] = BCM2141_CHIP_STARTING;
                s->state = BCM2141_STATE_STARTING;
            }
            break;

        case BCM2141_STATE_STARTING:
            if (command == BCM2141_HOST_GO)
                bcm2141_start_firmware(s);
            break;

        default:
            break;
    }
}

static void bcm2141_handle_host_ack(Bcm2141 *s, uint8_t ack) {
    uint8_t *mailbox = s->memory;

    if (s->state == BCM2141_STATE_HANDSHAKE && ack == mailbox[0]) {
        DEBUG_LOG(&s->dbg, "bootstrap handshake done\n");
        mailbox[1] = mailbox[2];
        s->state = BCM2141_STATE_LOADER;
    }
}

static void bcm2141_reset_chip(Bcm2141 *s) {
    s->state = BCM2141_STATE_OFF;
    memset(s->memory, 0, sizeof(s->memory));
}

static void bcm2141_run_bootstrap(Bcm2141 *s) {
    DEBUG_LOG(&s->dbg, "bootstrap running\n");
    s->state = BCM2141_STATE_HANDSHAKE;
    s->memory[0] = (~s->memory[3]) | BCM2141_MAILBOX_TOGGLE;
}

static uint64_t bcm2141_read(void *opaque, hwaddr offset, unsigned size) {
    Bcm2141 *s = opaque;

    return ldn_le_p(&s->memory[offset], size);
}

static void bcm2141_write(void *opaque, hwaddr offset, uint64_t value, unsigned size) {
    Bcm2141 *s = opaque;

    stn_le_p(&s->memory[offset], size, value);
    switch (offset) {
        case 2:
            bcm2141_handle_host_command(s, value);
            break;

        case 3:
            bcm2141_handle_host_ack(s, value);
            break;

        case BCM2141_SHM_QB_WR:
            if (s->state == BCM2141_STATE_RUNNING)
                bcm2141_consume_queue_b(s);
            break;

        case BCM2141_HIF_EVENT_ACK:
            stl_le_p(&s->memory[BCM2141_HIF_EVENTS], ldl_le_p(&s->memory[BCM2141_HIF_EVENTS]) & ~value);
            break;

        case BCM2141_HIF_SEMA_REQ: {
            uint32_t status = ldl_le_p(&s->memory[BCM2141_HIF_SEMA_STATUS]) & ~BCM2141_HIF_SEMA_GRANTED;
            stl_le_p(&s->memory[BCM2141_HIF_SEMA_STATUS], status | (value ? BCM2141_HIF_SEMA_GRANTED : 0));
            break;
        }

        case BCM2141_RESET_CTRL:
            if (value == BCM2141_RESET_ASSERT)
                bcm2141_reset_chip(s);
            break;

        case BCM2141_CPU_RUN:
            if (value == 0)
                bcm2141_run_bootstrap(s);
            break;

        default:
            break;
    }
}

static const MemoryRegionOps bcm2141_ops = {
    .read = bcm2141_read,
    .write = bcm2141_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void bcm2141_init(Object *obj) {
    Bcm2141 *s = BCM2141(obj);

    memory_region_init_io(&s->mmio, obj, &bcm2141_ops, s, TYPE_BCM2141, BCM2141_IO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    qdev_init_gpio_out_named(DEVICE(obj), &s->irq, "IRQ", 1);
}

static void bcm2141_realize(DeviceState *dev, Error **_errp) {
    Bcm2141 *s = BCM2141(dev);

    debug_channel_init_device(&s->dbg, dev);
    debug_channel_attach_mr(&s->dbg, &s->mmio, 0);
}

static void bcm2141_reset(DeviceState *dev) {
    bcm2141_reset_chip(BCM2141(dev));
}

static void bcm2141_unrealize(DeviceState *dev) {
    debug_channel_cleanup(&BCM2141(dev)->dbg);
}

static void bcm2141_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bcm2141_realize;
    dc->unrealize = bcm2141_unrealize;
    device_class_set_legacy_reset(dc, bcm2141_reset);
}

static const TypeInfo bcm2141_types[] = {
    {
        .name = TYPE_BCM2141,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Bcm2141),
        .instance_init = bcm2141_init,
        .class_init = bcm2141_class_init,
    },
};

DEFINE_TYPES(bcm2141_types)
