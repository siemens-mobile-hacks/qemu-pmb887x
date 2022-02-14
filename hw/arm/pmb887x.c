#include "qemu/osdep.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "hw/arm/boot.h"
#include "net/net.h"
#include "sysemu/sysemu.h"
#include "hw/irq.h"
#include "hw/boards.h"
#include "hw/block/flash.h"
#include "qemu/error-report.h"
#include "qom/object.h"
#include "exec/address-spaces.h"
#include "qemu/datadir.h"
#include "hw/loader.h"
#include "sysemu/block-backend.h"
#include "qapi/qapi-commands-machine.h"

#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/devices.h"

static MemoryRegion tcm_memory[2];
static uint32_t tcm_regs[2] = {0x10, 0x10};

static ARMCPU *cpu;

/*
	TCM
*/
static void pmb8876_tcm_update(void) {
	bool locked = qemu_mutex_iothread_locked();
	if (!locked)
		qemu_mutex_lock_iothread();
	
	for (int i = 0; i < 2; ++i) {
		uint32_t base = tcm_regs[i] & 0xFFFFF000;
		uint32_t size = (tcm_regs[i] >> 2) & 0x1F;
		bool enabled = (tcm_regs[i] & 1) && size > 0;
		
		if (size > 0)
			size = (1 << (size - 1)) * 1024;
		
		fprintf(stderr, "TCM%d: %08X (%08X, enabled=%d)\n", i, base, size, enabled);
		
		if (memory_region_is_mapped(&tcm_memory[i])) 
			memory_region_del_subregion(get_system_memory(), &tcm_memory[i]);
		
		if (enabled && !memory_region_is_mapped(&tcm_memory[i])) {
			memory_region_set_size(&tcm_memory[i], size);
			memory_region_add_subregion_overlap(get_system_memory(), base, &tcm_memory[i], 20002 - i);
		}
	}
	
	if (!locked)
		qemu_mutex_unlock_iothread();
}

static uint64_t pmb8876_atcm_read(CPUARMState *env, const ARMCPRegInfo *ri) {
	return tcm_regs[0];
}

static uint64_t pmb8876_btcm_read(CPUARMState *env, const ARMCPRegInfo *ri) {
	return tcm_regs[1];
}

static void pmb8876_atcm_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value) {
	tcm_regs[0] = value;
	pmb8876_tcm_update();
}

static void pmb8876_btcm_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value) {
	tcm_regs[1] = value;
	pmb8876_tcm_update();
}

static const ARMCPRegInfo cp_reginfo[] = {
	// TCM
	{ .name = "ATCM", .cp = 15, .opc1 = 0, .crn = 9, .crm = 1, .opc2 = 0,
		.access = PL1_RW, .readfn = pmb8876_atcm_read, .writefn = pmb8876_atcm_write },
	{ .name = "BTCM", .cp = 15, .opc1 = 0, .crn = 9, .crm = 1, .opc2 = 1,
		.access = PL1_RW, .readfn = pmb8876_btcm_read, .writefn = pmb8876_btcm_write },
	REGINFO_SENTINEL
};

static uint64_t cpu_io_read(void *opaque, hwaddr offset, unsigned size) {
	hwaddr addr = (size_t) opaque + offset;
	uint32_t value = 0xFFFFFFFF;
	
	if (addr == 0xF4C0001C)
		value = 0;
	
	#ifdef PMB887X_IO_BRIDGE
	value = pmb8876_io_bridge_read(addr, size);
	pmb887x_dump_io(addr, size, value, false);
	return value;
	#endif
	
	pmb887x_dump_io(addr, size, 0xFFFFFFFF, false);
	
	fprintf(stderr, "READ: unknown reg access: %08lX\n", addr);
	//exit(1);
	
	return value;
}

static void cpu_io_write(void *opaque, hwaddr offset, uint64_t value, unsigned size) {
	hwaddr addr = (size_t) opaque + offset;
	
	pmb887x_dump_io(addr, size, value, true);
	
	#ifdef PMB887X_IO_BRIDGE
	pmb8876_io_bridge_write(addr, size, value);
	return;
	#endif
	
	fprintf(stderr, "WRITE: unknown reg access: %08lX\n", addr);
	//exit(1);
}

