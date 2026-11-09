/* SPDX-License-Identifier: GPL-2.0-or-later */
/* NOR FLASH (Intel/ST CFI) */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qemu/bitmap.h"
#include "qemu/aio-wait.h"
#include "system/runstate.h"
#include "hw/core/qdev-properties.h"
#include "hw/block/block.h"

#include "system/debug-channel.h"
#include "parts.h"
#include "hw/block/cfi-flash.h"

OBJECT_DECLARE_SIMPLE_TYPE(CFIFlashState, CFI_FLASH)

#define CFI_ADDR    0x10
#define CFI_INDEX_MASK  0xFFF

#define CFI_FLASH_ROMD_THRESHOLD_MIN        42
#define CFI_FLASH_ROMD_THRESHOLD_MAX        (64 * 1024)
#define CFI_FLASH_ROMD_SHORT_PERIOD_NS      (1 * SCALE_MS)

#define CFI_FLASH_WRITEBACK_PAGE_SIZE       4096
#define CFI_FLASH_WRITEBACK_DELAY_MS        1000

#define CFI_FLASH_STATUS_BLOCK_LOCKED       BIT(1)
#define CFI_FLASH_STATUS_PROGRAM_ERROR      BIT(4)
#define CFI_FLASH_STATUS_BLOCK_ERASE_ERROR  BIT(5)
#define CFI_FLASH_STATUS_READY              BIT(7)
#define CFI_FLASH_STATUS_ERRORS             (BIT(1) | BIT(3) | BIT(4) | BIT(5) | BIT(8) | BIT(9))
#define CFI_FLASH_STATUS_SEQUENCE_ERROR     (CFI_FLASH_STATUS_PROGRAM_ERROR | CFI_FLASH_STATUS_BLOCK_ERASE_ERROR)

#define CFI_FLASH_LOCK_STATUS_LOCKED        BIT(0)
#define CFI_FLASH_LOCK_STATUS_LOCKED_DOWN   BIT(1)
#define CFI_FLASH_EFA_LOCK_STATUS_LOCKED        BIT(4)
#define CFI_FLASH_EFA_LOCK_STATUS_LOCKED_DOWN   BIT(5)

typedef struct CFIFlashPart CFIFlashPart;
typedef struct CFIFlashBuffer CFIFlashBuffer;
typedef struct CFIFlashBlock CFIFlashBlock;
typedef struct CFIFlashDirtyRange CFIFlashDirtyRange;

struct CFIFlashBuffer {
    uint32_t offset;
    uint32_t value;
    uint8_t size;
};

struct CFIFlashBlock {
    uint32_t offset;
    uint32_t size;
    bool locked;
    bool locked_down;
};

struct CFIFlashDirtyRange {
    uint32_t start;
    uint32_t end;
};

struct CFIFlashPart {
    uint16_t n;
    uint32_t size;
    uint32_t offset;

    uint8_t wcycle;
    uint8_t cmd;
    uint32_t cmd_addr;
    uint16_t status;
    uint32_t mmio_array_reads;
    uint32_t romd_threshold;
    int64_t romd_since_ns;

    uint8_t *storage;
    unsigned long *dirty_pages;

    uint32_t buffer_size;
    uint32_t buffer_index;
    uint32_t buffer_end;
    CFIFlashBuffer *buffer;
    const CFIFlashChipPart *cfg;

    uint32_t blocks_n;
    CFIFlashBlock *blocks;

    MemoryRegion mem;
    CFIFlashState *flash;
};

struct CFIFlashState {
    SysBusDevice parent_obj;
    DebugChannel dbg;
    DeviceState *dev;
    MemoryRegion mmio;

    CFIFlashStorage *blk;
    const CFIFlashChip *cfg;

    char *name;
    char *otp0_file;
    char *otp1_file;
    char *efa_file;
    int otp0_fd;
    int otp1_fd;
    int efa_fd;
    CFIFlashDirtyRange otp0_dirty;
    CFIFlashDirtyRange otp1_dirty;
    CFIFlashDirtyRange efa_dirty;

    bool writable;
    QEMUTimer *writeback_timer;
    uint32_t writeback_in_flight;

    uint16_t vid;
    uint16_t pid;

    char *hex_otp0_data;
    char *hex_otp1_data;

    uint32_t size;
    uint32_t offset;

    /* Refuse program/buffer opcodes the part's CFI command set does not
     * document (see flash_command_is_documented). Off by default. */
    bool strict_opcodes;

    uint16_t *otp0_data;
    uint16_t *otp1_data;
    uint8_t *efa_storage;
    uint32_t efa_size;
    uint32_t efa_blocks_n;
    CFIFlashBlock *efa_blocks;

    uint32_t parts_n;
    CFIFlashPart **parts;
};

static void cfi_flash_error(CFIFlashState *flash, const char *format, ...) G_GNUC_PRINTF(2, 3);

static void cfi_flash_load_file(CFIFlashState *flash, const char *path, void *data, size_t size, const char *region);
static void cfi_flash_save_file(CFIFlashState *flash, int *fd, const char *path, const void *data, size_t total_size,
    size_t offset, size_t size, const char *region);

static void cfi_flash_buffer_clear(CFIFlashPart *p) {
    g_clear_pointer(&p->buffer, g_free);
    p->buffer_size = 0;
    p->buffer_index = 0;
    p->buffer_end = 0;
}

static void cfi_flash_romd_map(CFIFlashPart *p) {
    p->romd_since_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    memory_region_rom_device_set_romd(&p->mem, true);
}

static void cfi_flash_romd_unmap(CFIFlashPart *p) {
    int64_t romd_period_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - p->romd_since_ns;

    if (romd_period_ns < CFI_FLASH_ROMD_SHORT_PERIOD_NS) {
        p->romd_threshold = MIN(p->romd_threshold * 2, CFI_FLASH_ROMD_THRESHOLD_MAX);
    } else {
        p->romd_threshold = MAX(p->romd_threshold / 2, CFI_FLASH_ROMD_THRESHOLD_MIN);
    }
    memory_region_rom_device_set_romd(&p->mem, false);
}

static void cfi_flash_sequence_error(CFIFlashPart *p) {
    cfi_flash_buffer_clear(p);
    p->cmd = 0x70;
    p->wcycle = 0;
    p->status |= CFI_FLASH_STATUS_READY | CFI_FLASH_STATUS_SEQUENCE_ERROR;
}

static void cfi_flash_unmap_inactive_read_array(CFIFlashPart *p) {
    if (p->cmd == 0)
        return;
    if (memory_region_is_romd(&p->mem))
        cfi_flash_romd_unmap(p);
    p->mmio_array_reads = 0;
}

