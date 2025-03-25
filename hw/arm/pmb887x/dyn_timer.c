/*
 * Generic Timers
 * */
#include <stdint.h>
#define PMB887X_TRACE_ID		TIMER
#define PMB887X_TRACE_PREFIX	"pmb887x-timer"

#include "qemu/osdep.h"
#include "hw/arm/pmb887x/dyn_timer.h"
#include "hw/arm/pmb887x/trace.h"
#include "qemu/timer.h"

typedef struct {
	bool enabled;
	bool fired;
	uint32_t threshold;
} pmb887x_dyn_timer_irq_t;

struct pmb887x_dyn_timer_t {
	QEMUTimer *qemu_timer;
	bool is_running;
	bool is_suspended;
	uint32_t freq;
	uint32_t counter;
	uint64_t start;
	uint32_t overflow;
	int irqs_n;
	pmb887x_dyn_timer_irq_t *irqs;
	pmb887x_dyn_timer_callback_t irq_callback;
	void *irq_callback_data;

	uint32_t next_counter;
	uint64_t next_timestamp;
};

static void pmb887x_dyn_timer_reset_handler(void *opaque) {
	pmb887x_dyn_timer_t *p = opaque;
	pmb887x_dyn_timer_run(p);
}

static uint64_t pmb887x_dyn_timer_ticks_to_ns(pmb887x_dyn_timer_t *p, uint64_t ticks) {
	return muldiv64(ticks, NANOSECONDS_PER_SECOND, p->freq);
}

pmb887x_dyn_timer_t *pmb887x_dyn_timer_new(int irqs_n, pmb887x_dyn_timer_callback_t irq_callback, void *irq_callback_data) {
	pmb887x_dyn_timer_t *p = g_new0(pmb887x_dyn_timer_t, 1);
	p->qemu_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pmb887x_dyn_timer_reset_handler, p);
	p->irqs_n = irqs_n;
	p->irqs = g_new0(pmb887x_dyn_timer_irq_t, irqs_n);
	p->irq_callback = irq_callback;
	p->irq_callback_data = irq_callback_data;
	return p;
}

uint64_t pmb887x_dyn_timer_get_raw_counter(pmb887x_dyn_timer_t *p) {
	uint64_t counter = p->counter;
	if (p->is_running) {
		uint64_t delta_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - p->start;
		counter += muldiv64(delta_ns, p->freq, NANOSECONDS_PER_SECOND);
	}
	return MIN(p->next_counter, counter);
}

uint64_t pmb887x_dyn_timer_get_counter(pmb887x_dyn_timer_t *p) {
	return (pmb887x_dyn_timer_get_raw_counter(p) % (p->overflow + 1));
}

void pmb887x_dyn_timer_irq_set_threshold(pmb887x_dyn_timer_t *p, int irq_id, uint32_t value) {
	g_assert(irq_id >= 0 && irq_id < p->irqs_n);
	int64_t counter = pmb887x_dyn_timer_get_counter(p);
	if (counter > value)
		p->irqs[irq_id].fired = true;
	p->irqs[irq_id].threshold = value;
	pmb887x_dyn_timer_run(p);
}

uint32_t pmb887x_dyn_timer_irq_get_threshold(pmb887x_dyn_timer_t *p, int irq_id) {
	g_assert(irq_id >= 0 && irq_id < p->irqs_n);
	return p->irqs[irq_id].threshold;
}

void pmb887x_dyn_timer_start(pmb887x_dyn_timer_t *p) {
	if (p->is_running)
		return;
	DPRINTF("pmb887x_dyn_timer_start\n");
	p->start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	p->is_running = true;
	pmb887x_dyn_timer_run(p);
}

void pmb887x_dyn_timer_stop(pmb887x_dyn_timer_t *p) {
	if (!p->is_running)
		return;
	DPRINTF("pmb887x_dyn_timer_stop\n");
	p->counter = pmb887x_dyn_timer_get_counter(p);
	p->start = 0;
	p->is_running = false;
	timer_del(p->qemu_timer);
}