static const MemoryRegionOps cpu_io_opts = {
	.read			= cpu_io_read,
	.write			= cpu_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
};

static uint64_t unmapped_io_read(void *opaque, hwaddr offset, unsigned size) {
	uint32_t addr = (size_t) opaque + offset;
	fprintf(stderr, "UNMAPPED READ[%d] %08X (PC: %08X)\n", size, addr, cpu->env.regs[15]);
	if (addr >= 0xA0000000 && addr <= 0xA4000000)
		return 0xFFFFFFFF;
	exit(1);
	return 0xFFFFFFFF;
}

static void unmapped_io_write(void *opaque, hwaddr offset, uint64_t value, unsigned size) {
	uint32_t addr = (size_t) opaque + offset;
	fprintf(stderr, "UNMAPPED WRITE[%d] %08X = %08lX (from: %08X)\n", size, addr, value, cpu->env.regs[15]);
	exit(1);
}

static const MemoryRegionOps unmapped_io_opts = {
	.read			= unmapped_io_read,
	.write			= unmapped_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
};

__attribute__((destructor))
static void memory_dump_at_exit(void) {
	fprintf(stderr, "sorry died\n");
	
	qmp_pmemsave(0xA8000000, 16 * 1024 * 1024, "/tmp/ram.bin", NULL);
	qmp_pmemsave(0x00000000, 0x4000, "/tmp/tcm.bin", NULL);
	qmp_pmemsave(0x00080000, 96 * 1024, "/tmp/sram.bin", NULL);
}

static DeviceState *create_flash(BlockBackend *blk, uint16_t vendor_id, uint32_t *banks, int banks_n) {
	DeviceState *flash = qdev_new("pmb887x-flash");
	qdev_prop_set_string(flash, "name", "fullflash");
	qdev_prop_set_drive(flash, "drive", blk);
	qdev_prop_set_string(flash, "otp0-data", getenv("OTP_ESN") ?: ""); // ESN
	qdev_prop_set_string(flash, "otp1-data", getenv("OTP_IMEI") ?: ""); // IMEI
	qdev_prop_set_uint16(flash, "vendor-id", vendor_id);
	qdev_prop_set_uint32(flash, "len-bank-size", banks_n);
	
	char bank_name[32];
	for (int i = 0; i < banks_n; i++) {
		sprintf(bank_name, "bank-size[%d]", i);
		qdev_prop_set_uint32(flash, bank_name, banks[i]);
	}
	
	sysbus_realize_and_unref(SYS_BUS_DEVICE(flash), &error_fatal);
	
	return flash;
}

