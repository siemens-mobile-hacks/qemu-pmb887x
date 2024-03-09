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
#include "qapi/qmp/qlist.h"
#include "exec/address-spaces.h"
#include "qemu/datadir.h"
#include "hw/loader.h"
#include "sysemu/block-backend.h"
#include "qapi/qapi-commands-machine.h"
#include "sysemu/cpu-throttle.h"
#include "sysemu/accel-ops.h"
#include "sysemu/cpu-timers.h"
#include "target/arm/cpregs.h"

#include "hw/arm/pmb887x/flash-blk.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/devices.h"
#include "hw/arm/pmb887x/i2c.h"
#include "hw/arm/pmb887x/dif/lcd_common.h"
#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/boards.h"

static MemoryRegion tcm_memory[2];
static uint32_t tcm_regs[2] = {0x10, 0x10};

static const pmb887x_board_t *board = NULL;
static struct pmb887x_pll_t *cpu_pll = NULL;
static ARMCPU *cpu = NULL;

/*
	TCM
*/
static void pmb8876_tcm_update(void) {
	for (int i = 0; i < 2; ++i) {
		uint32_t base = tcm_regs[i] & 0xFFFFF000;
		uint32_t size = (tcm_regs[i] >> 2) & 0x1F;
		bool enabled = (tcm_regs[i] & 1) && size > 0;
		
		if (size > 0)
			size = (1 << (size - 1)) * 1024;
		
		// fprintf(stderr, "TCM%d: %08X (%08X, enabled=%d)\n", i, base, size, enabled);
		
		if (memory_region_is_mapped(&tcm_memory[i])) 
			memory_region_del_subregion(get_system_memory(), &tcm_memory[i]);
		
		if (enabled && !memory_region_is_mapped(&tcm_memory[i])) {
			memory_region_set_size(&tcm_memory[i], size);
			memory_region_add_subregion_overlap(get_system_memory(), base, &tcm_memory[i], 20002 - i);
		}
	}
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
		.access = PL1_RW, .type = ARM_CP_IO, .readfn = pmb8876_atcm_read, .writefn = pmb8876_atcm_write },
	{ .name = "BTCM", .cp = 15, .opc1 = 0, .crn = 9, .crm = 1, .opc2 = 1,
		.access = PL1_RW, .type = ARM_CP_IO, .readfn = pmb8876_btcm_read, .writefn = pmb8876_btcm_write },
};

static uint32_t unk_reg_F4600040 = 1;

static uint64_t cpu_io_read(void *opaque, hwaddr offset, unsigned size) {
	hwaddr addr = (size_t) opaque + offset;
	uint32_t value = 0;
	
	if (addr == 0xF4C0001C)
		value = 0;
	
	if (addr == 0xF4C00000)
		value = 0xFFFFFFFF;
	
	if (addr == 0xF4600024)
		value = 0x800000 | 0x11;
	
	if (addr == 0xF4600040)
		value = unk_reg_F4600040;
	
	#ifdef PMB887X_IO_BRIDGE
	value = pmb8876_io_bridge_read(addr, size);
	pmb887x_dump_io(addr, size, value, false);
	return value;
	#endif
	
	#ifdef PMB887X_TRACE_UNHANDLED_IO
	pmb887x_dump_io(addr, size, value, false);
	#endif
	
	// hw_error("READ: unknown reg access: %08lX\n", addr);
	
	return value;
}

static void cpu_io_write(void *opaque, hwaddr offset, uint64_t value, unsigned size) {
	hwaddr addr = (size_t) opaque + offset;
	
	#ifdef PMB887X_TRACE_UNHANDLED_IO
	pmb887x_dump_io(addr, size, value, true);
	#endif
	
	if (addr == 0xf460001c) {
		if (value == 0x8) {
			unk_reg_F4600040 = 2;
		} else {
			unk_reg_F4600040 = 1;
		}
	}
	
	if (addr == 0xF460002C) {
		if (value == 2) {
			unk_reg_F4600040 = 1;
		} else {
			unk_reg_F4600040 = 0;
		}
	}
	
	#ifdef PMB887X_IO_BRIDGE
	pmb887x_dump_io(addr, size, value, true);
	pmb8876_io_bridge_write(addr, size, value);
	return;
	#endif
	
	//fprintf(stderr, "WRITE: unknown reg access: %08lX\n", addr);
	//exit(1);
}

