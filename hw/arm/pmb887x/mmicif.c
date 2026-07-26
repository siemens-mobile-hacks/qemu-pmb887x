/*
 * Multi Media Controller Interface
 */
#define PMB887X_TRACE_ID MMICIF
#define PMB887X_TRACE_PREFIX "pmb887x-mmicif"

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/mmicif.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/trace.h"

#define PMB887X_MMICIF(obj) OBJECT_CHECK(pmb887x_mmicif_t, (obj), TYPE_PMB887X_MMICIF)

#define MMICIF_HOST_INDEX_OFFSET 0x00020000
#define MMICIF_HOST_STATUS_OFFSET 0x000F0000
#define MMICIF_HOST_STATUS_READ_READY (1U << 7)
#define MMICIF_HOST_ADDRESS_MASK 0x00FFFFFF

typedef struct pmb887x_mmicif_t pmb887x_mmicif_t;

struct pmb887x_mmicif_bus_t {
    BusState parent_obj;
};

struct pmb887x_mmicif_t {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion mmap;
    pmb887x_mmicif_bus_t *bus;
    uint32_t revision;
    pmb887x_clc_reg_t clc;
    uint32_t config;
    uint32_t unk2c;
    uint32_t unk44;
    uint32_t transfer_config;
    uint32_t unk4c;
    uint32_t unk50;
    uint32_t unk54;
    uint32_t irqsm;
    uint32_t irqss;
    uint32_t unk80;
    uint32_t host_address;
    uint32_t host_read_value;
    uint16_t host_status;
    uint8_t host_index;
    bool host_address_valid;
    bool host_read_value_valid;
    bool host_index_valid;
};

static DeviceState *mmicif_bus_get_device(pmb887x_mmicif_bus_t *bus) {
    BusChild *child = QTAILQ_FIRST(&BUS(bus)->children);
    return child ? child->child : NULL;
}

bool pmb887x_mmicif_bus_config_read(pmb887x_mmicif_bus_t *bus, uint8_t index, uint8_t *value) {
    DeviceState *dev = mmicif_bus_get_device(bus);
    if (!dev)
        return false;

    pmb887x_mmicif_device_t *device = PMB887X_MMICIF_DEVICE(dev);
    pmb887x_mmicif_device_class_t *klass = PMB887X_MMICIF_DEVICE_GET_CLASS(device);
    return klass->config_read && klass->config_read(device, index, value);
}

bool pmb887x_mmicif_bus_config_write(pmb887x_mmicif_bus_t *bus, uint8_t index, uint8_t value) {
    DeviceState *dev = mmicif_bus_get_device(bus);
    if (!dev)
        return false;

    pmb887x_mmicif_device_t *device = PMB887X_MMICIF_DEVICE(dev);
    pmb887x_mmicif_device_class_t *klass = PMB887X_MMICIF_DEVICE_GET_CLASS(device);
    return klass->config_write && klass->config_write(device, index, value);
}

bool pmb887x_mmicif_bus_read(pmb887x_mmicif_bus_t *bus, uint32_t address, uint32_t *value) {
    DeviceState *dev = mmicif_bus_get_device(bus);
    if (!dev)
        return false;

    pmb887x_mmicif_device_t *device = PMB887X_MMICIF_DEVICE(dev);
    pmb887x_mmicif_device_class_t *klass = PMB887X_MMICIF_DEVICE_GET_CLASS(device);
    return klass->read && klass->read(device, address, value);
}

bool pmb887x_mmicif_bus_write(pmb887x_mmicif_bus_t *bus, uint32_t address, uint32_t value) {
    DeviceState *dev = mmicif_bus_get_device(bus);
    if (!dev)
        return false;

    pmb887x_mmicif_device_t *device = PMB887X_MMICIF_DEVICE(dev);
    pmb887x_mmicif_device_class_t *klass = PMB887X_MMICIF_DEVICE_GET_CLASS(device);
    return klass->write && klass->write(device, address, value);
}

static bool mmicif_bus_check_address(BusState *bus, DeviceState *dev, Error **errp) {
    if (!QTAILQ_EMPTY(&bus->children)) {
        error_setg(errp, "MMICIF bus already has a device");
        return false;
    }
    return true;
}

static void mmicif_bus_class_init(ObjectClass *klass, const void *data) {
    BusClass *bc = BUS_CLASS(klass);
    bc->check_address = mmicif_bus_check_address;
}

static const TypeInfo mmicif_bus_info = {
    .name = TYPE_PMB887X_MMICIF_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(pmb887x_mmicif_bus_t),
    .class_init = mmicif_bus_class_init,
};

