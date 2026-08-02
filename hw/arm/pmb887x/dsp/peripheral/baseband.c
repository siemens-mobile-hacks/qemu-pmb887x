#define PMB887X_TRACE_ID		DSP_BASEBAND
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp-baseband"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_DSP

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bitops.h"
#include "qemu/thread.h"
#include "qemu/timer.h"

#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/trace.h"

#define BASEBAND_CTRL_MASK		(TEAK_BB_CTRL_BB_STOP | TEAK_BB_CTRL_CORDICON | TEAK_BB_CTRL_BB_ON | \
	TEAK_BB_CTRL_BB_ADCMODE | TEAK_BB_CTRL_BBADAP_EN)
#define BASEBAND_FILTER_CTRL_MASK	(TEAK_BB_BRFILTER_CTRL_LENGTH | TEAK_BB_BRFILTER_CTRL_SCALING | \
	TEAK_BB_BRFILTER_CTRL_DECIMATION)
#define BASEBAND_RING_WORDS		0x03C0U
#define BASEBAND_WORD_RATE_NUMERATOR	13U
#define BASEBAND_WORD_RATE_DENOMINATOR_NS	12000U
#define BASEBAND_DECIMATION_DIVISOR	2U
#define BASEBAND_INTERRUPT_GROUP	0U
#define BASEBAND_CTRL_RESET		0x0110U

typedef struct baseband_state_t baseband_state_t;
typedef struct baseband_event_t baseband_event_t;
typedef QTAILQ_HEAD(baseband_event_queue_t, baseband_event_t) baseband_event_queue_t;

struct baseband_event_t {
	pmb887x_dsp_gsm_signal_t signal;
	int64_t time;
	bool level;
	QTAILQ_ENTRY(baseband_event_t) entry;
};

struct baseband_state_t {
	dsp_device_t *interrupt;
	dsp_host_t host;
	QemuMutex *event_mutex;
	baseband_event_queue_t events;
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
	int64_t current_time;
	int64_t full_time;
	uint64_t produced_words;
	uint32_t queued_events;
	uint16_t rate_divisor;
	bool full_scheduled;
	bool job_active;
};

static void baseband_clear_events(baseband_state_t *state) {
	baseband_event_t *event;

	while ((event = QTAILQ_FIRST(&state->events)) != NULL) {
		QTAILQ_REMOVE(&state->events, event, entry);
		g_free(event);
	}
	qatomic_set(&state->queued_events, 0);
}

static void baseband_destroy(dsp_device_t *device) {
	baseband_state_t *state = device->state;

	qemu_mutex_lock(state->event_mutex);
	baseband_clear_events(state);
	qemu_mutex_unlock(state->event_mutex);
	qemu_mutex_destroy(state->event_mutex);
	g_free(state->event_mutex);
	g_free(device->state);
}

static void baseband_reset(dsp_device_t *device) {
	baseband_state_t *state = device->state;
	dsp_device_t *interrupt = state->interrupt;
	dsp_host_t host = state->host;
	QemuMutex *event_mutex = state->event_mutex;
	uint16_t ram_base = state->ram_base;
	uint16_t ram_size = state->ram_size;

	qemu_mutex_lock(event_mutex);
	baseband_clear_events(state);
	memset(state, 0, sizeof(*state));
	state->interrupt = interrupt;
	state->host = host;
	state->event_mutex = event_mutex;
	QTAILQ_INIT(&state->events);
	state->ram_base = ram_base;
	state->ram_size = ram_size;
	state->control = BASEBAND_CTRL_RESET;
	qemu_mutex_unlock(event_mutex);

	for (size_t i = 0; i < MIN((size_t) ram_size, (size_t) BASEBAND_RING_WORDS); i++)
		state->host.data_write(state->host.opaque, ram_base + i, 0);
}