static const MemoryRegionOps cpu_io_opts = {
	.read			= cpu_io_read,
	.write			= cpu_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static uint64_t unmapped_io_read(void *opaque, hwaddr offset, unsigned size) {
	uint32_t addr = (size_t) opaque + offset;
	fprintf(stderr, "UNMAPPED READ[%d] %08X (PC: %08X)\n", size, addr, cpu->env.regs[15]);
//	exit(1);
	return 0;
}

static void unmapped_io_write(void *opaque, hwaddr offset, uint64_t value, unsigned size) {
	uint32_t addr = (size_t) opaque + offset;
	fprintf(stderr, "UNMAPPED WRITE[%d] %08X = %08lX (from: %08X)\n", size, addr, value, cpu->env.regs[15]);
//	exit(1);
}

static const MemoryRegionOps unmapped_io_opts = {
	.read			= unmapped_io_read,
	.write			= unmapped_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
};

__attribute__((destructor))
static void memory_dump_at_exit(void) {
//	fprintf(stderr, "sorry died at %08X\n", ARM_CPU(qemu_get_cpu(0))->env.regs[15]);
//	
//	qmp_pmemsave(0xB0000000, 16 * 1024 * 1024, "/tmp/ram.bin", NULL);
//	qmp_pmemsave(0xFFFF0000, 0x4000, "/tmp/tcm.bin", NULL);
//	qmp_pmemsave(0x00080000, 96 * 1024, "/tmp/sram.bin", NULL);
//	
//	qmp_pmemsave(0xA8000000, 16 * 1024 * 1024, "/tmp/ram.bin", NULL);
//	qmp_pmemsave(0x00000000, 0x4000, "/tmp/tcm.bin", NULL);
//	qmp_pmemsave(0x00080000, 96 * 1024, "/tmp/sram.bin", NULL);
}

static void pmb887x_create_sdram(DeviceState *ebuc, const pmb887x_board_memory_t *memory_list) {
	char bank_name[32];
	char cs_name[32];
	int bank_id = 0;
	
	for (int cs = 0; cs < 4; cs++) {
		const pmb887x_board_memory_t *memory = &memory_list[cs];
		if (memory->type == PMB887X_MEMORY_TYPE_RAM) {
			sprintf(bank_name, "SDRAM[%d]", bank_id);
			
			MemoryRegion *sdram = g_new(MemoryRegion, 1);
			memory_region_init_ram(sdram, NULL, bank_name, memory->size, &error_fatal);
			
			// Main EBU_CSx
			sprintf(cs_name, "cs%d", cs);
			object_property_set_link(OBJECT(ebuc), cs_name, OBJECT(sdram), &error_fatal);
			
			// Mirror EBU_CSx
			sprintf(cs_name, "cs%d", cs + 4);
			object_property_set_link(OBJECT(ebuc), cs_name, OBJECT(sdram), &error_fatal);
			
			bank_id++;
		}
	}
}

static void pmb887x_create_flash(BlockBackend *blk, DeviceState *ebuc, const pmb887x_board_memory_t *memory_list) {
	int flash_id = 0;
	char flash_name[32];
	char cs_name[32];
	
	DeviceState *flash_blk = qdev_new("pmb887x-flash-blk");
	qdev_prop_set_drive(flash_blk, "drive", blk);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(flash_blk), &error_fatal);
	
	uint32_t flash_offset = 0;
	
	for (int cs = 0; cs < 4; cs++) {
		const pmb887x_board_memory_t *memory = &memory_list[cs];
		if (memory->type == PMB887X_MEMORY_TYPE_FLASH) {
			sprintf(flash_name, "flash%d", flash_id);
			
			DeviceState *flash = qdev_new("pmb887x-flash");
			qdev_prop_set_string(flash, "name", flash_name);
			qdev_prop_set_string(flash, "otp0-data", getenv("PMB887X_FLASH_OTP0") ?: ""); // ESN
			qdev_prop_set_string(flash, "otp1-data", getenv("PMB887X_FLASH_OTP1") ?: ""); // IMEI
			qdev_prop_set_uint16(flash, "vid", memory->vid);
			qdev_prop_set_uint16(flash, "pid", memory->pid);
			qdev_prop_set_uint32(flash, "offset", flash_offset);
			object_property_set_link(OBJECT(flash), "blk", OBJECT(flash_blk), &error_fatal);
			sysbus_realize_and_unref(SYS_BUS_DEVICE(flash), &error_fatal);
			
			MemoryRegion *bank = sysbus_mmio_get_region(SYS_BUS_DEVICE(flash), 0);
			
			// Main EBU_CSx
			sprintf(cs_name, "cs%d", cs);
			object_property_set_link(OBJECT(ebuc), cs_name, OBJECT(bank), &error_fatal);
			
			// Mirror EBU_CSx
			sprintf(cs_name, "cs%d", cs + 4);
			object_property_set_link(OBJECT(ebuc), cs_name, OBJECT(bank), &error_fatal);
			
			flash_offset += object_property_get_uint(OBJECT(flash), "size", &error_fatal);
			flash_id++;
		}
	}
	
	int64_t total_flash_size = pmb887x_flash_blk_size(pmb887x_flash_blk_self(flash_blk));
	if (flash_offset != total_flash_size) {
		error_report("Invalid fullflash size=0x%08"PRIX64". Please, specify fullflash with size=0x%08X", total_flash_size, flash_offset);
		exit(1);
	}
}

