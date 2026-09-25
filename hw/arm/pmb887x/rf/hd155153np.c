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

#define TYPE_PMB887X_RF_TRX	"hd155153np"
#define PMB887X_RF_TRX(obj)	OBJECT_CHECK(pmb887x_rf_trx_t, (obj), TYPE_PMB887X_RF_TRX)

#define HD155153NP_REGISTER_ADDRESS_MASK	0x7
#define HD155153NP_REGISTER_ADDRESS_SHIFT	0U
#define HD155153NP_REGISTER_VALUE_SHIFT	3U
#define HD155153NP_BITS_TO_BYTES_SHIFT	3U
#define HD155153NP_TELEGRAM_HALF_BITS	12U
#define HD155153NP_TELEGRAM_HALF_MASK	MAKE_64BIT_MASK(0, HD155153NP_TELEGRAM_HALF_BITS)
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
	pmb887x_rfssc_device_t parent_obj;
	uint32_t telegram;
	uint32_t frequency;
	int32_t amplitude;
	uint16_t telegram_high;
	uint8_t telegram_bits;
	bool telegram_pending;
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

static void rf_trx_transfer(pmb887x_rfssc_device_t *device, uint32_t value, uint8_t bits) {
	pmb887x_rf_trx_t *p = PMB887X_RF_TRX(device);

	if (bits != HD155153NP_TELEGRAM_HALF_BITS) {
		p->telegram_pending = false;
		rf_trx_write_telegram(p, value & HD155153NP_TELEGRAM_MASK, bits);
		return;
	}

	uint16_t telegram_half = (value & HD155153NP_TELEGRAM_HALF_MASK);

	if (!p->telegram_pending) {
		p->telegram_high = telegram_half;
		p->telegram_pending = true;
		return;
	}

	uint32_t telegram = ((uint32_t) p->telegram_high << HD155153NP_TELEGRAM_HALF_BITS) | telegram_half;
	p->telegram_pending = false;
	rf_trx_write_telegram(p, telegram, HD155153NP_TELEGRAM_BITS);
}

static pmb887x_iq_sample_t rf_trx_read_iq(pmb887x_rf_iq_source_t *source, int64_t timestamp, uint64_t sample_index) {
	pmb887x_rf_trx_t *p = PMB887X_RF_TRX(source);
	uint32_t frequency = qatomic_read(&p->frequency);
	int32_t amplitude = qatomic_read(&p->amplitude);

	(void) sample_index;

	return pmb887x_gsm_read_iq(frequency, timestamp, amplitude);
}

static void rf_trx_reset(DeviceState *device) {
	pmb887x_rf_trx_t *p = PMB887X_RF_TRX(device);

	p->telegram = 0;
	qatomic_set(&p->frequency, 0);
	qatomic_set(&p->amplitude, 0);
	p->telegram_high = 0;
	p->telegram_bits = 0;
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
