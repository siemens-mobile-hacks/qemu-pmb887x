#define PMB887X_TRACE_ID		RF
#define PMB887X_TRACE_PREFIX	"pmb6272"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_PMB6272

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#include "hw/arm/pmb887x/gen/peripheral/PMB6272.h"
#include "hw/arm/pmb887x/rf.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_RF_TRX	"pmb6272"
#define PMB887X_RF_TRX(obj)	OBJECT_CHECK(pmb887x_rf_trx_t, (obj), TYPE_PMB887X_RF_TRX)

#define PMB6272_REGISTER_ADDRESS_MASK	0xF
#define PMB6272_REGISTER_ADDRESS_SHIFT	0U
#define PMB6272_BITS_TO_BYTES_SHIFT	3U
#define PMB6272_REFERENCE_FREQUENCY	26000000U
#define PMB6272_TELEGRAM_HALF_BITS	12U
#define PMB6272_TELEGRAM_HALF_MASK	MAKE_64BIT_MASK(0, PMB6272_TELEGRAM_HALF_BITS)
#define PMB6272_TELEGRAM_BITS	24U
#define PMB6272_TELEGRAM_MASK	MAKE_64BIT_MASK(0, PMB6272_TELEGRAM_BITS)
#define PMB6272_REGISTER_COUNT	16U
#define PMB6272_CHANNEL_FRACTION_BITS	23U
#define PMB6272_SIGNAL_AMPLITUDE	4000

typedef struct pmb887x_rf_trx_t pmb887x_rf_trx_t;

struct pmb887x_rf_trx_t {
	pmb887x_rfssc_device_t parent_obj;
	uint32_t registers[PMB6272_REGISTER_COUNT];
	uint32_t telegram;
	uint32_t channel_fraction;
	uint32_t frequency;
	uint16_t telegram_high;
	uint8_t telegram_bits;
	uint8_t channel_integer;
	uint8_t band;
	bool transmit;
	bool telegram_pending;
};

static void rf_trx_write_telegram(pmb887x_rf_trx_t *p, uint32_t telegram, uint8_t bits) {
	static const char *band_names[] = { "GSM850", "GSM900", "DCS1800", "PCS1900" };
	uint8_t address = ((telegram & PMB6272_REGISTER_ADDRESS_MASK) >> PMB6272_REGISTER_ADDRESS_SHIFT);

	p->telegram = telegram;
	p->telegram_bits = bits;
	p->registers[address] = telegram;

	switch (address) {
		case PMB6272_CHANNEL1:
			p->channel_fraction = deposit32(p->channel_fraction, 0, 20,
				((telegram & PMB6272_CHANNEL1_FRACTION) >> PMB6272_CHANNEL1_FRACTION_SHIFT));
			break;

		case PMB6272_CHANNEL2: {
			p->channel_fraction = deposit32(p->channel_fraction, 20, 3,
				((telegram & PMB6272_CHANNEL2_FRACTION) >> PMB6272_CHANNEL2_FRACTION_SHIFT));
			p->channel_integer = ((telegram & PMB6272_CHANNEL2_INTEGER) >> PMB6272_CHANNEL2_INTEGER_SHIFT);
			p->transmit = (telegram & PMB6272_CHANNEL2_TRX) != 0;
			p->band = ((telegram & PMB6272_CHANNEL2_BAND) >> PMB6272_CHANNEL2_BAND_SHIFT);

			uint64_t synthesizer = ((uint64_t) p->channel_integer << PMB6272_CHANNEL_FRACTION_BITS) |
				p->channel_fraction;
			uint32_t divisor = p->band < 2 ? 4 : 2;
			uint32_t frequency = muldiv64(synthesizer, PMB6272_REFERENCE_FREQUENCY,
				((uint64_t) 1 << PMB6272_CHANNEL_FRACTION_BITS)) / divisor;

			qatomic_set(&p->frequency, frequency);
			DPRINTF("tuned to %u Hz: band=%s path=%s\n", frequency, band_names[p->band],
				p->transmit ? "TX" : "RX");
			break;
		}
	}

	IO_DUMP_WRITE(address, bits >> PMB6272_BITS_TO_BYTES_SHIFT, telegram);
}

static void rf_trx_transfer(pmb887x_rfssc_device_t *device, uint32_t value, uint8_t bits) {
	pmb887x_rf_trx_t *p = PMB887X_RF_TRX(device);

	if (bits != PMB6272_TELEGRAM_HALF_BITS) {
		p->telegram_pending = false;
		rf_trx_write_telegram(p, value & PMB6272_TELEGRAM_MASK, bits);
		return;
	}

	uint16_t telegram_half = (value & PMB6272_TELEGRAM_HALF_MASK);

	if (!p->telegram_pending) {
		p->telegram_high = telegram_half;
		p->telegram_pending = true;
		return;
	}

	uint32_t telegram = ((uint32_t) p->telegram_high << PMB6272_TELEGRAM_HALF_BITS) | telegram_half;
	p->telegram_pending = false;
	rf_trx_write_telegram(p, telegram, PMB6272_TELEGRAM_BITS);
}

static pmb887x_iq_sample_t rf_trx_read_iq(pmb887x_rf_iq_source_t *source, int64_t timestamp, uint64_t sample_index) {
	pmb887x_rf_trx_t *p = PMB887X_RF_TRX(source);
	uint32_t frequency = p->transmit ? 0 : qatomic_read(&p->frequency);

	(void) sample_index;

	return pmb887x_gsm_read_iq(frequency, timestamp, PMB6272_SIGNAL_AMPLITUDE);
}

static void rf_trx_reset(DeviceState *device) {
	pmb887x_rf_trx_t *p = PMB887X_RF_TRX(device);

	memset(p->registers, 0, sizeof(p->registers));
	p->telegram = 0;
	p->channel_fraction = 0;
	qatomic_set(&p->frequency, 0);
	p->telegram_high = 0;
	p->telegram_bits = 0;
	p->channel_integer = 0;
	p->band = 0;
	p->transmit = false;
	p->telegram_pending = false;
}

static void rf_trx_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *device_class = DEVICE_CLASS(klass);
	pmb887x_rfssc_device_class_t *rfssc_class = PMB887X_RFSSC_DEVICE_CLASS(klass);
	pmb887x_rf_iq_source_class_t *iq_class = PMB887X_RF_IQ_SOURCE_CLASS(klass);

	(void) data;

	rfssc_class->transfer = rf_trx_transfer;
	iq_class->read_iq = rf_trx_read_iq;
	device_class_set_legacy_reset(device_class, rf_trx_reset);
}

static const TypeInfo rf_trx_info = {
	.name = TYPE_PMB887X_RF_TRX,
	.parent = TYPE_PMB887X_RFSSC_DEVICE,
	.instance_size = sizeof(pmb887x_rf_trx_t),
	.class_init = rf_trx_class_init,
	.interfaces = (const InterfaceInfo[]) {
		{ TYPE_PMB887X_RF_IQ_SOURCE },
		{ NULL },
	},
};

static void rf_trx_register_types(void) {
	type_register_static(&rf_trx_info);
}

type_init(rf_trx_register_types)
