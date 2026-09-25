#include "qemu/osdep.h"
#include <math.h>

#include "hw/arm/pmb887x/rf.h"

#define GSM_BTS_FREQUENCY 936000000U
#define GSM_HYPERFRAME_FRAMES 2715648U
#define GSM_CONTROL_MULTIFRAME_FRAMES 51U
#define GSM_FRAME_QUARTER_SYMBOLS 5000U
#define GSM_TIMESLOT_QUARTER_SYMBOLS 625U
#define GSM_BURST_SYMBOLS 148U
#define GSM_SCH_DATA_BITS 25U
#define GSM_SCH_PARITY_BITS 10U
#define GSM_SCH_TAIL_BITS 4U
#define GSM_SCH_UNCODED_BITS (GSM_SCH_DATA_BITS + GSM_SCH_PARITY_BITS + GSM_SCH_TAIL_BITS)
#define GSM_SCH_ENCODED_BITS (GSM_SCH_UNCODED_BITS * 2U)
#define GSM_SCH_DATA_FIRST_START 3U
#define GSM_SCH_TRAINING_START 42U
#define GSM_SCH_DATA_SECOND_START 106U
#define GSM_SCH_CRC_MASK 0x03FFU
#define GSM_SCH_CRC_POLYNOMIAL 0x0175U
#define GSM_GMSK_SAMPLES_PER_SYMBOL 4U
#define GSM_GMSK_COEFFICIENT_SYMBOLS (GSM_BURST_SYMBOLS + 2U)
#define GSM_BSIC 0U

typedef enum gsm_burst_t gsm_burst_t;
typedef struct gsm_complex_t gsm_complex_t;

enum gsm_burst_t {
	GSM_BURST_NORMAL,
	GSM_BURST_FCCH,
	GSM_BURST_SCH,
};

struct gsm_complex_t {
	double real;
	double imaginary;
};

static const char GSM_SCH_TRAINING_SEQUENCE[] =
	"1011100101100010000001000000111100101101010001010111011000011011";
static const char GSM_DUMMY_BURST[] =
	"0001111101101110110000010100100111000001001000100000001111100011100010111000101110001010111010010100"
	"011001100111001111010011111000100101111101010000";
/* Four-sample Laurent C0/C1 pulse decomposition for BT=0.3 GMSK. */
static const double GSM_GMSK_LAURENT_C0[] = {
	0.0, 4.46348606e-03, 2.84385729e-02, 1.03184855e-01,
	2.56065552e-01, 4.76375085e-01, 7.05961177e-01, 8.71291644e-01,
	9.29453645e-01, 8.71291644e-01, 7.05961177e-01, 4.76375085e-01,
	2.56065552e-01, 1.03184855e-01, 2.84385729e-02, 4.46348606e-03,
};
static const double GSM_GMSK_LAURENT_C1[] = {
	0.0, 8.16373112e-03, 2.84385729e-02, 5.64158904e-02,
	7.05463553e-02, 5.64158904e-02, 2.84385729e-02, 8.16373112e-03,
};

static bool gsm_fcch_frame(uint32_t frame) {
	return frame <= 40U && frame % 10U == 0;
}

static bool gsm_sch_frame(uint32_t frame) {
	return frame <= 41U && frame % 10U == 1U;
}

static uint32_t gsm_frame_number_to_sch_information(uint32_t frame_number) {
	uint32_t t1 = frame_number / (26U * GSM_CONTROL_MULTIFRAME_FRAMES);
	uint32_t t2 = frame_number % 26U;
	uint32_t t3 = frame_number % GSM_CONTROL_MULTIFRAME_FRAMES;
	uint32_t t3_prime = (t3 - 1U) / 10U;

	return ((t1 >> 9U) & 0x03U) | (GSM_BSIC << 2U) | (((t1 >> 1U) & 0xFFU) << 8U) |
		(((t3_prime >> 1U) & 0x03U) << 16U) | ((t2 & 0x1FU) << 18U) | ((t1 & 1U) << 23U) |
		((t3_prime & 1U) << 24U);
}

static bool gsm_sch_information_bit(uint32_t information, size_t bit) {
	return ((information >> bit) & 1U) != 0;
}

static uint16_t gsm_sch_parity(uint32_t information) {
	uint16_t remainder = 0;

	for (size_t bit = 0; bit < GSM_SCH_DATA_BITS; bit++) {
		bool input = gsm_sch_information_bit(information, bit);
		bool feedback = ((remainder >> 9) & 1U) != input;

		remainder = (remainder << 1) & GSM_SCH_CRC_MASK;
		if (feedback)
			remainder ^= GSM_SCH_CRC_POLYNOMIAL;
	}

	return remainder ^ GSM_SCH_CRC_MASK;
}