static void cfi_flash_reset(CFIFlashPart *p) {
    DEBUG_LOG(&p->flash->dbg, "<%d> back to read array mode\n", p->n);
    cfi_flash_buffer_clear(p);
    p->cmd = 0;
    p->wcycle = 0;
}

static CFIFlashBlock *cfi_flash_part_find_block(CFIFlashPart *p, uint32_t offset) {
    offset -= p->offset;
    for (uint32_t i = 0; i < p->blocks_n; i++) {
        CFIFlashBlock *blk = &p->blocks[i];
        if (offset >= blk->offset && offset < blk->offset + blk->size)
            return blk;
    }
    g_assert_not_reached();
}

static CFIFlashBlock *cfi_flash_find_efa_block(CFIFlashPart *p, uint32_t offset) {
    offset -= p->offset;
    for (uint32_t i = 0; i < p->flash->efa_blocks_n; i++) {
        CFIFlashBlock *blk = &p->flash->efa_blocks[i];
        if (offset >= blk->offset && offset < blk->offset + blk->size)
            return blk;
    }
    return NULL;
}

static uint32_t cfi_flash_find_sector_size(CFIFlashPart *p, uint32_t offset) {
    offset -= p->offset;

    for (uint32_t i = 0; i < p->cfg->erase_regions_cnt; i++) {
        const CFIFlashEraseRegion *region = &p->cfg->erase_regions[i];
        if (offset >= region->offset && offset < region->offset + region->size)
            return region->sector;
    }

    g_assert_not_reached();
}

static void cfi_flash_writeback_schedule(CFIFlashState *flash) {
    if (!timer_pending(flash->writeback_timer))
        timer_mod(flash->writeback_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + CFI_FLASH_WRITEBACK_DELAY_MS);
}

static void cfi_flash_part_mark_dirty(CFIFlashPart *p, uint32_t storage_offset, uint32_t size) {
    if (!p->flash->writable)
        return;

    uint32_t first_page = storage_offset / CFI_FLASH_WRITEBACK_PAGE_SIZE;
    uint32_t end_page = DIV_ROUND_UP(storage_offset + size, CFI_FLASH_WRITEBACK_PAGE_SIZE);
    bitmap_set(p->dirty_pages, first_page, end_page - first_page);
    cfi_flash_writeback_schedule(p->flash);
}

static void cfi_flash_side_file_mark_dirty(
    CFIFlashState *flash,
    CFIFlashDirtyRange *range,
    uint32_t offset,
    uint32_t size
) {
    if (!flash->writable)
        return;

    if (range->start == range->end) {
        range->start = offset;
        range->end = offset + size;
    } else {
        range->start = MIN(range->start, offset);
        range->end = MAX(range->end, offset + size);
    }
    cfi_flash_writeback_schedule(flash);
}

static void cfi_flash_side_file_save(
    CFIFlashState *flash,
    CFIFlashDirtyRange *range,
    int *fd,
    const char *path,
    const void *data,
    size_t total_size,
    const char *region
) {
    if (range->start == range->end)
        return;
    cfi_flash_save_file(flash, fd, path, data, total_size, range->start, range->end - range->start, region);
    *range = (CFIFlashDirtyRange) { };
}

static void cfi_flash_writeback_done(void *opaque, int ret) {
    CFIFlashState *flash = opaque;
    flash->writeback_in_flight--;
    aio_wait_kick();
    if (ret < 0) {
        cfi_flash_error(flash, "Can't write to flash file: %d, %s", ret, strerror(-ret));
        exit(1);
    }
}

static void cfi_flash_part_writeback_start(CFIFlashPart *p) {
    uint32_t pages_n = DIV_ROUND_UP(p->size, CFI_FLASH_WRITEBACK_PAGE_SIZE);
    uint32_t page = find_first_bit(p->dirty_pages, pages_n);
    while (page < pages_n) {
        uint32_t end_page = find_next_zero_bit(p->dirty_pages, pages_n, page);
        bitmap_clear(p->dirty_pages, page, end_page - page);

        uint32_t storage_offset = page * CFI_FLASH_WRITEBACK_PAGE_SIZE;
        uint32_t size = MIN(end_page * CFI_FLASH_WRITEBACK_PAGE_SIZE, p->size) - storage_offset;
        p->flash->writeback_in_flight++;
        cfi_flash_storage_aio_pwrite(p->flash->blk, p->flash->offset + p->offset + storage_offset, size,
            p->storage + storage_offset, cfi_flash_writeback_done, p->flash);

        page = find_next_bit(p->dirty_pages, pages_n, end_page);
    }
}

static void cfi_flash_writeback_start(CFIFlashState *flash) {
    const CFIFlashChip *cfg = flash->cfg;

    for (uint32_t i = 0; i < flash->parts_n; i++)
        cfi_flash_part_writeback_start(flash->parts[i]);

    cfi_flash_side_file_save(flash, &flash->otp0_dirty, &flash->otp0_fd, flash->otp0_file, flash->otp0_data,
        cfg->otp0_size, "OTP0");
    cfi_flash_side_file_save(flash, &flash->otp1_dirty, &flash->otp1_fd, flash->otp1_file, flash->otp1_data,
        cfg->otp1_size, "OTP1");
    cfi_flash_side_file_save(flash, &flash->efa_dirty, &flash->efa_fd, flash->efa_file, flash->efa_storage,
        flash->efa_size, "EFA");
}

static void cfi_flash_writeback_expire(void *opaque) {
    CFIFlashState *flash = opaque;
    if (flash->writeback_in_flight > 0) {
        cfi_flash_writeback_schedule(flash);
        return;
    }
    cfi_flash_writeback_start(flash);
}

static void cfi_flash_vm_state_change(void *opaque, bool running, RunState _state) {
    CFIFlashState *flash = opaque;
    if (running)
        return;

    timer_del(flash->writeback_timer);
    AIO_WAIT_WHILE(NULL, flash->writeback_in_flight > 0);
    cfi_flash_writeback_start(flash);
    AIO_WAIT_WHILE(NULL, flash->writeback_in_flight > 0);
}

static void cfi_flash_data_write(CFIFlashPart *p, uint32_t offset, uint32_t value, uint32_t size) {
    uint8_t *data = p->storage;

    g_assert(offset >= p->offset && offset + size <= p->offset + p->size);
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
            g_assert_not_reached();
    }

    cfi_flash_part_mark_dirty(p, offset, size);
}

