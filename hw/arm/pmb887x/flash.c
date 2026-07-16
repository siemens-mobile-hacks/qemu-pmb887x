/*
 * NOR FLASH (Intel/ST CFI)
 * */
#define PMB887X_TRACE_ID		FLASH
#define PMB887X_TRACE_PREFIX	"pmb887x-flash"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "cpu.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/core/qdev-properties.h"
#include "hw/block/block.h"

#include "hw/arm/pmb887x/trace.h"
#include "hw/arm/pmb887x/flash.h"
#include "hw/arm/pmb887x/flash-blk.h"

#define TYPE_PMB887X_FLASH	"pmb887x-flash"
#define PMB887X_FLASH(obj)	OBJECT_CHECK(pmb887x_flash_t, (obj), TYPE_PMB887X_FLASH)

#define CFI_ADDR	0x10
#define CFI_INDEX_MASK	0xFFF

#define FLASH_STATUS_BLOCK_LOCKED		BIT(1)
#define FLASH_STATUS_PROGRAM_ERROR		BIT(4)
#define FLASH_STATUS_BLOCK_ERASE_ERROR	BIT(5)
#define FLASH_STATUS_READY				BIT(7)
#define FLASH_STATUS_ERRORS				(BIT(1) | BIT(3) | BIT(4) | BIT(5) | BIT(8) | BIT(9))

#define FLASH_LOCK_STATUS_LOCKED		BIT(0)
#define FLASH_LOCK_STATUS_LOCKED_DOWN	BIT(1)
#define FLASH_EFA_LOCK_STATUS_LOCKED		BIT(4)
#define FLASH_EFA_LOCK_STATUS_LOCKED_DOWN	BIT(5)

typedef struct pmb887x_flash_t pmb887x_flash_t;
typedef struct pmb887x_flash_part_t pmb887x_flash_part_t;
typedef struct pmb887x_flash_buffer_t pmb887x_flash_buffer_t;
typedef struct pmb887x_flash_block_t pmb887x_flash_block_t;

struct pmb887x_flash_buffer_t {
	uint32_t offset;
	uint32_t value;
	uint8_t size;
};

struct pmb887x_flash_block_t {
	uint32_t offset;
	uint32_t size;
	bool locked;
	bool locked_down;
};

struct pmb887x_flash_part_t {
	uint16_t n;
	uint32_t size;
	uint32_t offset;
	
	uint8_t wcycle;
	uint8_t cmd;
	uint32_t cmd_addr;
	uint16_t status;
	
	uint8_t *storage;
	
	uint32_t buffer_size;
	uint32_t buffer_index;
	pmb887x_flash_buffer_t *buffer;
	const pmb887x_flash_cfg_part_t *cfg;
	
	uint32_t blocks_n;
	pmb887x_flash_block_t *blocks;
	
	MemoryRegion mem;
	pmb887x_flash_t *flash;
};

struct pmb887x_flash_t {
	SysBusDevice parent_obj;
	DeviceState *dev;
	MemoryRegion mmio;
	
	pmb887x_flash_blk_t *blk;
	const pmb887x_flash_cfg_t *cfg;
	
	char *name;
	char *otp0_file;
	char *otp1_file;
	char *efa_file;
	
	uint16_t vid;
	uint16_t pid;
	
	char *hex_otp0_data;
	char *hex_otp1_data;
	
	uint32_t size;
	uint32_t offset;
	
	uint16_t *otp0_data;
	uint16_t *otp1_data;
	uint8_t *efa_storage;
	uint32_t efa_size;
	uint32_t efa_blocks_n;
	pmb887x_flash_block_t *efa_blocks;
	
	uint32_t parts_n;
	pmb887x_flash_part_t **parts;
};

static void flash_trace(pmb887x_flash_t *flash, const char *format, ...) G_GNUC_PRINTF(2, 3);
static void flash_error(pmb887x_flash_t *flash, const char *format, ...) G_GNUC_PRINTF(2, 3);

static void flash_trace_part(pmb887x_flash_part_t *p, const char *format, ...) G_GNUC_PRINTF(2, 3);
static void flash_error_part(pmb887x_flash_part_t *p, const char *format, ...) G_GNUC_PRINTF(2, 3);

static void flash_load_file(pmb887x_flash_t *flash, const char *path, void *data, size_t size, const char *region);
static void flash_save_file(pmb887x_flash_t *flash, const char *path, const void *data, size_t size, const char *region);

static void flash_reset(pmb887x_flash_part_t *p) {
	flash_trace_part(p, "back to read array mode");
	g_clear_pointer(&p->buffer, g_free);
	p->buffer_size = 0;
	p->buffer_index = 0;
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
	flash_error_part(p, "[data] Unknown addr %08X", p->flash->offset + p->offset + offset);
	exit(1);
}