static bool gsm_sch_uncoded_bit(uint32_t information, uint16_t parity, int bit) {
	if (bit < 0 || bit >= (int) (GSM_SCH_DATA_BITS + GSM_SCH_PARITY_BITS))
		return false;
	if (bit < (int) GSM_SCH_DATA_BITS)
		return gsm_sch_information_bit(information, bit);

	return ((parity >> (GSM_SCH_PARITY_BITS - 1U - (bit - GSM_SCH_DATA_BITS))) & 1U) != 0;
}

static bool gsm_sch_encoded_bit(uint32_t information, uint16_t parity, size_t bit) {
	int input = bit / 2;
	bool value = gsm_sch_uncoded_bit(information, parity, input);
	value ^= gsm_sch_uncoded_bit(information, parity, input - 3);
	value ^= gsm_sch_uncoded_bit(information, parity, input - 4);

	if ((bit & 1U))
		value ^= gsm_sch_uncoded_bit(information, parity, input - 1);

	return value;
}

static bool gsm_sch_burst_bit(uint32_t information, uint16_t parity, size_t symbol) {
	if (symbol >= GSM_SCH_DATA_FIRST_START && symbol < GSM_SCH_TRAINING_START)
		return gsm_sch_encoded_bit(information, parity, symbol - GSM_SCH_DATA_FIRST_START);
	if (symbol >= GSM_SCH_TRAINING_START && symbol < GSM_SCH_DATA_SECOND_START)
		return GSM_SCH_TRAINING_SEQUENCE[symbol - GSM_SCH_TRAINING_START] == '1';
	if (symbol >= GSM_SCH_DATA_SECOND_START && symbol < GSM_BURST_SYMBOLS - 3U)
		return gsm_sch_encoded_bit(information, parity,
			symbol - GSM_SCH_DATA_SECOND_START + GSM_SCH_ENCODED_BITS / 2U);
	return false;
}

static bool gsm_normal_burst_bit(size_t symbol) {
	return GSM_DUMMY_BURST[symbol] == '1';
}

static bool gsm_burst_bit(uint32_t information, uint16_t parity, gsm_burst_t burst, size_t symbol) {
	switch (burst) {
		case GSM_BURST_NORMAL:
			return gsm_normal_burst_bit(symbol);

		case GSM_BURST_SCH:
			return gsm_sch_burst_bit(information, parity, symbol);

		case GSM_BURST_FCCH:
			return false;
	}

	abort();
}

static bool gsm_gmsk_raw_bit(uint32_t information, uint16_t parity, gsm_burst_t burst, uint32_t coefficient_symbol) {
	if (coefficient_symbol == 0U || coefficient_symbol == GSM_BURST_SYMBOLS + 1U)
		return false;

	return gsm_burst_bit(information, parity, burst, coefficient_symbol - 1U);
}

static gsm_complex_t gsm_gmsk_c0_coefficient(
	uint32_t information,
	uint16_t parity,
	gsm_burst_t burst,
	uint32_t coefficient_symbol
) {
	int32_t value = gsm_gmsk_raw_bit(information, parity, burst, coefficient_symbol) ? 1 : -1;
	gsm_complex_t coefficient = { 0.0, 0.0 };

	switch ((coefficient_symbol & 3U)) {
		case 0:
			coefficient.real = value;
			break;

		case 1:
			coefficient.imaginary = value;
			break;

		case 2:
			coefficient.real = -value;
			break;

		case 3:
			coefficient.imaginary = -value;
			break;
	}

	return coefficient;
}

static gsm_complex_t gsm_gmsk_c1_coefficient(
	uint32_t information,
	uint16_t parity,
	gsm_burst_t burst,
	uint32_t coefficient_symbol
) {
	gsm_complex_t coefficient = { 0.0, 0.0 };
	int32_t phase;

	if (coefficient_symbol < 2U)
		return coefficient;

	if (coefficient_symbol == 2U) {
		phase = -1;
	} else {
		bool current_bit = gsm_burst_bit(information, parity, burst, coefficient_symbol - 2U);
		bool previous_bit = gsm_burst_bit(information, parity, burst, coefficient_symbol - 3U);

		phase = current_bit != previous_bit ? 1 : -1;
	}

	gsm_complex_t c0 = gsm_gmsk_c0_coefficient(information, parity, burst, coefficient_symbol);
	coefficient.real = -c0.imaginary * phase;
	coefficient.imaginary = c0.real * phase;
	return coefficient;
}