static void mmicif_device_class_init(ObjectClass *klass, const void *data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->bus_type = TYPE_PMB887X_MMICIF_BUS;
}

static const TypeInfo mmicif_device_info = {
    .name = TYPE_PMB887X_MMICIF_DEVICE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(pmb887x_mmicif_device_t),
    .class_size = sizeof(pmb887x_mmicif_device_class_t),
    .class_init = mmicif_device_class_init,
    .abstract = true,
};

static uint64_t mmicif_io_read(void *opaque, hwaddr haddr, unsigned size) {
    pmb887x_mmicif_t *p = opaque;
    uint64_t value = 0;

    switch (haddr) {
        case MMICIF_CLC:
            value = pmb887x_clc_get(&p->clc);
            break;

        case MMICIF_ID:
            value = 0xF053C000 | p->revision;
            break;

        case MMICIF_CONFIG:
            value = p->config;
            break;

        case MMICIF_UNK2C:
            value = p->unk2c;
            break;

        case MMICIF_UNK44:
            value = p->unk44;
            break;

        case MMICIF_TRANSFER_CONFIG:
            value = p->transfer_config;
            break;

        case MMICIF_UNK4C:
            value = p->unk4c;
            break;

        case MMICIF_UNK50:
            value = p->unk50;
            break;

        case MMICIF_UNK54:
            value = p->unk54;
            break;

        case MMICIF_IRQSM:
            value = p->irqsm;
            break;

        case MMICIF_IRQSS:
            value = p->irqss;
            break;

        case MMICIF_IRQSC:
            break;

        case MMICIF_UNK80:
            value = p->unk80;
            break;

        default:
            EPRINTF("unknown reg access: %02" PRIX64 "\n", haddr);
            break;
    }

    IO_DUMP(haddr + p->mmio.addr, size, value, false);
    return value;
}

static void mmicif_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
    pmb887x_mmicif_t *p = opaque;

    IO_DUMP(haddr + p->mmio.addr, size, value, true);

    switch (haddr) {
        case MMICIF_CLC:
            pmb887x_clc_set(&p->clc, value);
            break;

        case MMICIF_CONFIG:
            p->config = value;
            break;

        case MMICIF_UNK2C:
            p->unk2c = value;
            break;

        case MMICIF_UNK44:
            p->unk44 = value;
            break;

        case MMICIF_TRANSFER_CONFIG: {
            uint32_t old_mode = p->transfer_config & MMICIF_TRANSFER_CONFIG_MODE;
            uint32_t new_mode = value & MMICIF_TRANSFER_CONFIG_MODE;
            p->transfer_config = value;
            if (old_mode != new_mode) {
                p->host_address_valid = false;
                p->host_read_value_valid = false;
                p->host_status &= ~MMICIF_HOST_STATUS_READ_READY;
            }
            break;
        }

        case MMICIF_UNK4C:
            p->unk4c = value;
            break;

        case MMICIF_UNK50:
            p->unk50 = value;
            break;

        case MMICIF_UNK54:
            p->unk54 = value;
            break;

        case MMICIF_IRQSM:
            p->irqsm = value & 0xFF;
            break;

        case MMICIF_IRQSS:
            break;

        case MMICIF_IRQSC:
            p->irqss &= ~(value & 0xFF);
            break;

        case MMICIF_UNK80:
            p->unk80 = value & 0x0F;
            break;

        default:
            EPRINTF("unknown reg access: %02" PRIX64 "\n", haddr);
            break;
    }
}