static pmb887x_flash_block_t *flash_find_efa_block(pmb887x_flash_part_t *p, uint32_t offset) {
	offset -= p->offset;
	for (uint32_t i = 0; i < p->flash->efa_blocks_n; i++) {
		pmb887x_flash_block_t *blk = &p->flash->efa_blocks[i];
		if (offset >= blk->offset && offset < blk->offset + blk->size)
			return blk;
	}
	flash_error_part(p, "[EFA] Unknown addr %08X", p->flash->offset + p->offset + offset);
	exit(1);
}

static uint32_t flash_find_sector_size(pmb887x_flash_part_t *p, uint32_t offset) {
	offset -= p->offset;
	
	for (uint32_t i = 0; i < p->cfg->erase_regions_cnt; i++) {
		const pmb887x_flash_erase_region_t *region = &p->cfg->erase_regions[i];
		if (offset >= region->offset && offset < region->offset + region->size)
			return region->sector;
	}
	
	flash_error_part(p, "[data] Unknown sector size for addr %08X", p->flash->offset + p->offset + offset);
	exit(1);
}

static void flash_data_write(pmb887x_flash_part_t *p, uint32_t offset, uint32_t value, uint32_t size) {
	uint8_t *data = p->storage;
	
	if (offset < p->offset || (offset + size) > p->offset + p->size) {
		flash_error_part(p, "[data] Unknown write addr %08X [part %08X-%08X]", offset, p->offset, p->offset + p->size - 1);
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
			flash_error_part(p, "[data] Unknown write size %d", size);
			exit(1);
	}
	
	if (pmb887x_flash_blk_is_rw(p->flash->blk)) {
		int ret = pmb887x_flash_blk_pwrite(p->flash->blk, p->flash->offset + p->offset + offset, size, p->storage + offset);
		if (ret < 0) {
			flash_error_part(p, "Can't write to flash file: %d, %s", ret, strerror(ret));
			exit(1);
		}
	}
}

static uint32_t flash_efa_read(pmb887x_flash_part_t *p, uint32_t offset, uint32_t size) {
	uint32_t efa_offset = offset - p->offset;
	if (efa_offset + size > p->flash->efa_size) {
		flash_error_part(p, "[EFA] Unknown read addr %08X", p->flash->offset + offset);
		exit(1);
	}
	return lduw_le_p(p->flash->efa_storage + efa_offset);
}

static uint32_t flash_query_read(pmb887x_flash_part_t *p, uint32_t offset) {
	const pmb887x_flash_cfg_t *cfg = p->flash->cfg;
	uint16_t index = offset >> 1 & CFI_INDEX_MASK;
	uint32_t value;

	if (index >= CFI_ADDR && index < CFI_ADDR + cfg->cfi_size) {
		value = cfg->cfi[index - CFI_ADDR];
		flash_trace_part(p, "CFI %02X: %02X", index, value);
	} else if (index >= cfg->pri_addr && index < cfg->pri_addr + cfg->pri_size) {
		value = cfg->pri[index - cfg->pri_addr];
		flash_trace_part(p, "PRI %02X: %02X", index - cfg->pri_addr, value);
	} else if (index >= cfg->otp0_addr && index < cfg->otp0_addr + cfg->otp0_size / 2) {
		value = p->flash->otp0_data[index - cfg->otp0_addr];
		flash_trace_part(p, "OTP0 %02X: %04X", index - cfg->otp0_addr, value);
	} else if (index >= cfg->otp1_addr && index < cfg->otp1_addr + cfg->otp1_size / 2) {
		value = p->flash->otp1_data[index - cfg->otp1_addr];
		flash_trace_part(p, "OTP1 %02X: %04X", index - cfg->otp1_addr, value);
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

			case 0x02: {
				pmb887x_flash_block_t *blk = flash_part_find_block(p, offset);
				uint32_t efa_offset = offset - p->offset;
				value = (blk->locked ? FLASH_LOCK_STATUS_LOCKED : 0) |
					(blk->locked_down ? FLASH_LOCK_STATUS_LOCKED_DOWN : 0);
				if (efa_offset < p->flash->efa_size) {
					pmb887x_flash_block_t *efa_blk = flash_find_efa_block(p, offset);
					value |= (efa_blk->locked ? FLASH_EFA_LOCK_STATUS_LOCKED : 0) |
						(efa_blk->locked_down ? FLASH_EFA_LOCK_STATUS_LOCKED_DOWN : 0);
				}
				flash_trace_part(p, "lock status: %02X", value);
				break;
			}

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
				flash_error_part(p, "%08X: read unknown cfi index 0x%02X", offset, index);
				break;
		}
	}

	return value;
}