static gsm_complex_t gsm_gmsk_laurent_sample(
	uint32_t information,
	uint16_t parity,
	gsm_burst_t burst,
	uint32_t quarter_symbol
) {
	gsm_complex_t sample = { 0.0, 0.0 };
	uint32_t first_delay = quarter_symbol % GSM_GMSK_SAMPLES_PER_SYMBOL;
	uint32_t c0_last_delay = MIN(quarter_symbol, (uint32_t) ARRAY_SIZE(GSM_GMSK_LAURENT_C0) - 1U);

	for (uint32_t delay = first_delay; delay <= c0_last_delay; delay += GSM_GMSK_SAMPLES_PER_SYMBOL) {
		uint32_t coefficient_symbol = (quarter_symbol - delay) / GSM_GMSK_SAMPLES_PER_SYMBOL;

		if (coefficient_symbol >= GSM_GMSK_COEFFICIENT_SYMBOLS)
			continue;

		gsm_complex_t coefficient = gsm_gmsk_c0_coefficient(information, parity, burst, coefficient_symbol);
		uint32_t pulse_sample = ARRAY_SIZE(GSM_GMSK_LAURENT_C0) - delay - 1U;
		double pulse = GSM_GMSK_LAURENT_C0[pulse_sample];

		sample.real += coefficient.real * pulse;
		sample.imaginary += coefficient.imaginary * pulse;
	}

	uint32_t c1_last_delay = MIN(quarter_symbol, (uint32_t) ARRAY_SIZE(GSM_GMSK_LAURENT_C1) - 1U);
	for (uint32_t delay = first_delay; delay <= c1_last_delay; delay += GSM_GMSK_SAMPLES_PER_SYMBOL) {
		uint32_t coefficient_symbol = (quarter_symbol - delay) / GSM_GMSK_SAMPLES_PER_SYMBOL;

		if (coefficient_symbol >= GSM_GMSK_COEFFICIENT_SYMBOLS)
			continue;

		gsm_complex_t coefficient = gsm_gmsk_c1_coefficient(information, parity, burst, coefficient_symbol);
		uint32_t pulse_sample = ARRAY_SIZE(GSM_GMSK_LAURENT_C1) - delay - 1U;
		double pulse = GSM_GMSK_LAURENT_C1[pulse_sample];

		sample.real += coefficient.real * pulse;
		sample.imaginary += coefficient.imaginary * pulse;
	}

	return sample;
}

static pmb887x_iq_sample_t gsm_gmsk_sample(
	uint32_t frame_number,
	gsm_burst_t burst,
	uint32_t quarter_symbol,
	int32_t amplitude
) {
	uint32_t information = burst == GSM_BURST_SCH ? gsm_frame_number_to_sch_information(frame_number) : 0;
	uint16_t parity = burst == GSM_BURST_SCH ? gsm_sch_parity(information) : 0;
	gsm_complex_t reference = gsm_gmsk_laurent_sample(information, parity, burst, quarter_symbol);

	/* Preserve the baseband model's existing complex orientation and initial phase. */
	pmb887x_iq_sample_t sample = {
		(int16_t) lround(-(double) amplitude * reference.real),
		(int16_t) lround((double) amplitude * reference.imaginary),
	};
	return sample;
}

pmb887x_iq_sample_t pmb887x_gsm_read_iq(uint32_t frequency, int64_t timestamp, int32_t amplitude) {
	pmb887x_iq_sample_t sample = { 0, 0 };

	if (frequency != GSM_BTS_FREQUENCY || timestamp < 0)
		return sample;

	uint64_t quarter_symbols = muldiv64((uint64_t) timestamp, 13U, 12000U);
	uint32_t frame_number = quarter_symbols / GSM_FRAME_QUARTER_SYMBOLS % GSM_HYPERFRAME_FRAMES;
	uint32_t frame_quarter_symbol = quarter_symbols % GSM_FRAME_QUARTER_SYMBOLS;
	uint32_t timeslot = frame_quarter_symbol / GSM_TIMESLOT_QUARTER_SYMBOLS;
	uint32_t timeslot_quarter_symbol = frame_quarter_symbol % GSM_TIMESLOT_QUARTER_SYMBOLS;
	uint32_t multiframe_frame = frame_number % GSM_CONTROL_MULTIFRAME_FRAMES;
	gsm_burst_t burst = GSM_BURST_NORMAL;

	if (timeslot == 0 && gsm_fcch_frame(multiframe_frame))
		burst = GSM_BURST_FCCH;
	if (timeslot == 0 && gsm_sch_frame(multiframe_frame))
		burst = GSM_BURST_SCH;

	return gsm_gmsk_sample(frame_number, burst, timeslot_quarter_symbol, amplitude);
}
