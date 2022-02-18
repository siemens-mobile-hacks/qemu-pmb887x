#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/regs.h"
#include "qemu/error-report.h"

#define MOD_DEBUG

#ifdef MOD_DEBUG
#define DPRINTF(fmt, ...) do { fprintf(stderr, "[pmb887x-mod]: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

void pmb887x_clc_init(struct pmb887x_clc_reg_t *reg) {
	pmb887x_clc_set(reg, 1 << MOD_CLC_RMC_SHIFT);
}

uint8_t pmb887x_clc_get_rmc(struct pmb887x_clc_reg_t *reg) {
	return (reg->value & MOD_CLC_RMC) >> MOD_CLC_RMC_SHIFT;
}

uint8_t pmb887x_clc_is_enabled(struct pmb887x_clc_reg_t *reg) {
	return (reg->value & MOD_CLC_DISR) == 0;
}

uint32_t pmb887x_clc_get(struct pmb887x_clc_reg_t *reg) {
	return reg->value;
}

void pmb887x_clc_set(struct pmb887x_clc_reg_t *reg, uint32_t value) {
	if ((value & MOD_CLC_DISR)) {
		value |= MOD_CLC_DISS;
	} else {
		value &= ~MOD_CLC_DISS;
	}
	reg->value = value;
}

void pmb887x_src_init(struct pmb887x_src_reg_t *reg, qemu_irq irq) {
	reg->irq = irq;
	reg->value = 0;
	reg->last_irq_state = false;
	
	if (!reg->irq) {
		error_report("[pmb887x-mod] irq is not set\n");
		abort();
	}
}

uint32_t pmb887x_src_get(struct pmb887x_src_reg_t *reg) {
	return reg->value;
}

void pmb887x_src_update(struct pmb887x_src_reg_t *reg, uint32_t clear, uint32_t set) {
	pmb887x_src_set(reg, (reg->value & ~clear) | set);
}

void pmb887x_src_set(struct pmb887x_src_reg_t *reg, uint32_t value) {
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
				int priority = (reg->value & MOD_SRC_SRPN) >> MOD_SRC_SRPN_SHIFT;
				qemu_set_irq(reg->irq, 1 + priority);
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

void pmb887x_srb_init(struct pmb887x_srb_reg_t *reg, qemu_irq *irq, int irq_n) {
	reg->irq = irq;
	reg->irq_n = irq_n;
	reg->last_irq_state = g_new0(bool, reg->irq_n);
	reg->irq_lock = g_new0(int, reg->irq_n);
	reg->irq_router = pmb887x_srb_irq_router;
	reg->irq_router_opaque = reg;
	reg->imsc = 0;
	reg->ris = 0;
}

void pmb887x_srb_set_irq_router(struct pmb887x_srb_reg_t *reg, void *opaque, int (*callback)(void *, int)) {
	reg->irq_router = callback;
	reg->irq_router_opaque = opaque;
}

uint32_t pmb887x_srb_get_imsc(struct pmb887x_srb_reg_t *reg) {
	return reg->imsc;
}

uint32_t pmb887x_srb_get_mis(struct pmb887x_srb_reg_t *reg) {
	return (reg->ris & reg->imsc);
}

uint32_t pmb887x_srb_get_ris(struct pmb887x_srb_reg_t *reg) {
	return reg->ris;
}

static void pmb887x_srb_set_irq(struct pmb887x_srb_reg_t *reg, int n, int level) {
	int irq_n = reg->irq_router(reg->irq_router_opaque, n);
	
	if (irq_n < 0 || irq_n >= reg->irq_n) {
		error_report("[pmb887x-mod] invalid irq index: %d\n", irq_n);
		abort();
	}
	
	if (level != 0) {
		reg->irq_lock[irq_n]++;
	} else {
		reg->irq_lock[irq_n]--;
	}
	
	int state = reg->irq_lock[irq_n] != 0 ? 1 : 0;
	if (reg->last_irq_state[irq_n] != state) {
		qemu_set_irq(reg->irq[irq_n], state);
		reg->last_irq_state[irq_n] = state;
	}
}

static void pmb887x_srb_set_event(struct pmb887x_srb_reg_t *reg, int n, int level) {
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

void pmb887x_srb_set_imsc(struct pmb887x_srb_reg_t *reg, uint32_t value) {
	reg->imsc = value;
}

void pmb887x_srb_set_icr(struct pmb887x_srb_reg_t *reg, uint32_t value) {
	for (int i = 0; i < 32; i++) {
		uint8_t mask = 1 << i;
		if ((value & mask))
			pmb887x_srb_set_event(reg, i, 0);
	}
}

void pmb887x_srb_set_isr(struct pmb887x_srb_reg_t *reg, uint32_t value) {
	for (int i = 0; i < 32; i++) {
		uint8_t mask = 1 << i;
		if ((value & mask))
			pmb887x_srb_set_event(reg, i, 1);
	}
}
