/*
 * NOR FLASH (Intel/ST CFI)
 * */
#define PMB887X_TRACE_ID		FLASH
#define PMB887X_TRACE_PREFIX	"pmb887x-flash"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/ptimer.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/block/block.h"
#include "sysemu/block-backend.h"
#include "sysemu/blockdev.h"

#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/flash-blk.h"

#define TYPE_PMB887X_FLASH	"pmb887x-flash"
#define PMB887X_FLASH(obj)	OBJECT_CHECK(struct pmb887x_flash_t, (obj), TYPE_PMB887X_FLASH)

#define CFI_ADDR	0x10
#define CFI_SIZE	0x22
#define PRI_SIZE	0x50

#define OTP0_SIZE	16
#define OTP1_SIZE	0xFF

#define FLASH_VENDOR_INTEL	0x0089
#define FLASH_VENDOR_ST		0x0020

const uint8_t default_intel_cfi[] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 
	0x51, 0x52, 0x59, 0x01, 0x00, 0x0A, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x17, 0x20, 0x85, 0x95, 0x08,
	0x09, 0x0A, 0x00, 0x01, 0x01, 0x02, 0x00, 0x19,
	0x01, 0x00, 0x06, 0x00, 0x02, 0xFE, 0x00, 0x00,
	0x02, 0x03, 
};

const uint8_t default_intel_pri[] = {
	0x50, 0x52, 0x49, 0x31, 0x33, 0xE6, 0x03, 0x00,
	0x40, 0x01, 0x03, 0x00, 0x18, 0x90, 0x02, 0x80,
	0x00, 0x03, 0x03, 0x89, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x10, 0x00, 0x04, 0x03, 0x04, 0x01,
	0x02, 0x03, 0x07, 0x02, 0x0F, 0x00, 0x11, 0x00,
	0x00, 0x01, 0x0F, 0x00, 0x00, 0x02, 0x64, 0x00,
	0x02, 0x03, 0x01, 0x00, 0x11, 0x00, 0x00, 0x02,
	0x0E, 0x00, 0x00, 0x02, 0x64, 0x00, 0x02, 0x03,
	0x03, 0x00, 0x80, 0x00, 0x64, 0x00, 0x02, 0x03,
	0x10, 0x00, 0x00, 0x40, 0x30, 0xFF, 0xFF, 0xFF,
};

const uint8_t default_st_cfi[] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0x51, 0x52, 0x59, 0x00, 0x02, 0x0A, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x17, 0x20, 0x85, 0x95, 0x06,
	0x0B, 0x0A, 0x00, 0x02, 0x02, 0x02, 0x00, 0x1A,
	0x01, 0x00, 0x0A, 0x00, 0x01, 0xFF, 0x00, 0x00,
	0x04, 0xFF
};

const uint8_t default_st_pri[] = {
	0x50, 0x52, 0x49, 0x31, 0x34, 0xE6, 0x07, 0x00,
	0x00, 0x01, 0x33, 0x00, 0x18, 0x90, 0x02, 0x80,
	0x00, 0x03, 0x03, 0x89, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x10, 0x00, 0x04, 0x05, 0x03, 0x02,
	0x03, 0x07, 0x01, 0x16, 0x00, 0x08, 0x00, 0x11,
	0x00, 0x00, 0x01, 0x1F, 0x00, 0x00, 0x04, 0x64,
	0x00, 0x12, 0x03, 0x0A, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x01, 0x16, 0x00, 0x01, 0x00, 0x11, 0x00,
	0x00, 0x01, 0x03, 0x00, 0x20, 0x00, 0x64, 0x00,
	0x01, 0x03, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80,
};

struct pmb887x_flash_t;

struct pmb887x_flash_buffer_t {
	uint32_t offset;
	uint32_t value;
	uint8_t size;
};

struct pmb887x_flash_erase_region_t {
	uint32_t offset;
	uint32_t size;
	uint32_t sector;
};

struct pmb887x_flash_part_t {
	uint16_t n;
	uint32_t size;
	uint32_t offset;
	
	uint8_t wcycle;
	uint8_t cmd;
	uint32_t cmd_addr;
	uint8_t status;
	
	void *storage;
	
	uint32_t buffer_size;
	uint32_t buffer_index;
	struct pmb887x_flash_buffer_t *buffer;
	struct pmb887x_flash_erase_region_t *erase_regions;
	int erase_regions_cnt;
	
	MemoryRegion mem;
	struct pmb887x_flash_t *flash;
};

