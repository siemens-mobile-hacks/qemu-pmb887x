#define PMB887X_TRACE_ID		RF
#define PMB887X_TRACE_PREFIX	"hd155153np"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_HD155153NP

#include "qemu/osdep.h"
#include <math.h>

#include "qemu/atomic.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#include "hw/arm/pmb887x/gen/peripheral/HD155153NP.h"
#include "hw/arm/pmb887x/rf.h"
#include "hw/arm/pmb887x/trace.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/three-wire-serial.h"

#define TYPE_PMB887X_RF_TRX	"hd155153np"
#define PMB887X_RF_TRX(obj)	OBJECT_CHECK(pmb887x_rf_trx_t, (obj), TYPE_PMB887X_RF_TRX)

#define HD155153NP_REGISTER_ADDRESS_MASK	0x7
#define HD155153NP_REGISTER_ADDRESS_SHIFT	0U
#define HD155153NP_REGISTER_VALUE_SHIFT	3U
#define HD155153NP_BITS_TO_BYTES_SHIFT	3U
#define HD155153NP_TELEGRAM_BITS	24U
#define HD155153NP_TELEGRAM_MASK	MAKE_64BIT_MASK(0, HD155153NP_TELEGRAM_BITS)
#define HD155153NP_REFERENCE_FREQUENCY	26000000U
#define HD155153NP_CHANNEL_RASTER_DIVIDER	130U
#define HD155153NP_PLL_DIVIDER_MASK	0xFFFF
#define HD155153NP_PLL_BAND_MASK	0x3
#define HD155153NP_PLL_BAND_GSM850	0U
#define HD155153NP_PLL_BAND_GSM900	1U
#define HD155153NP_PLL_BAND_DCS1800	2U
#define HD155153NP_PLL_BAND_PCS1900	3U
#define HD155153NP_PLL_LOW_BAND_DIVIDER_SHIFT	3U
#define HD155153NP_PLL_HIGH_BAND_DIVIDER_SHIFT	2U
#define HD155153NP_RECEIVER_BASE_GAIN_DB	25
#define HD155153NP_PGA_STEP_DB	2
#define HD155153NP_MIXER1_LOW_GAIN_REDUCTION_DB	36
#define HD155153NP_MIXER2_LOW_GAIN_REDUCTION_DB	17
#define HD155153NP_FAKE_SIGNAL_LEVEL_DBM	(-60)
#define HD155153NP_ADC_AMPLITUDE_OFFSET_DB	60
#define HD155153NP_ADC_MAX_AMPLITUDE	16000

typedef struct pmb887x_rf_trx_t pmb887x_rf_trx_t;

struct pmb887x_rf_trx_t {
	SysBusDevice parent_obj;
	ThreeWireSerialTargetPort serial_port;
	pmb887x_iq_source_port_t iq_port;
	uint32_t telegram;
	uint32_t frequency;
	int32_t amplitude;
	uint64_t shift_register;
	uint8_t telegram_bits;
	uint8_t shift_bits;
	bool strobe_asserted;
	bool shift_overflow;
};

static void rf_trx_set_gain(pmb887x_rf_trx_t *p, uint32_t value) {
	uint32_t step = ((value & HD155153NP_GAIN_CONTROL_PGA_GAIN) >> HD155153NP_GAIN_CONTROL_PGA_GAIN_SHIFT);
	int32_t gain_db = HD155153NP_RECEIVER_BASE_GAIN_DB + (int32_t) step * HD155153NP_PGA_STEP_DB;

	if ((value & HD155153NP_GAIN_CONTROL_MIXER1_LOW_GAIN) != 0)
		gain_db -= HD155153NP_MIXER1_LOW_GAIN_REDUCTION_DB;
	if ((value & HD155153NP_GAIN_CONTROL_MIXER2_LOW_GAIN) != 0)
		gain_db -= HD155153NP_MIXER2_LOW_GAIN_REDUCTION_DB;

	/* EL71 v45 AGC calibration describes 2 dB PGA steps and 36/17 dB mixer range reductions. */
	int32_t adc_level_db = HD155153NP_FAKE_SIGNAL_LEVEL_DBM + HD155153NP_ADC_AMPLITUDE_OFFSET_DB + gain_db;
	int32_t amplitude = (int32_t) lround(pow(10.0, adc_level_db / 20.0));

	qatomic_set(&p->amplitude, MIN(amplitude, HD155153NP_ADC_MAX_AMPLITUDE));
}