static void pmb887x_init(MachineState *machine, uint32_t cpu_type) {
	#ifdef PMB887X_IO_BRIDGE
	fprintf(stderr, "Wait for IO bridge...\n");
	pmb8876_io_bridge_init();
	#endif
	
	MemoryRegion *sysmem = get_system_memory();
	
	Object *cpuobj = object_new(machine->cpu_type);
	cpu = ARM_CPU(cpuobj);
	
	if (object_property_find(cpuobj, "has_el3"))
		object_property_set_bool(cpuobj, "has_el3", false, &error_fatal);
	object_property_set_bool(cpuobj, "realized", false, &error_fatal);
	
	// TCM regs
	define_arm_cp_regs(cpu, cp_reginfo);
	
	qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
	
	// TCM cache memory (8k ATCM, 8k BTCM)
	MemoryRegion *btcm = g_new(MemoryRegion, 1);
	MemoryRegion *atcm = g_new(MemoryRegion, 1);
	memory_region_init_ram(btcm, NULL, "BTCM", 8 * 1024, &error_fatal);
	memory_region_init_ram(atcm, NULL, "ATCM", 8 * 1024, &error_fatal);
	memory_region_init_alias(&tcm_memory[0], NULL, "BTCM_ALIAS", btcm, 0, memory_region_size(btcm));
	memory_region_init_alias(&tcm_memory[1], NULL, "ATCM_ALIAS", atcm, 0, memory_region_size(atcm));
	pmb8876_tcm_update();
	
	// 0x00000000-0xFFFFFFFF (Unmapped IO access)
    MemoryRegion *unmapped_io = g_new(MemoryRegion, 1);
    memory_region_init_io(unmapped_io, NULL, &unmapped_io_opts, (void *) 0x00000000, "UNMAPPED_IO", 0xFFFFFFFF);
	memory_region_add_subregion(sysmem, 0x00000000, unmapped_io);
	
	// 0x00000000-0xFFFFFFFF (Unmapped IO access)
    MemoryRegion *img = g_new(MemoryRegion, 1);
    memory_region_init_io(img, NULL, &cpu_io_opts, (void *) 0xa8da8080, "UNMAPPED_IO", 240*320*3);
	memory_region_add_subregion_overlap(sysmem, 0xA8DA8080, img, 9999999);
	
	// 0xF0000000-0xFFFFFFFF (CPU IO)
	MemoryRegion *io = g_new(MemoryRegion, 1);
    memory_region_init_io(io, NULL, &cpu_io_opts, (void *) 0xF0000000, "IO", 0x0FFFFFFF);
	memory_region_add_subregion(sysmem, 0xF0000000, io);
	
	// 0x00800000 (Internal SRAM, 96k)
	MemoryRegion *sram = g_new(MemoryRegion, 1);
	memory_region_init_ram(sram, NULL, "SRAM", 0x18000, &error_fatal);
	memory_region_add_subregion(sysmem, 0x00800000, sram);
	
	// Mirrors of SRAM
	for (uint32_t i = 0x00000000; i < 0x00800000; i += 0x20000) {
		char salias_name[64];
		sprintf(salias_name, "SRAM_MIRROR_%X", i / 0x20000);
		MemoryRegion *sram_alias = g_new(MemoryRegion, 1);
		memory_region_init_alias(sram_alias, NULL, salias_name, sram, 0, memory_region_size(sram));
		memory_region_add_subregion(sysmem, i, sram_alias);
	}
	
	// 0x00400000 (BROM, 32k)
	MemoryRegion *brom = g_new(MemoryRegion, 1);
	memory_region_init_rom(brom, NULL, "BROM", 0x8000, &error_fatal);
	memory_region_add_subregion(sysmem, 0x00400000, brom);
	
	// Mirror of BROM at top of IO (enabled/disabled by SCU)
	MemoryRegion *brom_mirror = g_new(MemoryRegion, 1);
	memory_region_init_alias(brom_mirror, NULL, "BROM_MIRROR", brom, 0, memory_region_size(brom));
	memory_region_set_enabled(brom_mirror, true);
	memory_region_add_subregion(sysmem, 0x00000000, brom_mirror);
	
	// BootROM image
	const char *bios_name = machine->firmware;
	if (bios_name) {
		char *fn = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
		if (!fn) {
			error_report("Could not find BootROM image '%s'", bios_name);
			exit(1);
		}
		
		int image_size = load_image_targphys(fn, 0, 0x8000);
		g_free(fn);
		
		if (image_size < 0) {
			error_report("Could not load BootROM image '%s'", bios_name);
			exit(1);
		}
    } else {
		error_report("BootROM must be specified with -bios");
		exit(1);
	}
	
	// FLASH image
	DriveInfo *flash_dinfo = drive_get(IF_PFLASH, 0, 0);
	if (!flash_dinfo) {
		error_report("Flash ROM must be specified with -drive if=pflash,format=raw,file=fullflash.bin");
		exit(1);
	}
	
	BlockBackend *flash_blk = blk_by_legacy_dinfo(flash_dinfo);
	uint64_t flash_sz = blk_getlength(flash_blk);
	
	if (machine->ram_size != 0x800000 && machine->ram_size != 0x1000000 && machine->ram_size != 0x2000000) {
		error_report("Invalid flash RAM size (%ld bytes). Valid only: 8M, 16M or 32M", flash_sz);
		exit(1);
	}
	
	// SDRAM
	MemoryRegion *sdram = g_new(MemoryRegion, 1);
	memory_region_init_ram(sdram, NULL, "SDRAM", machine->ram_size, &error_fatal);
	
	// NVIC
	DeviceState *nvic = pmb887x_new_dev(cpu_type, "NVIC", NULL);
	sysbus_connect_irq(SYS_BUS_DEVICE(nvic), 0, qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ));
	sysbus_connect_irq(SYS_BUS_DEVICE(nvic), 1, qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ));
	sysbus_realize_and_unref(SYS_BUS_DEVICE(nvic), &error_fatal);
	
	// PLL
	DeviceState *pll = pmb887x_new_dev(cpu_type, "PLL", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(pll), &error_fatal);
	
	// System Timer
	DeviceState *stm = pmb887x_new_dev(cpu_type, "STM", nvic);
	object_property_set_link(OBJECT(stm), "pll", OBJECT(pll), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(stm), &error_fatal);
	
	// Time Processing Unit
	DeviceState *tpu = pmb887x_new_dev(cpu_type, "TPU", nvic);
	object_property_set_link(OBJECT(tpu), "pll", OBJECT(pll), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(tpu), &error_fatal);
	
	// DSP
	DeviceState *dsp = pmb887x_new_dev(cpu_type, "DSP", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(dsp), &error_fatal);
	
	#ifndef PMB887X_IO_BRIDGE
	// System Control Unit
	DeviceState *scu = pmb887x_new_dev(cpu_type, "SCU", nvic);
	object_property_set_link(OBJECT(scu), "brom_mirror", OBJECT(brom_mirror), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(scu), &error_fatal);
	
	// CAPCOM0
	DeviceState *capcom0 = pmb887x_new_dev(cpu_type, "CAPCOM0", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(capcom0), &error_fatal);
	
	// CAPCOM1
	DeviceState *capcom1 = pmb887x_new_dev(cpu_type, "CAPCOM1", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(capcom1), &error_fatal);
	
	// Port Control Logic
	DeviceState *pcl = pmb887x_new_dev(cpu_type, "PCL", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(pcl), &error_fatal);
	
	// HW_DET4=1
	qemu_set_irq(qdev_get_gpio_in(pcl, 69), 1);
	
	// RTC
	DeviceState *rtc = pmb887x_new_dev(cpu_type, "RTC", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(rtc), &error_fatal);
	
	if (cpu_type == PMB8876) {
		// I2C
		DeviceState *i2c = pmb887x_new_dev(cpu_type, "I2C", nvic);
		sysbus_realize_and_unref(SYS_BUS_DEVICE(i2c), &error_fatal);
	}
	
	// USART0
	DeviceState *usart0 = pmb887x_new_dev(cpu_type, "USART0", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(usart0), &error_fatal);
	
	// USART1
	DeviceState *usart1 = pmb887x_new_dev(cpu_type, "USART1", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(usart1), &error_fatal);
	
	// GPTU0
	DeviceState *gptu0 = pmb887x_new_dev(cpu_type, "GPTU0", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(gptu0), &error_fatal);
	
	// GPTU1
	DeviceState *gptu1 = pmb887x_new_dev(cpu_type, "GPTU1", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(gptu1), &error_fatal);
	
	// DMA Controller
	DeviceState *dmac = pmb887x_new_dev(cpu_type, "DMA", nvic);
	object_property_set_link(OBJECT(dmac), "downstream", OBJECT(sysmem), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(dmac), &error_fatal);
	#else
	pmb8876_io_bridge_set_nvic(nvic);
	#endif
	
	// External Bus Unit
	DeviceState *ebuc = pmb887x_new_dev(cpu_type, "EBU", NULL);
	
	// RAM always CS1
	object_property_set_link(OBJECT(ebuc), "cs1", OBJECT(sdram), &error_fatal);
	
	// Flash depends on size
	switch (flash_sz) {
		case 0x1000000: // 16M
		case 0x2000000: // 32M
		{
			uint32_t banks[] = {flash_sz};
			DeviceState *flash = create_flash(blk_by_legacy_dinfo(flash_dinfo), 0x0089, banks, ARRAY_SIZE(banks));
			
			MemoryRegion *bank0 = sysbus_mmio_get_region(SYS_BUS_DEVICE(flash), 0);
			
			object_property_set_link(OBJECT(ebuc), "cs0", OBJECT(bank0), &error_fatal);
			object_property_set_link(OBJECT(ebuc), "cs4", OBJECT(bank0), &error_fatal);
		}
		break;
		
		case 0x4000000: // 64M
		{
			uint32_t banks[] = {0x4000000};
			DeviceState *flash = create_flash(blk_by_legacy_dinfo(flash_dinfo), 0x0020, banks, ARRAY_SIZE(banks));
			
			MemoryRegion *bank0 = sysbus_mmio_get_region(SYS_BUS_DEVICE(flash), 0);
			
			object_property_set_link(OBJECT(ebuc), "cs0", OBJECT(bank0), &error_fatal);
			object_property_set_link(OBJECT(ebuc), "cs4", OBJECT(bank0), &error_fatal);
		}
		break;
		
		case 0x6000000: // 96M
		{
			uint32_t banks[] = {0x2000000, 0x2000000, 0x2000000};
			DeviceState *flash = create_flash(blk_by_legacy_dinfo(flash_dinfo), 0x0089, banks, ARRAY_SIZE(banks));
			
			MemoryRegion *bank0 = sysbus_mmio_get_region(SYS_BUS_DEVICE(flash), 0);
			MemoryRegion *bank1 = sysbus_mmio_get_region(SYS_BUS_DEVICE(flash), 1);
			MemoryRegion *bank2 = sysbus_mmio_get_region(SYS_BUS_DEVICE(flash), 2);
			
			object_property_set_link(OBJECT(ebuc), "cs0", OBJECT(bank0), &error_fatal);
			object_property_set_link(OBJECT(ebuc), "cs2", OBJECT(bank1), &error_fatal);
			object_property_set_link(OBJECT(ebuc), "cs3", OBJECT(bank2), &error_fatal);
			object_property_set_link(OBJECT(ebuc), "cs4", OBJECT(bank0), &error_fatal);
			object_property_set_link(OBJECT(ebuc), "cs6", OBJECT(bank1), &error_fatal);
			object_property_set_link(OBJECT(ebuc), "cs7", OBJECT(bank2), &error_fatal);
		}
		break;
		
		default:
			error_report("Invalid flash ROM size (%ld bytes). Valid only: 16M, 32M, 64M, 96M", flash_sz);
			exit(1);
		break;
	}
	
	sysbus_realize_and_unref(SYS_BUS_DEVICE(ebuc), &error_fatal);
	
	#ifdef PMB887X_IO_BRIDGE
	// Exec BootROM
	cpu_set_pc(CPU(cpu), 0x00400000);
	#else
	// Exec BootROM
	cpu_set_pc(CPU(cpu), 0x00000000);
	#endif
}

