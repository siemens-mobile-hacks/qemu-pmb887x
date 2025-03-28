#pragma once
#include "qemu/osdep.h"
#include "hw/irq.h"

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
	
	int (*irq_router)(void *, int);
	void *irq_router_opaque;
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
uint32_t pmb887x_src_get(pmb887x_src_reg_t *reg);
void pmb887x_src_set(pmb887x_src_reg_t *reg, uint32_t value);
void pmb887x_src_update(pmb887x_src_reg_t *reg, uint32_t clear, uint32_t set);

// Service Request Block
void pmb887x_srb_init(pmb887x_srb_reg_t *reg, qemu_irq *irq, int irq_n);
void pmb887x_srb_set_irq_router(pmb887x_srb_reg_t *reg, void *opaque, int (*callback)(void *, int));

uint32_t pmb887x_srb_get_imsc(pmb887x_srb_reg_t *reg);
uint32_t pmb887x_srb_get_mis(pmb887x_srb_reg_t *reg);
uint32_t pmb887x_srb_get_ris(pmb887x_srb_reg_t *reg);

void pmb887x_srb_set_imsc(pmb887x_srb_reg_t *reg, uint32_t value);
void pmb887x_srb_set_icr(pmb887x_srb_reg_t *reg, uint32_t value);
void pmb887x_srb_set_isr(pmb887x_srb_reg_t *reg, uint32_t value);

// Service Request Block Extended
void pmb887x_srb_ext_init(pmb887x_srb_ext_reg_t *reg, pmb887x_srb_reg_t *parent, uint32_t events);

uint32_t pmb887x_srb_ext_get_imsc(pmb887x_srb_ext_reg_t *reg);
uint32_t pmb887x_srb_ext_get_mis(pmb887x_srb_ext_reg_t *reg);
uint32_t pmb887x_srb_ext_get_ris(pmb887x_srb_ext_reg_t *reg);

void pmb887x_srb_ext_set_imsc(pmb887x_srb_ext_reg_t *reg, uint32_t value);
void pmb887x_srb_ext_set_icr(pmb887x_srb_ext_reg_t *reg, uint32_t value);
void pmb887x_srb_ext_set_isr(pmb887x_srb_ext_reg_t *reg, uint32_t value);