struct pmb887x_flash_t {
	SysBusDevice parent_obj;
	DeviceState *dev;
	MemoryRegion mmio;
	
	pmb887x_flash_blk_t *blk;
	
	char *name;
	
	uint16_t vid;
	uint16_t pid;
	
	uint16_t hex_otp0_lock;
	char *hex_otp0_data;
	
	uint16_t hex_otp1_lock;
	char *hex_otp1_data;
	
	uint32_t size;
	uint32_t offset;
	
	uint8_t cfi_table[CFI_ADDR + CFI_SIZE];
	
	uint16_t pri_addr;
	uint8_t pri_table[PRI_SIZE];
	
	uint16_t otp0_addr;
	uint16_t *otp0_data;
	uint16_t otp0_size;
	
	uint16_t otp1_addr;
	uint16_t *otp1_data;
	uint16_t otp1_size;
	
	uint32_t writeblock_size;
	
	uint32_t parts_n;
};

typedef struct pmb887x_flash_t pmb887x_flash_t;
typedef struct pmb887x_flash_part_t pmb887x_flash_part_t;
typedef struct pmb887x_flash_buffer_t pmb887x_flash_buffer_t;
typedef struct pmb887x_flash_erase_region_t pmb887x_flash_erase_region_t;

static void flash_trace(pmb887x_flash_t *flash, const char *format, ...) G_GNUC_PRINTF(2, 3);
static void flash_error(pmb887x_flash_t *flash, const char *format, ...) G_GNUC_PRINTF(2, 3);

static void flash_trace_part(pmb887x_flash_part_t *p, const char *format, ...) G_GNUC_PRINTF(2, 3);
static void flash_error_part(pmb887x_flash_part_t *p, const char *format, ...) G_GNUC_PRINTF(2, 3);

static void flash_reset(pmb887x_flash_part_t *p) {
	flash_trace_part(p, "back to read array mode");
	p->cmd = 0;
	p->wcycle = 0;
	memory_region_rom_device_set_romd(&p->mem, true);
}

static uint32_t flash_find_sector_size(pmb887x_flash_part_t *p, uint32_t offset) {
	offset -= p->offset;
	
	for (int i = 0; i < p->erase_regions_cnt; i++) {
		pmb887x_flash_erase_region_t *region = &p->erase_regions[i];
		if (offset >= region->offset && offset < region->offset + region->size)
			return region->sector;
	}
	
	flash_error_part(p, "[data] Unknown sector size for addr %08X\n", offset);
	exit(1);
}

/*
static uint32_t flash_data_read(pmb887x_flash_part_t *p, uint32_t offset, unsigned size) {
	uint8_t *data = p->storage;
	
	if (offset < p->offset || (offset + size) > p->offset + p->size) {
		flash_error_part(p, "[data] Unknown read addr %08X\n", offset);
		exit(1);
	}
	
	offset -= p->offset;
	
	switch (size) {
		case 1:		return data[offset];
		case 2:		return data[offset] | (data[offset + 1] << 8);
		case 4:		return data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) | (data[offset + 3] << 24);
	}
	
	flash_error_part(p, "[data] Unknown read size %d\n", size);
	exit(1);
	
    return 0;
}
*/

static void flash_data_write(pmb887x_flash_part_t *p, uint32_t offset, uint32_t value, unsigned size) {
	uint8_t *data = p->storage;
	
	if (offset < p->offset || (offset + size) > p->offset + p->size) {
		flash_error_part(p, "[data] Unknown write addr %08X [part %08X-%08X]\n", offset, p->offset, p->offset + p->size - 1);
		exit(1);
	}
	
	offset -= p->offset;
	
	switch (size) {
		case 1:
			data[offset] &= value & 0xFF;
		break;
		
		case 2:
			data[offset] &= value & 0xFF;
			data[offset + 1] &= (value >> 8) & 0xFF;
		break;
		
		case 4:
			data[offset] &= value & 0xFF;
			data[offset + 1] &= (value >> 8) & 0xFF;
			data[offset + 2] &= (value >> 16) & 0xFF;
			data[offset + 3] &= (value >> 24) & 0xFF;
		break;
		
		default:
			flash_error_part(p, "[data] Unknown write size %d\n", size);
			exit(1);
		break;
	}
}

