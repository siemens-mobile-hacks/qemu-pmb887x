#include "qemu/osdep.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/sysbus.h"
#include "hw/arm/boot.h"
#include "net/net.h"
#include "hw/boards.h"
#include "qemu/error-report.h"
#include "qobject/qlist.h"
#include "qom/object.h"
#include "exec/address-spaces.h"
#include "hw/loader.h"
#include "hw/irq.h"
#include "hw/ssi/ssi.h"
#include "system/system.h"
#include "target/arm/cpregs.h"

#include "hw/arm/pmb887x/board/dsp.h"
#include "hw/arm/pmb887x/board/devices.h"
#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/board/analog.h"
#include "hw/arm/pmb887x/board/gpio.h"
#include "hw/arm/pmb887x/board/cpu_module.h"

#include "hw/arm/pmb887x/gen/brom.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"

#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/trace_common.h"

void qmp_pmemsave(uint64_t addr, uint64_t size, const char *filename, Error **errp);

static bool machine_used = false;

__attribute__((destructor))
static void memory_dump_at_exit(void) {
	if (!machine_used)
		return;

	CPUState *cpu = qemu_get_cpu(0);
	if (!cpu)
		return;

	fprintf(stderr, "sorry died at %08X LR %08X\n", ARM_CPU(cpu)->env.regs[15], ARM_CPU(cpu)->env.regs[14]);

//	qmp_pmemsave(0xB0000000, 16 * 1024 * 1024, "/tmp/ram.bin", NULL);
//	qmp_pmemsave(0xFFFF0000, 0x4000, "/tmp/tcm.bin", NULL);
//	qmp_pmemsave(0x00080000, 96 * 1024, "/tmp/sram.bin", NULL);
//	
//	qmp_pmemsave(0xA8000000, 16 * 1024 * 1024, "/tmp/ram.bin", NULL);
//	qmp_pmemsave(0x00000000, 0x4000, "/tmp/tcm.bin", NULL);
//	qmp_pmemsave(0x00000000, 96 * 1024, "/tmp/sram.bin", NULL);
}

static void pmb887x_init_keymap(DeviceState *keypad, const uint32_t *map, int map_size) {
	QList *list = qlist_new();
	for (int i = 0; i < map_size; i++)
		qlist_append_int(list, map[i]);
    qdev_prop_set_array(keypad, "map", list);
}