static uint64_t baseband_words_at(const baseband_state_t *state, int64_t now) {
	int64_t elapsed = MAX(now - qatomic_read(&state->job_start_time), 0);
	uint32_t denominator = BASEBAND_WORD_RATE_DENOMINATOR_NS * qatomic_read(&state->rate_divisor);
	uint64_t periods = (uint64_t) elapsed / denominator;
	uint64_t remainder = (uint64_t) elapsed % denominator;
	return periods * BASEBAND_WORD_RATE_NUMERATOR +
		(remainder * BASEBAND_WORD_RATE_NUMERATOR + denominator / 2) / denominator;
}

static uint16_t baseband_update_pointer(baseband_state_t *state, int64_t now) {
	uint64_t words = baseband_words_at(state, now);
	uint64_t produced = qatomic_read(&state->produced_words);

	while (words > produced) {
		uint64_t actual = qatomic_cmpxchg(&state->produced_words, produced, words);
		if (actual == produced) {
			for (uint64_t i = produced; i < words; i++)
				state->host.data_write(state->host.opaque, state->ram_base + i % BASEBAND_RING_WORDS, 0);
			break;
		}
		produced = actual;
	}

	uint16_t pointer = qatomic_read(&state->produced_words) % BASEBAND_RING_WORDS;
	qatomic_set(&state->write_pointer, pointer);
	return pointer;
}

