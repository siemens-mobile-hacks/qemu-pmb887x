/*
 * ARM TCM
 * */
#define PMB887X_TRACE_ID		TCM
#define PMB887X_TRACE_PREFIX	"pmb887x-tcm"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "exec/memory.h"
#include "system/system.h"
#include "exec/address-spaces.h"
#include "cpu.h"
#include "cpregs.h"
#include "hw/qdev-properties.h"

#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_TCM	"pmb887x-tcm"
#define PMB887X_TCM(obj)	OBJECT_CHECK(pmb887x_tcm_t, (obj), TYPE_PMB887X_TCM)

typedef struct pmb887x_tcm_t pmb887x_tcm_t;

struct pmb887x_tcm_t {
	SysBusDevice parent_obj;
	MemoryRegion memory[2];
	uint32_t regs[2];
	CPUState *cpu;
};

/*
 *	TCM
 */
static void tcm_update_state(pmb887x_tcm_t *p, int i) {
	char tcm_names[] = { 'B', 'A' };
	uint32_t base = p->regs[i] & 0xFFFFF000;
	uint32_t size = (p->regs[i] >> 2) & 0x1F;
	bool enabled = (p->regs[i] & 1) && size > 0;

	if (size > 0)
		size = (1 << (size - 1)) * 1024;

	DPRINTF("TCM%c %08X (%08X, enabled=%d)\n", tcm_names[i], base, size, enabled);

	if (memory_region_is_mapped(&p->memory[i]))
		memory_region_del_subregion(p->cpu->memory, &p->memory[i]);

	if (enabled && !memory_region_is_mapped(&p->memory[i])) {
		memory_region_set_size(&p->memory[i], size);
		memory_region_add_subregion_overlap(p->cpu->memory, base, &p->memory[i], 20002 - i);
	}
}

static uint64_t pmb8876_atcm_read(CPUARMState *env, const ARMCPRegInfo *ri) {
	pmb887x_tcm_t *p = ri->opaque;
	return p->regs[0];
}

static uint64_t pmb8876_btcm_read(CPUARMState *env, const ARMCPRegInfo *ri) {
	pmb887x_tcm_t *p = ri->opaque;
	return p->regs[1];
}

static void pmb8876_atcm_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value) {
	pmb887x_tcm_t *p = ri->opaque;
	if (p->regs[0] != value) {
		p->regs[0] = value;
		tcm_update_state(p, 0);
	}
}

static void pmb8876_btcm_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value) {
	pmb887x_tcm_t *p = ri->opaque;
	if (p->regs[1] != value) {
		p->regs[1] = value;
		tcm_update_state(p, 1);
	}
}

static const ARMCPRegInfo tcm_cp_reginfo[] = {
	{
		.name = "ATCM", .cp = 15, .opc1 = 0, .crn = 9, .crm = 1, .opc2 = 0,
		.access = PL1_RW, .type = ARM_CP_IO,
		.readfn = pmb8876_atcm_read, .writefn = pmb8876_atcm_write
	},
	{
		.name = "BTCM", .cp = 15, .opc1 = 0, .crn = 9, .crm = 1, .opc2 = 1,
		.access = PL1_RW, .type = ARM_CP_IO,
		.readfn = pmb8876_btcm_read, .writefn = pmb8876_btcm_write
	},
};

static const Property tcm_properties[] = {
	DEFINE_PROP_LINK("cpu", pmb887x_tcm_t, cpu, TYPE_CPU, CPUState *),
};

static void tcm_init(Object *obj) {
	struct pmb887x_tcm_t *p = PMB887X_TCM(obj);
	memory_region_init_ram(&p->memory[0], NULL, "BTCM", 8 * 1024, &error_fatal);
	memory_region_init_ram(&p->memory[1], NULL, "ATCM", 8 * 1024, &error_fatal);
}

static void tcm_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_tcm_t *p = PMB887X_TCM(dev);
	define_arm_cp_regs_with_opaque(ARM_CPU(p->cpu), tcm_cp_reginfo, p);
	p->regs[0] = 0x10;
	p->regs[1] = 0x10;
	tcm_update_state(p, 0);
	tcm_update_state(p, 1);
}

static void tcm_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, tcm_properties);
	dc->realize = tcm_realize;
}

static const TypeInfo tcm_info = {
	.name          	= TYPE_PMB887X_TCM,
	.parent        	= TYPE_SYS_BUS_DEVICE,
	.instance_size 	= sizeof(struct pmb887x_tcm_t),
	.instance_init 	= tcm_init,
	.class_init    	= tcm_class_init,
};

static void tcm_register_types(void) {
	type_register_static(&tcm_info);
}
type_init(tcm_register_types)