static void rf_trx_set_channel(pmb887x_rf_trx_t *p, uint32_t value) {
	uint32_t synthesizer = (value & HD155153NP_CHANNEL_PLL_SYNTHESIZER);
	uint32_t mode = ((value & HD155153NP_CHANNEL_PLL_MODE) >> HD155153NP_CHANNEL_PLL_MODE_SHIFT);
	uint32_t band_code = (synthesizer & HD155153NP_PLL_BAND_MASK);
	const char *band;
	uint8_t divider_shift;

	switch (band_code) {
		case HD155153NP_PLL_BAND_GSM850:
			band = "GSM850";
			divider_shift = HD155153NP_PLL_LOW_BAND_DIVIDER_SHIFT;
			break;
		case HD155153NP_PLL_BAND_GSM900:
			band = "GSM900";
			divider_shift = HD155153NP_PLL_LOW_BAND_DIVIDER_SHIFT;
			break;
		case HD155153NP_PLL_BAND_DCS1800:
			band = "DCS1800";
			divider_shift = HD155153NP_PLL_HIGH_BAND_DIVIDER_SHIFT;
			break;
		case HD155153NP_PLL_BAND_PCS1900:
			band = "PCS1900";
			divider_shift = HD155153NP_PLL_HIGH_BAND_DIVIDER_SHIFT;
			break;
	}

	uint32_t divider = ((synthesizer & HD155153NP_PLL_DIVIDER_MASK) >> divider_shift);

	if (divider == 0) {
		qatomic_set(&p->frequency, 0);
		return;
	}

	uint32_t channel_raster = HD155153NP_REFERENCE_FREQUENCY / HD155153NP_CHANNEL_RASTER_DIVIDER;
	uint32_t frequency = divider * channel_raster;

	qatomic_set(&p->frequency, frequency);
	DPRINTF("tuned: frequency=%u Hz band=%s mode=%u\n", frequency, band, mode);
}

static void rf_trx_write_telegram(pmb887x_rf_trx_t *p, uint32_t telegram, uint8_t bits) {
	uint8_t address = ((telegram & HD155153NP_REGISTER_ADDRESS_MASK) >> HD155153NP_REGISTER_ADDRESS_SHIFT);
	uint32_t value = telegram >> HD155153NP_REGISTER_VALUE_SHIFT;

	p->telegram = telegram;
	p->telegram_bits = bits;

	switch (address) {
		case HD155153NP_CHANNEL_PLL:
			rf_trx_set_channel(p, value);
			break;

		case HD155153NP_GAIN_CONTROL:
			rf_trx_set_gain(p, value);
			break;
	}

	IO_DUMP_WRITE(address, bits >> HD155153NP_BITS_TO_BYTES_SHIFT, value);
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
		rf_trx_write_telegram(p, p->shift_register & HD155153NP_TELEGRAM_MASK, p->shift_bits);
}

static void rf_trx_write(ThreeWireSerialTarget *target, uint32_t value, unsigned int bits) {
	pmb887x_rf_trx_t *p = PMB887X_RF_TRX(OBJECT(target));

	if (!p->strobe_asserted || p->shift_overflow)
		return;
	if (bits > HD155153NP_TELEGRAM_BITS - p->shift_bits) {
		p->shift_overflow = true;
		return;
	}

	p->shift_register = (p->shift_register << bits) | (value & MAKE_64BIT_MASK(0, bits));
	p->shift_bits += bits;
}

static pmb887x_iq_sample_t rf_trx_read_iq(pmb887x_rf_iq_source_t *source, int64_t timestamp, uint64_t _sample_index) {
	pmb887x_rf_trx_t *p = PMB887X_RF_TRX(source);
	uint32_t frequency = qatomic_read(&p->frequency);
	int32_t amplitude = qatomic_read(&p->amplitude);

	return pmb887x_gsm_read_iq(frequency, timestamp, amplitude);
}

static void rf_trx_reset(DeviceState *device) {
	pmb887x_rf_trx_t *p = PMB887X_RF_TRX(device);

	p->telegram = 0;
	qatomic_set(&p->frequency, 0);
	qatomic_set(&p->amplitude, 0);
	p->shift_register = 0;
	p->telegram_bits = 0;
	p->shift_bits = 0;
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