static const MemoryRegionOps mmicif_io_ops = {
    .read = mmicif_io_read,
    .write = mmicif_io_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static uint64_t mmicif_mmap_read(void *opaque, hwaddr haddr, unsigned size) {
    pmb887x_mmicif_t *p = opaque;
    uint64_t value = UINT32_MAX;
    uint32_t mode = p->transfer_config & MMICIF_TRANSFER_CONFIG_MODE;

    if (haddr == MMICIF_HOST_STATUS_OFFSET) {
        value = p->host_status;
    } else if (p->host_index_valid) {
        uint8_t config_value;
        if (pmb887x_mmicif_bus_config_read(p->bus, p->host_index, &config_value))
            value = config_value;
        p->host_index_valid = false;
    } else if (p->host_address_valid && mode == MMICIF_TRANSFER_CONFIG_MODE_READ) {
        uint32_t endpoint_value;
        bool success;

        if (p->host_read_value_valid) {
            value = p->host_read_value;
            p->host_read_value_valid = false;
            success = true;
        } else {
            success = pmb887x_mmicif_bus_read(p->bus, p->host_address, &endpoint_value);
            if (success)
                value = endpoint_value;
        }
        if (success)
            p->host_address += sizeof(uint32_t);
    }

    IO_DUMP(haddr + p->mmap.addr, size, value, false);
    return value;
}

static void mmicif_mmap_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
    pmb887x_mmicif_t *p = opaque;
    uint32_t mode = p->transfer_config & MMICIF_TRANSFER_CONFIG_MODE;

    IO_DUMP(haddr + p->mmap.addr, size, value, true);

    if (haddr == MMICIF_HOST_STATUS_OFFSET) {
        p->host_status = value & ~MMICIF_HOST_STATUS_READ_READY;
        p->host_address_valid = false;
        p->host_read_value_valid = false;
        p->host_index_valid = false;
        return;
    }

    if (haddr == MMICIF_HOST_INDEX_OFFSET) {
        p->host_index = value & 0x0F;
        p->host_index |= value >> 3 & 0x10;
        p->host_index_valid = p->host_index != 0;
        p->host_address_valid = false;
        p->host_read_value_valid = false;
        return;
    }

    if (p->host_index_valid) {
        pmb887x_mmicif_bus_config_write(p->bus, p->host_index, value);
        p->host_index_valid = false;
        return;
    }

    if (!p->host_address_valid) {
        p->host_address = value & MMICIF_HOST_ADDRESS_MASK;
        p->host_address_valid = true;
        p->host_read_value_valid = false;
        if (mode == MMICIF_TRANSFER_CONFIG_MODE_READ) {
            if (pmb887x_mmicif_bus_read(p->bus, p->host_address, &p->host_read_value)) {
                p->host_read_value_valid = true;
                p->host_status |= MMICIF_HOST_STATUS_READ_READY;
            }
        }
        return;
    }

    if (mode == MMICIF_TRANSFER_CONFIG_MODE_WRITE && pmb887x_mmicif_bus_write(p->bus, p->host_address, value))
        p->host_address += sizeof(uint32_t);
}

static const MemoryRegionOps mmicif_mmap_ops = {
    .read = mmicif_mmap_read,
    .write = mmicif_mmap_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 2,
        .max_access_size = 4,
    },
};

static void mmicif_init(Object *obj) {
    pmb887x_mmicif_t *p = PMB887X_MMICIF(obj);
    memory_region_init_io(&p->mmio, obj, &mmicif_io_ops, p, TYPE_PMB887X_MMICIF, MMICIF_IO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
    memory_region_init_io(&p->mmap, obj, &mmicif_mmap_ops, p, "pmb887x-mmicif-mmap", MMICIF_MMAP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmap);
    p->bus = PMB887X_MMICIF_BUS(qbus_new(TYPE_PMB887X_MMICIF_BUS, DEVICE(obj), TYPE_PMB887X_MMICIF));
}

static void mmicif_reset(DeviceState *dev) {
    pmb887x_mmicif_t *p = PMB887X_MMICIF(dev);
    pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
    p->config = 0;
    p->unk2c = 0;
    p->unk44 = 0;
    p->transfer_config = 0;
    p->unk4c = 0;
    p->unk50 = 0;
    p->unk54 = 0;
    p->irqsm = 0;
    p->irqss = 0;
    p->unk80 = 0;
    p->host_address = 0;
    p->host_read_value = 0;
    p->host_status = 0;
    p->host_index = 0;
    p->host_address_valid = false;
    p->host_read_value_valid = false;
    p->host_index_valid = false;
}

static const Property mmicif_properties[] = {
    DEFINE_PROP_UINT32("revision", pmb887x_mmicif_t, revision, 0),
    DEFINE_PROP_LINK("bus", pmb887x_mmicif_t, bus, TYPE_PMB887X_MMICIF_BUS, pmb887x_mmicif_bus_t *),
};

static void mmicif_class_init(ObjectClass *klass, const void *data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, mmicif_properties);
    device_class_set_legacy_reset(dc, mmicif_reset);
}

static const TypeInfo mmicif_info = {
    .name = TYPE_PMB887X_MMICIF,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(pmb887x_mmicif_t),
    .instance_init = mmicif_init,
    .class_init = mmicif_class_init,
};

static void mmicif_register_types(void) {
    type_register_static(&mmicif_bus_info);
    type_register_static(&mmicif_device_info);
    type_register_static(&mmicif_info);
}
type_init(mmicif_register_types)
