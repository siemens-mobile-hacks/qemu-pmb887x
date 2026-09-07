/*
 * Infineon PMB6272 (SMARTi Power 3i RF transceiver)
 * */
#define PMB887X_TRACE_ID		RF
#define PMB887X_TRACE_PREFIX	"pmb6272"

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/arm/pmb887x/trace.h"

typedef struct pmb887x_rf_t pmb887x_rf_t;

struct pmb887x_rf_t {
	SSIPeripheral dev;
};

#define TYPE_PMB887X_RF	"pmb6272"
#define PMB887X_RF(obj)	OBJECT_CHECK(pmb887x_rf_t, (obj), TYPE_PMB887X_RF)

static uint32_t rf_transfer(SSIPeripheral *dev, uint32_t in) {
	DPRINTF("telegram: %06X\n", in);
	return 0;
}

static void rf_realize(SSIPeripheral *d, Error **errp) {
	// Nothing to do.
}

static void rf_class_init(ObjectClass *klass, const void *data) {
	SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
	k->realize = rf_realize;
	k->transfer = rf_transfer;
	k->cs_polarity = SSI_CS_NONE;
}

static const TypeInfo rf_info = {
	.name          = TYPE_PMB887X_RF,
	.parent        = TYPE_SSI_PERIPHERAL,
	.instance_size = sizeof(pmb887x_rf_t),
	.class_init    = rf_class_init,
};

static void rf_register_types(void) {
	type_register_static(&rf_info);
}

type_init(rf_register_types)
