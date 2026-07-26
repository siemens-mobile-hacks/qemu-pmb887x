#pragma once

#include "qemu/osdep.h"
#include "hw/core/qdev.h"

#define TYPE_PMB887X_MMICIF "pmb887x-mmicif"

#define TYPE_PMB887X_MMICIF_BUS "pmb887x-mmicif-bus"
#define PMB887X_MMICIF_BUS(obj) OBJECT_CHECK(pmb887x_mmicif_bus_t, (obj), TYPE_PMB887X_MMICIF_BUS)

#define TYPE_PMB887X_MMICIF_DEVICE "pmb887x-mmicif-device"
#define PMB887X_MMICIF_DEVICE(obj) OBJECT_CHECK(pmb887x_mmicif_device_t, (obj), TYPE_PMB887X_MMICIF_DEVICE)
#define PMB887X_MMICIF_DEVICE_CLASS(klass) \
    OBJECT_CLASS_CHECK(pmb887x_mmicif_device_class_t, (klass), TYPE_PMB887X_MMICIF_DEVICE)
#define PMB887X_MMICIF_DEVICE_GET_CLASS(obj) \
    OBJECT_GET_CLASS(pmb887x_mmicif_device_class_t, (obj), TYPE_PMB887X_MMICIF_DEVICE)

typedef struct pmb887x_mmicif_bus_t pmb887x_mmicif_bus_t;
typedef struct pmb887x_mmicif_device_t pmb887x_mmicif_device_t;
typedef struct pmb887x_mmicif_device_class_t pmb887x_mmicif_device_class_t;

struct pmb887x_mmicif_device_t {
    DeviceState parent_obj;
};

struct pmb887x_mmicif_device_class_t {
    DeviceClass parent_class;
    bool (*config_read)(pmb887x_mmicif_device_t *dev, uint8_t index, uint8_t *value);
    bool (*config_write)(pmb887x_mmicif_device_t *dev, uint8_t index, uint8_t value);
    bool (*read)(pmb887x_mmicif_device_t *dev, uint32_t address, uint32_t *value);
    bool (*write)(pmb887x_mmicif_device_t *dev, uint32_t address, uint32_t value);
};

bool pmb887x_mmicif_bus_config_read(pmb887x_mmicif_bus_t *bus, uint8_t index, uint8_t *value);
bool pmb887x_mmicif_bus_config_write(pmb887x_mmicif_bus_t *bus, uint8_t index, uint8_t value);
bool pmb887x_mmicif_bus_read(pmb887x_mmicif_bus_t *bus, uint32_t address, uint32_t *value);
bool pmb887x_mmicif_bus_write(pmb887x_mmicif_bus_t *bus, uint32_t address, uint32_t value);