static uint64_t flash_io_read(void *opaque, hwaddr part_offset, unsigned size) {
	pmb887x_flash_part_t *p = (pmb887x_flash_part_t *) opaque;
	
	hwaddr offset = p->offset + part_offset;
	
	uint16_t index;
	uint32_t value = 0;
	
	switch (p->cmd) {
		case 0x90:
		case 0x98:
			index = offset >> 1;
			
			if (index >= CFI_ADDR && index < CFI_ADDR + CFI_SIZE) {
				value = p->flash->cfi_table[index];
				flash_trace_part(p, "CFI %02X: %02X", index, value);
			} else if (index >= p->flash->pri_addr && index < p->flash->pri_addr + PRI_SIZE) {
				value = p->flash->pri_table[index - p->flash->pri_addr];
				flash_trace_part(p, "PRI %02X: %02X", index - p->flash->pri_addr, value);
			} else if (index >= p->flash->otp0_addr && index < p->flash->otp0_addr + p->flash->otp0_size) {
				value = p->flash->otp0_data[index - p->flash->otp0_addr];
				flash_trace_part(p, "OTP0 %02X: %04X", index - p->flash->otp0_addr, value);
			} else if (index >= p->flash->otp1_addr && index < p->flash->otp1_addr + p->flash->otp1_size) {
				value = p->flash->otp1_data[index - p->flash->otp1_addr];
				flash_trace_part(p, "OTP1 %02X: %04X", index - p->flash->otp1_addr, value);
			} else {
				switch (index) {
					case 0x00:
						value = p->flash->vid;
						flash_trace_part(p, "vendor id: %04X", value);
					break;
					
					case 0x01:
						value = p->flash->pid;
						flash_trace_part(p, "device id: %04X", value);
					break;
					
					case 0x02:
						value = 0x11;
						flash_trace_part(p, "lock status: %02X", value);
					break;
					
					case 0x05:
						value = 0xF907;
						flash_trace_part(p, "configuration register: %02X", value);
					break;
					
					case 0x06:
						value = 0x0004;
						flash_trace_part(p, "enhanced configuration register: %02X", value);
					break;
					
					default:
						value = 0xFF;
						flash_error_part(p, "%08lX: read unknown cfi index 0x%02X", offset, index);
					//	exit(1);
					break;
				}
			}
		break;
		
		case 0x20:	// Erase
		case 0x70:	// Status
		case 0xe8:	// buffered program
		case 0xE9:	// buffered program
		case 0x41:	// program word
		case 0x40:	// program word
		case 0x10:	// program word
			value = p->status;
		break;
		
		default:
			flash_error_part(p, "not implemented read for command %02X [addr: %08lX]", p->cmd, offset);
			exit(1);
		break;
	}
	
	return value;
}

