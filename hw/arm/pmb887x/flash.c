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
#include "system/block-backend.h"
#include "system/blockdev.h"

#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/flash.h"
#include "hw/arm/pmb887x/flash-blk.h"

#define TYPE_PMB887X_FLASH	"pmb887x-flash"
#define PMB887X_FLASH(obj)	OBJECT_CHECK(struct pmb887x_flash_t, (obj), TYPE_PMB887X_FLASH)

#define CFI_ADDR	0x10

struct pmb887x_flash_t;

struct pmb887x_flash_buffer_t {
	uint32_t offset;
	uint32_t value;
	uint8_t size;
};

struct pmb887x_flash_block_t {
	uint32_t offset;
	uint32_t size;
	bool locked;
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
	const pmb887x_flash_cfg_part_t *cfg;
	
	uint32_t blocks_n;
	struct pmb887x_flash_block_t *blocks;
	
	MemoryRegion mem;
	struct pmb887x_flash_t *flash;
};

struct pmb887x_flash_t {
	SysBusDevice parent_obj;
	DeviceState *dev;
	MemoryRegion mmio;
	
	pmb887x_flash_blk_t *blk;
	const pmb887x_flash_cfg_t *cfg;
	
	char *name;
	
	uint16_t vid;
	uint16_t pid;
	
	uint16_t hex_otp0_lock;
	char *hex_otp0_data;
	
	uint16_t hex_otp1_lock;
	char *hex_otp1_data;
	
	uint32_t size;
	uint32_t offset;
	
	uint16_t *otp0_data;
	uint16_t *otp1_data;
	
	uint32_t parts_n;
};

typedef struct pmb887x_flash_t pmb887x_flash_t;
typedef struct pmb887x_flash_part_t pmb887x_flash_part_t;
typedef struct pmb887x_flash_buffer_t pmb887x_flash_buffer_t;
typedef struct pmb887x_flash_block_t pmb887x_flash_block_t;

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

static pmb887x_flash_block_t *flash_part_find_block(pmb887x_flash_part_t *p, uint32_t offset) {
	offset -= p->offset;
	for (uint32_t i = 0; i < p->blocks_n; i++) {
		pmb887x_flash_block_t *blk = &p->blocks[i];
		if (offset >= blk->offset && offset < blk->offset + blk->size)
			return blk;
	}
	flash_error_part(p, "[data] Unknown addr %08X\n", p->flash->offset + p->offset + offset);
	exit(1);
	return NULL;
}

static uint32_t flash_find_sector_size(pmb887x_flash_part_t *p, uint32_t offset) {
	offset -= p->offset;
	
	for (int i = 0; i < p->cfg->erase_regions_cnt; i++) {
		const pmb887x_flash_erase_region_t *region = &p->cfg->erase_regions[i];
		if (offset >= region->offset && offset < region->offset + region->size)
			return region->sector;
	}
	
	flash_error_part(p, "[data] Unknown sector size for addr %08X\n", p->flash->offset + p->offset + offset);
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
	
	if (pmb887x_flash_blk_is_rw(p->flash->blk)) {
		int ret = pmb887x_flash_blk_pwrite(p->flash->blk, p->flash->offset + p->offset + offset, size, p->storage + offset);
		if (ret < 0) {
			flash_error_part(p, "Can't read to flash file: %d, %s\n", ret, strerror(ret));
			exit(1);
		}
	}
}

