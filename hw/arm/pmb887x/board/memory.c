#include "hw/arm/pmb887x/board/memory.h"

#include "hw/core/sysbus.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
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

	g_autofree char *otp0_env = g_strdup_printf("PMB887X_%s_OTP0", id);
	g_autofree char *otp1_env = g_strdup_printf("PMB887X_%s_OTP1", id);
	g_autofree char *otp0_file_env = g_strdup_printf("PMB887X_%s_OTP0_FILE", id);
	g_autofree char *otp1_file_env = g_strdup_printf("PMB887X_%s_OTP1_FILE", id);
	g_autofree char *efa_file_env = g_strdup_printf("PMB887X_%s_EFA_FILE", id);
	DeviceState *flash = qdev_new("pmb887x-flash");
	flash->id = g_strdup(id);
	qdev_prop_set_string(flash, "name", id);
	qdev_prop_set_string(flash, "otp0-data", g_getenv(otp0_env) ?: ""); // ESN
	qdev_prop_set_string(flash, "otp1-data", g_getenv(otp1_env) ?: ""); // IMEI
	qdev_prop_set_string(flash, "otp0-file", g_getenv(otp0_file_env) ?: "");
	qdev_prop_set_string(flash, "otp1-file", g_getenv(otp1_file_env) ?: "");
	qdev_prop_set_string(flash, "efa-file", g_getenv(efa_file_env) ?: "");
	qdev_prop_set_uint16(flash, "vid", vid);
	qdev_prop_set_uint16(flash, "pid", pid);
	qdev_prop_set_uint32(flash, "offset", offset);
	object_property_set_link(OBJECT(flash), "blk", OBJECT(flash_blk), &error_fatal);
	sysbus_realize_and_unref(SYS_BUS_DEVICE(flash), &error_fatal);

	*size = object_property_get_uint(OBJECT(flash), "size", &error_fatal);

	return sysbus_mmio_get_region(SYS_BUS_DEVICE(flash), 0);
}