static void flash_io_write(void *opaque, hwaddr part_offset, uint64_t value, unsigned size) {
	pmb887x_flash_part_t *p = (pmb887x_flash_part_t *) opaque;
	
	hwaddr offset = p->offset + part_offset;
	
	bool valid_cmd = false;
	
	if (p->wcycle == 0) {
		memory_region_rom_device_set_romd(&p->mem, false);
		
		valid_cmd = true;
		p->cmd_addr = offset;
		
		switch (value) {
			// Read
			case 0xFF:
				flash_reset(p);
			break;
			
			case 0x00:
				flash_trace_part(p, "cmd AMD probe (%02lX)", value);
				flash_reset(p);
			break;
			
			case 0xAA:
				flash_trace_part(p, "cmd AMD probe (%02lX)", value);
				flash_reset(p);
			break;
			
			case 0x55:
				flash_trace_part(p, "cmd AMD probe (%02lX)", value);
				flash_reset(p);
			break;
			
			case 0xF0:
				flash_trace_part(p, "cmd AMD probe (%02lX)", value);
				flash_reset(p);
			break;
			
			case 0x70:
				flash_trace_part(p, "cmd read status (%02lX)", value);
				p->cmd = value;
			break;
			
			case 0x90:
				flash_trace_part(p, "cmd read devid (%02lX)", value);
				p->cmd = value;
			break;
			
			case 0x98:
				flash_trace_part(p, "cmd read cfi (%02lX)", value);
				p->cmd = value;
			break;
			
			case 0x50:
				flash_trace_part(p, "cmd clear status (%02lX)", value);
				p->cmd = value;
				p->wcycle++;
			break;
			
			// Write
			case 0x41:
			case 0x40:
			case 0x10:
				flash_trace_part(p, "cmd program word (%02lX)", value);
				p->cmd = value;
				p->wcycle++;
			break;
			
			case 0xE9:
			case 0xE8:
				flash_trace_part(p, "cmd buffered program (%02lX)", value);
				p->cmd = value;
				p->wcycle++;
				p->status |= 0x80;
			break;
			
			case 0x80:
				flash_trace_part(p, "cmd buffered EFP (%02lX)", value);
				p->cmd = value;
				p->wcycle++;
			break;
			
			// Erase
			case 0x20:
				flash_trace_part(p, "cmd block erase (%02lX)", value);
				p->cmd = value;
				p->wcycle++;
				p->status |= 0x80;
			break;
			
			// Suspend
			case 0xB0:
				flash_trace_part(p, "cmd suspend (%02lX)", value);
				p->cmd = value;
				p->wcycle++;
			break;
			
			// Lock / Configuration
			case 0x60:
				flash_trace_part(p, "cmd block lock or read configuration (%02lX)", value);
				p->cmd = value;
				p->wcycle++;
			break;
			
			// Protection
			case 0xC0:
				flash_trace_part(p, "cmd protection program (%02lX)", value);
				p->cmd = value;
				p->wcycle++;
			break;
			
			default:
				flash_error_part(p, "cmd unknown (%02lX) at %08lX", value, offset);
				exit(1);
			break;
		}
	} else if (p->wcycle == 1) {
		switch (p->cmd) {
			case 0x70:	// read status
			case 0x90:	// read devid
			case 0x98:	// read cfi
				if (value == 0xFF) {
					valid_cmd = true;
					flash_reset(p);
				}
			break;
			
			case 0x60:	// lock or configuration
				if (value == 0xFF) {
					valid_cmd = true;
					flash_reset(p);
				} else if (value == 0x03) {
					valid_cmd = true;
					flash_trace_part(p, "program read configuration register (%02lX)", offset);
					flash_reset(p);
				} else if (value == 0x04) {
					valid_cmd = true;
					flash_trace_part(p, "program read enhanced configuration register (%02lX)", offset);
					flash_reset(p);
				} else if (value == 0x01) {
					valid_cmd = true;
					flash_trace_part(p, "lock block %08lX", offset);
					p->wcycle = 0;
					p->status |= 0x80;
				} else if (value == 0xD0) {
					valid_cmd = true;
					flash_trace_part(p, "unlock block %08lX", offset);
					p->wcycle = 0;
					p->status |= 0x80;
				} else if (value == 0x2F) {
					valid_cmd = true;
					flash_trace_part(p, "lock-down block %08lX", offset);
					p->wcycle = 0;
					p->status |= 0x80;
				}
			break;
			
			case 0x20:	// erase
				if (value == 0xD0) {
					uint32_t sector_size = flash_find_sector_size(p, offset);
					uint32_t mask = ~(sector_size - 1);
					uint32_t base = (p->cmd_addr & mask);
					
					flash_trace_part(p, "confirm erase block %08X...%08X (sector: %08X)", base, base + sector_size - 1, sector_size);
					
					if ((offset & mask) != (p->cmd_addr & mask)) {
						flash_error_part(p, "erase sector mismatch: %08lX != %08X\n", offset, p->cmd_addr);
						exit(1);
					}
					
					// fill sector with 0xFF's
					memset(p->storage + (base - p->offset), 0xFF, sector_size);
					
					valid_cmd = true;
					p->wcycle = 0;
					p->status |= 0x80;
				}
			break;
			
			case 0xE9:	// buffered program
			case 0xE8:	// buffered program
				valid_cmd = true;
				p->buffer_size = (value & 0xFFFF) + 1;
				p->buffer_index = 0;
				p->buffer = g_new0(pmb887x_flash_buffer_t, p->buffer_size);
				
				flash_trace_part(p, "buffered program %d words", p->buffer_size);
				
				p->wcycle++;
			break;
			
			case 0x10:	// program word
			case 0x40:	// program word
			case 0x41:	// program word
				valid_cmd = true;
				flash_trace_part(p, "program single word [%d]: %08lX to %08lX", size, value, offset);
				flash_data_write(p, offset, value, size);
				p->wcycle = 0;
				p->status |= 0x80;
			break;
		}
	} else if (p->wcycle == 2) {
		switch (p->cmd) {
			case 0xE9:	// buffered program
			case 0xE8:	// buffered program
			{
				uint32_t sector_size = flash_find_sector_size(p, offset);
				uint32_t mask = ~(sector_size - 1);
				
				valid_cmd = true;
				
				flash_trace_part(p, "program word [%d]: %08lX to %08lX", size, value, offset);
				
				if ((offset & mask) != (p->cmd_addr & mask)) {
					flash_error_part(p, "program sector mismatch: %08lX != %08X", offset, p->cmd_addr);
					exit(1);
				}
				
				if (size != 2 && size != 4) {
					flash_error_part(p, "invalid write size: %d", size);
					exit(1);
				}
				
				for (int i = 0; i < size; i += 2) {
					p->buffer[p->buffer_index].offset = offset + i;
					p->buffer[p->buffer_index].value = (value >> (i * 8)) & 0xFFFF;
					p->buffer[p->buffer_index].size = 2;
					p->buffer_index++;
					
					if (p->buffer_index == p->buffer_size)
						break;
				}
				
				if (p->buffer_index == p->buffer_size) {
					flash_trace_part(p, "buffered program finished");
					p->wcycle++;
				}
			}
			break;
		}
	} else if (p->wcycle == 3) {
		switch (p->cmd) {
			case 0xE9:	// buffered program
			case 0xE8:	// buffered program
				if (value == 0xD0) {
					for (uint32_t i = 0; i < p->buffer_size; i++)
						flash_data_write(p, p->buffer[i].offset, p->buffer[i].value, p->buffer[i].size);
					
					g_free(p->buffer);
					p->buffer = NULL;
					
					valid_cmd = true;
					flash_trace_part(p, "confirm buffered program");
					p->wcycle = 0;
					p->status |= 0x80;
				}
			break;
		}
	}
	
	if (!valid_cmd) {
		flash_error_part(p, "not implemented %d cycle for command %02X [addr: %08lX, value: %08lX]", p->wcycle, p->cmd, offset, value);
		exit(1);
	}
}