static void baseband_schedule_interrupt(baseband_state_t *state, int64_t now) {
	if (!state->job_active || state->interrupt_pointer >= BASEBAND_RING_WORDS) {
		state->full_scheduled = false;
		return;
	}

	uint64_t words = baseband_words_at(state, now);
	uint16_t pointer = words % BASEBAND_RING_WORDS;
	uint16_t target = state->interrupt_pointer;
	uint16_t distance = target > pointer ? target - pointer : BASEBAND_RING_WORDS - pointer + target;
	uint64_t target_words = words + distance;
	uint64_t numerator = target_words * BASEBAND_WORD_RATE_DENOMINATOR_NS * state->rate_divisor;

	state->full_time = state->job_start_time + (numerator + BASEBAND_WORD_RATE_NUMERATOR - 1) /
		BASEBAND_WORD_RATE_NUMERATOR;
	state->full_scheduled = true;
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

		case TEAK_BB_WR_POINTER:
			if (qatomic_read(&state->job_active)) {
				*value = baseband_update_pointer(state, state->current_time);
			} else {
				*value = qatomic_read(&state->write_pointer);
			}
			break;

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
			*value = qatomic_read(&state->base_power) >> 16 & TEAK_BB_PBASE_MSB_VALUE;
			break;

		case TEAK_BB_PBASE_LSB:
			*value = qatomic_read(&state->base_power);
			break;

		case TEAK_BB_PADJ_MSB:
			*value = qatomic_read(&state->adjacent_power) >> 16 & TEAK_BB_PADJ_MSB_VALUE;
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
			qatomic_set(&state->control, value & BASEBAND_CTRL_MASK);
			break;

		case TEAK_BB_INT_POINTER:
			qatomic_set(&state->interrupt_pointer, value & TEAK_BB_INT_POINTER_VALUE);
			baseband_schedule_interrupt(state, state->current_time);
			break;

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
			qatomic_set(&state->filter_control, value & BASEBAND_FILTER_CTRL_MASK);
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

dsp_device_t *baseband_create(const pmb887x_dsp_peripheral_config_t *config, dsp_device_t *interrupt, const dsp_host_t *host) {
	baseband_state_t *state = g_new0(baseband_state_t, 1);
	state->interrupt = interrupt;
	state->host = *host;
	state->ram_base = config->ram_base;
	state->ram_size = config->ram_size;
	state->event_mutex = g_new(QemuMutex, 1);
	qemu_mutex_init(state->event_mutex);
	QTAILQ_INIT(&state->events);
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

static void baseband_apply_signal(baseband_state_t *state, const baseband_event_t *event) {
	pmb887x_dsp_gsm_signal_t signal = event->signal;
	bool level = event->level;
	uint16_t mask = baseband_status_mask(signal);
	uint16_t old_status = qatomic_read(&state->status);
	bool interrupt_signal = signal != PMB887X_DSP_GSM_SIGNAL_RXON;
	int64_t now = event->time;

	if (mask == 0)
		return;

	state->current_time = now;
	DPRINTF("signal: time=%" PRId64 " ns signal=%u level=%u status=%04X pointer=%u interrupt_pointer=%u\n",
		now, signal, level, old_status, qatomic_read(&state->write_pointer), qatomic_read(&state->interrupt_pointer));

	if (level) {
		if (interrupt_signal) {
			qatomic_set(&state->write_pointer, 0);
			qatomic_set(&state->produced_words, 0);

			if ((qatomic_read(&state->control) & TEAK_BB_CTRL_BB_STOP) == 0) {
				bool decimation = qatomic_read(&state->filter_control) & TEAK_BB_BRFILTER_CTRL_DECIMATION;
				uint16_t rate_divisor = decimation ? BASEBAND_DECIMATION_DIVISOR : 1;

				qatomic_set(&state->rate_divisor, rate_divisor);
				qatomic_set(&state->job_start_time, now);
				qatomic_set(&state->job_active, true);
				baseband_schedule_interrupt(state, now);
			}
		}

		qatomic_or(&state->status, mask);

		if (interrupt_signal)
			dsp_int_set_flags(state->interrupt, BASEBAND_INTERRUPT_GROUP, TEAK_INT_FINTA0_BBHI);
	} else {
		if (interrupt_signal && qatomic_read(&state->job_active)) {
			baseband_update_pointer(state, now);
			qatomic_set(&state->job_active, false);
			state->full_scheduled = false;
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
	baseband_event_t *event = g_new(baseband_event_t, 1);
	event->signal = signal;
	event->level = level;
	event->time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

	qemu_mutex_lock(state->event_mutex);
	QTAILQ_INSERT_TAIL(&state->events, event, entry);
	qatomic_inc(&state->queued_events);
	qemu_mutex_unlock(state->event_mutex);
}

void baseband_advance(dsp_device_t *device, size_t cycles) {
	baseband_state_t *state = device->state;
	baseband_event_t *event = NULL;
	uint16_t flags;
	int64_t now;
	bool deliver_full;
	bool deliver_queued;

	(void) cycles;

	if (qatomic_read(&state->queued_events) == 0 && !state->full_scheduled)
		return;

	flags = dsp_int_get_flags(state->interrupt, BASEBAND_INTERRUPT_GROUP);
	if ((flags & (TEAK_INT_FINTA0_BBHI | TEAK_INT_FINTA0_BBLO | TEAK_INT_FINTA0_BB_FULL)) != 0)
		return;

	now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

	qemu_mutex_lock(state->event_mutex);
	event = QTAILQ_FIRST(&state->events);

	deliver_full = state->full_scheduled && state->full_time <= now &&
		(event == NULL || state->full_time <= event->time);
	deliver_queued = !deliver_full && event != NULL && event->time <= now;
	if (deliver_queued) {
		QTAILQ_REMOVE(&state->events, event, entry);
		qatomic_dec(&state->queued_events);
	} else {
		event = NULL;
	}
	qemu_mutex_unlock(state->event_mutex);

	if (deliver_full) {
		state->current_time = state->full_time;
		baseband_update_pointer(state, state->current_time);
		state->full_scheduled = false;
		dsp_int_set_flags(state->interrupt, BASEBAND_INTERRUPT_GROUP, TEAK_INT_FINTA0_BB_FULL);
	} else if (event != NULL) {
		baseband_apply_signal(state, event);
		g_free(event);
	}
}

bool baseband_is_active(const dsp_device_t *device) {
	const baseband_state_t *state = device->state;
	return qatomic_read(&state->queued_events) != 0 || state->full_scheduled;
}