static uint32_t cfi_flash_efa_read(CFIFlashPart *p, uint32_t offset, uint32_t size) {
    uint32_t efa_offset = offset - p->offset;
    if (efa_offset + size > p->flash->efa_size) {
        DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> EFA read outside EFA: %08X\n", p->n, p->flash->offset + offset);
        return 0;
    }
    return lduw_le_p(p->flash->efa_storage + efa_offset);
}

static uint32_t cfi_flash_array_read(CFIFlashPart *p, uint32_t offset, uint32_t size) {
    uint32_t value = 0;
    offset -= p->offset;
    for (uint32_t i = 0; i < size; i++)
        value |= (uint32_t) p->storage[offset + i] << (i * 8);
    return value;
}

static uint32_t cfi_flash_query_read(CFIFlashPart *p, uint32_t offset) {
    const CFIFlashChip *cfg = p->flash->cfg;
    uint16_t index = offset >> 1 & CFI_INDEX_MASK;
    uint32_t value;

    if (index >= CFI_ADDR && index < CFI_ADDR + cfg->cfi_size) {
        value = cfg->cfi[index - CFI_ADDR];
        DEBUG_LOG(&p->flash->dbg, "<%d> CFI %02X: %02X\n", p->n, index, value);
    } else if (index >= cfg->pri_addr && index < cfg->pri_addr + cfg->pri_size) {
        value = cfg->pri[index - cfg->pri_addr];
        DEBUG_LOG(&p->flash->dbg, "<%d> PRI %02X: %02X\n", p->n, index - cfg->pri_addr, value);
    } else if (index >= cfg->otp0_addr && index < cfg->otp0_addr + cfg->otp0_size / 2) {
        value = p->flash->otp0_data[index - cfg->otp0_addr];
        DEBUG_LOG(&p->flash->dbg, "<%d> OTP0 %02X: %04X\n", p->n, index - cfg->otp0_addr, value);
    } else if (index >= cfg->otp1_addr && index < cfg->otp1_addr + cfg->otp1_size / 2) {
        value = p->flash->otp1_data[index - cfg->otp1_addr];
        DEBUG_LOG(&p->flash->dbg, "<%d> OTP1 %02X: %04X\n", p->n, index - cfg->otp1_addr, value);
    } else {
        switch (index) {
            case 0x00:
                value = p->flash->vid;
                DEBUG_LOG(&p->flash->dbg, "<%d> vendor id: %04X\n", p->n, value);
                break;

            case 0x01:
                value = p->flash->pid;
                DEBUG_LOG(&p->flash->dbg, "<%d> device id: %04X\n", p->n, value);
                break;

            case 0x02: {
                CFIFlashBlock *blk = cfi_flash_part_find_block(p, offset);
                uint32_t efa_offset = offset - p->offset;
                value = (blk->locked ? CFI_FLASH_LOCK_STATUS_LOCKED : 0) |
                    (blk->locked_down ? CFI_FLASH_LOCK_STATUS_LOCKED_DOWN : 0);
                if (efa_offset < p->flash->efa_size) {
                    CFIFlashBlock *efa_blk = cfi_flash_find_efa_block(p, offset);
                    value |= (efa_blk->locked ? CFI_FLASH_EFA_LOCK_STATUS_LOCKED : 0) |
                        (efa_blk->locked_down ? CFI_FLASH_EFA_LOCK_STATUS_LOCKED_DOWN : 0);
                }
                DEBUG_LOG(&p->flash->dbg, "<%d> lock status: %02X\n", p->n, value);
                break;
            }

            case 0x05:
                value = cfg->cr;
                DEBUG_LOG(&p->flash->dbg, "<%d> configuration register: %02X\n", p->n, value);
                break;

            case 0x06:
                value = cfg->ehcr;
                DEBUG_LOG(&p->flash->dbg, "<%d> enhanced configuration register: %02X\n", p->n, value);
                break;

            default:
                value = 0xFFFF;
                DEBUG_LOG(&p->flash->dbg, "<%d> %08X: read unknown cfi index 0x%02X\n", p->n, offset, index);
                break;
        }
    }

    return value;
}

static bool cfi_flash_efa_lock(CFIFlashPart *p, uint32_t offset, uint64_t value) {
    CFIFlashBlock *blk = cfi_flash_find_efa_block(p, offset);
    if (!blk) {
        DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> EFA lock outside EFA: %08X\n", p->n, p->flash->offset + offset);
        cfi_flash_sequence_error(p);
        return true;
    }
    if (value == 0x01) {
        DEBUG_LOG(&p->flash->dbg, "<%d> lock EFA block %08X\n", p->n, p->flash->offset + offset);
        blk->locked = true;
    } else if (value == 0xD0) {
        DEBUG_LOG(&p->flash->dbg, "<%d> unlock EFA block %08X\n", p->n, p->flash->offset + offset);
        blk->locked = false;
    } else if (value == 0x2F) {
        DEBUG_LOG(&p->flash->dbg, "<%d> lock-down EFA block %08X\n", p->n, p->flash->offset + offset);
        blk->locked = true;
        blk->locked_down = true;
    } else {
        return false;
    }

    p->wcycle = 0;
    p->status |= CFI_FLASH_STATUS_READY;
    return true;
}