static bool flash_efa_lock(pmb887x_flash_part_t *p, uint32_t offset, uint64_t value) {
	pmb887x_flash_block_t *blk = flash_find_efa_block(p, offset);
	if (value == 0x01) {
		flash_trace_part(p, "lock EFA block %08X", p->flash->offset + offset);
		blk->locked = true;
	} else if (value == 0xD0) {
		flash_trace_part(p, "unlock EFA block %08X", p->flash->offset + offset);
		blk->locked = false;
	} else if (value == 0x2F) {
		flash_trace_part(p, "lock-down EFA block %08X", p->flash->offset + offset);
		blk->locked = true;
		blk->locked_down = true;
	} else {
		return false;
	}

	p->wcycle = 0;
	p->status |= FLASH_STATUS_READY;
	return true;
}

static void flash_efa_erase(pmb887x_flash_part_t *p, uint32_t offset) {
	pmb887x_flash_block_t *blk = flash_find_efa_block(p, offset);
	flash_trace_part(p, "erase EFA block %08X...%08X", p->flash->offset + p->offset + blk->offset,
		p->flash->offset + p->offset + blk->offset + blk->size - 1);
	if (blk->locked) {
		p->status |= FLASH_STATUS_BLOCK_LOCKED;
		return;
	}

	bool changed = false;
	for (uint32_t i = 0; i < blk->size; i++) {
		if (p->flash->efa_storage[blk->offset + i] != 0xFF) {
			changed = true;
			break;
		}
	}
	memset(p->flash->efa_storage + blk->offset, 0xFF, blk->size);
	if (changed)
		flash_save_file(p->flash, p->flash->efa_file, p->flash->efa_storage, p->flash->efa_size, "EFA");
}

static void flash_efa_program(pmb887x_flash_part_t *p, uint32_t offset, uint64_t value, uint32_t size) {
	uint32_t efa_offset = offset - p->offset;
	pmb887x_flash_block_t *blk = flash_find_efa_block(p, offset);
	flash_trace_part(p, "program EFA word [%d]: %08"PRIX64" to %08X", size, value, p->flash->offset + offset);
	if (efa_offset + size > p->flash->efa_size) {
		flash_error_part(p, "[EFA] Unknown write addr %08X", p->flash->offset + offset);
		exit(1);
	}
	if (blk->locked) {
		p->status |= FLASH_STATUS_BLOCK_LOCKED;
		return;
	}

	bool changed = false;
	for (uint32_t i = 0; i < size; i++) {
		uint8_t old_value = p->flash->efa_storage[efa_offset + i];
		uint8_t programmed_value = value >> i * 8;
		uint8_t new_value = old_value & programmed_value;
		p->flash->efa_storage[efa_offset + i] = new_value;
		if (old_value != new_value)
			changed = true;
	}
	if (changed)
		flash_save_file(p->flash, p->flash->efa_file, p->flash->efa_storage, p->flash->efa_size, "EFA");
}

static void flash_otp_program(pmb887x_flash_part_t *p, uint32_t offset, uint64_t value, uint32_t size) {
	const pmb887x_flash_cfg_t *cfg = p->flash->cfg;
	uint16_t index = offset >> 1 & CFI_INDEX_MASK;
	uint16_t *otp_data = NULL;
	uint32_t data_index = 0;
	uint16_t lock_mask = 0;

	if (index >= cfg->otp0_addr && index < cfg->otp0_addr + cfg->otp0_size / 2) {
		data_index = index - cfg->otp0_addr;
		otp_data = p->flash->otp0_data;
		if (data_index) {
			uint32_t data_offset = (data_index - 1) * sizeof(uint16_t);
			lock_mask = data_offset < cfg->otp0_factory_size ? BIT(0) : BIT(1);
		}
	} else if (index >= cfg->otp1_addr && index < cfg->otp1_addr + cfg->otp1_size / 2) {
		data_index = index - cfg->otp1_addr;
		otp_data = p->flash->otp1_data;
		if (data_index) {
			uint32_t lock_bits = sizeof(otp_data[0]) * CHAR_BIT;
			uint32_t group_size = (cfg->otp1_size - sizeof(uint16_t)) / lock_bits;
			uint32_t group = (data_index - 1) * sizeof(uint16_t) / group_size;
			lock_mask = BIT(group);
		}
	}

	flash_trace_part(p, "program protection word [%d]: %08"PRIX64" to %08X", size, value, p->flash->offset + offset);
	if (!otp_data || size != sizeof(uint16_t) || (data_index && ((otp_data[0] & lock_mask) == 0))) {
		p->status |= FLASH_STATUS_PROGRAM_ERROR;
		return;
	}

	uint16_t old_value = otp_data[data_index];
	otp_data[data_index] &= (uint16_t) value;
	if (otp_data[data_index] == old_value)
		return;
	if (otp_data == p->flash->otp0_data) {
		flash_save_file(p->flash, p->flash->otp0_file, p->flash->otp0_data, cfg->otp0_size, "OTP0");
	} else {
		flash_save_file(p->flash, p->flash->otp1_file, p->flash->otp1_data, cfg->otp1_size, "OTP1");
	}
}

