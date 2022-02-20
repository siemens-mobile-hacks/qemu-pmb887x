#pragma once
#include "qemu/osdep.h"
#include "hw/irq.h"

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
	
	int (*irq_router)(void *, int);
	void *irq_router_opaque;
};

// Clock Control Register
void pmb887x_clc_init(struct pmb887x_clc_reg_t *reg);
uint8_t pmb887x_clc_get_rmc(struct pmb887x_clc_reg_t *reg);
uint8_t pmb887x_clc_is_enabled(struct pmb887x_clc_reg_t *reg);
uint32_t pmb887x_clc_get(struct pmb887x_clc_reg_t *reg);
void pmb887x_clc_set(struct pmb887x_clc_reg_t *reg, uint32_t value);

// Service Routing Config
void pmb887x_src_init(struct pmb887x_src_reg_t *reg, qemu_irq irq);
uint32_t pmb887x_src_get(struct pmb887x_src_reg_t *reg);
void pmb887x_src_set(struct pmb887x_src_reg_t *reg, uint32_t value);
void pmb887x_src_update(struct pmb887x_src_reg_t *reg, uint32_t clear, uint32_t set);

// Service Request Block
void pmb887x_srb_init(struct pmb887x_srb_reg_t *reg, qemu_irq *irq, int irq_n);
void pmb887x_srb_set_irq_router(struct pmb887x_srb_reg_t *reg, void *opaque, int (*callback)(void *, int));

uint32_t pmb887x_srb_get_imsc(struct pmb887x_srb_reg_t *reg);
uint32_t pmb887x_srb_get_mis(struct pmb887x_srb_reg_t *reg);
uint32_t pmb887x_srb_get_ris(struct pmb887x_srb_reg_t *reg);
uint32_t pmb887x_srb_get_icr(struct pmb887x_srb_reg_t *reg);
uint32_t pmb887x_srb_get_isr(struct pmb887x_srb_reg_t *reg);

void pmb887x_srb_set_imsc(struct pmb887x_srb_reg_t *reg, uint32_t value);
void pmb887x_srb_set_icr(struct pmb887x_srb_reg_t *reg, uint32_t value);
void pmb887x_srb_set_isr(struct pmb887x_srb_reg_t *reg, uint32_t value);
