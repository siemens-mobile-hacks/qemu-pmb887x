#define PMB887X_TRACE_ID		DSP_BASEBAND
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-baseband"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bitops.h"
#include "qemu/timer.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/rf.h"
#include "hw/arm/pmb887x/trace.h"

#define BASEBAND_CTRL_UNDOCUMENTED	0x0100U
#define BASEBAND_CTRL_MASK		(TEAK_BB_CTRL_BB_STOP | TEAK_BB_CTRL_CORDICON | TEAK_BB_CTRL_BB_ON | \
	TEAK_BB_CTRL_BB_ADCMODE | TEAK_BB_CTRL_BBADAP_EN | BASEBAND_CTRL_UNDOCUMENTED)
#define BASEBAND_FILTER_CTRL_BRCFG	TEAK_BB_BRFILTER_CTRL_DECIMATION
#define BASEBAND_FILTER_CTRL_MASK	(TEAK_BB_BRFILTER_CTRL_LENGTH | TEAK_BB_BRFILTER_CTRL_SCALING | \
	BASEBAND_FILTER_CTRL_BRCFG)
#define BASEBAND_RING_WORDS		0x03C0U
#define BASEBAND_WORDS_PER_SAMPLE	2U
#define BASEBAND_SAMPLE_CLOCK_HZ		13000000U
#define BASEBAND_NARROW_SAMPLE_CLOCK_DIVISOR	24U
#define BASEBAND_STANDARD_SAMPLE_CLOCK_DIVISOR	96U
#define BASEBAND_SAMPLE_PHASE_PARTS	8U
#define BASEBAND_JOB_PHASE_PARTS		3U
#define BASEBAND_DECIMATION_DIVISOR	2U
#define BASEBAND_FILTER_NORMAL_HALF_LENGTH	16U
#define BASEBAND_FILTER_MAX_HALF_LENGTH	32U
#define BASEBAND_FILTER_MAX_TAPS		(BASEBAND_FILTER_MAX_HALF_LENGTH * 2U + 1U)
#define BASEBAND_FILTER_SCALING_BIAS	13U
#define BASEBAND_INTERRUPT_GROUP	0U
#define BASEBAND_CTRL_RESET		0x0110U
#define BASEBAND_NO_DEADLINE		0
#define BASEBAND_NORMAL_BURST_SYMBOLS	148U
#define BASEBAND_TRAINING_START		61U
#define BASEBAND_SIGNAL_AMPLITUDE	16000

/* GSM normal-burst training sequence code 0. Payload, stealing, and tail bits are zero. */
static const char BASEBAND_TRAINING_SEQUENCE[] = "00100101110000100010010111";
static const int16_t BASEBAND_PHASE_I[] = {
	BASEBAND_SIGNAL_AMPLITUDE, 11314, 0, -11314,
	-BASEBAND_SIGNAL_AMPLITUDE, -11314, 0, 11314,
};
static const int16_t BASEBAND_PHASE_Q[] = {
	0, -11314, -BASEBAND_SIGNAL_AMPLITUDE, -11314,
	0, 11314, BASEBAND_SIGNAL_AMPLITUDE, 11314,
};

typedef struct baseband_state_t baseband_state_t;

struct baseband_state_t {
	dsp_device_t *interrupt;
	dsp_host_t host;
	pmb887x_rf_iq_source_t *iq_source;
	QEMUTimer *full_timer;
	uint16_t ram_base;
	uint16_t ram_size;
	uint16_t control;
	uint16_t interrupt_pointer;
	uint16_t write_pointer;
	uint16_t status;
	uint16_t dc_offset_i;
	uint16_t dc_offset_q;
	uint16_t frequency_shift;
	uint16_t filter_control;
	uint32_t base_power;
	uint32_t adjacent_power;
	uint16_t iq_imbalance;
	int64_t job_start_time;
	int64_t rx_start_time;
	int64_t full_time;
	uint64_t produced_words;
	uint64_t job_first_input_sample;
	uint16_t rate_divisor;
	uint16_t job_signal;
	bool job_active;
	bool rx_active;
	uint8_t startup_pointer_reads;
	pmb887x_iq_sample_t filter_sample;
	int16_t filter_history_i[BASEBAND_FILTER_MAX_TAPS];
	int16_t filter_history_q[BASEBAND_FILTER_MAX_TAPS];
	uint64_t filter_next_input_sample;
	uint8_t filter_position;
};

