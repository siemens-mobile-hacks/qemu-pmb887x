/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "block/block.h"
#include "hw/core/bus-port.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/sd/sd.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qemu/option.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/reset.h"

#define TYPE_SD_SLOT "sd-slot"
OBJECT_DECLARE_SIMPLE_TYPE(SDSlot, SD_SLOT)

struct SDSlot {
    DeviceState parent_obj;
    QDevBusPort card_port;
    BusState *card_bus;
    qemu_irq cd;
    uint32_t index;
    bool is_inserted;
};

static DeviceState *sd_slot_create_card(QDevBusPort *port, Error **errp) {
    SDSlot *s = SD_SLOT(port->dev);
    DeviceState *card = qdev_new(TYPE_SD_CARD);
    DriveInfo *drive = drive_get(IF_SD, 0, s->index);

    if (drive)
        qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(drive), errp);
    return card;
}

static bool sd_slot_reopen_snapshot(SDSlot *s, Error **errp) {
    DriveInfo *drive = drive_get(IF_SD, 0, s->index);
    BlockBackend *blk = drive ? blk_by_legacy_dinfo(drive) : NULL;
    BlockDriverState *old_bs = blk ? blk_bs(blk) : NULL;

    if (!old_bs || !(bdrv_get_flags(old_bs) & BDRV_O_TEMPORARY))
        return true;

    QDict *options = qdict_new();
    const char *format = qemu_opt_get(drive->opts, "format");
    if (format)
        qdict_put_str(options, "driver", format);

    const char *file = qemu_opt_get(drive->opts, "file");
    BlockDriverState *bs = bdrv_open(file, NULL, options, BDRV_O_RDWR | BDRV_O_SNAPSHOT, errp);
    if (!bs)
        return false;

    blk_remove_bs(blk);
    int ret = blk_insert_bs(blk, bs, errp);
    bdrv_unref(bs);
    return ret == 0;
}

static void sd_slot_update_cd(SDSlot *s) {
    bool is_card_present = s->card_port.bus && sdbus_get_inserted(SD_BUS(s->card_port.bus));

    qemu_set_irq(s->cd, !is_card_present);
}

static bool sd_slot_get_inserted(Object *obj, Error **_errp) {
    return SD_SLOT(obj)->is_inserted;
}

static void sd_slot_set_inserted(Object *obj, bool is_inserted, Error **errp) {
    SDSlot *s = SD_SLOT(obj);

    if (!DEVICE(obj)->realized) {
        error_setg(errp, "the card can only be inserted or removed while the machine runs");
        return;
    }
    if (is_inserted == s->is_inserted)
        return;

    if (!is_inserted)
        s->card_bus = s->card_port.bus;
    if (is_inserted && !sd_slot_reopen_snapshot(s, errp))
        return;
    if (!qdev_bus_port_bind(DEVICE(obj), "card", is_inserted ? s->card_bus : NULL, errp))
        return;
    s->is_inserted = is_inserted;
    sd_slot_update_cd(s);
}

static void sd_slot_init(Object *obj) {
    SDSlot *s = SD_SLOT(obj);

    s->is_inserted = true;
    qdev_init_bus_port(DEVICE(obj), &s->card_port, "card", TYPE_SD_BUS, sd_slot_create_card);
    qdev_init_gpio_out_named(DEVICE(obj), &s->cd, "cd", 1);
}

static void sd_slot_realize(DeviceState *dev, Error **_errp) {
    qemu_register_resettable(OBJECT(dev));
}

static void sd_slot_unrealize(DeviceState *dev) {
    qemu_unregister_resettable(OBJECT(dev));
}

static void sd_slot_reset(DeviceState *dev) {
    sd_slot_update_cd(SD_SLOT(dev));
}

static const Property sd_slot_properties[] = {
    DEFINE_PROP_UINT32("index", SDSlot, index, 0),
};

static void sd_slot_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, sd_slot_properties);
    object_class_property_add_bool(klass, "inserted", sd_slot_get_inserted, sd_slot_set_inserted);
    object_class_property_set_description(klass, "inserted", "Whether a card is in the slot");
    device_class_set_legacy_reset(dc, sd_slot_reset);
    dc->realize = sd_slot_realize;
    dc->unrealize = sd_slot_unrealize;
}

static const TypeInfo sd_slot_info = {
    .name = TYPE_SD_SLOT,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SDSlot),
    .instance_init = sd_slot_init,
    .class_init = sd_slot_class_init,
};

static void sd_slot_register_types(void) {
    type_register_static(&sd_slot_info);
}

type_init(sd_slot_register_types)
