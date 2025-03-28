/*
 * Standart parts for all modules: CLC, SRB, SRC
 * */
#define PMB887X_TRACE_ID		MOD
#define PMB887X_TRACE_PREFIX	"pmb887x-mod"

#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/hw.h"
#include "hw/arm/pmb887x/trace.h"

void pmb887x_clc_init(pmb887x_clc_reg_t *reg) {
	pmb887x_clc_set(reg, 1 << MOD_CLC_RMC_SHIFT);
}

uint8_t pmb887x_clc_get_rmc(pmb887x_clc_reg_t *reg) {
	return (reg->value & MOD_CLC_RMC) >> MOD_CLC_RMC_SHIFT;
}

uint8_t pmb887x_clc_is_enabled(pmb887x_clc_reg_t *reg) {
	return (reg->value & MOD_CLC_DISR) == 0;
}

uint32_t pmb887x_clc_get(pmb887x_clc_reg_t *reg) {
	return reg->value;
}

void pmb887x_clc_set(pmb887x_clc_reg_t *reg, uint32_t value) {
	if ((value & MOD_CLC_DISR)) {
		value |= MOD_CLC_DISS;
	} else {
		value &= ~MOD_CLC_DISS;
	}
	reg->value = value;
}

void pmb887x_src_init(pmb887x_src_reg_t *reg, qemu_irq irq) {
	reg->irq = irq;
	reg->value = 0;
	reg->last_irq_state = false;
	
	if (!reg->irq)
		hw_error("[pmb887x-mod] irq is not set\n");
}

uint32_t pmb887x_src_get(pmb887x_src_reg_t *reg) {
	return reg->value;
}

void pmb887x_src_update(pmb887x_src_reg_t *reg, uint32_t clear, uint32_t set) {
	pmb887x_src_set(reg, (reg->value & ~clear) | set);
}

void pmb887x_src_set(pmb887x_src_reg_t *reg, uint32_t value) {
	bool has_irq = (reg->value & MOD_SRC_SRR) != 0;
	
	if ((value & MOD_SRC_CLRR)) {
		has_irq = false;
		value &= ~MOD_SRC_CLRR; // write only
	}
	
	if ((value & MOD_SRC_SETR)) {
		has_irq = true;
		value &= ~MOD_SRC_SETR; // write only
	}
	
	if (has_irq) {
		value |= MOD_SRC_SRR;
	} else {
		value &= ~MOD_SRC_SRR;
	}
	
	reg->value = value;
	
	if (has_irq != reg->last_irq_state) {
		if (has_irq) {
			if ((reg->value & MOD_SRC_SRE)) {
				uint32_t priority = (reg->value & MOD_SRC_SRPN) >> MOD_SRC_SRPN_SHIFT;
				qemu_set_irq(reg->irq, 1 + (int) priority);
				reg->last_irq_state = true;
			}
		} else {
			qemu_set_irq(reg->irq, 0);
			reg->last_irq_state = false;
		}
	}
}

static int pmb887x_srb_irq_router(void *opaque, int event_id) {
	return event_id;
}

void pmb887x_srb_init(pmb887x_srb_reg_t *reg, qemu_irq *irq, int irq_n) {
	reg->irq = irq;
	reg->irq_n = irq_n;
	reg->last_irq_state = g_new0(bool, reg->irq_n);
	reg->irq_events = g_new0(uint32_t, reg->irq_n);
	reg->irq_router = pmb887x_srb_irq_router;
	reg->irq_router_opaque = reg;
	reg->imsc = 0;
	reg->ris = 0;
}

void pmb887x_srb_set_irq_router(pmb887x_srb_reg_t *reg, void *opaque, int (*callback)(void *, int)) {
	reg->irq_router = callback;
	reg->irq_router_opaque = opaque;
}

uint32_t pmb887x_srb_get_imsc(pmb887x_srb_reg_t *reg) {
	return reg->imsc;
}

uint32_t pmb887x_srb_get_mis(pmb887x_srb_reg_t *reg) {
	return (reg->ris & reg->imsc);
}

uint32_t pmb887x_srb_get_ris(pmb887x_srb_reg_t *reg) {
	return reg->ris;
}