static void baseband_destroy(dsp_device_t *device) {
	baseband_state_t *state = device->state;

	timer_free(state->full_timer);
	g_free(device->state);
}

static void baseband_reset(dsp_device_t *device) {
	baseband_state_t *state = device->state;

	timer_del(state->full_timer);
	qatomic_set(&state->control, BASEBAND_CTRL_RESET);
	qatomic_set(&state->interrupt_pointer, 0);
	qatomic_set(&state->write_pointer, 0);
	qatomic_set(&state->status, 0);
	qatomic_set(&state->dc_offset_i, 0);
	qatomic_set(&state->dc_offset_q, 0);
	qatomic_set(&state->frequency_shift, 0);
	qatomic_set(&state->filter_control, 0);
	qatomic_set(&state->base_power, 0);
	qatomic_set(&state->adjacent_power, 0);
	qatomic_set(&state->iq_imbalance, 0);
	qatomic_set(&state->job_start_time, 0);
	state->rx_start_time = 0;
	qatomic_set(&state->full_time, BASEBAND_NO_DEADLINE);
	qatomic_set(&state->produced_words, 0);
	state->job_first_input_sample = 0;
	qatomic_set(&state->rate_divisor, 0);
	qatomic_set(&state->job_signal, 0);
	qatomic_set(&state->job_active, false);
	state->rx_active = false;
	qatomic_set(&state->startup_pointer_reads, 0);
	state->filter_sample = (pmb887x_iq_sample_t) { 0, 0 };
	memset(state->filter_history_i, 0, sizeof(state->filter_history_i));
	memset(state->filter_history_q, 0, sizeof(state->filter_history_q));
	state->filter_next_input_sample = 0;
	state->filter_position = 0;

	for (size_t i = 0; i < MIN((size_t) state->ram_size, (size_t) BASEBAND_RING_WORDS); i++)
		state->host.data_write(state->host.opaque, state->ram_base + i, 0);
}

static uint16_t baseband_get_output_clock_divisor(const baseband_state_t *state) {
	uint16_t control = qatomic_read(&state->filter_control);
	pmb887x_dsp_gsm_signal_t signal = qatomic_read(&state->job_signal);

	if (signal == PMB887X_DSP_GSM_SIGNAL_MONON)
		return BASEBAND_STANDARD_SAMPLE_CLOCK_DIVISOR;

	return (control & TEAK_BB_BRFILTER_CTRL_LENGTH) != 0 ?
		BASEBAND_NARROW_SAMPLE_CLOCK_DIVISOR : BASEBAND_STANDARD_SAMPLE_CLOCK_DIVISOR;
}

static uint16_t baseband_get_input_stride(const baseband_state_t *state) {
	return baseband_get_output_clock_divisor(state) / BASEBAND_NARROW_SAMPLE_CLOCK_DIVISOR * qatomic_read(&state->rate_divisor);
}

static uint64_t baseband_input_samples_before(const baseband_state_t *state, int64_t now) {
	int64_t elapsed = MAX(now - state->rx_start_time, 0);

	if (elapsed == 0)
		return 0;

	elapsed--;
	uint64_t sample_clocks = muldiv64_round_up((uint64_t) elapsed, BASEBAND_SAMPLE_CLOCK_HZ, NANOSECONDS_PER_SECOND);
	return DIV_ROUND_UP(sample_clocks, BASEBAND_NARROW_SAMPLE_CLOCK_DIVISOR);
}