void pmb887x_dyn_timer_reset(pmb887x_dyn_timer_t *p) {
	p->start = 0;
	p->counter = 0;
	p->next_counter = 0;
	p->next_timestamp = 0;
	for (int i = 0; i < p->irqs_n; i++)
		p->irqs[i].fired = false;
	pmb887x_dyn_timer_run(p);
}

void pmb887x_dyn_timer_set_freq(pmb887x_dyn_timer_t *p, uint32_t freq) {
	if (p->freq == freq)
		return;

	if (p->is_running) {
		p->counter = pmb887x_dyn_timer_get_counter(p);
		p->start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	}

	p->freq = freq;
	pmb887x_dyn_timer_run(p);
}

uint32_t pmb887x_dyn_timer_get_freq(pmb887x_dyn_timer_t *p) {
	return p->freq;
}

void pmb887x_dyn_timer_set_overflow(pmb887x_dyn_timer_t *p, uint32_t overflow) {
	if (overflow == p->overflow)
		return;
	p->overflow = overflow;
	p->next_counter = MIN(p->overflow, p->next_counter);
	pmb887x_dyn_timer_run(p);
}

uint32_t pmb887x_dyn_timer_get_overflow(pmb887x_dyn_timer_t *p) {
	return p->overflow;
}

void pmb887x_dyn_timer_get_next_checkpoint(pmb887x_dyn_timer_t *p, uint64_t *next_timestamp, uint32_t *next_counter) {
	uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	uint64_t overflow = p->overflow + 1;
	uint64_t counter = MIN(overflow, pmb887x_dyn_timer_get_raw_counter(p));

	*next_timestamp = now + pmb887x_dyn_timer_ticks_to_ns(p, overflow - counter);
	*next_counter = overflow;

	for (int i = 0; i < p->irqs_n; i++) {
		uint64_t irq_threshold = p->irqs[i].threshold;
		if (!p->irqs[i].fired && irq_threshold <= overflow) {
			DPRINTF(" ..... irq_threshold=%ld, counter=%ld\n", irq_threshold, counter);
			uint64_t remaining = irq_threshold >= counter ? irq_threshold - counter : 0;
			uint64_t irq_timestamp = now + pmb887x_dyn_timer_ticks_to_ns(p, remaining);
			if (*next_timestamp > irq_timestamp) {
				*next_counter = irq_threshold;
				*next_timestamp = irq_timestamp;
			}
		}
	}
}

void pmb887x_dyn_timer_run_irqs(pmb887x_dyn_timer_t *p) {
	uint64_t overflow = p->overflow + 1;
	uint64_t counter = pmb887x_dyn_timer_get_raw_counter(p);

	for (int i = 0; i < p->irqs_n; i++) {
		uint64_t threshold = p->irqs[i].threshold;
		if (counter >= threshold || (counter % overflow) >= threshold) {
			if (!p->irqs[i].fired) {
				DPRINTF("IRQ%d call\n", i);
				p->irqs[i].fired = true;
				p->irq_callback(p->irq_callback_data, i);
			}
		}
	}
}

void pmb887x_dyn_timer_run(pmb887x_dyn_timer_t *p) {
	uint64_t overflow = p->overflow + 1;
	uint64_t counter = pmb887x_dyn_timer_get_raw_counter(p);
	uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

	if (!p->is_running)
		return;

	DPRINTF("RUN: %ld\n", counter);

	pmb887x_dyn_timer_run_irqs(p);

	if (counter >= overflow) {
		p->counter = pmb887x_dyn_timer_get_counter(p);
		p->start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
		p->irq_callback(p->irq_callback_data, -1);
		for (int i = 0; i < p->irqs_n; i++)
			p->irqs[i].fired = false;
	}

	pmb887x_dyn_timer_run_irqs(p);

	pmb887x_dyn_timer_get_next_checkpoint(p, &p->next_timestamp, &p->next_counter);
	timer_mod(p->qemu_timer, p->next_timestamp);

	DPRINTF(">>> next_timestamp=+%ld, next_counter=%d, overflow=%ld, counter=%ld\n", p->next_timestamp - now, p->next_counter, overflow, pmb887x_dyn_timer_get_raw_counter(p));
}
