
#include "hw/arm/pmb887x/qemu-machines.h"
#include "hw/arm/pmb887x/regs.h"

static void pmb887x_siemens_c81_init(MachineState *machine) {
	return pmb887x_init(machine, BOARD_C81);
}

static void pmb887x_siemens_c81_class_init(ObjectClass *oc, void *data) {
	MachineClass *mc = MACHINE_CLASS(oc);
	mc->desc = "Siemens C81 (PMB8876)";
	mc->init = pmb887x_siemens_c81_init;
	mc->block_default_type = IF_PFLASH;
	mc->ignore_memory_transaction_failures = true;
	mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
	mc->default_ram_size = 16 * 1024 * 1024;
}

static const TypeInfo pmb887x_siemens_c81_type = {
	.name = MACHINE_TYPE_NAME("siemens-c81"),
	.parent = TYPE_MACHINE,
	.class_init = pmb887x_siemens_c81_class_init
};

static void pmb887x_siemens_c81_machine_init(void) {
	type_register_static(&pmb887x_siemens_c81_type);
}

type_init(pmb887x_siemens_c81_machine_init);


static void pmb887x_siemens_cx75_init(MachineState *machine) {
	return pmb887x_init(machine, BOARD_CX75);
}

static void pmb887x_siemens_cx75_class_init(ObjectClass *oc, void *data) {
	MachineClass *mc = MACHINE_CLASS(oc);
	mc->desc = "Siemens CX75 (PMB8875)";
	mc->init = pmb887x_siemens_cx75_init;
	mc->block_default_type = IF_PFLASH;
	mc->ignore_memory_transaction_failures = true;
	mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
	mc->default_ram_size = 16 * 1024 * 1024;
}

static const TypeInfo pmb887x_siemens_cx75_type = {
	.name = MACHINE_TYPE_NAME("siemens-cx75"),
	.parent = TYPE_MACHINE,
	.class_init = pmb887x_siemens_cx75_class_init
};

static void pmb887x_siemens_cx75_machine_init(void) {
	type_register_static(&pmb887x_siemens_cx75_type);
}

type_init(pmb887x_siemens_cx75_machine_init);


static void pmb887x_siemens_el71_init(MachineState *machine) {
	return pmb887x_init(machine, BOARD_EL71);
}

static void pmb887x_siemens_el71_class_init(ObjectClass *oc, void *data) {
	MachineClass *mc = MACHINE_CLASS(oc);
	mc->desc = "Siemens EL71 (PMB8876)";
	mc->init = pmb887x_siemens_el71_init;
	mc->block_default_type = IF_PFLASH;
	mc->ignore_memory_transaction_failures = true;
	mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
	mc->default_ram_size = 16 * 1024 * 1024;
}

static const TypeInfo pmb887x_siemens_el71_type = {
	.name = MACHINE_TYPE_NAME("siemens-el71"),
	.parent = TYPE_MACHINE,
	.class_init = pmb887x_siemens_el71_class_init
};

static void pmb887x_siemens_el71_machine_init(void) {
	type_register_static(&pmb887x_siemens_el71_type);
}

type_init(pmb887x_siemens_el71_machine_init);