static uint64_t baseband_words_at(const baseband_state_t *state, int64_t now) {
	int64_t elapsed = MAX(now - qatomic_read(&state->job_start_time), 0);
	uint16_t rate_divisor = qatomic_read(&state->rate_divisor);
	uint16_t sample_clock_divisor = baseband_get_output_clock_divisor(state);
	uint64_t phase_clocks = muldiv64((uint64_t) elapsed, BASEBAND_SAMPLE_CLOCK_HZ * BASEBAND_SAMPLE_PHASE_PARTS,
		NANOSECONDS_PER_SECOND);
	/* Hardware clock probes constrain the job phase between the quarter- and half-sample boundaries. */
	uint64_t samples = (phase_clocks + sample_clock_divisor * BASEBAND_JOB_PHASE_PARTS) /
		(sample_clock_divisor * BASEBAND_SAMPLE_PHASE_PARTS);

	return samples / rate_divisor * BASEBAND_WORDS_PER_SAMPLE;
}

static bool baseband_is_normal_burst_bit(size_t symbol) {
	if (symbol < BASEBAND_TRAINING_START)
		return false;

	size_t training_symbol = symbol - BASEBAND_TRAINING_START;
	if (training_symbol >= ARRAY_SIZE(BASEBAND_TRAINING_SEQUENCE) - 1)
		return false;

	return BASEBAND_TRAINING_SEQUENCE[training_symbol] == '1';
}

static size_t baseband_filter_get_half_length(const baseband_state_t *state) {
	uint16_t control = qatomic_read(&state->filter_control);
	size_t half_length = ((control & TEAK_BB_BRFILTER_CTRL_LENGTH) >> TEAK_BB_BRFILTER_CTRL_LENGTH_SHIFT);
	size_t maximum = (control & BASEBAND_FILTER_CTRL_BRCFG) ?
		BASEBAND_FILTER_MAX_HALF_LENGTH : BASEBAND_FILTER_NORMAL_HALF_LENGTH;

	return MIN(half_length, maximum);
}

static void baseband_filter_push(baseband_state_t *state, pmb887x_iq_sample_t sample) {
	state->filter_history_i[state->filter_position] = sample.i;
	state->filter_history_q[state->filter_position] = sample.q;
	state->filter_position = (state->filter_position + 1U) % BASEBAND_FILTER_MAX_TAPS;
}

static pmb887x_iq_sample_t baseband_filter_output(baseband_state_t *state, size_t half_length) {
	size_t taps = half_length * 2U + 1U;
	int64_t accumulator_i = 0;
	int64_t accumulator_q = 0;

	for (size_t tap = 0; tap < taps; tap++) {
		size_t history_index = (state->filter_position + BASEBAND_FILTER_MAX_TAPS - taps + tap) %
			BASEBAND_FILTER_MAX_TAPS;
		size_t coefficient_index = tap <= half_length ? tap : taps - tap - 1U;
		int16_t coefficient = state->host.data_read(state->host.opaque,
			state->ram_base + BASEBAND_RING_WORDS + coefficient_index);

		accumulator_i += (int32_t) state->filter_history_i[history_index] * coefficient;
		accumulator_q += (int32_t) state->filter_history_q[history_index] * coefficient;
	}

	uint16_t filter_control = qatomic_read(&state->filter_control);
	uint32_t scaling = ((filter_control & TEAK_BB_BRFILTER_CTRL_SCALING) >>
		TEAK_BB_BRFILTER_CTRL_SCALING_SHIFT);
	uint32_t shift = BASEBAND_FILTER_SCALING_BIAS + scaling;
	int64_t output_i = (accumulator_i >> shift);
	int64_t output_q = (accumulator_q >> shift);
	pmb887x_iq_sample_t sample = {
		.i = output_i < INT16_MIN ? INT16_MIN : MIN(output_i, INT16_MAX),
		.q = output_q < INT16_MIN ? INT16_MIN : MIN(output_q, INT16_MAX),
	};
	return sample;
}

static void baseband_filter_start(baseband_state_t *state, int64_t now) {
	state->rx_start_time = now;
	state->job_first_input_sample = 0;
	state->filter_sample = (pmb887x_iq_sample_t) { 0, 0 };
	memset(state->filter_history_i, 0, sizeof(state->filter_history_i));
	memset(state->filter_history_q, 0, sizeof(state->filter_history_q));
	state->filter_next_input_sample = 0;
	state->filter_position = 0;
	state->rx_active = true;
}