static uint64_t flash_io_unaligned_read(void *opaque, hwaddr offset, unsigned size) {
	// Native read
	if (size == 2 && (offset & 0x1) == 0)
		return flash_io_read(opaque, offset, 2);
	
	// Unaligned read
	uint32_t value = 0;
	if ((offset & 0x1) == 0) {
		for (int i = 0; i < size; i += 2)
			value |= flash_io_read(opaque, offset + i, 2) << (i * 8);
	} else {
		value |= (flash_io_read(opaque, offset - 1, 2) >> 8) & 0xFF;
		for (int i = 1; i < size; i += 2)
			value |= flash_io_read(opaque, offset + i + 1, 2) << (i * 8);
	}
	
	value &= ((1 << (size * 8)) - 1);
	// pmb887x_flash_part_t *p = (pmb887x_flash_part_t *) opaque;
	// flash_trace_part(p, "unaligned %08lX[%d] = %08lX", offset, size, value);
	return value;
}

static const MemoryRegionOps io_ops = {
	.read			= flash_io_unaligned_read,
	.write			= flash_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN
};

static uint32_t flash_size_by_id(uint32_t id) {
	switch (id) {
		case 0x0089880B:	return 0x800000; // 8M
		case 0x0089880C:	return 0x1000000; // 16M
		case 0x0089880D:	return 0x2000000; // 32M
		
		case 0x00208818:	return 0x2000000; // 32M
		case 0x00208819:	return 0x4000000; // 64M
		case 0x0020880F:	return 0x8000000; // 128M
	}
	return 0;
}

static bool fill_data_from_hex(uint8_t *dst, uint32_t max_size, const char *src_hex) {
	uint32_t len = strlen(src_hex);
	
	if (!len)
		return true;
	
	if (len % 2 != 0)
		return false;
	
	if (len > max_size * 2)
		return false;
	
	for (int i = 0; i < len; i += 2) {
		uint8_t tmp[2];
		
		for (int j = 0; j < 2; j++) {
			char c = src_hex[i + j];
			if (c >= 'A' && c <= 'F') {
				tmp[j] = (c - 'A') + 0x0A;
			} else if (c >= 'a' && c <= 'f') {
				tmp[j] = (c - 'a') + 0x0A;
			} else if (c >= '0' && c <= '9') {
				tmp[j] = c - '0';
			} else {
				// Invalid hex
				return false;
			}
		}
		
		dst[i / 2] = (tmp[0] << 4) | tmp[1];
	}
	
	return true;
}