static uint64_t flash_io_read(void *opaque, hwaddr part_offset, unsigned size) {
	pmb887x_flash_part_t *p = (pmb887x_flash_part_t *) opaque;
	const pmb887x_flash_cfg_t *cfg = p->flash->cfg;
	
	hwaddr offset = p->offset + part_offset;
	
	uint16_t index;
	uint32_t value = 0;
	
	switch (p->cmd) {
		case 0x90:
		case 0x98:
			index = offset >> 1;
			
			// CFI
			if (index >= CFI_ADDR && index < CFI_ADDR + cfg->cfi_size) {
				value = cfg->cfi[index - CFI_ADDR];
				flash_trace_part(p, "CFI %02X: %02X", index, value);
			}
			// PRI
			else if (index >= cfg->pri_addr && index < cfg->pri_addr + cfg->pri_size) {
				value = cfg->pri[index - cfg->pri_addr];
				flash_trace_part(p, "PRI %02X: %02X", index - cfg->pri_addr, value);
			}
			// OTP0
			else if (index >= cfg->otp0_addr && index < cfg->otp0_addr + (cfg->otp0_size / 2)) {
				value = p->flash->otp0_data[index - cfg->otp0_addr];
				flash_trace_part(p, "OTP0 %02X: %04X", index - cfg->otp0_addr, value);
			}
			// OTP1
			else if (index >= cfg->otp1_addr && index < cfg->otp1_addr + (cfg->otp1_size / 2)) {
				value = p->flash->otp1_data[index - cfg->otp1_addr];
				flash_trace_part(p, "OTP1 %02X: %04X", index - cfg->otp1_addr, value);
			}
			// Other info
			else {
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
					{
						pmb887x_flash_block_t *blk = flash_part_find_block(p, offset);
						value = blk->locked ? cfg->lock : 0;
						flash_trace_part(p, "lock status: %02X", value);
					}
					break;
					
					case 0x05:
						value = cfg->cr;
						flash_trace_part(p, "configuration register: %02X", value);
					break;
					
					case 0x06:
						value = cfg->ehcr;
						flash_trace_part(p, "enhanced configuration register: %02X", value);
					break;
					
					default:
						value = 0xFFFF;
						flash_error_part(p, "%08"PRIX64": read unknown cfi index 0x%02X", offset, index);
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
			flash_error_part(p, "not implemented read for command %02X [addr: %08"PRIX64"]", p->cmd, offset);
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
				flash_trace_part(p, "cmd AMD probe (%02"PRIX64")", value);
				flash_reset(p);
			break;
			
			case 0xAA:
				flash_trace_part(p, "cmd AMD probe (%02"PRIX64")", value);
				flash_reset(p);
			break;
			
			case 0x55:
				flash_trace_part(p, "cmd AMD probe (%02"PRIX64")", value);
				flash_reset(p);
			break;
			
			case 0xF0:
				flash_trace_part(p, "cmd AMD probe (%02"PRIX64")", value);
				flash_reset(p);
			break;
			
			case 0x70:
				flash_trace_part(p, "cmd read status (%02"PRIX64")", value);
				p->cmd = value;
			break;
			
			case 0x90:
				flash_trace_part(p, "cmd read devid (%02"PRIX64")", value);
				p->cmd = value;
			break;
			
			case 0x98:
				flash_trace_part(p, "cmd read cfi (%02"PRIX64")", value);
				p->cmd = value;
			break;
			
			case 0x50:
				flash_trace_part(p, "cmd clear status (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
			break;
			
			// Write
			case 0x41:
			case 0x40:
			case 0x10:
				flash_trace_part(p, "cmd program word (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
			break;
			
			case 0xE9:
			case 0xE8:
				flash_trace_part(p, "cmd buffered program (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
				p->status |= 0x80;
			break;
			
			case 0x80:
				flash_trace_part(p, "cmd buffered EFP (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
			break;
			
			// Erase
			case 0x20:
				flash_trace_part(p, "cmd block erase (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
				p->status |= 0x80;
			break;
			
			// Suspend
			case 0xB0:
				flash_trace_part(p, "cmd suspend (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
			break;
			
			// Lock / Configuration
			case 0x60:
				flash_trace_part(p, "cmd block lock or read configuration (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
			break;
			
			// Protection
			case 0xC0:
				flash_trace_part(p, "cmd protection program (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
			break;
			
			default:
				flash_error_part(p, "cmd unknown (%02"PRIX64") at %08"PRIX64"", value, p->flash->offset + offset);
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
					flash_trace_part(p, "program read configuration register (%02"PRIX64")", p->flash->offset + offset);
					flash_reset(p);
				} else if (value == 0x04) {
					valid_cmd = true;
					flash_trace_part(p, "program read enhanced configuration register (%02"PRIX64")", p->flash->offset + offset);
					flash_reset(p);
				} else if (value == 0x01) {
					valid_cmd = true;
					
					flash_trace_part(p, "lock block %08"PRIX64"", p->flash->offset + offset);
					pmb887x_flash_block_t *blk = flash_part_find_block(p, offset);
					blk->locked = true;
					
					p->wcycle = 0;
					p->status |= 0x80;
				} else if (value == 0xD0) {
					valid_cmd = true;
					
					flash_trace_part(p, "unlock block %08"PRIX64"", p->flash->offset + offset);
					pmb887x_flash_block_t *blk = flash_part_find_block(p, offset);
					blk->locked = false;
					
					p->wcycle = 0;
					p->status |= 0x80;
				} else if (value == 0x2F) {
					valid_cmd = true;
					flash_trace_part(p, "lock-down block %08"PRIX64"", p->flash->offset + offset);
					p->wcycle = 0;
					p->status |= 0x80;
				}
			break;
			
			case 0x20:	// erase
				if (value == 0xD0) {
					uint32_t sector_size = flash_find_sector_size(p, offset);
					uint32_t mask = ~(sector_size - 1);
					uint32_t base = (p->cmd_addr & mask);
					
					flash_trace_part(p, "confirm erase block %08X...%08X (sector: %08X)", p->flash->offset + base, p->flash->offset + base + sector_size - 1, sector_size);
					
					if ((offset & mask) != (p->cmd_addr & mask)) {
						flash_error_part(p, "erase sector mismatch: %08"PRIX64" != %08X\n", p->flash->offset + offset, p->flash->offset + p->cmd_addr);
						exit(1);
					}
					
					// fill sector with 0xFF's
					uint32_t erase_offset = (base - p->offset);
					memset(p->storage + erase_offset, 0xFF, sector_size);
					
					if (pmb887x_flash_blk_is_rw(p->flash->blk)) {
						int ret = pmb887x_flash_blk_pwrite(p->flash->blk, p->flash->offset + p->offset + erase_offset, sector_size, p->storage + erase_offset);
						if (ret < 0) {
							flash_error_part(p, "Can't read to flash file: %d, %s\n", ret, strerror(ret));
							exit(1);
						}
					}
					
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
				flash_trace_part(p, "program single word [%d]: %08"PRIX64" to %08"PRIX64"", size, value, p->flash->offset + offset);
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
				
				flash_trace_part(p, "program word [%d]: %08"PRIX64" to %08"PRIX64"", size, value, p->flash->offset + offset);
				
				if ((offset & mask) != (p->cmd_addr & mask)) {
					flash_error_part(p, "program sector mismatch: %08"PRIX64" != %08X", offset, p->cmd_addr);
				//	exit(1);
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
		flash_error_part(p, "not implemented %d cycle for command %02X [addr: %08"PRIX64", value: %08"PRIX64"]", p->wcycle, p->cmd, p->flash->offset + offset, value);
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
	// flash_trace_part(p, "unaligned %08"PRIX64"[%d] = %08"PRIX64"", p->flash->offset + offset, size, value);
	return value;
}

static const MemoryRegionOps io_ops = {
	.read			= flash_io_unaligned_read,
	.write			= flash_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN
};

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

static void flash_init_part(pmb887x_flash_t *flash, const pmb887x_flash_cfg_part_t *part_cfg) {
	pmb887x_flash_part_t *p = g_new0(pmb887x_flash_part_t, 1);
	p->n = flash->parts_n++;
	p->flash = flash;
	p->offset = part_cfg->offset;
	p->size = part_cfg->size;
	p->cfg = part_cfg;
	
	char *name = g_strdup_printf("pmb887x-flash[%s][%d]", p->flash->name, p->n);
	memory_region_init_rom_device(&p->mem, OBJECT(p->flash->dev), &io_ops, p, name, p->size, NULL);
	memory_region_rom_device_set_romd(&p->mem, true);
	memory_region_add_subregion(&flash->mmio, p->offset, &p->mem);
	g_free(name);
	
	p->storage = memory_region_get_ram_ptr(&p->mem);
	
	flash_trace_part(p, "hw partition 0x%08X ... 0x%08X", p->flash->offset + p->offset, p->flash->offset + p->offset + p->size - 1);
	
	int ret = pmb887x_flash_blk_pread(p->flash->blk, flash->offset + p->offset, p->size, p->storage);
	if (ret < 0) {
		flash_error(p->flash, "failed to read the initial flash content [offset=%08X, size=%08X]", p->flash->offset + p->offset, p->size);
		exit(1);
	}
	
	p->blocks_n = 0;
	for (uint32_t i = 0; i < p->cfg->erase_regions_cnt; i++)
		p->blocks_n += p->cfg->erase_regions[i].size / p->cfg->erase_regions[i].sector;
	
	p->blocks = g_new0(pmb887x_flash_block_t, p->blocks_n);
	
	uint32_t block_id = 0;
	uint32_t block_offset = 0;
	
	for (uint32_t i = 0; i < p->cfg->erase_regions_cnt; i++) {
		const pmb887x_flash_erase_region_t *region = &p->cfg->erase_regions[i];
		uint32_t sectors = region->size / region->sector;
		for (uint32_t j = 0; j < sectors; j++) {
			p->blocks[block_id].offset = block_offset;
			p->blocks[block_id].size = region->sector;
			p->blocks[block_id].locked = true;
			block_offset += region->sector;
			block_id++;
		}
	}
}

static void flash_realize(DeviceState *dev, Error **errp) {
	pmb887x_flash_t *flash = PMB887X_FLASH(dev);
	flash->dev = dev;
	
	const pmb887x_flash_cfg_t *cfg = pmb887x_flash_find(flash->vid, flash->pid);
	if (!cfg) {
		flash_error(flash, "unimplemented %04X:%04X", flash->vid, flash->pid);
		exit(1);
	}
	
	flash->cfg = cfg;
	flash->size = cfg->size;
	
	flash_trace(flash, "FLASH %04X:%04X, 0x%08X ... 0x%08X", flash->vid, flash->pid, flash->offset, flash->offset + flash->size - 1);
	
	char *mmio_name = g_strdup_printf("pmb887x-flash[%s]", flash->name);
	memory_region_init(&flash->mmio, OBJECT(flash->dev), mmio_name, flash->size);
	g_free(mmio_name);
	
	// OTP0
	if (cfg->otp0_size > 0) {
		flash->otp0_data = g_new(uint16_t, cfg->otp0_size / 2);
		memset(flash->otp0_data, 0xFF, cfg->otp0_size);
		flash->otp0_data[0] = 0x0002;
		
		if (!fill_data_from_hex((uint8_t *) flash->otp0_data, cfg->otp0_size, flash->hex_otp0_data)) {
			flash_error(flash, "Invalid OTP0 hex data: %s [max_size=%d, len=%"PRId64"d]", flash->hex_otp0_data, cfg->otp0_size, strlen(flash->hex_otp0_data) / 2);
			exit(1);
		}
	}
	
	// OTP1
	if (cfg->otp1_size > 0) {
		flash->otp1_data = g_new(uint16_t, cfg->otp1_size / 2);
		memset(flash->otp1_data, 0xFF, cfg->otp1_size);
		flash->otp1_data[0] = 0xFFFF;
		
		if (!fill_data_from_hex((uint8_t *) flash->otp1_data, cfg->otp1_size, flash->hex_otp1_data)) {
			flash_error(flash, "Invalid OTP1 hex data: %s [max_size=%d, len=%"PRId64"d]", flash->hex_otp1_data, cfg->otp1_size, strlen(flash->hex_otp1_data) / 2);
			exit(1);
		}
	}
	
	// Init hw partitions
	for (size_t i = 0; i < cfg->parts_count; i++)
		flash_init_part(flash, &cfg->parts[i]);
	
	sysbus_init_mmio(SYS_BUS_DEVICE(flash->dev), &flash->mmio);
}

static void flash_error(pmb887x_flash_t *flash, const char *format, ...) {
	g_autoptr(GString) s = g_string_new("");
	
	va_list args;
	va_start(args, format);
	g_string_append_vprintf(s, format, args);
	va_end(args);
	
	error_report("[%s] %s %s\n", PMB887X_TRACE_PREFIX, flash->name, s->str);
}

static void flash_error_part(pmb887x_flash_part_t *p, const char *format, ...) {
	g_autoptr(GString) s = g_string_new("");
	
	va_list args;
	va_start(args, format);
	g_string_append_vprintf(s, format, args);
	va_end(args);
	
	error_report("[%s] %s <%d> %s\n", PMB887X_TRACE_PREFIX, p->flash->name, p->n, s->str);
}

static void flash_trace_part(pmb887x_flash_part_t *p, const char *format, ...) {
	if (!pmb887x_trace_log_enabled(PMB887X_TRACE_FLASH))
		return;
	
	g_autoptr(GString) s = g_string_new("");
	
	va_list args;
	va_start(args, format);
	g_string_append_vprintf(s, format, args);
	va_end(args);
	
	qemu_log_mask(LOG_TRACE, "[%s] %s <%d> %s\n", PMB887X_TRACE_PREFIX, p->flash->name, p->n, s->str);
}

static void flash_trace(pmb887x_flash_t *flash, const char *format, ...) {
	if (!pmb887x_trace_log_enabled(PMB887X_TRACE_FLASH))
		return;
	
	g_autoptr(GString) s = g_string_new("");
	
	va_list args;
	va_start(args, format);
	g_string_append_vprintf(s, format, args);
	va_end(args);
	
	qemu_log_mask(LOG_TRACE, "[%s] %s %s\n", PMB887X_TRACE_PREFIX, flash->name, s->str);
}

static const Property flash_properties[] = {
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