static void cfi_flash_efa_erase(CFIFlashPart *p, uint32_t offset) {
    CFIFlashBlock *blk = cfi_flash_find_efa_block(p, offset);
    if (!blk) {
        DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> EFA erase outside EFA: %08X\n", p->n, p->flash->offset + offset);
        cfi_flash_sequence_error(p);
        return;
    }
    DEBUG_LOG(&p->flash->dbg, "<%d> erase EFA block %08X...%08X\n", p->n, p->flash->offset + p->offset + blk->offset,
        p->flash->offset + p->offset + blk->offset + blk->size - 1);
    if (blk->locked) {
        p->status |= CFI_FLASH_STATUS_BLOCK_LOCKED;
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
        cfi_flash_side_file_mark_dirty(p->flash, &p->flash->efa_dirty, blk->offset, blk->size);
}

static void cfi_flash_efa_program(CFIFlashPart *p, uint32_t offset, uint64_t value, uint32_t size) {
    uint32_t efa_offset = offset - p->offset;
    CFIFlashBlock *blk = cfi_flash_find_efa_block(p, offset);
    DEBUG_LOG(&p->flash->dbg, "<%d> program EFA word [%d]: %08"PRIX64" to %08X\n",
        p->n, size, value, p->flash->offset + offset);
    if (!blk || efa_offset + size > p->flash->efa_size) {
        DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> EFA program outside EFA: %08X\n", p->n, p->flash->offset + offset);
        cfi_flash_sequence_error(p);
        return;
    }
    if (blk->locked) {
        p->status |= CFI_FLASH_STATUS_BLOCK_LOCKED;
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
        cfi_flash_side_file_mark_dirty(p->flash, &p->flash->efa_dirty, efa_offset, size);
}

static void cfi_flash_otp_program(CFIFlashPart *p, uint32_t offset, uint64_t value, uint32_t size) {
    const CFIFlashChip *cfg = p->flash->cfg;
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

    DEBUG_LOG(&p->flash->dbg, "<%d> program protection word [%d]: %08"PRIX64" to %08X\n",
        p->n, size, value, p->flash->offset + offset);
    if (!otp_data || size != sizeof(uint16_t) || (data_index && ((otp_data[0] & lock_mask) == 0))) {
        p->status |= CFI_FLASH_STATUS_PROGRAM_ERROR;
        return;
    }

    uint16_t old_value = otp_data[data_index];
    otp_data[data_index] &= (uint16_t) value;
    if (otp_data[data_index] == old_value)
        return;
    uint32_t data_offset = data_index * sizeof(otp_data[0]);
    if (otp_data == p->flash->otp0_data) {
        cfi_flash_side_file_mark_dirty(p->flash, &p->flash->otp0_dirty, data_offset, sizeof(otp_data[0]));
    } else {
        cfi_flash_side_file_mark_dirty(p->flash, &p->flash->otp1_dirty, data_offset, sizeof(otp_data[0]));
    }
}

static bool cfi_flash_lock_command(CFIFlashPart *p, uint32_t offset, uint64_t value) {
    if (value == 0xFF) {
        cfi_flash_reset(p);
        return true;
    }
    if (value == 0x03) {
        DEBUG_LOG(&p->flash->dbg, "<%d> program read configuration register at %08X\n",
            p->n, p->flash->offset + offset);
        cfi_flash_reset(p);
        return true;
    }
    if (value == 0x04) {
        DEBUG_LOG(&p->flash->dbg, "<%d> program enhanced configuration register at %08X\n",
            p->n, p->flash->offset + offset);
        cfi_flash_reset(p);
        return true;
    }
    if (value != 0x01 && value != 0xD0 && value != 0x2F)
        return false;

    CFIFlashBlock *blk = cfi_flash_part_find_block(p, offset);
    if (value == 0x01) {
        DEBUG_LOG(&p->flash->dbg, "<%d> lock block %08X\n", p->n, p->flash->offset + offset);
        blk->locked = true;
    } else if (value == 0xD0) {
        DEBUG_LOG(&p->flash->dbg, "<%d> unlock block %08X\n", p->n, p->flash->offset + offset);
        blk->locked = false;
    } else if (value == 0x2F) {
        DEBUG_LOG(&p->flash->dbg, "<%d> lock-down block %08X\n", p->n, p->flash->offset + offset);
        blk->locked = true;
        blk->locked_down = true;
    }

    p->wcycle = 0;
    p->status |= CFI_FLASH_STATUS_READY;
    return true;
}

static void cfi_flash_blank_check(CFIFlashPart *p, uint32_t offset) {
    CFIFlashBlock *blk = cfi_flash_part_find_block(p, offset);
    const uint8_t *data = p->storage + blk->offset;
    bool blank = true;
    for (uint32_t i = 0; i < blk->size; i++) {
        if (data[i] != 0xFF) {
            blank = false;
            break;
        }
    }

    DEBUG_LOG(&p->flash->dbg, "<%d> blank check block %08X...%08X: %s\n", p->n,
        p->flash->offset + p->offset + blk->offset,
        p->flash->offset + p->offset + blk->offset + blk->size - 1,
        blank ? "blank" : "not blank");
    p->status &= ~CFI_FLASH_STATUS_BLOCK_ERASE_ERROR;
    if (!blank)
        p->status |= CFI_FLASH_STATUS_BLOCK_ERASE_ERROR;
    p->status |= CFI_FLASH_STATUS_READY;
    p->cmd = 0x70;
    p->wcycle = 0;
}

static void cfi_flash_block_erase(CFIFlashPart *p, uint32_t offset) {
    CFIFlashBlock *blk = cfi_flash_part_find_block(p, offset);
    uint32_t sector_size = cfi_flash_find_sector_size(p, offset);
    uint32_t mask = ~(sector_size - 1);
    uint32_t base = p->cmd_addr & mask;
    DEBUG_LOG(&p->flash->dbg, "<%d> confirm erase block %08X...%08X (sector: %08X)\n", p->n, p->flash->offset + base,
        p->flash->offset + base + sector_size - 1, sector_size);
    if ((offset & mask) != (p->cmd_addr & mask)) {
        DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> erase confirm at %08X, setup at %08X\n", p->n,
            p->flash->offset + offset, p->flash->offset + p->cmd_addr);
        cfi_flash_sequence_error(p);
        return;
    }
    if (blk->locked) {
        p->status |= CFI_FLASH_STATUS_BLOCK_LOCKED;
        return;
    }

    uint32_t erase_offset = base - p->offset;
    memset(p->storage + erase_offset, 0xFF, sector_size);
    cfi_flash_part_mark_dirty(p, erase_offset, sector_size);
}

static void cfi_flash_word_program(CFIFlashPart *p, uint32_t offset, uint64_t value, uint32_t size) {
    DEBUG_LOG(&p->flash->dbg, "<%d> program single word [%d]: %08"PRIX64" to %08X\n",
        p->n, size, value, p->flash->offset + offset);
    if (cfi_flash_part_find_block(p, offset)->locked) {
        p->status |= CFI_FLASH_STATUS_BLOCK_LOCKED;
    } else {
        cfi_flash_data_write(p, offset, value, size);
    }
}

static bool cfi_flash_buffer_address_in_block(CFIFlashPart *p, uint32_t offset, uint32_t size) {
    CFIFlashBlock *command_block = cfi_flash_part_find_block(p, p->cmd_addr);
    uint32_t block_start = p->offset + command_block->offset;
    uint32_t block_end = block_start + command_block->size;

    return offset >= block_start && offset < block_end && size <= block_end - offset;
}

static void cfi_flash_buffer_abort(CFIFlashPart *p, uint32_t offset) {
    DEBUG_LOG(&p->flash->dbg, "<%d> abort buffered program at %08X (setup at %08X)\n", p->n, p->flash->offset + offset,
        p->flash->offset + p->cmd_addr);
    cfi_flash_sequence_error(p);
}

static void cfi_flash_buffer_add(CFIFlashPart *p, uint32_t offset, uint64_t value, uint32_t size) {
    DEBUG_LOG(&p->flash->dbg, "<%d> program word [%d]: %08"PRIX64" to %08X\n",
        p->n, size, value, p->flash->offset + offset);
    if (size != 2 && size != 4) {
        cfi_flash_buffer_abort(p, offset);
        return;
    }
    if (!cfi_flash_buffer_address_in_block(p, offset, size)) {
        cfi_flash_buffer_abort(p, offset);
        return;
    }

    for (uint32_t i = 0; i < size; i += 2) {
        CFIFlashBuffer *buffer_entry = NULL;
        if (offset + i < p->buffer_end) {
            for (uint32_t j = 0; j < p->buffer_index; j++) {
                if (p->buffer[j].offset == offset + i && p->buffer[j].size == 2) {
                    buffer_entry = &p->buffer[j];
                    break;
                }
            }
        }

        if (!buffer_entry) {
            buffer_entry = &p->buffer[p->buffer_index];
            buffer_entry->offset = offset + i;
            buffer_entry->size = 2;
            p->buffer_index++;
            p->buffer_end = MAX(p->buffer_end, offset + i + 2);
        }
        buffer_entry->value = value >> i * 8 & 0xFFFF;

        if (p->buffer_index == p->buffer_size)
            break;
    }

    if (p->buffer_index == p->buffer_size) {
        DEBUG_LOG(&p->flash->dbg, "<%d> buffered program finished\n", p->n);
        p->wcycle++;
    }
}

static void cfi_flash_buffer_commit(CFIFlashPart *p) {
    if (cfi_flash_part_find_block(p, p->cmd_addr)->locked) {
        p->status |= CFI_FLASH_STATUS_BLOCK_LOCKED;
    } else {
        for (uint32_t i = 0; i < p->buffer_size; i++)
            cfi_flash_data_write(p, p->buffer[i].offset, p->buffer[i].value, p->buffer[i].size);
    }

    cfi_flash_buffer_clear(p);
    DEBUG_LOG(&p->flash->dbg, "<%d> confirm buffered program\n", p->n);
    p->wcycle = 0;
    p->status |= CFI_FLASH_STATUS_READY;
}

static uint64_t cfi_flash_io_read(void *opaque, hwaddr part_offset, uint32_t size) {
    CFIFlashPart *p = opaque;

    hwaddr offset = p->offset + part_offset;

    uint32_t value = 0;

    switch (p->cmd) {
        case 0x00:
            value = cfi_flash_array_read(p, offset, size);
            if (++p->mmio_array_reads > p->romd_threshold)
                cfi_flash_romd_map(p);
            break;

        case 0x94:
            value = cfi_flash_efa_read(p, offset, size);
            break;

        case 0x90:
        case 0x98:
            value = cfi_flash_query_read(p, offset);
            break;

        case 0x20:  // Erase
        case 0x24:  // EFA erase
        case 0x44:  // EFA program word
        case 0x60:  // Lock or configuration
        case 0x64:  // EFA lock
        case 0x70:  // Status
        case 0xC0:  // Protection register program
        case 0xE8:  // buffered program
        case 0xE9:  // buffered program
        case 0x41:  // program word
        case 0x40:  // program word
        case 0x10:  // program word
            value = p->status;
            // DEBUG_LOG(&p->flash->dbg, "<%d> %08"PRIX64": status 0x%02X\n", p->n, offset, value);
            break;

        default:
            DEBUG_UNIMP(&p->flash->dbg, "<%d> read in command %02X mode at %08"PRIX64"\n", p->n, p->cmd,
                p->flash->offset + offset);
            break;
    }

    return value;
}

static void cfi_flash_io_write(void *opaque, hwaddr part_offset, uint64_t value, uint32_t size) {
    CFIFlashPart *p = opaque;

    hwaddr offset = p->offset + part_offset;

    bool valid_command = false;

    if (p->wcycle == 0) {
        valid_command = true;
        p->cmd_addr = offset;

        switch (value) {
            case 0xFF:
                cfi_flash_reset(p);
                break;

            case 0x00:
            case 0xAA:
            case 0x55:
            case 0xF0:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd AMD probe (%02"PRIX64")\n", p->n, value);
                cfi_flash_reset(p);
                break;

            case 0x70:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd read status (%02"PRIX64")\n", p->n, value);
                p->cmd = value;
                break;

            case 0x90:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd read devid (%02"PRIX64")\n", p->n, value);
                p->cmd = value;
                break;

            case 0x94:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd read EFA (%02"PRIX64")\n", p->n, value);
                p->cmd = value;
                break;

            case 0x98:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd read cfi (%02"PRIX64")\n", p->n, value);
                p->cmd = value;
                break;

            case 0x50:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd clear status (%02"PRIX64")\n", p->n, value);
                p->status &= ~CFI_FLASH_STATUS_ERRORS;
                break;

            case 0x10:
                if (p->flash->strict_opcodes) {
                    DEBUG_LOG(&p->flash->dbg, "<%d> cmd program word (%02"PRIX64") is not documented for command set 0200 -> ignored\n",
                        p->n, value);
                    cfi_flash_reset(p);
                    break;
                }
                QEMU_FALLTHROUGH;
            case 0x41:
            case 0x40:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd program word (%02"PRIX64")\n", p->n, value);
                p->cmd = value;
                p->wcycle++;
                break;

            case 0x44:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd program EFA word (%02"PRIX64")\n", p->n, value);
                p->cmd = value;
                p->wcycle++;
                break;

            case 0xE8:
                if (p->flash->strict_opcodes) {
                    DEBUG_LOG(&p->flash->dbg, "<%d> cmd buffered program (%02"PRIX64") is not documented for command set 0200 -> ignored\n",
                        p->n, value);
                    cfi_flash_reset(p);
                    break;
                }
                QEMU_FALLTHROUGH;
            case 0xE9:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd buffered program (%02"PRIX64")\n", p->n, value);
                p->cmd = value;
                p->wcycle++;
                p->status |= CFI_FLASH_STATUS_READY;
                break;

            case 0x80:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd buffered EFP (%02"PRIX64")\n", p->n, value);
                p->cmd = value;
                p->wcycle++;
                break;

            case 0x20:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd block erase (%02"PRIX64")\n", p->n, value);
                p->cmd = value;
                p->wcycle++;
                p->status |= CFI_FLASH_STATUS_READY;
                break;

            case 0x24:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd EFA block erase (%02"PRIX64")\n", p->n, value);
                p->cmd = value;
                p->wcycle++;
                p->status |= CFI_FLASH_STATUS_READY;
                break;

            case 0xBC:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd blank check (%02"PRIX64")\n", p->n, value);
                p->cmd = value;
                p->wcycle++;
                break;

            case 0xB0:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd suspend (%02"PRIX64")\n", p->n, value);
                break;

            case 0x60:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd block lock or read configuration (%02"PRIX64")\n", p->n, value);
                p->cmd = value;
                p->wcycle++;
                break;

            case 0x64:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd EFA block lock (%02"PRIX64")\n", p->n, value);
                p->cmd = value;
                p->wcycle++;
                break;

            case 0xC0:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd protection program (%02"PRIX64")\n", p->n, value);
                p->cmd = value;
                p->wcycle++;
                break;

            default:
                DEBUG_UNIMP(&p->flash->dbg, "<%d> command %02"PRIX64" at %08"PRIX64"\n",
                    p->n, value, p->flash->offset + offset);
                cfi_flash_reset(p);
                break;
        }
    } else if (p->wcycle == 1) {
        switch (p->cmd) {
            case 0x70:  // read status
            case 0x90:  // read devid
            case 0x98:  // read cfi
                if (value == 0xFF) {
                    valid_command = true;
                    cfi_flash_reset(p);
                }
                break;

            case 0x60:  // lock or configuration
                valid_command = cfi_flash_lock_command(p, offset, value);
                break;

            case 0x64:
                valid_command = cfi_flash_efa_lock(p, offset, value);
                break;

            case 0xBC:
                if (value == 0xD0) {
                    cfi_flash_blank_check(p, offset);
                    valid_command = true;
                }
                break;

            case 0x20:  // erase
                if (value == 0xD0) {
                    cfi_flash_block_erase(p, offset);
                    valid_command = true;
                    p->wcycle = 0;
                    p->status |= CFI_FLASH_STATUS_READY;
                }
                break;

            case 0x24: // EFA erase
                if (value == 0xD0) {
                    cfi_flash_efa_erase(p, offset);
                    valid_command = true;
                    p->wcycle = 0;
                    p->status |= CFI_FLASH_STATUS_READY;
                }
                break;

            case 0xE9:  // buffered program
            case 0xE8:  // buffered program
                valid_command = true;
                if (!cfi_flash_buffer_address_in_block(p, offset, size)) {
                    cfi_flash_buffer_abort(p, offset);
                } else {
                    p->buffer_size = (value & 0xFFFF) + 1;
                    p->buffer_index = 0;
                    p->buffer_end = 0;
                    p->buffer = g_new0(CFIFlashBuffer, p->buffer_size);
                    DEBUG_LOG(&p->flash->dbg, "<%d> buffered program %d words\n", p->n, p->buffer_size);
                    p->wcycle++;
                }
                break;

            case 0x10:  // program word
            case 0x40:  // program word
            case 0x41:  // program word
                valid_command = true;
                cfi_flash_word_program(p, offset, value, size);
                p->wcycle = 0;
                p->status |= CFI_FLASH_STATUS_READY;
                break;

            case 0x44: // EFA program word
                valid_command = true;
                cfi_flash_efa_program(p, offset, value, size);
                p->wcycle = 0;
                p->status |= CFI_FLASH_STATUS_READY;
                break;

            case 0xC0: // Protection register program
                cfi_flash_otp_program(p, offset, value, size);
                valid_command = true;
                p->wcycle = 0;
                p->status |= CFI_FLASH_STATUS_READY;
                break;
        }
    } else if (p->wcycle == 2) {
        switch (p->cmd) {
            case 0xE9:  // buffered program
            case 0xE8:  // buffered program
                valid_command = true;
                cfi_flash_buffer_add(p, offset, value, size);
                break;
        }
    } else if (p->wcycle == 3) {
        switch (p->cmd) {
            case 0xE9:  // buffered program
            case 0xE8:  // buffered program
                valid_command = true;
                if (value != 0xD0 || !cfi_flash_buffer_address_in_block(p, offset, size)) {
                    cfi_flash_buffer_abort(p, offset);
                } else {
                    cfi_flash_buffer_commit(p);
                }
                break;
        }
    }

    if (!valid_command) {
        DEBUG_UNIMP(&p->flash->dbg, "<%d> command %02X cycle %d: %08"PRIX64" at %08"PRIX64"\n", p->n, p->cmd,
            p->wcycle, value, p->flash->offset + offset);
        cfi_flash_sequence_error(p);
    }

    cfi_flash_unmap_inactive_read_array(p);
}

static uint64_t cfi_flash_io_unaligned_read(void *opaque, hwaddr offset, uint32_t size) {
    // Native read
    if (size == 2 && (offset & 0x1) == 0)
        return cfi_flash_io_read(opaque, offset, 2);

    // Unaligned read
    uint32_t value = 0;
    if ((offset & 0x1) == 0) {
        for (uint32_t i = 0; i < size; i += 2)
            value |= cfi_flash_io_read(opaque, offset + i, 2) << (i * 8);
    } else {
        value |= (cfi_flash_io_read(opaque, offset - 1, 2) >> 8) & 0xFF;
        for (uint32_t i = 1; i < size; i += 2)
            value |= cfi_flash_io_read(opaque, offset + i + 1, 2) << (i * 8);
    }

    if (size < sizeof(value))
        value &= (1U << (size * 8)) - 1;
    // CFIFlashPart *p = (CFIFlashPart *) opaque;
    // DEBUG_LOG(&p->flash->dbg, "<%d> unaligned %08"PRIX64"[%d] = %08"PRIX64"\n", p->n, p->flash->offset + offset, size, value);
    return value;
}

static const MemoryRegionOps io_ops = {
    .read           = cfi_flash_io_unaligned_read,
    .write          = cfi_flash_io_write,
    .endianness     = DEVICE_NATIVE_ENDIAN
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

static void cfi_flash_load_file(CFIFlashState *flash, const char *path, void *data, size_t size, const char *region) {
    if (!path || !path[0])
        return;

    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    size_t contents_size = 0;
    if (!g_file_get_contents(path, &contents, &contents_size, &error)) {
        if (g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            return;
        cfi_flash_error(flash, "Can't read %s file %s: %s", region, path, error->message);
        exit(1);
    }
    if (!contents_size)
        return;
    if (contents_size != size) {
        cfi_flash_error(flash, "Invalid %s file size: %s is %zu bytes, expected %zu",
            region, path, contents_size, size);
        exit(1);
    }

    memcpy(data, contents, size);
    DEBUG_LOG(&flash->dbg, "loaded %s from %s\n", region, path);
}

static void cfi_flash_save_file(CFIFlashState *flash, int *fd, const char *path, const void *data, size_t total_size,
    size_t offset, size_t size, const char *region)
{
    if (!path || !path[0] || !flash->writable)
        return;

    if (*fd < 0) {
        Error *error = NULL;
        *fd = qemu_create(path, O_WRONLY, 0644, &error);
        if (*fd < 0) {
            cfi_flash_error(flash, "Can't open %s file %s: %s", region, path, error_get_pretty(error));
            error_free(error);
            exit(1);
        }
        // The file may not exist yet, so materialize the whole region once.
        offset = 0;
        size = total_size;
    }

    if (lseek(*fd, offset, SEEK_SET) == (off_t) -1 ||
        qemu_write_full(*fd, (const uint8_t *) data + offset, size) != (ssize_t) size)
    {
        cfi_flash_error(flash, "Can't write %s file %s: %s", region, path, strerror(errno));
        exit(1);
    }
    DEBUG_LOG(&flash->dbg, "saved %s to %s [%08zX...%08zX]\n", region, path, offset, offset + size - 1);
}

static void cfi_flash_init_file_paths(CFIFlashState *flash) {
    const CFIFlashChip *cfg = flash->cfg;
    if (flash->blk && strcmp(flash->name, "FLASH0") == 0) {
        const char *fullflash_file = cfi_flash_storage_filename(flash->blk);
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
        cfi_flash_error(flash, "OTP0 file was provided, but this flash has no OTP0 region");
        exit(1);
    }
    if (!cfg->otp1_size && flash->otp1_file && flash->otp1_file[0]) {
        cfi_flash_error(flash, "OTP1 file was provided, but this flash has no OTP1 region");
        exit(1);
    }
    if (!cfg->efa_erase_regions_count && flash->efa_file && flash->efa_file[0]) {
        cfi_flash_error(flash, "EFA file was provided, but this flash has no EFA region");
        exit(1);
    }
}

static void cfi_flash_init_efa(CFIFlashState *flash) {
    for (uint32_t i = 0; i < flash->cfg->efa_erase_regions_count; i++) {
        const CFIFlashEraseRegion *region = &flash->cfg->efa_erase_regions[i];
        flash->efa_size += region->size;
        flash->efa_blocks_n += region->size / region->sector;
    }

    if (!flash->efa_size)
        return;

    flash->efa_storage = g_malloc(flash->efa_size);
    memset(flash->efa_storage, 0xFF, flash->efa_size);
    flash->efa_blocks = g_new0(CFIFlashBlock, flash->efa_blocks_n);

    uint32_t block_id = 0;
    for (uint32_t i = 0; i < flash->cfg->efa_erase_regions_count; i++) {
        const CFIFlashEraseRegion *region = &flash->cfg->efa_erase_regions[i];
        uint32_t blocks = region->size / region->sector;
        for (uint32_t j = 0; j < blocks; j++) {
            CFIFlashBlock *blk = &flash->efa_blocks[block_id++];
            blk->offset = region->offset + j * region->sector;
            blk->size = region->sector;
            blk->locked = true;
        }
    }

    DEBUG_LOG(&flash->dbg, "EFA 0x00000000 ... 0x%08X\n", flash->efa_size - 1);
}

static void cfi_flash_init_part(CFIFlashState *flash, const CFIFlashChipPart *part_cfg) {
    CFIFlashPart *p = g_new0(CFIFlashPart, 1);
    p->n = flash->parts_n;
    flash->parts = g_renew(CFIFlashPart *, flash->parts, flash->parts_n + 1);
    flash->parts[flash->parts_n++] = p;
    p->flash = flash;
    p->offset = part_cfg->offset;
    p->size = part_cfg->size;
    p->cfg = part_cfg;
    p->status = CFI_FLASH_STATUS_READY;
    p->romd_threshold = CFI_FLASH_ROMD_THRESHOLD_MIN;

    char *name = g_strdup_printf("cfi-flash[%s][%d]", p->flash->name, p->n);
    memory_region_init_rom_device(&p->mem, OBJECT(p->flash->dev), &io_ops, p, name, p->size, NULL);
    memory_region_rom_device_set_romd(&p->mem, true);
    memory_region_add_subregion(&flash->mmio, p->offset, &p->mem);
    g_free(name);

    p->storage = memory_region_get_ram_ptr(&p->mem);
    p->dirty_pages = bitmap_new(DIV_ROUND_UP(p->size, CFI_FLASH_WRITEBACK_PAGE_SIZE));

    DEBUG_LOG(&p->flash->dbg, "<%d> hw partition 0x%08X ... 0x%08X\n",
        p->n, p->flash->offset + p->offset, p->flash->offset + p->offset + p->size - 1);

    if (flash->blk) {
        int ret = cfi_flash_storage_pread(flash->blk, flash->offset + p->offset, p->size, p->storage);
        if (ret < 0) {
            cfi_flash_error(flash, "failed to read the initial flash content [offset=%08X, size=%08X]",
                flash->offset + p->offset, p->size);
            exit(1);
        }
    } else {
        memset(p->storage, 0xFF, p->size);
    }

    p->blocks_n = 0;
    for (uint32_t i = 0; i < p->cfg->erase_regions_cnt; i++)
        p->blocks_n += p->cfg->erase_regions[i].size / p->cfg->erase_regions[i].sector;

    p->blocks = g_new0(CFIFlashBlock, p->blocks_n);

    uint32_t block_id = 0;
    uint32_t block_offset = 0;

    for (uint32_t i = 0; i < p->cfg->erase_regions_cnt; i++) {
        const CFIFlashEraseRegion *region = &p->cfg->erase_regions[i];
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

static void cfi_flash_realize(DeviceState *dev, Error **errp) {
    CFIFlashState *flash = CFI_FLASH(dev);
    debug_channel_init_device(&flash->dbg, dev);
    flash->dev = dev;
    flash->otp0_fd = -1;
    flash->otp1_fd = -1;
    flash->efa_fd = -1;

    const CFIFlashChip *cfg = cfi_flash_find_chip(flash->vid, flash->pid);
    if (!cfg) {
        cfi_flash_error(flash, "unimplemented %04X:%04X", flash->vid, flash->pid);
        exit(1);
    }

    flash->cfg = cfg;
    if (!flash->size)
        flash->size = cfg->size;
    if (flash->size > cfg->size) {
        cfi_flash_error(flash, "invalid size 0x%08X, maximum is 0x%08X", flash->size, cfg->size);
        exit(1);
    }

    bool complete_partition = false;
    for (size_t i = 0; i < cfg->parts_count; i++) {
        const CFIFlashChipPart *part = &cfg->parts[i];
        if (part->offset + part->size == flash->size) {
            complete_partition = true;
            break;
        }
    }
    if (!complete_partition) {
        cfi_flash_error(flash, "size 0x%08X does not end on a hardware partition boundary", flash->size);
        exit(1);
    }
    cfi_flash_init_file_paths(flash);
    flash->writable = flash->blk && cfi_flash_storage_is_rw(flash->blk);
    if (flash->writable) {
        flash->writeback_timer = timer_new_ms(QEMU_CLOCK_REALTIME, cfi_flash_writeback_expire, flash);
        qemu_add_vm_change_state_handler(cfi_flash_vm_state_change, flash);
    }

    DEBUG_LOG(&flash->dbg, "FLASH %04X:%04X, 0x%08X ... 0x%08X\n",
        flash->vid, flash->pid, flash->offset, flash->offset + flash->size - 1);

    char *mmio_name = g_strdup_printf("cfi-flash[%s]", flash->name);
    memory_region_init(&flash->mmio, OBJECT(flash->dev), mmio_name, flash->size);
    g_free(mmio_name);

    // OTP0
    if (cfg->otp0_size > 0) {
        flash->otp0_data = g_new(uint16_t, cfg->otp0_size / 2);
        memset(flash->otp0_data, 0xFF, cfg->otp0_size);
        flash->otp0_data[0] = 0x0002;

        if (!fill_data_from_hex((uint8_t *) flash->otp0_data, cfg->otp0_size, flash->hex_otp0_data)) {
            cfi_flash_error(flash, "Invalid OTP0 hex data: %s [max_size=%d, len=%zu]",
                flash->hex_otp0_data, cfg->otp0_size, strlen(flash->hex_otp0_data) / 2);
            exit(1);
        }
    }

    // OTP1
    if (cfg->otp1_size > 0) {
        flash->otp1_data = g_new(uint16_t, cfg->otp1_size / 2);
        memset(flash->otp1_data, 0xFF, cfg->otp1_size);

        if (!fill_data_from_hex((uint8_t *) flash->otp1_data, cfg->otp1_size, flash->hex_otp1_data)) {
            cfi_flash_error(flash, "Invalid OTP1 hex data: %s [max_size=%d, len=%zu]",
                flash->hex_otp1_data, cfg->otp1_size, strlen(flash->hex_otp1_data) / 2);
            exit(1);
        }
    }

    if (cfg->otp0_size)
        cfi_flash_load_file(flash, flash->otp0_file, flash->otp0_data, cfg->otp0_size, "OTP0");
    if (cfg->otp1_size)
        cfi_flash_load_file(flash, flash->otp1_file, flash->otp1_data, cfg->otp1_size, "OTP1");

    // Init hw partitions
    cfi_flash_init_efa(flash);
    if (flash->efa_size)
        cfi_flash_load_file(flash, flash->efa_file, flash->efa_storage, flash->efa_size, "EFA");
    for (size_t i = 0; i < cfg->parts_count && cfg->parts[i].offset < flash->size; i++)
        cfi_flash_init_part(flash, &cfg->parts[i]);

    sysbus_init_mmio(SYS_BUS_DEVICE(flash->dev), &flash->mmio);
}

static void cfi_flash_device_reset(DeviceState *dev) {
    CFIFlashState *flash = CFI_FLASH(dev);

    for (uint32_t i = 0; i < flash->parts_n; i++) {
        CFIFlashPart *p = flash->parts[i];
        p->status = CFI_FLASH_STATUS_READY;
        for (uint32_t j = 0; j < p->blocks_n; j++) {
            if (p->blocks[j].locked_down) {
                p->blocks[j].locked = true;
                p->blocks[j].locked_down = false;
            }
        }
        cfi_flash_reset(p);
        p->mmio_array_reads = 0;
        p->romd_threshold = CFI_FLASH_ROMD_THRESHOLD_MIN;
        cfi_flash_romd_map(p);
    }

    for (uint32_t i = 0; i < flash->efa_blocks_n; i++) {
        if (flash->efa_blocks[i].locked_down) {
            flash->efa_blocks[i].locked = true;
            flash->efa_blocks[i].locked_down = false;
        }
    }
}

static void cfi_flash_error(CFIFlashState *flash, const char *format, ...) {
    g_autoptr(GString) s = g_string_new("");

    va_list args;
    va_start(args, format);
    g_string_append_vprintf(s, format, args);
    va_end(args);

    error_report("%s: %s", flash->name, s->str);
}

static const Property cfi_flash_properties[] = {
    DEFINE_PROP_LINK("blk", CFIFlashState, blk, TYPE_CFI_FLASH_STORAGE, CFIFlashStorage *),

    DEFINE_PROP_STRING("name", CFIFlashState, name),
    DEFINE_PROP_UINT16("vid", CFIFlashState, vid, 0),
    DEFINE_PROP_UINT16("pid", CFIFlashState, pid, 0),
    DEFINE_PROP_UINT32("offset", CFIFlashState, offset, 0),
    DEFINE_PROP_UINT32("size", CFIFlashState, size, 0),
    /* Refuse program/buffer opcodes the part's CFI command set does not
     * document; see flash_command_is_documented(). */
    DEFINE_PROP_BOOL("strict-opcodes", CFIFlashState, strict_opcodes, false),
    DEFINE_PROP_STRING("otp0-file", CFIFlashState, otp0_file),
    DEFINE_PROP_STRING("otp1-file", CFIFlashState, otp1_file),
    DEFINE_PROP_STRING("efa-file", CFIFlashState, efa_file),

    /* OTP0 Initial Data */
    DEFINE_PROP_STRING("otp0-data", CFIFlashState, hex_otp0_data),

    /* OTP1 Initial Data */
    DEFINE_PROP_STRING("otp1-data", CFIFlashState, hex_otp1_data),
};

static void cfi_flash_class_init(ObjectClass *klass, const void *data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, cfi_flash_properties);
    device_class_set_legacy_reset(dc, cfi_flash_device_reset);
    dc->realize = cfi_flash_realize;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo cfi_flash_info = {
    .name           = TYPE_CFI_FLASH,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(CFIFlashState),
    .class_init     = cfi_flash_class_init,
};

static void cfi_flash_register_types(void) {
    type_register_static(&cfi_flash_info);
}
type_init(cfi_flash_register_types)