static void flash_init_part(pmb887x_flash_t *flash, uint32_t offset, uint32_t size, pmb887x_flash_erase_region_t *erase_regions, int erase_regions_cnt) {
	pmb887x_flash_part_t *p = g_new0(pmb887x_flash_part_t, 1);
	p->n = flash->parts_n++;
	p->flash = flash;
	p->offset = offset;
	p->size = size;
	
	char *name = g_strdup_printf("pmb887x-flash[%s][%d]", p->flash->name, p->n);
	memory_region_init_rom_device(&p->mem, OBJECT(p->flash->dev), &io_ops, p, name, size, NULL);
	memory_region_rom_device_set_romd(&p->mem, true);
	memory_region_add_subregion(&flash->mmio, offset, &p->mem);
	g_free(name);
	
	p->storage = memory_region_get_ram_ptr(&p->mem);
	
	int ret = pmb887x_flash_blk_pread(p->flash->blk, offset, size, p->storage);
	if (ret < 0) {
		flash_error(p->flash, "failed to read the initial flash content [offset=%08X, size=%08X]", offset, size);
		exit(1);
	}
	
	p->erase_regions_cnt = erase_regions_cnt;
	p->erase_regions = erase_regions;
}

static void flash_realize(DeviceState *dev, Error **errp) {
	pmb887x_flash_t *flash = PMB887X_FLASH(dev);
	flash->dev = dev;
	
	flash->size = flash_size_by_id((flash->vid << 16) | flash->pid);
	
	flash_trace(flash, "FLASH %04X:%04X, 0x%08X ... 0x%08X", flash->vid, flash->pid, flash->offset, flash->offset + flash->size - 1);
	
	char *mmio_name = g_strdup_printf("pmb887x-flash[%s]", flash->name);
	memory_region_init(&flash->mmio, OBJECT(flash->dev), mmio_name, flash->size);
	g_free(mmio_name);
	
	// Init default values for all tables
	memset(flash->cfi_table, 0xFF, sizeof(flash->cfi_table));
	memset(flash->pri_table, 0xFF, sizeof(flash->pri_table));
	
	if (flash->vid == FLASH_VENDOR_INTEL) {
		memcpy(flash->cfi_table, default_intel_cfi, sizeof(default_intel_cfi));
		memcpy(flash->pri_table, default_intel_pri, sizeof(default_intel_pri));
		
		// Override size
		flash->cfi_table[0x27] = ctz32(flash->size);
		
		if (flash->pid == 0x880B) { // 8M
			flash->cfi_table[0x31] = 0x3E;
			flash->pri_table[0x24] = 0x07;
			flash->pri_table[0x2A] = 0x07;
			flash->pri_table[0x38] = 0x06;
		} else if (flash->pid == 0x880C) { // 16M
			flash->cfi_table[0x31] = 0x7E;
			flash->pri_table[0x24] = 0x0F;
			flash->pri_table[0x2A] = 0x07;
			flash->pri_table[0x38] = 0x06;
		} else if (flash->pid == 0x880D) { // 32M
			flash->cfi_table[0x31] = 0xFE;
			flash->pri_table[0x24] = 0x0F;
			flash->pri_table[0x2A] = 0x0F;
			flash->pri_table[0x38] = 0x0E;
		} else {
			flash_error(flash, "unimplemented %04X:%04X", flash->vid, flash->pid);
			exit(1);
		}
	} else if (flash->vid == FLASH_VENDOR_ST) {
		memcpy(flash->cfi_table, default_st_cfi, sizeof(default_st_cfi));
		memcpy(flash->pri_table, default_st_pri, sizeof(default_st_pri));
		
		// Override size
		flash->cfi_table[0x27] = ctz32(flash->size);
		
		if (flash->pid == 0x8818) { // 32M
			flash->cfi_table[0x2D] = 0x7F;
			flash->cfi_table[0x2E] = 0x00;
		} else if (flash->pid == 0x8819) { // 64M
			flash->cfi_table[0x2D] = 0xFF;
			flash->cfi_table[0x2E] = 0x00;
		} else if (flash->pid == 0x880F) { // 128M
			flash->cfi_table[0x2D] = 0xFF;
			flash->cfi_table[0x2E] = 0x01;
		} else {
			flash_error(flash, "unimplemented %04X:%04X", flash->vid, flash->pid);
			exit(1);
		}
	} else {
		flash_error(flash, "unimplemented %04X:%04X", flash->vid, flash->pid);
		exit(1);
	}
	
	flash->pri_addr = (flash->cfi_table[0x16] << 8) | flash->cfi_table[0x15];
	flash->writeblock_size = 1 << flash->cfi_table[0x2A];
	
	// OTP0
	uint32_t otp0_size_f = 1 << flash->pri_table[0x11];
	uint32_t otp0_size_u = 1 << flash->pri_table[0x12];
	flash->otp0_addr = (flash->pri_table[0x10] << 8) | flash->pri_table[0x0F];
	flash->otp0_size = MAX(otp0_size_f, otp0_size_u) / 2 + 1;
	flash->otp0_data = g_new(uint16_t, flash->otp0_size);
	memset(flash->otp0_data, 0xFFFF, flash->otp0_size * 2);
	flash->otp0_data[0] = 0x0002;
	if (!fill_data_from_hex((uint8_t *) flash->otp0_data, flash->otp0_size * 2, flash->hex_otp0_data)) {
		flash_error(flash, "Invalid OTP0 hex data: %s [max_size=%d, len=%ld]", flash->hex_otp0_data, flash->otp0_size * 2, strlen(flash->hex_otp0_data) / 2);
		exit(1);
	}
	
	// OTP1
	uint32_t otp1_groups_f = (flash->pri_table[0x18] << 8) | flash->pri_table[0x17];
	uint32_t otp1_groups_u = (flash->pri_table[0x1B] << 8) | flash->pri_table[0x1A];
	uint32_t otp1_size_f = 1 << flash->pri_table[0x19];
	uint32_t otp1_size_u = 1 << flash->pri_table[0x1C];
	flash->otp1_addr = (flash->pri_table[0x16] << 24) | (flash->pri_table[0x15] << 16) | (flash->pri_table[0x14] << 8) | flash->pri_table[0x13];
	flash->otp1_size = MAX(otp1_groups_f * otp1_size_f, otp1_groups_u * otp1_size_u) / 2 + 1;
	flash->otp1_data = g_new(uint16_t, flash->otp1_size);
	memset(flash->otp1_data, 0xFFFF, flash->otp1_size * 2);
	flash->otp1_data[0] = 0xFFFF;
	if (!fill_data_from_hex((uint8_t *) flash->otp1_data, flash->otp1_size * 2, flash->hex_otp1_data)) {
		flash_error(flash, "Invalid OTP1 hex data: %s [max_size=%d, len=%ld]", flash->hex_otp1_data, flash->otp1_size * 2, strlen(flash->hex_otp1_data) / 2);
		exit(1);
	}
	
	if (flash->vid == FLASH_VENDOR_INTEL) {
		// Partitions
		uint32_t info_offset = 0;
		uint32_t flash_offset = 0;
		
		int hw_regions = flash->pri_table[0x23];
		for (int i = 0; i < hw_regions; i++) {
			int identical_cnt = (flash->pri_table[info_offset + 0x25] << 8) | flash->pri_table[info_offset + 0x24];
			
			uint32_t part_size = 0;
			int erase_regions_cnt = flash->pri_table[info_offset + 0x29];
			pmb887x_flash_erase_region_t *erase_regions = g_new0(pmb887x_flash_erase_region_t, erase_regions_cnt);
			
			for (int j = 0; j < erase_regions_cnt; j++) {
				uint32_t blocks = ((flash->pri_table[info_offset + 0x2B + j * 8] << 8) | flash->pri_table[info_offset + 0x2A + j * 8]) + 1;
				uint32_t sector_len = (flash->pri_table[info_offset + 0x2D + j * 8] << 16) | (flash->pri_table[info_offset + 0x2C + j * 8] << 8);
				
				erase_regions[j].offset = part_size;
				erase_regions[j].size = sector_len * blocks;
				erase_regions[j].sector = sector_len;
				
				part_size += sector_len * blocks;
			}
			
			for (int j = 0; j < identical_cnt; j++) {
				flash_init_part(flash, flash_offset, part_size, erase_regions, erase_regions_cnt);
				flash_offset += part_size;
			}
			
			info_offset += 0x6 + 0x8 * erase_regions_cnt;
		}
		
		if (flash_offset != flash->size) {
			flash_error(flash, "invalid PRI table");
			exit(1);
		}
	} else if (flash->vid == FLASH_VENDOR_ST) {
		// Partitions
		uint32_t info_offset = 0;
		uint32_t flash_offset = 0;
		
		int hw_regions = flash->pri_table[0x22];
		for (int i = 0; i < hw_regions; i++) {
			int self_size = (flash->pri_table[info_offset + 0x24] << 8) | flash->pri_table[info_offset + 0x23];
			int identical_cnt = (flash->pri_table[info_offset + 0x26] << 8) | flash->pri_table[info_offset + 0x25];
			
			uint32_t part_size = 0;
			int erase_regions_cnt = flash->pri_table[info_offset + 0x2A];
			pmb887x_flash_erase_region_t *erase_regions = g_new0(pmb887x_flash_erase_region_t, erase_regions_cnt);
			
			for (int j = 0; j < erase_regions_cnt; j++) {
				uint32_t blocks = ((flash->pri_table[info_offset + 0x2C + j * 8] << 8) | flash->pri_table[info_offset + 0x2B + j * 8]) + 1;
				uint32_t sector_len = (flash->pri_table[info_offset + 0x2E + j * 8] << 16) | (flash->pri_table[info_offset + 0x2D + j * 8] << 8);
				
				erase_regions[j].offset = part_size;
				erase_regions[j].size = sector_len * blocks;
				erase_regions[j].sector = sector_len;
				
				part_size += sector_len * blocks;
			}
			
			for (int j = 0; j < identical_cnt; j++) {
				flash_init_part(flash, flash_offset, part_size, erase_regions, erase_regions_cnt);
				flash_offset += part_size;
			}
			
			info_offset += self_size;
		}
		
		if (flash_offset != flash->size) {
			flash_error(flash, "invalid PRI table");
			exit(1);
		}
	}
	
	sysbus_init_mmio(SYS_BUS_DEVICE(flash->dev), &flash->mmio);
}

