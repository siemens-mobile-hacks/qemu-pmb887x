#include "hw/arm/pmb887x/board/memory.h"

#include "qemu/error-report.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"

void pmb887x_board_ebu_connect(DeviceState *ebuc, int cs, MemoryRegion *region) {
	char cs_name[64];

	// Main EBU_CSx
	sprintf(cs_name, "cs%d", cs);
	object_property_set_link(OBJECT(ebuc), cs_name, OBJECT(region), &error_fatal);

	// Mirror EBU_CSx
	sprintf(cs_name, "cs%d", cs + 4);
	object_property_set_link(OBJECT(ebuc), cs_name, OBJECT(region), &error_fatal);
}

MemoryRegion *pmb887x_board_create_sdram(const char *id, uint32_t size) {
	MemoryRegion *sdram = g_new(MemoryRegion, 1);
	memory_region_init_ram(sdram, NULL, id, size, &error_fatal);
	return sdram;
}

MemoryRegion *pmb887x_board_create_nor_flash(const char *id, uint32_t vid, uint32_t pid, uint32_t offset, uint32_t *size) {
	DeviceState *flash_blk = qdev_find_recursive(sysbus_get_default(), "FULLFLASH");
	g_assert(flash_blk != NULL);

	char flash_otp0_env[32];
	char flash_otp1_env[32];
	sprintf(flash_otp0_env, "PMB887X_%s_OTP0", id);
	sprintf(flash_otp1_env, "PMB887X_%s_OTP1", id);

	const char *bank_otp0 = getenv(flash_otp0_env) ?: getenv("PMB887X_FLASH_OTP0");
	const char *bank_otp1 = getenv(flash_otp1_env) ?: getenv("PMB887X_FLASH_OTP1");

	DeviceState *flash = qdev_new("pmb887x-flash");
	flash->id = g_strdup(id);
	qdev_prop_set_string(flash, "name", id);
	qdev_prop_set_string(flash, "otp0-data", bank_otp0 ?: ""); // ESN
	qdev_prop_set_string(flash, "otp1-data", bank_otp1 ?: ""); // IMEI
	qdev_prop_set_uint16(flash, "vid", vid);
	qdev_prop_set_uint16(flash, "pid", pid);
	qdev_prop_set_uint32(flash, "offset", offset);
	object_property_set_link(OBJECT(flash), "blk", OBJECT(flash_blk), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(flash), &error_fatal);

	*size = object_property_get_uint(OBJECT(flash), "size", &error_fatal);

	return sysbus_mmio_get_region(SYS_BUS_DEVICE(flash), 0);


//	int64_t total_flash_size = pmb887x_flash_blk_size(pmb887x_flash_blk_self(flash_blk));
//	if (flash_offset != total_flash_size)
//		hw_error("Invalid fullflash size=0x%08"PRIX64". Please, specify fullflash with size=0x%08X", total_flash_size, flash_offset);
}