static void baseband_filter_stop(baseband_state_t *state) {
	state->rx_active = false;
}

static void baseband_filter_advance(baseband_state_t *state, uint64_t target_input_sample) {
	pmb887x_rf_iq_source_t *iq_source = qatomic_read(&state->iq_source);
	uint64_t history_samples = BASEBAND_FILTER_MAX_TAPS - 1U;
	uint64_t first_needed = target_input_sample > history_samples ? target_input_sample - history_samples : 0;

	if (state->filter_next_input_sample < first_needed) {
		memset(state->filter_history_i, 0, sizeof(state->filter_history_i));
		memset(state->filter_history_q, 0, sizeof(state->filter_history_q));
		state->filter_next_input_sample = first_needed;
		state->filter_position = 0;
	}

	while (state->filter_next_input_sample <= target_input_sample) {
		uint64_t input_sample = state->filter_next_input_sample;
		int64_t timestamp = state->rx_start_time + (int64_t) muldiv64_round_up(
			input_sample * BASEBAND_NARROW_SAMPLE_CLOCK_DIVISOR, NANOSECONDS_PER_SECOND,
			BASEBAND_SAMPLE_CLOCK_HZ);

		baseband_filter_push(state, pmb887x_rf_iq_read(iq_source, timestamp, input_sample));
		state->filter_next_input_sample++;
	}
}

static pmb887x_iq_sample_t baseband_filter_sample(baseband_state_t *state, uint64_t output_sample) {
	pmb887x_rf_iq_source_t *iq_source = qatomic_read(&state->iq_source);

	if (!state->rx_active || iq_source == NULL)
		return (pmb887x_iq_sample_t) { 0, 0 };

	size_t half_length = baseband_filter_get_half_length(state);
	uint16_t input_stride = baseband_get_input_stride(state);
	uint64_t target_input_sample = state->job_first_input_sample + output_sample * input_stride;

	baseband_filter_advance(state, target_input_sample);

	return baseband_filter_output(state, half_length);
}

/* Symbol-rate MSK approximation of GMSK: differential encoding followed by +/-pi/2 phase steps. */
static uint16_t baseband_signal_word(baseband_state_t *state, uint64_t word) {
	pmb887x_rf_iq_source_t *iq_source = qatomic_read(&state->iq_source);
	bool fcch = qatomic_read(&state->job_signal) == PMB887X_DSP_GSM_SIGNAL_FCON;

	if (iq_source != NULL) {
		uint64_t sample_index = word / BASEBAND_WORDS_PER_SAMPLE;

		if ((word % BASEBAND_WORDS_PER_SAMPLE) == 0)
			state->filter_sample = baseband_filter_sample(state, sample_index);

		return word % BASEBAND_WORDS_PER_SAMPLE ?
			(uint16_t) state->filter_sample.q : (uint16_t) state->filter_sample.i;
	}

	size_t symbol = word / BASEBAND_WORDS_PER_SAMPLE % BASEBAND_NORMAL_BURST_SYMBOLS;
	uint8_t phase = 0;
	bool previous_bit = false;

	for (size_t i = 0; i <= symbol; i++) {
		bool bit = !fcch && baseband_is_normal_burst_bit(i);
		uint8_t phase_delta = bit != previous_bit ? 3U : 1U;
		phase = (phase + phase_delta) % ARRAY_SIZE(BASEBAND_PHASE_I);
		previous_bit = bit;
	}

	int16_t sample = word % BASEBAND_WORDS_PER_SAMPLE ? BASEBAND_PHASE_Q[phase] : BASEBAND_PHASE_I[phase];
	return (uint16_t) sample;
}