static bool flash_lock_command(pmb887x_flash_part_t *p, uint32_t offset, uint64_t value) {
	if (value == 0xFF) {
		flash_reset(p);
		return true;
	}
	if (value == 0x03) {
		flash_trace_part(p, "program read configuration register at %08X", p->flash->offset + offset);
		flash_reset(p);
		return true;
	}
	if (value == 0x04) {
		flash_trace_part(p, "program enhanced configuration register at %08X", p->flash->offset + offset);
		flash_reset(p);
		return true;
	}
	if (value != 0x01 && value != 0xD0 && value != 0x2F)
		return false;

	pmb887x_flash_block_t *blk = flash_part_find_block(p, offset);
	if (value == 0x01) {
		flash_trace_part(p, "lock block %08X", p->flash->offset + offset);
		blk->locked = true;
	} else if (value == 0xD0) {
		flash_trace_part(p, "unlock block %08X", p->flash->offset + offset);
		blk->locked = false;
	} else if (value == 0x2F) {
		flash_trace_part(p, "lock-down block %08X", p->flash->offset + offset);
		blk->locked = true;
		blk->locked_down = true;
	}

	p->wcycle = 0;
	p->status |= FLASH_STATUS_READY;
	return true;
}

static void flash_blank_check(pmb887x_flash_part_t *p, uint32_t offset) {
	pmb887x_flash_block_t *blk = flash_part_find_block(p, offset);
	const uint8_t *data = p->storage + blk->offset;
	bool blank = true;
	for (uint32_t i = 0; i < blk->size; i++) {
		if (data[i] != 0xFF) {
			blank = false;
			break;
		}
	}

	flash_trace_part(p, "blank check block %08X...%08X: %s", p->flash->offset + p->offset + blk->offset,
		p->flash->offset + p->offset + blk->offset + blk->size - 1, blank ? "blank" : "not blank");
	p->status &= ~FLASH_STATUS_BLOCK_ERASE_ERROR;
	if (!blank)
		p->status |= FLASH_STATUS_BLOCK_ERASE_ERROR;
	p->status |= FLASH_STATUS_READY;
	p->cmd = 0x70;
	p->wcycle = 0;
}

static void flash_block_erase(pmb887x_flash_part_t *p, uint32_t offset) {
	pmb887x_flash_block_t *blk = flash_part_find_block(p, offset);
	uint32_t sector_size = flash_find_sector_size(p, offset);
	uint32_t mask = ~(sector_size - 1);
	uint32_t base = p->cmd_addr & mask;
	flash_trace_part(p, "confirm erase block %08X...%08X (sector: %08X)", p->flash->offset + base,
		p->flash->offset + base + sector_size - 1, sector_size);
	if ((offset & mask) != (p->cmd_addr & mask)) {
		flash_error_part(p, "erase sector mismatch: %08X != %08X", p->flash->offset + offset, p->flash->offset + p->cmd_addr);
		exit(1);
	}
	if (blk->locked) {
		p->status |= FLASH_STATUS_BLOCK_LOCKED;
		return;
	}

	uint32_t erase_offset = base - p->offset;
	memset(p->storage + erase_offset, 0xFF, sector_size);
	if (pmb887x_flash_blk_is_rw(p->flash->blk)) {
		int ret = pmb887x_flash_blk_pwrite(p->flash->blk, p->flash->offset + p->offset + erase_offset, sector_size,
			p->storage + erase_offset);
		if (ret < 0) {
			flash_error_part(p, "Can't write to flash file: %d, %s", ret, strerror(ret));
			exit(1);
		}
	}
}

static void flash_word_program(pmb887x_flash_part_t *p, uint32_t offset, uint64_t value, uint32_t size) {
	flash_trace_part(p, "program single word [%d]: %08"PRIX64" to %08X", size, value, p->flash->offset + offset);
	if (flash_part_find_block(p, offset)->locked) {
		p->status |= FLASH_STATUS_BLOCK_LOCKED;
	} else {
		flash_data_write(p, offset, value, size);
	}
}