static void pmb887x_init_keymap(DeviceState *keypad, const uint32_t *map, int map_size) {
	QList *list = qlist_new();
	for (int i = 0; i < map_size; i++)
		qlist_append_int(list, map[i]);
    qdev_prop_set_array(keypad, "map", list);
}

static void pmb887x_init(MachineState *machine) {
	const char *board_config = getenv("PMB887X_BOARD");
	if (!board_config) {
		error_report("Please, set board config with env PMB887X_BOARD=path/to/board.cfg");
		exit(1);
	}
	
	if (!(board = pmb887x_get_board(board_config))) {
		error_report("Invalid board specified.");
		exit(1);
	}
	
	#ifdef PMB887X_IO_BRIDGE
	fprintf(stderr, "Waiting for IO bridge...\n");
	pmb8876_io_bridge_init();
	#endif
	
	pmb887x_io_dump_init(board);
	
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
	
	// 0xF0000000-0xFF000000 (CPU IO)
	MemoryRegion *io = g_new(MemoryRegion, 1);
    memory_region_init_io(io, NULL, &cpu_io_opts, (void *) 0xF0000000, "IO", 0xF000000);
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
	
	// Custom BootROM
	const char *bios_name = machine->firmware;
	if (bios_name) {
		char *fn = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
		if (!fn) {
			error_report("Could not find BootROM image '%s'", bios_name);
			exit(1);
		}
		
		int image_size = load_image_targphys(fn, 0x00400000, 0x8000);
		g_free(fn);
		
		if (image_size < 0) {
			error_report("Could not load BootROM image '%s'", bios_name);
			exit(1);
		}
    }
    // Built-in BootROM
    else {
		size_t brom_size;
		const uint8_t *brom_data = pmb887x_get_brom_image(board->cpu, &brom_size);
		rom_add_blob_fixed("BROM", brom_data, brom_size, 0x00400000);
	}
	
	// NVIC
	DeviceState *nvic = pmb887x_new_dev(board->cpu, "NVIC", NULL);
	sysbus_connect_irq(SYS_BUS_DEVICE(nvic), 0, qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ));
	sysbus_connect_irq(SYS_BUS_DEVICE(nvic), 1, qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ));
	sysbus_realize_and_unref(SYS_BUS_DEVICE(nvic), &error_fatal);
	
	// PLL
	DeviceState *pll = pmb887x_new_dev(board->cpu, "PLL", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(pll), &error_fatal);
	cpu_pll = pmb887x_pll_get_self(pll);
	
	// System Timer
	DeviceState *stm = pmb887x_new_dev(board->cpu, "STM", nvic);
	object_property_set_link(OBJECT(stm), "pll", OBJECT(pll), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(stm), &error_fatal);
	
	// Time Processing Unit
	DeviceState *tpu = pmb887x_new_dev(board->cpu, "TPU", nvic);
	object_property_set_link(OBJECT(tpu), "pll", OBJECT(pll), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(tpu), &error_fatal);
	
	// DMA Controller
	DeviceState *dmac = pmb887x_new_dev(board->cpu, "DMA", nvic);
	object_property_set_link(OBJECT(dmac), "downstream", OBJECT(sysmem), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(dmac), &error_fatal);
	
	// DSP
	DeviceState *dsp = pmb887x_new_dev(board->cpu, "DSP", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(dsp), &error_fatal);
	
	if (board->cpu == CPU_PMB8876) {
		// MMCI
		DeviceState *mmci = pmb887x_new_dev(board->cpu, "MMCI", nvic);
		sysbus_realize_and_unref(SYS_BUS_DEVICE(mmci), &error_fatal);
	}
	
	// LCD panel
	DeviceState *lcd = pmb887x_new_lcd_dev(board->display.type);
	qdev_prop_set_uint32(lcd, "width", board->display.width);
	qdev_prop_set_uint32(lcd, "height", board->display.height);
	qdev_prop_set_uint32(lcd, "rotation", board->display.rotation);
	object_property_set_bool(OBJECT(lcd), "flip_horizontal", board->display.flip_horizontal, &error_fatal);
	object_property_set_bool(OBJECT(lcd), "flip_vertical", board->display.flip_vertical, &error_fatal);
	qdev_realize_and_unref(DEVICE(lcd), NULL, &error_fatal);
	
	// DIF
	DeviceState *dif = pmb887x_new_dev(board->cpu, "DIF", nvic);
	object_property_set_link(OBJECT(dif), "dmac", OBJECT(dmac), &error_fatal);
	object_property_set_link(OBJECT(dif), "lcd", OBJECT(lcd), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(dif), &error_fatal);
	
	// USART0
	DeviceState *usart0 = pmb887x_new_dev(board->cpu, "USART0", nvic);
	qdev_prop_set_chr(DEVICE(usart0), "chardev", serial_hd(0));
	sysbus_realize_and_unref(SYS_BUS_DEVICE(usart0), &error_fatal);
	
	// USART1
	DeviceState *usart1 = pmb887x_new_dev(board->cpu, "USART1", nvic);
	qdev_prop_set_chr(DEVICE(usart1), "chardev", serial_hd(1));
	sysbus_realize_and_unref(SYS_BUS_DEVICE(usart1), &error_fatal);
	
	if (board->cpu == CPU_PMB8876) {
		// I2C
		DeviceState *i2c = pmb887x_new_dev(board->cpu, "I2C", nvic);
		sysbus_realize_and_unref(SYS_BUS_DEVICE(i2c), &error_fatal);
		
		for (uint32_t i = 0; i < board->i2c_devices_count; i++) {
			I2CSlave *dev = pmb887x_new_i2c_dev(&board->i2c_devices[i]);
			i2c_slave_realize_and_unref(dev, pmb887x_i2c_bus(i2c), &error_fatal);
		}
	}
	
	// Standby Clock Control Unit
	DeviceState *sccu = pmb887x_new_dev(board->cpu, "SCCU", nvic);
	object_property_set_link(OBJECT(sccu), "pll", OBJECT(pll), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(sccu), &error_fatal);
	
	// Port Control Logic
	DeviceState *pcl = pmb887x_new_dev(board->cpu, "PCL", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(pcl), &error_fatal);
	
	// System Control Unit
	DeviceState *scu = pmb887x_new_dev(board->cpu, "SCU", nvic);
	object_property_set_link(OBJECT(scu), "brom_mirror", OBJECT(brom_mirror), &error_fatal);
	object_property_set_link(OBJECT(scu), "sccu", OBJECT(sccu), &error_fatal);
	object_property_set_link(OBJECT(scu), "pcl", OBJECT(pcl), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(scu), &error_fatal);
	
	// CAPCOM0
	DeviceState *capcom0 = pmb887x_new_dev(board->cpu, "CAPCOM0", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(capcom0), &error_fatal);
	
	// CAPCOM1
	DeviceState *capcom1 = pmb887x_new_dev(board->cpu, "CAPCOM1", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(capcom1), &error_fatal);
	
	// Fixed values for some GPIO's
	for (int i = 0; i < board->gpios_count; i++) {
		const pmb887x_board_gpio_t *gpio = &board->gpios[i];
		qemu_set_irq(qdev_get_gpio_in(pcl, gpio->id), gpio->value);
	}
	
	// RTC
	DeviceState *rtc = pmb887x_new_dev(board->cpu, "RTC", nvic);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(rtc), &error_fatal);
	
	#ifndef PMB887X_IO_BRIDGE
	// GPTU0
	DeviceState *gptu0 = pmb887x_new_dev(board->cpu, "GPTU0", nvic);
	object_property_set_link(OBJECT(gptu0), "pll", OBJECT(pll), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(gptu0), &error_fatal);
	
	// GPTU1
	DeviceState *gptu1 = pmb887x_new_dev(board->cpu, "GPTU1", nvic);
	object_property_set_link(OBJECT(gptu1), "pll", OBJECT(pll), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(gptu1), &error_fatal);
	#endif
	
	// ADC
	DeviceState *adc = pmb887x_new_dev(board->cpu, "ADC", nvic);
	object_property_set_link(OBJECT(adc), "pll", OBJECT(pll), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(adc), &error_fatal);
	
	// Fixed values for some GPIO's
	for (int i = 0; i < ARRAY_SIZE(board->adc_inputs); i++)
		pmb887x_adc_set_input(adc, i, &board->adc_inputs[i]);
	
	// KEYPAD
	DeviceState *keypad = pmb887x_new_dev(board->cpu, "KEYPAD", nvic);
	pmb887x_init_keymap(keypad, board->keymap, Q_KEY_CODE__MAX);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(keypad), &error_fatal);
	
	// External Bus Unit
	DeviceState *ebuc = pmb887x_new_dev(board->cpu, "EBU", NULL);
	
	// Flash
	DriveInfo *flash_dinfo = drive_get(IF_PFLASH, 0, 0);
	if (!flash_dinfo) {
		error_report("Flash ROM must be specified with -drive if=pflash,format=raw,file=fullflash.bin");
		exit(1);
	}
	pmb887x_create_flash(blk_by_legacy_dinfo(flash_dinfo), ebuc, board->cs2memory);
	
	// SDRAM
	pmb887x_create_sdram(ebuc, board->cs2memory);
	
	sysbus_realize_and_unref(SYS_BUS_DEVICE(ebuc), &error_fatal);
	
	// Exec BootROM
	#ifdef PMB887X_IO_BRIDGE
	pmb8876_io_bridge_set_nvic(nvic);
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