static uint16_t baseband_publish_words(baseband_state_t *state, uint64_t words) {
	uint64_t produced = qatomic_read(&state->produced_words);
	if (!qatomic_read(&state->job_active))
		return produced % BASEBAND_RING_WORDS;

	for (uint64_t i = produced; i < words; i++) {
		uint16_t sample = baseband_signal_word(state, i);
		state->host.data_write(state->host.opaque, state->ram_base + i % BASEBAND_RING_WORDS, sample);
	}
	if (words > produced) {
		smp_wmb();
		qatomic_set(&state->produced_words, words);
		produced = words;
	}

	uint16_t pointer = produced % BASEBAND_RING_WORDS;
	qatomic_set(&state->write_pointer, pointer);
	return pointer;
}

static uint16_t baseband_update_pointer(baseband_state_t *state, int64_t now) {
	return baseband_publish_words(state, baseband_words_at(state, now));
}

static void baseband_schedule_interrupt(baseband_state_t *state, int64_t now) {
	int64_t deadline = BASEBAND_NO_DEADLINE;

	bool can_schedule = qatomic_read(&state->job_active) &&
		qatomic_read(&state->interrupt_pointer) < BASEBAND_RING_WORDS;
	if (can_schedule) {
		uint64_t words = baseband_words_at(state, now);
		uint16_t pointer = words % BASEBAND_RING_WORDS;
		uint16_t target = qatomic_read(&state->interrupt_pointer);
		uint16_t distance = target > pointer ? target - pointer : BASEBAND_RING_WORDS - pointer + target;
		uint16_t batch_words = BASEBAND_WORDS_PER_SAMPLE / qatomic_read(&state->rate_divisor);

		distance = ROUND_UP(distance, batch_words);
		uint64_t target_words = words + distance;
		uint64_t target_samples = target_words / batch_words;
		deadline = qatomic_read(&state->job_start_time) + muldiv64_round_up(
			target_samples * baseband_get_output_clock_divisor(state), NANOSECONDS_PER_SECOND,
			BASEBAND_SAMPLE_CLOCK_HZ);
	}
	qatomic_set(&state->full_time, deadline);

	if (deadline == BASEBAND_NO_DEADLINE) {
		timer_del(state->full_timer);
	} else {
		timer_mod(state->full_timer, deadline);
	}
}

static bool baseband_read(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t *value) {
	baseband_state_t *state = device->state;

	switch (offset) {
		case TEAK_BB_CTRL:
			*value = qatomic_read(&state->control);
			break;

		case TEAK_BB_INT_POINTER:
			*value = qatomic_read(&state->interrupt_pointer);
			break;

		case TEAK_BB_WR_POINTER: {
			int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

			if (qatomic_read(&state->job_active) && qatomic_read(&state->startup_pointer_reads) < 2) {
				uint8_t reads = qatomic_read(&state->startup_pointer_reads) + 1;
				uint16_t batch_words = BASEBAND_WORDS_PER_SAMPLE / qatomic_read(&state->rate_divisor);

				qatomic_set(&state->startup_pointer_reads, reads);
				*value = baseband_publish_words(state, reads * batch_words);
			} else if (qatomic_read(&state->job_active)) {
				*value = baseband_update_pointer(state, now);
			} else {
				*value = qatomic_read(&state->write_pointer);
			}
			break;
		}

		case TEAK_BB_STATUS:
			*value = qatomic_read(&state->status);
			break;

		case TEAK_BB_DCOFFSET_I:
			*value = qatomic_read(&state->dc_offset_i);
			break;

		case TEAK_BB_DCOFFSET_Q:
			*value = qatomic_read(&state->dc_offset_q);
			break;

		case TEAK_BB_FSHIFT:
			*value = qatomic_read(&state->frequency_shift);
			break;

		case TEAK_BB_BRFILTER_CTRL:
			*value = qatomic_read(&state->filter_control);
			break;

		case TEAK_BB_PBASE_MSB:
			*value = ((qatomic_read(&state->base_power) >> 16) & TEAK_BB_PBASE_MSB_VALUE);
			break;

		case TEAK_BB_PBASE_LSB:
			*value = qatomic_read(&state->base_power);
			break;

		case TEAK_BB_PADJ_MSB:
			*value = ((qatomic_read(&state->adjacent_power) >> 16) & TEAK_BB_PADJ_MSB_VALUE);
			break;

		case TEAK_BB_PADJ_LSB:
			*value = qatomic_read(&state->adjacent_power);
			break;

		case TEAK_BB_IQ_IMBALANCE:
			*value = qatomic_read(&state->iq_imbalance);
			break;

		default:
			*value = 0;
			break;
	}

	IO_DUMP_READ_EX(device->config->base + offset, sizeof(*value), *value, pc, 0);
	return true;
}