static void flash_error(pmb887x_flash_t *flash, const char *format, ...) {
	g_autoptr(GString) s = g_string_new("");
	
	va_list args;
	va_start(args, format);
	g_string_append_vprintf(s, format, args);
	va_end(args);
	
	error_report("[%s] %s\n", PMB887X_TRACE_PREFIX, s->str);
}

static void flash_error_part(pmb887x_flash_part_t *p, const char *format, ...) {
	g_autoptr(GString) s = g_string_new("");
	
	va_list args;
	va_start(args, format);
	g_string_append_vprintf(s, format, args);
	va_end(args);
	
	error_report("[%s] <%d> %s\n", PMB887X_TRACE_PREFIX, p->n, s->str);
}

static void flash_trace_part(pmb887x_flash_part_t *p, const char *format, ...) {
	if (!pmb887x_trace_log_enabled(PMB887X_TRACE_FLASH))
		return;
	
	g_autoptr(GString) s = g_string_new("");
	
	va_list args;
	va_start(args, format);
	g_string_append_vprintf(s, format, args);
	va_end(args);
	
	qemu_log_mask(LOG_TRACE, "[%s] <%d> %s\n", PMB887X_TRACE_PREFIX, p->n, s->str);
}

static void flash_trace(pmb887x_flash_t *flash, const char *format, ...) {
	if (!pmb887x_trace_log_enabled(PMB887X_TRACE_FLASH))
		return;
	
	g_autoptr(GString) s = g_string_new("");
	
	va_list args;
	va_start(args, format);
	g_string_append_vprintf(s, format, args);
	va_end(args);
	
	qemu_log_mask(LOG_TRACE, "[%s] %s\n", PMB887X_TRACE_PREFIX, s->str);
}

