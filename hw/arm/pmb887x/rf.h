#pragma once

#include "hw/core/qdev.h"
#include "qom/object.h"

#define TYPE_PMB887X_RFSSC_BUS "pmb887x-rfssc-bus"
#define PMB887X_RFSSC_BUS(obj) OBJECT_CHECK(pmb887x_rfssc_bus_t, (obj), TYPE_PMB887X_RFSSC_BUS)

#define TYPE_PMB887X_RFSSC_DEVICE "pmb887x-rfssc-device"
#define PMB887X_RFSSC_DEVICE(obj) OBJECT_CHECK(pmb887x_rfssc_device_t, (obj), TYPE_PMB887X_RFSSC_DEVICE)
#define PMB887X_RFSSC_DEVICE_CLASS(klass) \
	OBJECT_CLASS_CHECK(pmb887x_rfssc_device_class_t, (klass), TYPE_PMB887X_RFSSC_DEVICE)
#define PMB887X_RFSSC_DEVICE_GET_CLASS(obj) \
	OBJECT_GET_CLASS(pmb887x_rfssc_device_class_t, (obj), TYPE_PMB887X_RFSSC_DEVICE)

#define TYPE_PMB887X_RF_IQ_SOURCE "pmb887x-rf-iq-source"

typedef struct pmb887x_rfssc_bus_t pmb887x_rfssc_bus_t;
typedef struct pmb887x_rfssc_device_t pmb887x_rfssc_device_t;
typedef struct pmb887x_rfssc_device_class_t pmb887x_rfssc_device_class_t;
typedef struct pmb887x_rf_iq_source_t pmb887x_rf_iq_source_t;
typedef struct pmb887x_rf_iq_source_class_t pmb887x_rf_iq_source_class_t;

#define PMB887X_RF_IQ_SOURCE(obj) INTERFACE_CHECK(pmb887x_rf_iq_source_t, (obj), TYPE_PMB887X_RF_IQ_SOURCE)
DECLARE_CLASS_CHECKERS(pmb887x_rf_iq_source_class_t, PMB887X_RF_IQ_SOURCE, TYPE_PMB887X_RF_IQ_SOURCE)

typedef struct pmb887x_iq_sample_t pmb887x_iq_sample_t;

struct pmb887x_rfssc_device_t {
	DeviceState parent_obj;
};

struct pmb887x_rfssc_device_class_t {
	DeviceClass parent_class;
	void (*transfer)(pmb887x_rfssc_device_t *device, uint32_t value, uint8_t bits);
};

struct pmb887x_iq_sample_t {
	int16_t i;
	int16_t q;
};

struct pmb887x_rf_iq_source_class_t {
	InterfaceClass parent_class;
	pmb887x_iq_sample_t (*read_iq)(pmb887x_rf_iq_source_t *source, int64_t timestamp, uint64_t sample_index);
};

pmb887x_rfssc_bus_t *pmb887x_rfssc_create_bus(DeviceState *parent, const char *name);
void pmb887x_rfssc_transfer(pmb887x_rfssc_bus_t *bus, uint32_t value, uint8_t bits);
pmb887x_iq_sample_t pmb887x_gsm_read_iq(uint32_t frequency, int64_t timestamp, int32_t amplitude);

/* timestamp is the virtual time of the requested sample. */
static inline pmb887x_iq_sample_t pmb887x_rf_iq_read(
	pmb887x_rf_iq_source_t *source,
	int64_t timestamp,
	uint64_t sample_index
) {
	pmb887x_rf_iq_source_class_t *klass = PMB887X_RF_IQ_SOURCE_GET_CLASS(source);

	return klass->read_iq(source, timestamp, sample_index);
}