static bool baseband_write(dsp_device_t *device, uint16_t offset, uint32_t pc, uint16_t value) {
	baseband_state_t *state = device->state;

	switch (offset) {
		case TEAK_BB_CTRL:
			qatomic_set(&state->control, (value & BASEBAND_CTRL_MASK));
			break;

		case TEAK_BB_INT_POINTER: {
			int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

			qatomic_set(&state->interrupt_pointer, (value & TEAK_BB_INT_POINTER_VALUE));

			baseband_schedule_interrupt(state, now);
			break;
		}

		case TEAK_BB_DCOFFSET_I:
			qatomic_set(&state->dc_offset_i, value);
			break;

		case TEAK_BB_DCOFFSET_Q:
			qatomic_set(&state->dc_offset_q, value);
			break;

		case TEAK_BB_FSHIFT:
			qatomic_set(&state->frequency_shift, value);
			break;

		case TEAK_BB_BRFILTER_CTRL:
			qatomic_set(&state->filter_control, (value & BASEBAND_FILTER_CTRL_MASK));
			break;

		case TEAK_BB_IQ_IMBALANCE:
			qatomic_set(&state->iq_imbalance, value);
			break;
	}

	IO_DUMP_WRITE_EX(device->config->base + offset, sizeof(value), value, pc, 0);
	return true;
}

static const dsp_device_ops_t baseband_ops = {
	.destroy = baseband_destroy,
	.reset = baseband_reset,
	.read = baseband_read,
	.write = baseband_write,
};

static void baseband_full_timer(void *opaque) {
	baseband_state_t *state = opaque;
	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	int64_t deadline = qatomic_read(&state->full_time);

	if (deadline == BASEBAND_NO_DEADLINE)
		return;

	if (deadline > now) {
		timer_mod(state->full_timer, deadline);
		return;
	}

	if (!qatomic_read(&state->job_active))
		return;
	if (qatomic_read(&state->full_time) != deadline)
		return;
	qatomic_set(&state->full_time, BASEBAND_NO_DEADLINE);

	qatomic_set(&state->startup_pointer_reads, 2);
	uint16_t pointer = baseband_update_pointer(state, deadline);
	DPRINTF("BB_FULL: time=%" PRId64 " ns signal=%u elapsed=%" PRId64 " ns words=%" PRIu64
		" pointer=%u interrupt_pointer=%u\n",
		deadline, qatomic_read(&state->job_signal),
		deadline - qatomic_read(&state->job_start_time), qatomic_read(&state->produced_words), pointer,
		qatomic_read(&state->interrupt_pointer));
	dsp_int_set_flags(state->interrupt, BASEBAND_INTERRUPT_GROUP, TEAK_INT_FINTA0_BB_FULL);
	baseband_schedule_interrupt(state, deadline);
}

dsp_device_t *baseband_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt, const dsp_host_t *host) {
	baseband_state_t *state = g_new0(baseband_state_t, 1);
	state->interrupt = interrupt;
	state->host = *host;
	state->ram_base = config->ram_base;
	state->ram_size = config->ram_size;
	state->full_time = BASEBAND_NO_DEADLINE;
	state->full_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, baseband_full_timer, state);
	return dsp_device_create(config, &baseband_ops, state);
}

