#pragma once

#include <stdint.h>

struct pmb887x_dyn_timer_t;
typedef struct pmb887x_dyn_timer_t pmb887x_dyn_timer_t;
typedef void (*pmb887x_dyn_timer_callback_t)(void *irq_callback_data, int irq_id);

pmb887x_dyn_timer_t *pmb887x_dyn_timer_new(int irqs_n, pmb887x_dyn_timer_callback_t irq_callback, void *irq_callback_data);
void pmb887x_dyn_timer_start(pmb887x_dyn_timer_t *p);
void pmb887x_dyn_timer_stop(pmb887x_dyn_timer_t *p);
void pmb887x_dyn_timer_reset(pmb887x_dyn_timer_t *p);

void pmb887x_dyn_timer_set_freq(pmb887x_dyn_timer_t *p, uint32_t freq);
uint32_t pmb887x_dyn_timer_get_freq(pmb887x_dyn_timer_t *p);

void pmb887x_dyn_timer_set_overflow(pmb887x_dyn_timer_t *p, uint32_t overflow);
uint32_t pmb887x_dyn_timer_get_overflow(pmb887x_dyn_timer_t *p);

void pmb887x_dyn_timer_irq_set_threshold(pmb887x_dyn_timer_t *p, int irq_id, uint32_t value);
uint32_t pmb887x_dyn_timer_irq_get_threshold(pmb887x_dyn_timer_t *p, int irq_id);

uint64_t pmb887x_dyn_timer_get_counter(pmb887x_dyn_timer_t *p);
uint64_t pmb887x_dyn_timer_get_raw_counter(pmb887x_dyn_timer_t *p);
uint64_t pmb887x_dyn_timer_get_next_time(pmb887x_dyn_timer_t *p);
void pmb887x_dyn_timer_run(pmb887x_dyn_timer_t *p);

void pmb887x_dyn_timer_get_next_checkpoint(pmb887x_dyn_timer_t *p, uint64_t *next_timestamp, uint32_t *next_counter);
void pmb887x_dyn_timer_run_irqs(pmb887x_dyn_timer_t *p);
