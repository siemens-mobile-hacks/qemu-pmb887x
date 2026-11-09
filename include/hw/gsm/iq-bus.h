#pragma once

#include "hw/core/bus-port.h"
#include "hw/core/qdev.h"
#include "qom/object.h"

#define TYPE_PMB887X_RF_IQ_SOURCE "pmb887x-rf-iq-source"
#define TYPE_PMB887X_IQ_BUS "pmb887x-iq-bus"
#define PMB887X_IQ_BUS(obj) OBJECT_CHECK(pmb887x_iq_bus_t, (obj), TYPE_PMB887X_IQ_BUS)

typedef struct pmb887x_rf_iq_source_t pmb887x_rf_iq_source_t;
typedef struct pmb887x_rf_iq_source_class_t pmb887x_rf_iq_source_class_t;
typedef struct pmb887x_iq_source_port_t pmb887x_iq_source_port_t;
typedef struct pmb887x_iq_bus_t pmb887x_iq_bus_t;

#define PMB887X_RF_IQ_SOURCE(obj) INTERFACE_CHECK(pmb887x_rf_iq_source_t, (obj), TYPE_PMB887X_RF_IQ_SOURCE)
DECLARE_CLASS_CHECKERS(pmb887x_rf_iq_source_class_t, PMB887X_RF_IQ_SOURCE, TYPE_PMB887X_RF_IQ_SOURCE)

typedef struct pmb887x_iq_sample_t pmb887x_iq_sample_t;

struct pmb887x_iq_sample_t {
	int16_t i;
	int16_t q;
};

struct pmb887x_rf_iq_source_class_t {
	InterfaceClass parent_class;
	pmb887x_iq_sample_t (*read_iq)(pmb887x_rf_iq_source_t *source, int64_t timestamp, uint64_t sample_index);
};

struct pmb887x_iq_source_port_t {
	QDevBusPort parent;
	pmb887x_rf_iq_source_t *source;
};

pmb887x_iq_bus_t *pmb887x_iq_bus_create(DeviceState *parent, const char *name);
void pmb887x_iq_source_port_init(
	pmb887x_iq_source_port_t *port,
	DeviceState *dev,
	const char *name,
	pmb887x_rf_iq_source_t *source
);
pmb887x_iq_sample_t pmb887x_gsm_read_iq(uint32_t frequency, int64_t timestamp, int32_t amplitude);

bool pmb887x_iq_bus_has_source(const pmb887x_iq_bus_t *bus);
/* timestamp is the virtual time of the requested sample. */
pmb887x_iq_sample_t pmb887x_iq_bus_read(pmb887x_iq_bus_t *bus, int64_t timestamp, uint64_t sample_index);