static uint16_t baseband_status_mask(pmb887x_dsp_gsm_signal_t signal) {
	switch (signal) {
		case PMB887X_DSP_GSM_SIGNAL_EQON:
			return TEAK_BB_STATUS_EQON;

		case PMB887X_DSP_GSM_SIGNAL_MONON:
			return TEAK_BB_STATUS_MONON;

		case PMB887X_DSP_GSM_SIGNAL_SCON:
			return TEAK_BB_STATUS_SCON;

		case PMB887X_DSP_GSM_SIGNAL_FCON:
			return TEAK_BB_STATUS_FCON;

		case PMB887X_DSP_GSM_SIGNAL_RXON:
			return TEAK_BB_STATUS_RXON;

		default:
			return 0;
	}
}

static void baseband_apply_signal(baseband_state_t *state, pmb887x_dsp_gsm_signal_t signal, bool level, int64_t now) {
	uint16_t mask = baseband_status_mask(signal);
	if (mask == 0)
		return;

	uint16_t old_status = qatomic_read(&state->status);
	bool old_level = (old_status & mask) != 0;
	bool interrupt_signal = signal != PMB887X_DSP_GSM_SIGNAL_RXON;

	if (signal == PMB887X_DSP_GSM_SIGNAL_RXON && level != old_level) {
		if (level && (qatomic_read(&state->control) & TEAK_BB_CTRL_BB_STOP) == 0) {
			baseband_filter_start(state, now);
		} else if (!level) {
			baseband_filter_stop(state);
		}
	}

	if (level) {
		if (interrupt_signal) {
			timer_del(state->full_timer);
			qatomic_set(&state->write_pointer, 0);
			qatomic_set(&state->full_time, BASEBAND_NO_DEADLINE);
			qatomic_set(&state->produced_words, 0);
			qatomic_set(&state->job_signal, signal);
			qatomic_set(&state->job_active, false);

			if ((qatomic_read(&state->control) & TEAK_BB_CTRL_BB_STOP) == 0 && state->rx_active) {
				bool decimation = (qatomic_read(&state->filter_control) & BASEBAND_FILTER_CTRL_BRCFG) != 0;
				uint16_t rate_divisor = decimation ? BASEBAND_DECIMATION_DIVISOR : 1;
				uint64_t first_input_sample = baseband_input_samples_before(state, now);

				qatomic_set(&state->rate_divisor, rate_divisor);
				qatomic_set(&state->job_start_time, now);
				uint16_t input_stride = baseband_get_input_stride(state);

				state->job_first_input_sample = ROUND_UP(first_input_sample, input_stride);
				qatomic_set(&state->job_active, true);
				qatomic_set(&state->startup_pointer_reads, 0);
			}

			baseband_schedule_interrupt(state, now);
		}

		qatomic_or(&state->status, mask);

		if (interrupt_signal)
			dsp_int_set_flags(state->interrupt, BASEBAND_INTERRUPT_GROUP, TEAK_INT_FINTA0_BBHI);
	} else {
		if (interrupt_signal) {
			bool job_active;

			job_active = qatomic_read(&state->job_active);
			if (job_active) {
				qatomic_set(&state->startup_pointer_reads, 2);
				baseband_update_pointer(state, now);
				qatomic_set(&state->job_active, false);
				qatomic_set(&state->full_time, BASEBAND_NO_DEADLINE);
			}
			if (job_active)
				timer_del(state->full_timer);
		}

		qatomic_and(&state->status, (uint16_t) ~mask);

		if (signal == PMB887X_DSP_GSM_SIGNAL_RXON)
			qatomic_and(&state->control, (uint16_t) ~TEAK_BB_CTRL_BB_STOP);

		if (interrupt_signal)
			dsp_int_set_flags(state->interrupt, BASEBAND_INTERRUPT_GROUP, TEAK_INT_FINTA0_BBLO);
	}
}

void baseband_set_signal(dsp_device_t *device, pmb887x_dsp_gsm_signal_t signal, bool level) {
	baseband_state_t *state = device->state;
	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

	baseband_apply_signal(state, signal, level, now);
}

void baseband_set_iq_source(dsp_device_t *device, pmb887x_rf_iq_source_t *source) {
	baseband_state_t *state = device->state;

	qatomic_set(&state->iq_source, source);
}