static Property flash_properties[] = {
	DEFINE_PROP_LINK("blk", struct pmb887x_flash_t, blk, "pmb887x-flash-blk", struct pmb887x_flash_blk_t *),
	
	DEFINE_PROP_STRING("name", pmb887x_flash_t, name),
	DEFINE_PROP_UINT16("vid", pmb887x_flash_t, vid, 0),
	DEFINE_PROP_UINT16("pid", pmb887x_flash_t, pid, 0),
	DEFINE_PROP_UINT32("offset", pmb887x_flash_t, offset, 0),
	DEFINE_PROP_UINT32("size", pmb887x_flash_t, size, 0),
	
	/* OTP0 Initial Data */
	DEFINE_PROP_STRING("otp0-data", pmb887x_flash_t, hex_otp0_data),
	
	/* OTP1 Initial Data */
	DEFINE_PROP_STRING("otp1-data", pmb887x_flash_t, hex_otp1_data),
	
	DEFINE_PROP_END_OF_LIST(),
};

static void flash_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, flash_properties);
	dc->realize = flash_realize;
	set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo flash_info = {
    .name          	= TYPE_PMB887X_FLASH,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_flash_t),
    .class_init    	= flash_class_init,
};

static void flash_register_types(void) {
	type_register_static(&flash_info);
}
type_init(flash_register_types)
