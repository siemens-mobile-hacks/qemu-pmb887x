#pragma once
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "system/cpu-timers.h"

/*
 * Clock for "completes now" device timers (dmac bursts, dif/ssc word
 * transfers).  QEMU_CLOCK_VIRTUAL under stock icount: the vCPU thread runs
 * those timers itself at the next TB boundary or in its idle warp, so a
 * completion never waits for a main-loop round trip and is deterministic.
 * The opt-in icount2 (precise-clocks) model runs a due virtual timer
 * synchronously inside timer_mod (timerlist_rearm -> icount2_sync), i.e.
 * re-entrantly from the device callback that armed it — keep the realtime
 * clock there, as before.
 */
static inline QEMUClockType pmb887x_completion_clock(void)
{
	return icount2_enabled() ? QEMU_CLOCK_REALTIME : QEMU_CLOCK_VIRTUAL;
}

typedef struct pmb887x_clc_reg_t pmb887x_clc_reg_t;
typedef struct pmb887x_src_reg_t pmb887x_src_reg_t;
typedef struct pmb887x_srb_reg_t pmb887x_srb_reg_t;
typedef struct pmb887x_srb_ext_reg_t pmb887x_srb_ext_reg_t;

struct pmb887x_clc_reg_t {
	uint32_t value;
};

struct pmb887x_src_reg_t {
	uint32_t value;
	qemu_irq irq;
	bool last_irq_state;
};

struct pmb887x_srb_reg_t {
	qemu_irq *irq;
	int irq_n;
	bool *last_irq_state;
	uint32_t *irq_events;
	
	uint32_t last_state;
	uint32_t imsc;
	uint32_t ris;
	uint32_t dmae;
	
	int (*irq_router)(void *, int);
	void *irq_router_opaque;

	void (*event_handler)(void *, int, int);
	void *event_handler_opaque;
};

struct pmb887x_srb_ext_reg_t {
	pmb887x_srb_reg_t *parent;
	uint32_t events;
	uint32_t imsc;
	uint32_t ris;
};

// Clock Control Register
void pmb887x_clc_init(pmb887x_clc_reg_t *reg);
uint8_t pmb887x_clc_get_rmc(pmb887x_clc_reg_t *reg);
uint8_t pmb887x_clc_is_enabled(pmb887x_clc_reg_t *reg);
uint32_t pmb887x_clc_get(pmb887x_clc_reg_t *reg);
void pmb887x_clc_set(pmb887x_clc_reg_t *reg, uint32_t value);

// Service Routing Config
void pmb887x_src_init(pmb887x_src_reg_t *reg, qemu_irq irq);
void pmb887x_src_reset(pmb887x_src_reg_t *reg);
uint32_t pmb887x_src_get(pmb887x_src_reg_t *reg);
void pmb887x_src_set(pmb887x_src_reg_t *reg, uint32_t value);
void pmb887x_src_update(pmb887x_src_reg_t *reg, uint32_t clear, uint32_t set);

// Service Request Block
void pmb887x_srb_init(pmb887x_srb_reg_t *reg, qemu_irq *irq, int irq_n);
void pmb887x_srb_reset(pmb887x_srb_reg_t *reg);
void pmb887x_srb_set_irq_router(pmb887x_srb_reg_t *reg, void *opaque, int (*callback)(void *, int));
void pmb887x_srb_set_event_handler(pmb887x_srb_reg_t *reg, void *opaque, void (*callback)(void *, int, int)) ;

uint32_t pmb887x_srb_get_imsc(pmb887x_srb_reg_t *reg);
uint32_t pmb887x_srb_get_mis(pmb887x_srb_reg_t *reg);
uint32_t pmb887x_srb_get_ris(pmb887x_srb_reg_t *reg);
uint32_t pmb887x_srb_get_dmae(pmb887x_srb_reg_t *reg);
uint32_t pmb887x_srb_get_ris_dma(pmb887x_srb_reg_t *reg);

void pmb887x_srb_set_imsc(pmb887x_srb_reg_t *reg, uint32_t value);
void pmb887x_srb_set_icr(pmb887x_srb_reg_t *reg, uint32_t value);
void pmb887x_srb_set_isr(pmb887x_srb_reg_t *reg, uint32_t value);
void pmb887x_srb_set_dmae(pmb887x_srb_reg_t *reg, uint32_t value);

// Service Request Block Extended
void pmb887x_srb_ext_init(pmb887x_srb_ext_reg_t *reg, pmb887x_srb_reg_t *parent, uint32_t events);
void pmb887x_srb_ext_reset(pmb887x_srb_ext_reg_t *reg);

uint32_t pmb887x_srb_ext_get_imsc(pmb887x_srb_ext_reg_t *reg);
uint32_t pmb887x_srb_ext_get_mis(pmb887x_srb_ext_reg_t *reg);
uint32_t pmb887x_srb_ext_get_ris(pmb887x_srb_ext_reg_t *reg);

void pmb887x_srb_ext_set_imsc(pmb887x_srb_ext_reg_t *reg, uint32_t value);
void pmb887x_srb_ext_set_icr(pmb887x_srb_ext_reg_t *reg, uint32_t value);
void pmb887x_srb_ext_set_isr(pmb887x_srb_ext_reg_t *reg, uint32_t value);