static void pmb887x_init(MachineState *machine) {
	machine_used = true;

	const char *board_config_file = getenv("PMB887X_BOARD");
	if (!board_config_file) {
		error_report("Please, set board config with env PMB887X_BOARD=path/to/board.cfg");
		exit(1);
	}

	pmb887x_trace_init();
	pmb887x_board_init(board_config_file);

#if PMB887X_IO_BRIDGE
	fprintf(stderr, "Waiting for IO bridge...\n");
	pmb8876_io_bridge_init();
#endif

	pmb887x_io_dump_init();

	MemoryRegion *sysmem = get_system_memory();

	Object *cpuobj = object_new(machine->cpu_type);
	ARMCPU *cpu = ARM_CPU(cpuobj);

	if (object_property_find(cpuobj, "has_el3"))
		object_property_set_bool(cpuobj, "has_el3", false, &error_fatal);
	object_property_set_bool(cpuobj, "realized", false, &error_fatal);

	qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);

	// TCM
	DeviceState *tcm = qdev_new("pmb887x-tcm");
	object_property_set_link(OBJECT(tcm), "cpu", OBJECT(cpu), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(tcm), &error_fatal);

	// 0x00000000-0xFFFFFFFF (Unmapped IO access)
	DeviceState *unknown_io = qdev_new("pmb887x-unknown");
	sysbus_mmio_map(SYS_BUS_DEVICE(unknown_io), 0, 0);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(unknown_io), &error_fatal);

	// 0x00800000 (Internal SRAM, 96k)
	MemoryRegion *sram = g_new(MemoryRegion, 1);
	memory_region_init_ram(sram, NULL, "SRAM", 0x18000, &error_fatal);
	memory_region_add_subregion_overlap(sysmem, 0x00800000, sram, 1);

	// Mirrors of SRAM
	for (uint32_t i = 0x00000000; i < 0x00800000; i += 0x20000) {
		char salias_name[64];
		sprintf(salias_name, "SRAM_MIRROR_%X", i / 0x20000);
		MemoryRegion *sram_alias = g_new(MemoryRegion, 1);
		memory_region_init_alias(sram_alias, NULL, salias_name, sram, 0, memory_region_size(sram));
		memory_region_add_subregion_overlap(sysmem, i, sram_alias, 1);
	}

	// 0x00400000 (BROM)
	size_t brom_size;
	const uint8_t *brom_data = pmb887x_get_brom_image(pmb887x_board()->cpu, &brom_size);
	MemoryRegion *brom = g_new(MemoryRegion, 1);
	memory_region_init_rom(brom, NULL, "BROM", brom_size, &error_fatal);
	memory_region_add_subregion_overlap(sysmem, 0x00400000, brom, 1);

	// Mirror of BROM at top of IO (enabled/disabled by SCU)
	MemoryRegion *brom_mirror = g_new(MemoryRegion, 1);
	memory_region_init_alias(brom_mirror, NULL, "BROM_MIRROR", brom, 0, memory_region_size(brom));
	memory_region_set_enabled(brom_mirror, true);
	memory_region_add_subregion_overlap(sysmem, 0x00000000, brom_mirror, 1);

	rom_add_blob_fixed("BROM", brom_data, brom_size, 0x00400000);

	// Port Control Logic
	DeviceState *pcl = pmb887x_new_cpu_module("GPIO");
	sysbus_realize_and_unref(SYS_BUS_DEVICE(pcl), &error_fatal);

	// VIC
	DeviceState *vic = pmb887x_new_cpu_module("VIC");
	sysbus_connect_irq(SYS_BUS_DEVICE(vic), 0, qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ));
	sysbus_connect_irq(SYS_BUS_DEVICE(vic), 1, qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ));
	sysbus_realize_and_unref(SYS_BUS_DEVICE(vic), &error_fatal);

	// PLL
	DeviceState *pll = pmb887x_new_cpu_module("PLL");
	sysbus_realize_and_unref(SYS_BUS_DEVICE(pll), &error_fatal);

	// System Timer
	DeviceState *stm = pmb887x_new_cpu_module("STM");
	object_property_set_link(OBJECT(stm), "pll", OBJECT(pll), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(stm), &error_fatal);

	// Time Processing Unit
	DeviceState *tpu = pmb887x_new_cpu_module("TPU");
	object_property_set_link(OBJECT(tpu), "pll", OBJECT(pll), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(tpu), &error_fatal);

	// DMA Controller
	DeviceState *dmac = pmb887x_new_cpu_module("DMAC");
	object_property_set_link(OBJECT(dmac), "downstream", OBJECT(sysmem), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(dmac), &error_fatal);

	// DSP
	DeviceState *dsp = pmb887x_new_cpu_module("DSP");
	pmb887x_board_init_dsp(dsp);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(dsp), &error_fatal);

	if (pmb887x_board()->cpu == CPU_PMB8876) {
		// MMCI
		DeviceState *mmci = pmb887x_new_cpu_module("MMCI");
		sysbus_realize_and_unref(SYS_BUS_DEVICE(mmci), &error_fatal);
	}

	// USART0
	DeviceState *usart0 = pmb887x_new_cpu_module("USART0");
	qdev_prop_set_chr(DEVICE(usart0), "chardev", serial_hd(0));
	sysbus_realize_and_unref(SYS_BUS_DEVICE(usart0), &error_fatal);

	// USART1
	DeviceState *usart1 = pmb887x_new_cpu_module("USART1");
	qdev_prop_set_chr(DEVICE(usart1), "chardev", serial_hd(1));
	sysbus_realize_and_unref(SYS_BUS_DEVICE(usart1), &error_fatal);

	// DIF
	DeviceState *dif = pmb887x_new_cpu_module("DIF");
	if (object_property_find(OBJECT(dif), "dmac"))
		object_property_set_link(OBJECT(dif), "dmac", OBJECT(dmac), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(dif), &error_fatal);

    // I2C
    DeviceState *i2c = pmb887x_new_cpu_module("I2C");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(i2c), &error_fatal);

	// Standby Clock Control Unit
	DeviceState *sccu = pmb887x_new_cpu_module("SCCU");
	object_property_set_link(OBJECT(sccu), "pll", OBJECT(pll), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(sccu), &error_fatal);

	// System Control Unit
	DeviceState *scu = pmb887x_new_cpu_module("SCU");
	object_property_set_link(OBJECT(scu), "brom_mirror", OBJECT(brom_mirror), &error_fatal);
	object_property_set_link(OBJECT(scu), "sccu", OBJECT(sccu), &error_fatal);
	object_property_set_link(OBJECT(scu), "pcl", OBJECT(pcl), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(scu), &error_fatal);
	
	// CAPCOM0
	DeviceState *capcom0 = pmb887x_new_cpu_module("CAPCOM0");
	sysbus_realize_and_unref(SYS_BUS_DEVICE(capcom0), &error_fatal);
	
	// CAPCOM1
	DeviceState *capcom1 = pmb887x_new_cpu_module("CAPCOM1");
	sysbus_realize_and_unref(SYS_BUS_DEVICE(capcom1), &error_fatal);

	// RTC
	DeviceState *rtc = pmb887x_new_cpu_module("RTC");
	sysbus_realize_and_unref(SYS_BUS_DEVICE(rtc), &error_fatal);

	// GPTU0
	DeviceState *gptu0 = pmb887x_new_cpu_module("GPTU0");
	object_property_set_link(OBJECT(gptu0), "pll", OBJECT(pll), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(gptu0), &error_fatal);
	
	// GPTU1
	DeviceState *gptu1 = pmb887x_new_cpu_module("GPTU1");
	object_property_set_link(OBJECT(gptu1), "pll", OBJECT(pll), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(gptu1), &error_fatal);

	// ADC
	DeviceState *adc = pmb887x_new_cpu_module("ADC");
	object_property_set_link(OBJECT(adc), "pll", OBJECT(pll), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(adc), &error_fatal);

	// KEYPAD
	DeviceState *keypad = pmb887x_new_cpu_module("KEYPAD");
	pmb887x_init_keymap(keypad, pmb887x_board()->keymap, Q_KEY_CODE__MAX);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(keypad), &error_fatal);

	// External Bus Unit
	DeviceState *ebuc = pmb887x_new_cpu_module("EBU");

	// Flash
	DriveInfo *flash_dinfo = drive_get(IF_PFLASH, 0, 0);
	if (!flash_dinfo) {
		error_report("Flash ROM must be specified with -drive if=pflash,format=raw,file=fullflash.bin");
		exit(1);
	}

	DeviceState *flash_blk = qdev_new("pmb887x-flash-blk");
	flash_blk->id = strdup("FULLFLASH");
	qdev_prop_set_drive(flash_blk, "drive", blk_by_legacy_dinfo(flash_dinfo));
	sysbus_realize_and_unref(SYS_BUS_DEVICE(flash_blk), &error_fatal);

	pmb887x_cpu_modules_post_init();
	pmb887x_board_init_analog();
	pmb887x_board_gpio_init_fixed_inputs();
	pmb887x_board_init_devices(ebuc);

	// Exec BootROM
	#if PMB887X_IO_BRIDGE
	pmb8876_io_bridge_set_vic(vic);
	cpu_set_pc(CPU(cpu), 0x00400000);
	#else
	cpu_set_pc(CPU(cpu), 0x00000000);
	#endif
}

/*
 * Generic PMB887X machine
 * */
static void pmb887x_class_init(ObjectClass *oc, void *data) {
	MachineClass *mc = MACHINE_CLASS(oc);
	mc->desc = "Generic PMB8875/PMB8876 board";
	mc->init = pmb887x_init;
	mc->block_default_type = IF_PFLASH;
	mc->ignore_memory_transaction_failures = true;
	mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
	mc->default_ram_size = 16 * 1024 * 1024;
}

static const TypeInfo pmb887x_type = {
	.name = MACHINE_TYPE_NAME("pmb887x"),
	.parent = TYPE_MACHINE,
	.class_init = pmb887x_class_init
};

static void pmb887x_type_init(void) {
	type_register_static(&pmb887x_type);
}

type_init(pmb887x_type_init);