static void flash_buffer_add(pmb887x_flash_part_t *p, uint32_t offset, uint64_t value, uint32_t size) {
	uint32_t sector_size = flash_find_sector_size(p, offset);
	uint32_t mask = ~(sector_size - 1);
	flash_trace_part(p, "program word [%d]: %08"PRIX64" to %08X", size, value, p->flash->offset + offset);
	if ((offset & mask) != (p->cmd_addr & mask))
		flash_error_part(p, "program sector mismatch: %08X != %08X", offset, p->cmd_addr);
	if (size != 2 && size != 4) {
		flash_error_part(p, "invalid write size: %d", size);
		exit(1);
	}

	for (uint32_t i = 0; i < size; i += 2) {
		pmb887x_flash_buffer_t *buffer_entry = NULL;
		for (uint32_t j = 0; j < p->buffer_size; j++) {
			if (p->buffer[j].offset == offset + i && p->buffer[j].size == 2) {
				buffer_entry = &p->buffer[j];
				break;
			}
		}

		if (!buffer_entry) {
			buffer_entry = &p->buffer[p->buffer_index];
			buffer_entry->offset = offset + i;
			buffer_entry->size = 2;
			p->buffer_index++;
		}
		buffer_entry->value = value >> i * 8 & 0xFFFF;

		if (p->buffer_index == p->buffer_size)
			break;
	}

	if (p->buffer_index == p->buffer_size) {
		flash_trace_part(p, "buffered program finished");
		p->wcycle++;
	}
}

static void flash_buffer_commit(pmb887x_flash_part_t *p) {
	if (flash_part_find_block(p, p->cmd_addr)->locked) {
		p->status |= FLASH_STATUS_BLOCK_LOCKED;
	} else {
		for (uint32_t i = 0; i < p->buffer_size; i++)
			flash_data_write(p, p->buffer[i].offset, p->buffer[i].value, p->buffer[i].size);
	}

	g_clear_pointer(&p->buffer, g_free);
	p->buffer_size = 0;
	p->buffer_index = 0;
	flash_trace_part(p, "confirm buffered program");
	p->wcycle = 0;
	p->status |= FLASH_STATUS_READY;
}

static uint64_t flash_io_read(void *opaque, hwaddr part_offset, uint32_t size) {
	pmb887x_flash_part_t *p = opaque;
	
	hwaddr offset = p->offset + part_offset;
	
	uint32_t value = 0;
	
	switch (p->cmd) {
		case 0x94:
			value = flash_efa_read(p, offset, size);
			break;

		case 0x90:
		case 0x98:
			value = flash_query_read(p, offset);
			break;

		case 0x20:	// Erase
		case 0x24:	// EFA erase
		case 0x44:	// EFA program word
		case 0x60:	// Lock or configuration
		case 0x64:	// EFA lock
		case 0x70:	// Status
		case 0xC0:	// Protection register program
		case 0xE8:	// buffered program
		case 0xE9:	// buffered program
		case 0x41:	// program word
		case 0x40:	// program word
		case 0x10:	// program word
			value = p->status;
			// flash_trace_part(p, "%08"PRIX64": status 0x%02X", offset, value);
			break;

		default:
			flash_error_part(p, "not implemented read for command %02X [addr: %08"PRIX64"]", p->cmd, offset);
			exit(1);
	}
	
	return value;
}

