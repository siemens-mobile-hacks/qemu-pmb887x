/*
 * Generic Timers
 * */
#define PMB887X_TRACE_ID		TIMER
#define PMB887X_TRACE_PREFIX	"pmb887x-timer"

#include "qemu/osdep.h"
#include "hw/arm/pmb887x/timer.h"
#include "hw/arm/pmb887x/trace.h"
#include "qemu/timer.h"

typedef struct {
	bool enabled;
	bool fired;
	uint32_t threshold;
} pmb887x_timer_irq_t;

struct pmb887x_timer_t {
	QEMUTimer *qemu_timer;
	bool is_running;
	uint32_t freq;
	uint32_t counter;
	uint64_t start;
	uint32_t overflow;
	int irqs_n;
	pmb887x_timer_irq_t *irqs;
	pmb887x_timer_callback_t irq_callback;
	void *irq_callback_data;
};

static void pmb887x_timer_reset_handler(void *opaque) {
	pmb887x_timer_t *p = opaque;
	pmb887x_timer_run(p);
}

static uint64_t pmb887x_timer_ticks_to_ns(pmb887x_timer_t *p, uint64_t ticks) {
	return muldiv64(ticks, NANOSECONDS_PER_SECOND, p->freq);
}

pmb887x_timer_t *pmb887x_timer_new(int irqs_n, pmb887x_timer_callback_t irq_callback, void *irq_callback_data) {
	pmb887x_timer_t *p = g_new0(pmb887x_timer_t, 1);
	p->qemu_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pmb887x_timer_reset_handler, p);
	p->irqs_n = irqs_n;
	p->irqs = g_new0(pmb887x_timer_irq_t, irqs_n);
	p->irq_callback = irq_callback;
	p->irq_callback_data = irq_callback_data;
	return p;
}

uint64_t pmb887x_timer_get_raw_counter(pmb887x_timer_t *p) {
	uint64_t counter = p->counter;
	if (p->is_running) {
		uint64_t delta_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - p->start;
		counter += muldiv64(delta_ns, p->freq, NANOSECONDS_PER_SECOND);
	}
	return counter;
}

uint64_t pmb887x_timer_get_counter(pmb887x_timer_t *p) {
	return (pmb887x_timer_get_raw_counter(p) % (p->overflow + 1));
}

void pmb887x_timer_irq_set_threshold(pmb887x_timer_t *p, int irq_id, uint32_t value) {
	g_assert(irq_id >= 0 && irq_id < p->irqs_n);
	int64_t counter = pmb887x_timer_get_counter(p);
	if (counter >= value)
		p->irqs[irq_id].fired = true;
	p->irqs[irq_id].threshold = value;
}

uint32_t pmb887x_timer_irq_get_threshold(pmb887x_timer_t *p, int irq_id) {
	g_assert(irq_id >= 0 && irq_id < p->irqs_n);
	return p->irqs[irq_id].threshold;
}

void pmb887x_timer_start(pmb887x_timer_t *p) {
	if (p->is_running)
		return;
	p->start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	p->is_running = true;
	pmb887x_timer_run(p);
}

void pmb887x_timer_stop(pmb887x_timer_t *p) {
	if (!p->is_running)
		return;
	p->counter = pmb887x_timer_get_counter(p);
	p->start = 0;
	p->is_running = false;
	timer_del(p->qemu_timer);
}

void pmb887x_timer_reset(pmb887x_timer_t *p) {
	bool do_restart = p->is_running;

	pmb887x_timer_stop(p);
	p->start = 0;
	p->counter = 0;

	for (int i = 0; i < p->irqs_n; i++)
		p->irqs[i].fired = false;

	if (do_restart)
		pmb887x_timer_start(p);
}

void pmb887x_timer_set_freq(pmb887x_timer_t *p, uint32_t freq) {
	if (p->freq == freq)
		return;

	bool do_restart = p->is_running;
	pmb887x_timer_stop(p);
	p->freq = freq / 10;
	if (do_restart)
		pmb887x_timer_start(p);
}

uint32_t pmb887x_timer_get_freq(pmb887x_timer_t *p) {
	return p->freq;
}

void pmb887x_timer_set_overflow(pmb887x_timer_t *p, uint32_t overflow) {
	p->overflow = overflow;
	pmb887x_timer_run(p);
}

uint32_t pmb887x_timer_get_overflow(pmb887x_timer_t *p) {
	return p->overflow;
}

uint64_t pmb887x_timer_get_next_time(pmb887x_timer_t *p) {
	uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	uint64_t overflow = p->overflow + 1;
	uint64_t counter = MIN(overflow, pmb887x_timer_get_raw_counter(p));
	uint64_t next = now + pmb887x_timer_ticks_to_ns(p, overflow - counter);

	for (int i = 0; i < p->irqs_n; i++) {
		uint64_t threshold = p->irqs[i].threshold;
		if (!p->irqs[i].fired)
			next = MIN(next, now + pmb887x_timer_ticks_to_ns(p, counter >= threshold ? 0 : threshold - counter));
	}

	return next;
}

void pmb887x_timer_run(pmb887x_timer_t *p) {
	uint64_t overflow = p->overflow + 1;
	uint64_t counter = MIN(overflow, pmb887x_timer_get_raw_counter(p));

	if (!p->is_running)
		return;

	for (int i = 0; i < p->irqs_n; i++) {
		uint64_t threshold = p->irqs[i].threshold;
		if (counter >= threshold) {
			if (!p->irqs[i].fired) {
				p->irqs[i].fired = true;
				p->irq_callback(p->irq_callback_data, i);
			}
		}
	}

	if (counter >= overflow) {
		p->counter = pmb887x_timer_get_counter(p);
		p->start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
		p->irq_callback(p->irq_callback_data, -1);
		for (int i = 0; i < p->irqs_n; i++)
			p->irqs[i].fired = false;
	}

	timer_mod(p->qemu_timer, pmb887x_timer_get_next_time(p));
}
