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
#include "hw/core/sysbus.h"
#include "hw/ssi/three-wire-serial.h"

#define TYPE_PMB887X_RF_TRX	"pmb6272"
#define PMB887X_RF_TRX(obj)	OBJECT_CHECK(pmb887x_rf_trx_t, (obj), TYPE_PMB887X_RF_TRX)

#define PMB6272_REGISTER_ADDRESS_MASK	0xF
#define PMB6272_REGISTER_ADDRESS_SHIFT	0U
#define PMB6272_BITS_TO_BYTES_SHIFT	3U
#define PMB6272_REFERENCE_FREQUENCY	26000000U
#define PMB6272_TELEGRAM_BITS	24U
#define PMB6272_TELEGRAM_MASK	MAKE_64BIT_MASK(0, PMB6272_TELEGRAM_BITS)
#define PMB6272_REGISTER_COUNT	16U
#define PMB6272_CHANNEL_FRACTION_BITS	23U
#define PMB6272_SIGNAL_AMPLITUDE	4000

typedef struct pmb887x_rf_trx_t pmb887x_rf_trx_t;

struct pmb887x_rf_trx_t {
	SysBusDevice parent_obj;
	ThreeWireSerialTargetPort serial_port;
	pmb887x_iq_source_port_t iq_port;
	uint32_t registers[PMB6272_REGISTER_COUNT];
	uint32_t telegram;
	uint32_t channel_fraction;
	uint32_t frequency;
	uint64_t shift_register;
	uint8_t telegram_bits;
	uint8_t shift_bits;
	uint8_t channel_integer;
	uint8_t band;
	bool transmit;
	bool strobe_asserted;
	bool shift_overflow;
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

static void rf_trx_set_strobe(ThreeWireSerialTarget *target, bool asserted) {
	pmb887x_rf_trx_t *p = PMB887X_RF_TRX(OBJECT(target));

	if (asserted) {
		p->shift_register = 0;
		p->shift_bits = 0;
		p->shift_overflow = false;
		p->strobe_asserted = true;
		return;
	}

	if (!p->strobe_asserted)
		return;

	p->strobe_asserted = false;
	if (!p->shift_overflow && p->shift_bits != 0)
		rf_trx_write_telegram(p, p->shift_register & PMB6272_TELEGRAM_MASK, p->shift_bits);
}

static void rf_trx_write(ThreeWireSerialTarget *target, uint32_t value, unsigned int bits) {
	pmb887x_rf_trx_t *p = PMB887X_RF_TRX(OBJECT(target));

	if (!p->strobe_asserted || p->shift_overflow)
		return;
	if (bits > PMB6272_TELEGRAM_BITS - p->shift_bits) {
		p->shift_overflow = true;
		return;
	}

	p->shift_register = (p->shift_register << bits) | (value & MAKE_64BIT_MASK(0, bits));
	p->shift_bits += bits;
}

static pmb887x_iq_sample_t rf_trx_read_iq(pmb887x_rf_iq_source_t *source, int64_t timestamp, uint64_t _sample_index) {
	pmb887x_rf_trx_t *p = PMB887X_RF_TRX(source);
	uint32_t frequency = p->transmit ? 0 : qatomic_read(&p->frequency);

	return pmb887x_gsm_read_iq(frequency, timestamp, PMB6272_SIGNAL_AMPLITUDE);
}

static void rf_trx_reset(DeviceState *device) {
	pmb887x_rf_trx_t *p = PMB887X_RF_TRX(device);

	memset(p->registers, 0, sizeof(p->registers));
	p->telegram = 0;
	p->channel_fraction = 0;
	qatomic_set(&p->frequency, 0);
	p->shift_register = 0;
	p->telegram_bits = 0;
	p->shift_bits = 0;
	p->channel_integer = 0;
	p->band = 0;
	p->transmit = false;
	p->strobe_asserted = false;
	p->shift_overflow = false;
}

static void rf_trx_init(Object *obj) {
	pmb887x_rf_trx_t *p = PMB887X_RF_TRX(obj);

	three_wire_serial_target_port_init(
		&p->serial_port,
		DEVICE(obj),
		"RFSSC",
		THREE_WIRE_SERIAL_TARGET(obj)
	);
	pmb887x_iq_source_port_init(&p->iq_port, DEVICE(obj), "IQ_TX", PMB887X_RF_IQ_SOURCE(obj));
}

static void rf_trx_class_init(ObjectClass *klass, const void *_data) {
	DeviceClass *device_class = DEVICE_CLASS(klass);
	ThreeWireSerialTargetClass *serial_class = THREE_WIRE_SERIAL_TARGET_CLASS(klass);
	pmb887x_rf_iq_source_class_t *iq_class = PMB887X_RF_IQ_SOURCE_CLASS(klass);

	serial_class->set_strobe = rf_trx_set_strobe;
	serial_class->write = rf_trx_write;
	iq_class->read_iq = rf_trx_read_iq;
	device_class_set_legacy_reset(device_class, rf_trx_reset);
}

static const TypeInfo rf_trx_info = {
	.name = TYPE_PMB887X_RF_TRX,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(pmb887x_rf_trx_t),
	.instance_init = rf_trx_init,
	.class_init = rf_trx_class_init,
	.interfaces = (const InterfaceInfo[]) {
		{ TYPE_THREE_WIRE_SERIAL_TARGET },
		{ TYPE_PMB887X_RF_IQ_SOURCE },
		{ NULL },
	},
};

static void rf_trx_register_types(void) {
	type_register_static(&rf_trx_info);
}

type_init(rf_trx_register_types)