static void flash_io_write(void *opaque, hwaddr part_offset, uint64_t value, uint32_t size) {
	pmb887x_flash_part_t *p = opaque;
	
	hwaddr offset = p->offset + part_offset;
	
	bool valid_command = false;
	
	if (p->wcycle == 0) {
		memory_region_rom_device_set_romd(&p->mem, false);
		
		valid_command = true;
		p->cmd_addr = offset;
		
		switch (value) {
			case 0xFF:
				flash_reset(p);
				break;

			case 0x00:
			case 0xAA:
			case 0x55:
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

			case 0x94:
				flash_trace_part(p, "cmd read EFA (%02"PRIX64")", value);
				p->cmd = value;
				break;

			case 0x98:
				flash_trace_part(p, "cmd read cfi (%02"PRIX64")", value);
				p->cmd = value;
				break;

			case 0x50:
				flash_trace_part(p, "cmd clear status (%02"PRIX64")", value);
				p->status &= ~FLASH_STATUS_ERRORS;
				memory_region_rom_device_set_romd(&p->mem, p->cmd == 0);
				break;

			case 0x41:
			case 0x40:
			case 0x10:
				flash_trace_part(p, "cmd program word (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
				break;

			case 0x44:
				flash_trace_part(p, "cmd program EFA word (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
				break;

			case 0xE9:
			case 0xE8:
				flash_trace_part(p, "cmd buffered program (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
				p->status |= FLASH_STATUS_READY;
				break;

			case 0x80:
				flash_trace_part(p, "cmd buffered EFP (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
				break;

			case 0x20:
				flash_trace_part(p, "cmd block erase (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
				p->status |= FLASH_STATUS_READY;
				break;

			case 0x24:
				flash_trace_part(p, "cmd EFA block erase (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
				p->status |= FLASH_STATUS_READY;
				break;

			case 0xBC:
				flash_trace_part(p, "cmd blank check (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
				break;

			case 0xB0:
				flash_trace_part(p, "cmd suspend (%02"PRIX64")", value);
				memory_region_rom_device_set_romd(&p->mem, p->cmd == 0);
				break;

			case 0x60:
				flash_trace_part(p, "cmd block lock or read configuration (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
				break;

			case 0x64:
				flash_trace_part(p, "cmd EFA block lock (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
				break;

			case 0xC0:
				flash_trace_part(p, "cmd protection program (%02"PRIX64")", value);
				p->cmd = value;
				p->wcycle++;
				break;

			default:
				flash_trace_part(p, "cmd unknown (%02"PRIX64") at %08"PRIX64"", value, p->flash->offset + offset);
				flash_reset(p);
				// flash_error_part(p, "cmd unknown (%02"PRIX64") at %08"PRIX64"", value, p->flash->offset + offset);
				// exit(1);
				break;
		}
	} else if (p->wcycle == 1) {
		switch (p->cmd) {
			case 0x70:	// read status
			case 0x90:	// read devid
			case 0x98:	// read cfi
				if (value == 0xFF) {
					valid_command = true;
					flash_reset(p);
				}
				break;
			
			case 0x60:	// lock or configuration
				valid_command = flash_lock_command(p, offset, value);
				break;

			case 0x64:
				valid_command = flash_efa_lock(p, offset, value);
				break;

			case 0xBC:
				if (value == 0xD0) {
					flash_blank_check(p, offset);
					valid_command = true;
				}
				break;
			
			case 0x20:	// erase
				if (value == 0xD0) {
					flash_block_erase(p, offset);
					valid_command = true;
					p->wcycle = 0;
					p->status |= FLASH_STATUS_READY;
				}
				break;

			case 0x24: // EFA erase
				if (value == 0xD0) {
					flash_efa_erase(p, offset);
					valid_command = true;
					p->wcycle = 0;
					p->status |= FLASH_STATUS_READY;
				}
				break;
			
			case 0xE9:	// buffered program
			case 0xE8:	// buffered program
				valid_command = true;
				p->buffer_size = (value & 0xFFFF) + 1;
				p->buffer_index = 0;
				p->buffer = g_new0(pmb887x_flash_buffer_t, p->buffer_size);
				
				flash_trace_part(p, "buffered program %d words", p->buffer_size);
				
				p->wcycle++;
				break;
			
			case 0x10:	// program word
			case 0x40:	// program word
			case 0x41:	// program word
				valid_command = true;
				flash_word_program(p, offset, value, size);
				p->wcycle = 0;
				p->status |= FLASH_STATUS_READY;
				break;

			case 0x44: // EFA program word
				valid_command = true;
				flash_efa_program(p, offset, value, size);
				p->wcycle = 0;
				p->status |= FLASH_STATUS_READY;
				break;

			case 0xC0: // Protection register program
				flash_otp_program(p, offset, value, size);
				valid_command = true;
				p->wcycle = 0;
				p->status |= FLASH_STATUS_READY;
				break;
		}
	} else if (p->wcycle == 2) {
		switch (p->cmd) {
			case 0xE9:	// buffered program
			case 0xE8:	// buffered program
				valid_command = true;
				flash_buffer_add(p, offset, value, size);
				break;
		}
	} else if (p->wcycle == 3) {
		switch (p->cmd) {
			case 0xE9:	// buffered program
			case 0xE8:	// buffered program
				if (value == 0xD0) {
					flash_buffer_commit(p);
					valid_command = true;
				}
				break;
		}
	}
	
	if (!valid_command) {
		flash_error_part(p, "not implemented %d cycle for command %02X [addr: %08"PRIX64", value: %08"PRIX64"]", p->wcycle, p->cmd, p->flash->offset + offset, value);
		exit(1);
	}
}

static uint64_t flash_io_unaligned_read(void *opaque, hwaddr offset, uint32_t size) {
	// Native read
	if (size == 2 && (offset & 0x1) == 0)
		return flash_io_read(opaque, offset, 2);
	
	// Unaligned read
	uint32_t value = 0;
	if ((offset & 0x1) == 0) {
		for (uint32_t i = 0; i < size; i += 2)
			value |= flash_io_read(opaque, offset + i, 2) << (i * 8);
	} else {
		value |= (flash_io_read(opaque, offset - 1, 2) >> 8) & 0xFF;
		for (uint32_t i = 1; i < size; i += 2)
			value |= flash_io_read(opaque, offset + i + 1, 2) << (i * 8);
	}
	
	if (size < sizeof(value))
		value &= (1U << (size * 8)) - 1;
	// pmb887x_flash_part_t *p = (pmb887x_flash_part_t *) opaque;
	// flash_trace_part(p, "unaligned %08"PRIX64"[%d] = %08"PRIX64"", p->flash->offset + offset, size, value);
	return value;
}

static const MemoryRegionOps io_ops = {
	.read			= flash_io_unaligned_read,
	.write			= flash_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN
};

static bool fill_data_from_hex(uint8_t *dst, size_t max_size, const char *src_hex) {
	size_t length = strlen(src_hex);
	
	if (!length)
		return true;
	
	if (length % 2 != 0)
		return false;
	
	if (length > max_size * 2)
		return false;
	
	for (size_t i = 0; i < length; i += 2) {
		uint8_t tmp[2];
		
		for (size_t j = 0; j < 2; j++) {
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

static void flash_load_file(pmb887x_flash_t *flash, const char *path, void *data, size_t size, const char *region) {
	if (!path || !path[0])
		return;

	g_autofree char *contents = NULL;
	g_autoptr(GError) error = NULL;
	size_t contents_size = 0;
	if (!g_file_get_contents(path, &contents, &contents_size, &error)) {
		if (g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
			return;
		flash_error(flash, "Can't read %s file %s: %s", region, path, error->message);
		exit(1);
	}
	if (!contents_size)
		return;
	if (contents_size != size) {
		flash_error(flash, "Invalid %s file size: %s is %zu bytes, expected %zu", region, path, contents_size, size);
		exit(1);
	}

	memcpy(data, contents, size);
	flash_trace(flash, "loaded %s from %s", region, path);
}

static void flash_save_file(pmb887x_flash_t *flash, const char *path, const void *data, size_t size, const char *region) {
	if (!path || !path[0] || !pmb887x_flash_blk_is_rw(flash->blk))
		return;

	g_autoptr(GError) error = NULL;
	if (!g_file_set_contents(path, data, size, &error)) {
		flash_error(flash, "Can't write %s file %s: %s", region, path, error->message);
		exit(1);
	}
	flash_trace(flash, "saved %s to %s", region, path);
}

static void flash_init_file_paths(pmb887x_flash_t *flash) {
	const pmb887x_flash_cfg_t *cfg = flash->cfg;
	if (strcmp(flash->name, "FLASH0") == 0) {
		const char *fullflash_file = pmb887x_flash_blk_filename(flash->blk);
		if ((!flash->otp0_file || !flash->otp0_file[0]) && cfg->otp0_size) {
			g_free(flash->otp0_file);
			flash->otp0_file = g_strdup_printf("%s.cfi-otp0", fullflash_file);
		}
		if ((!flash->otp1_file || !flash->otp1_file[0]) && cfg->otp1_size) {
			g_free(flash->otp1_file);
			flash->otp1_file = g_strdup_printf("%s.cfi-otp1", fullflash_file);
		}
		if ((!flash->efa_file || !flash->efa_file[0]) && cfg->efa_erase_regions_count) {
			g_free(flash->efa_file);
			flash->efa_file = g_strdup_printf("%s.cfi-efa", fullflash_file);
		}
	}

	if (!cfg->otp0_size && flash->otp0_file && flash->otp0_file[0]) {
		flash_error(flash, "OTP0 file was provided, but this flash has no OTP0 region");
		exit(1);
	}
	if (!cfg->otp1_size && flash->otp1_file && flash->otp1_file[0]) {
		flash_error(flash, "OTP1 file was provided, but this flash has no OTP1 region");
		exit(1);
	}
	if (!cfg->efa_erase_regions_count && flash->efa_file && flash->efa_file[0]) {
		flash_error(flash, "EFA file was provided, but this flash has no EFA region");
		exit(1);
	}
}

static void flash_init_efa(pmb887x_flash_t *flash) {
	for (uint32_t i = 0; i < flash->cfg->efa_erase_regions_count; i++) {
		const pmb887x_flash_erase_region_t *region = &flash->cfg->efa_erase_regions[i];
		flash->efa_size += region->size;
		flash->efa_blocks_n += region->size / region->sector;
	}

	if (!flash->efa_size)
		return;

	flash->efa_storage = g_malloc(flash->efa_size);
	memset(flash->efa_storage, 0xFF, flash->efa_size);
	flash->efa_blocks = g_new0(pmb887x_flash_block_t, flash->efa_blocks_n);

	uint32_t block_id = 0;
	for (uint32_t i = 0; i < flash->cfg->efa_erase_regions_count; i++) {
		const pmb887x_flash_erase_region_t *region = &flash->cfg->efa_erase_regions[i];
		uint32_t blocks = region->size / region->sector;
		for (uint32_t j = 0; j < blocks; j++) {
			pmb887x_flash_block_t *blk = &flash->efa_blocks[block_id++];
			blk->offset = region->offset + j * region->sector;
			blk->size = region->sector;
			blk->locked = true;
		}
	}

	flash_trace(flash, "EFA 0x00000000 ... 0x%08X", flash->efa_size - 1);
}

static void flash_init_part(pmb887x_flash_t *flash, const pmb887x_flash_cfg_part_t *part_cfg) {
	pmb887x_flash_part_t *p = g_new0(pmb887x_flash_part_t, 1);
	p->n = flash->parts_n;
	flash->parts = g_renew(pmb887x_flash_part_t *, flash->parts, flash->parts_n + 1);
	flash->parts[flash->parts_n++] = p;
	p->flash = flash;
	p->offset = part_cfg->offset;
	p->size = part_cfg->size;
	p->cfg = part_cfg;
	p->status = FLASH_STATUS_READY;
	
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
	flash_init_file_paths(flash);
	
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
			flash_error(flash, "Invalid OTP0 hex data: %s [max_size=%d, len=%zu]", flash->hex_otp0_data, cfg->otp0_size, strlen(flash->hex_otp0_data) / 2);
			exit(1);
		}
	}
	
	// OTP1
	if (cfg->otp1_size > 0) {
		flash->otp1_data = g_new(uint16_t, cfg->otp1_size / 2);
		memset(flash->otp1_data, 0xFF, cfg->otp1_size);
		
		if (!fill_data_from_hex((uint8_t *) flash->otp1_data, cfg->otp1_size, flash->hex_otp1_data)) {
			flash_error(flash, "Invalid OTP1 hex data: %s [max_size=%d, len=%zu]", flash->hex_otp1_data, cfg->otp1_size, strlen(flash->hex_otp1_data) / 2);
			exit(1);
		}
	}

	if (cfg->otp0_size)
		flash_load_file(flash, flash->otp0_file, flash->otp0_data, cfg->otp0_size, "OTP0");
	if (cfg->otp1_size)
		flash_load_file(flash, flash->otp1_file, flash->otp1_data, cfg->otp1_size, "OTP1");
	
	// Init hw partitions
	flash_init_efa(flash);
	if (flash->efa_size)
		flash_load_file(flash, flash->efa_file, flash->efa_storage, flash->efa_size, "EFA");
	for (size_t i = 0; i < cfg->parts_count; i++)
		flash_init_part(flash, &cfg->parts[i]);
	
	sysbus_init_mmio(SYS_BUS_DEVICE(flash->dev), &flash->mmio);
}

static void flash_device_reset(DeviceState *dev) {
	pmb887x_flash_t *flash = PMB887X_FLASH(dev);

	for (uint32_t i = 0; i < flash->parts_n; i++) {
		pmb887x_flash_part_t *p = flash->parts[i];
		p->status = FLASH_STATUS_READY;
		for (uint32_t j = 0; j < p->blocks_n; j++) {
			if (p->blocks[j].locked_down) {
				p->blocks[j].locked = true;
				p->blocks[j].locked_down = false;
			}
		}
		flash_reset(p);
	}

	for (uint32_t i = 0; i < flash->efa_blocks_n; i++) {
		if (flash->efa_blocks[i].locked_down) {
			flash->efa_blocks[i].locked = true;
			flash->efa_blocks[i].locked_down = false;
		}
	}
}

static void flash_error(pmb887x_flash_t *flash, const char *format, ...) {
	g_autoptr(GString) s = g_string_new("");
	
	va_list args;
	va_start(args, format);
	g_string_append_vprintf(s, format, args);
	va_end(args);
	
	error_report("[%s] %s %s", PMB887X_TRACE_PREFIX, flash->name, s->str);
}

static void flash_error_part(pmb887x_flash_part_t *p, const char *format, ...) {
	g_autoptr(GString) s = g_string_new("");
	
	va_list args;
	va_start(args, format);
	g_string_append_vprintf(s, format, args);
	va_end(args);
	
	error_report("[%s] %s <%d> %s", PMB887X_TRACE_PREFIX, p->flash->name, p->n, s->str);
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
	DEFINE_PROP_STRING("otp0-file", pmb887x_flash_t, otp0_file),
	DEFINE_PROP_STRING("otp1-file", pmb887x_flash_t, otp1_file),
	DEFINE_PROP_STRING("efa-file", pmb887x_flash_t, efa_file),
	
	/* OTP0 Initial Data */
	DEFINE_PROP_STRING("otp0-data", pmb887x_flash_t, hex_otp0_data),
	
	/* OTP1 Initial Data */
	DEFINE_PROP_STRING("otp1-data", pmb887x_flash_t, hex_otp1_data),
};

static void flash_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, flash_properties);
	device_class_set_legacy_reset(dc, flash_device_reset);
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