static void pmb887x_srb_set_irq(pmb887x_srb_reg_t *reg, int n, int level) {
	int irq_n = reg->irq_router(reg->irq_router_opaque, n);
	uint8_t mask = 1 << n;
	
	if (irq_n < 0 || irq_n >= reg->irq_n)
		hw_error("[pmb887x-mod] invalid irq index: %d\n", irq_n);
	
	if (level != 0) {
		reg->irq_events[irq_n] |= mask;
	} else {
		reg->irq_events[irq_n] &= ~mask;
	}
	
	int state = reg->irq_events[irq_n] ? 1 : 0;
	if (reg->last_irq_state[irq_n] != state) {
		qemu_set_irq(reg->irq[irq_n], state);
		reg->last_irq_state[irq_n] = state;
	}
}

static void pmb887x_srb_set_event(pmb887x_srb_reg_t *reg, int n, int level) {
	uint8_t mask = 1 << n;
	bool has_irq = level != 0;
	bool last_has_irq = (reg->last_state & mask) != 0;
	
	if (has_irq != last_has_irq) {
		if (has_irq) {
			if ((reg->imsc & mask)) {
				pmb887x_srb_set_irq(reg, n, level);
				reg->last_state |= mask;
			}
		} else {
			pmb887x_srb_set_irq(reg, n, level);
			reg->last_state &= ~mask;
		}
	}
	
	if (has_irq) {
		reg->ris |= mask;
	} else {
		reg->ris &= ~mask;
	}
}

void pmb887x_srb_set_imsc(pmb887x_srb_reg_t *reg, uint32_t value) {
	uint32_t new_enabled = value & ~reg->imsc;
	reg->imsc = value;
	
	if (!new_enabled)
		return;
	
	for (int i = 0; i < 32; i++) {
		uint8_t mask = 1 << i;
		if ((new_enabled & mask)) {
			if ((reg->ris & mask) != 0)
				pmb887x_srb_set_event(reg, i, 1);
		}
	}
}

void pmb887x_srb_set_icr(pmb887x_srb_reg_t *reg, uint32_t value) {
	for (int i = 0; i < 32; i++) {
		uint8_t mask = 1 << i;
		if ((value & mask))
			pmb887x_srb_set_event(reg, i, 0);
	}
}

void pmb887x_srb_set_isr(pmb887x_srb_reg_t *reg, uint32_t value) {
	for (int i = 0; i < 32; i++) {
		uint8_t mask = 1 << i;
		if ((value & mask))
			pmb887x_srb_set_event(reg, i, 1);
	}
}

void pmb887x_srb_ext_init(pmb887x_srb_ext_reg_t *reg, pmb887x_srb_reg_t *parent, uint32_t events) {
	reg->parent = parent;
	reg->events = events;
	reg->ris = 0;
	reg->imsc = 0;
}

uint32_t pmb887x_srb_ext_get_imsc(pmb887x_srb_ext_reg_t *reg) {
	return reg->imsc;
}

uint32_t pmb887x_srb_ext_get_mis(pmb887x_srb_ext_reg_t *reg) {
	return reg->ris & reg->imsc;
}

uint32_t pmb887x_srb_ext_get_ris(pmb887x_srb_ext_reg_t *reg) {
	return reg->ris;
}

void pmb887x_srb_ext_set_imsc(pmb887x_srb_ext_reg_t *reg, uint32_t value) {
	uint32_t new_enabled = value & ~reg->imsc;
	reg->imsc = value;
	
	if (!new_enabled)
		return;
	
	pmb887x_srb_ext_set_isr(reg, reg->ris & new_enabled);
}

void pmb887x_srb_ext_set_icr(pmb887x_srb_ext_reg_t *reg, uint32_t value) {
	reg->ris &= ~value;
	
	if (!reg->ris)
		pmb887x_srb_set_icr(reg->parent, reg->events);
}

void pmb887x_srb_ext_set_isr(pmb887x_srb_ext_reg_t *reg, uint32_t value) {
	reg->ris |= value;
	
	if (reg->ris)
		pmb887x_srb_set_isr(reg->parent, reg->events);
}