static void pmb8876_init(MachineState *machine) {
	return pmb887x_init(machine, PMB8876);
}

static void pmb8875_init(MachineState *machine) {
	return pmb887x_init(machine, PMB8875);
}

static void pmb8876_class_init(ObjectClass *oc, void *data) {
	MachineClass *mc = MACHINE_CLASS(oc);
	mc->desc = "Infineon PMB8876 (ARM926EJ-S)";
	mc->init = pmb8876_init;
	mc->block_default_type = IF_PFLASH;
	mc->ignore_memory_transaction_failures = true;
	mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
	mc->default_ram_size = 16 * 1024 * 1024;
}

static void pmb8875_class_init(ObjectClass *oc, void *data) {
	MachineClass *mc = MACHINE_CLASS(oc);
	mc->desc = "Infineon PMB8875 (ARM926EJ-S)";
	mc->init = pmb8875_init;
	mc->block_default_type = IF_PFLASH;
	mc->ignore_memory_transaction_failures = true;
	mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
	mc->default_ram_size = 16 * 1024 * 1024;
}

static const TypeInfo pmb8876_type = {
	.name = MACHINE_TYPE_NAME("pmb8876"),
	.parent = TYPE_MACHINE,
	.class_init = pmb8876_class_init,
};

static const TypeInfo pmb8875_type = {
	.name = MACHINE_TYPE_NAME("pmb8875"),
	.parent = TYPE_MACHINE,
	.class_init = pmb8875_class_init,
};

static void versatile_machine_init(void) {
	type_register_static(&pmb8876_type);
	type_register_static(&pmb8875_type);
}

type_init(versatile_machine_init)
