/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/bitmap.h"
#include "qemu/aio-wait.h"
#include "system/runstate.h"
#include "hw/core/qdev-properties.h"
#include "hw/block/block.h"
#include "cfi-flash.h"

#define CFI_PRIMARY_ALGORITHM 0x13

#define CFI_FLASH_ROMD_THRESHOLD_MIN        42
#define CFI_FLASH_ROMD_THRESHOLD_MAX        (64 * 1024)
#define CFI_FLASH_ROMD_SHORT_PERIOD_NS      (1 * SCALE_MS)

#define CFI_FLASH_WRITEBACK_PAGE_SIZE       4096
#define CFI_FLASH_WRITEBACK_DELAY_MS        1000

static void cfi_flash_error(CFIFlashState *flash, const char *format, ...) G_GNUC_PRINTF(2, 3);

static void cfi_flash_save_file(
    CFIFlashState *flash,
    int *fd,
    const char *path,
    const void *data,
    size_t total_size,
    size_t offset,
    size_t size,
    const char *region
);

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

static void cfi_flash_unmap_inactive_read_array(CFIFlashPart *p) {
    if (p->read_array)
        return;
    if (memory_region_is_romd(&p->mem))
        cfi_flash_romd_unmap(p);
    p->mmio_array_reads = 0;
}

CFIFlashBlock *cfi_flash_part_find_block(CFIFlashPart *p, uint32_t offset) {
    offset -= p->offset;
    for (uint32_t i = 0; i < p->blocks_n; i++) {
        CFIFlashBlock *blk = &p->blocks[i];
        if (offset >= blk->offset && offset < blk->offset + blk->size)
            return blk;
    }
    g_assert_not_reached();
}

CFIFlashBlock *cfi_flash_efa_find_block(CFIFlashState *flash, uint32_t efa_offset) {
    for (uint32_t i = 0; i < flash->efa_blocks_n; i++) {
        CFIFlashBlock *blk = &flash->efa_blocks[i];
        if (efa_offset >= blk->offset && efa_offset < blk->offset + blk->size)
            return blk;
    }
    return NULL;
}

uint32_t cfi_flash_find_sector_size(CFIFlashPart *p, uint32_t offset) {
    offset -= p->offset;

    for (uint32_t i = 0; i < p->info->erase_regions_count; i++) {
        const CFIFlashEraseRegion *region = &p->info->erase_regions[i];
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
        cfi_flash_storage_aio_pwrite(p->flash->storage, p->flash->offset + p->offset + storage_offset, size,
            p->storage + storage_offset, cfi_flash_writeback_done, p->flash);

        page = find_next_bit(p->dirty_pages, pages_n, end_page);
    }
}

static void cfi_flash_writeback_start(CFIFlashState *flash) {
    const CFIFlashChip *chip = flash->chip;

    for (uint32_t i = 0; i < flash->parts_n; i++)
        cfi_flash_part_writeback_start(flash->parts[i]);

    cfi_flash_side_file_save(flash, &flash->otp0_dirty, &flash->otp0_fd, flash->otp0_file, flash->otp0_data,
        chip->otp0_size, "OTP0");
    cfi_flash_side_file_save(flash, &flash->otp1_dirty, &flash->otp1_fd, flash->otp1_file, flash->otp1_data,
        chip->otp1_size, "OTP1");
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

static uint32_t cfi_flash_array_read(CFIFlashPart *p, uint32_t offset, uint32_t size) {
    uint32_t value = 0;
    offset -= p->offset;
    for (uint32_t i = 0; i < size; i++)
        value |= (uint32_t) p->storage[offset + i] << (i * 8);
    return value;
}

CFIFlashResult cfi_flash_program(CFIFlashPart *p, CFIFlashBlock *blk, uint32_t offset, uint32_t value, uint32_t size) {
    if (blk->protected)
        return CFI_FLASH_PROTECTED;
    cfi_flash_data_write(p, offset, value, size);
    return CFI_FLASH_OK;
}

CFIFlashResult cfi_flash_erase_block(CFIFlashPart *p, CFIFlashBlock *blk) {
    if (blk->protected)
        return CFI_FLASH_PROTECTED;
    memset(p->storage + blk->offset, 0xFF, blk->size);
    cfi_flash_part_mark_dirty(p, blk->offset, blk->size);
    return CFI_FLASH_OK;
}

bool cfi_flash_block_is_blank(CFIFlashPart *p, CFIFlashBlock *blk) {
    const uint8_t *data = p->storage + blk->offset;

    for (uint32_t i = 0; i < blk->size; i++) {
        if (data[i] != 0xFF)
            return false;
    }
    return true;
}

bool cfi_flash_query_read(CFIFlashPart *p, uint16_t index, uint32_t *value) {
    const CFIFlashChip *chip = p->flash->chip;

    if (index >= CFI_ADDR && index < CFI_ADDR + chip->cfi_size) {
        *value = chip->cfi[index - CFI_ADDR];
        DEBUG_LOG(&p->flash->dbg, "<%d> CFI %02X: %02X\n", p->n, index, *value);
        return true;
    }
    if (index >= chip->pri_addr && index < chip->pri_addr + chip->pri_size) {
        *value = chip->pri[index - chip->pri_addr];
        DEBUG_LOG(&p->flash->dbg, "<%d> PRI %02X: %02X\n", p->n, index - chip->pri_addr, *value);
        return true;
    }
    return false;
}

CFIFlashResult cfi_flash_efa_read(CFIFlashState *flash, uint32_t efa_offset, uint32_t size, uint32_t *value) {
    if (efa_offset + size > flash->efa_size)
        return CFI_FLASH_OUT_OF_RANGE;
    *value = lduw_le_p(flash->efa_storage + efa_offset);
    return CFI_FLASH_OK;
}

CFIFlashResult cfi_flash_efa_program(CFIFlashState *flash, uint32_t efa_offset, uint64_t value, uint32_t size) {
    CFIFlashBlock *blk = cfi_flash_efa_find_block(flash, efa_offset);

    if (!blk || efa_offset + size > flash->efa_size)
        return CFI_FLASH_OUT_OF_RANGE;
    if (blk->protected)
        return CFI_FLASH_PROTECTED;

    bool changed = false;
    for (uint32_t i = 0; i < size; i++) {
        uint8_t old_value = flash->efa_storage[efa_offset + i];
        uint8_t programmed_value = value >> (i * 8);
        uint8_t new_value = old_value & programmed_value;
        flash->efa_storage[efa_offset + i] = new_value;
        if (old_value != new_value)
            changed = true;
    }
    if (changed)
        cfi_flash_side_file_mark_dirty(flash, &flash->efa_dirty, efa_offset, size);
    return CFI_FLASH_OK;
}

CFIFlashResult cfi_flash_efa_erase_block(CFIFlashState *flash, CFIFlashBlock *blk) {
    if (blk->protected)
        return CFI_FLASH_PROTECTED;

    bool changed = false;
    for (uint32_t i = 0; i < blk->size; i++) {
        if (flash->efa_storage[blk->offset + i] != 0xFF) {
            changed = true;
            break;
        }
    }
    memset(flash->efa_storage + blk->offset, 0xFF, blk->size);
    if (changed)
        cfi_flash_side_file_mark_dirty(flash, &flash->efa_dirty, blk->offset, blk->size);
    return CFI_FLASH_OK;
}

void cfi_flash_otp_program(CFIFlashState *flash, uint16_t *otp_data, uint32_t data_index, uint16_t value) {
    uint16_t old_value = otp_data[data_index];

    otp_data[data_index] &= value;
    if (otp_data[data_index] == old_value)
        return;
    uint32_t data_offset = data_index * sizeof(otp_data[0]);
    if (otp_data == flash->otp0_data) {
        cfi_flash_side_file_mark_dirty(flash, &flash->otp0_dirty, data_offset, sizeof(otp_data[0]));
    } else {
        cfi_flash_side_file_mark_dirty(flash, &flash->otp1_dirty, data_offset, sizeof(otp_data[0]));
    }
}

static uint64_t cfi_flash_io_read(void *opaque, hwaddr part_offset, uint32_t size) {
    CFIFlashPart *p = opaque;
    uint32_t offset = p->offset + part_offset;

    if (!p->read_array)
        return p->flash->cmdset->read(p, offset, size);

    uint32_t value = cfi_flash_array_read(p, offset, size);
    if (++p->mmio_array_reads > p->romd_threshold)
        cfi_flash_romd_map(p);
    return value;
}

static void cfi_flash_io_write(void *opaque, hwaddr part_offset, uint64_t value, uint32_t size) {
    CFIFlashPart *p = opaque;

    p->flash->cmdset->write(p, p->offset + part_offset, value, size);
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

static void cfi_flash_save_file(
    CFIFlashState *flash,
    int *fd,
    const char *path,
    const void *data,
    size_t total_size,
    size_t offset,
    size_t size,
    const char *region
) {
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
        qemu_write_full(*fd, (const uint8_t *) data + offset, size) != (ssize_t) size) {
        cfi_flash_error(flash, "Can't write %s file %s: %s", region, path, strerror(errno));
        exit(1);
    }
    DEBUG_LOG(&flash->dbg, "saved %s to %s [%08zX...%08zX]\n", region, path, offset, offset + size - 1);
}

static void cfi_flash_init_file_paths(CFIFlashState *flash) {
    const CFIFlashChip *chip = flash->chip;
    if (flash->storage && strcmp(flash->name, "FLASH0") == 0) {
        const char *fullflash_file = cfi_flash_storage_filename(flash->storage);
        if ((!flash->otp0_file || !flash->otp0_file[0]) && chip->otp0_size) {
            g_free(flash->otp0_file);
            flash->otp0_file = g_strdup_printf("%s.cfi-otp0", fullflash_file);
        }
        if ((!flash->otp1_file || !flash->otp1_file[0]) && chip->otp1_size) {
            g_free(flash->otp1_file);
            flash->otp1_file = g_strdup_printf("%s.cfi-otp1", fullflash_file);
        }
        if ((!flash->efa_file || !flash->efa_file[0]) && chip->efa_erase_regions_count) {
            g_free(flash->efa_file);
            flash->efa_file = g_strdup_printf("%s.cfi-efa", fullflash_file);
        }
    }

    if (!chip->otp0_size && flash->otp0_file && flash->otp0_file[0]) {
        cfi_flash_error(flash, "OTP0 file was provided, but this flash has no OTP0 region");
        exit(1);
    }
    if (!chip->otp1_size && flash->otp1_file && flash->otp1_file[0]) {
        cfi_flash_error(flash, "OTP1 file was provided, but this flash has no OTP1 region");
        exit(1);
    }
    if (!chip->efa_erase_regions_count && flash->efa_file && flash->efa_file[0]) {
        cfi_flash_error(flash, "EFA file was provided, but this flash has no EFA region");
        exit(1);
    }
}

static void cfi_flash_init_efa(CFIFlashState *flash) {
    for (uint32_t i = 0; i < flash->chip->efa_erase_regions_count; i++) {
        const CFIFlashEraseRegion *region = &flash->chip->efa_erase_regions[i];
        flash->efa_size += region->size;
        flash->efa_blocks_n += region->size / region->sector;
    }

    if (!flash->efa_size)
        return;

    flash->efa_storage = g_malloc(flash->efa_size);
    memset(flash->efa_storage, 0xFF, flash->efa_size);
    flash->efa_blocks = g_new0(CFIFlashBlock, flash->efa_blocks_n);

    uint32_t block_id = 0;
    for (uint32_t i = 0; i < flash->chip->efa_erase_regions_count; i++) {
        const CFIFlashEraseRegion *region = &flash->chip->efa_erase_regions[i];
        uint32_t blocks = region->size / region->sector;
        for (uint32_t j = 0; j < blocks; j++) {
            CFIFlashBlock *blk = &flash->efa_blocks[block_id++];
            blk->offset = region->offset + j * region->sector;
            blk->size = region->sector;
        }
    }

    DEBUG_LOG(&flash->dbg, "EFA 0x00000000 ... 0x%08X\n", flash->efa_size - 1);
}

static void cfi_flash_init_part(CFIFlashState *flash, const CFIFlashChipPart *part_info) {
    CFIFlashPart *p = g_malloc0(flash->cmdset->part_size);
    p->n = flash->parts_n;
    flash->parts = g_renew(CFIFlashPart *, flash->parts, flash->parts_n + 1);
    flash->parts[flash->parts_n++] = p;
    p->flash = flash;
    p->offset = part_info->offset;
    p->size = part_info->size;
    p->info = part_info;
    p->read_array = true;
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

    if (flash->storage) {
        int ret = cfi_flash_storage_pread(flash->storage, flash->offset + p->offset, p->size, p->storage);
        if (ret < 0) {
            cfi_flash_error(flash, "failed to read the initial flash content [offset=%08X, size=%08X]",
                flash->offset + p->offset, p->size);
            exit(1);
        }
    } else {
        memset(p->storage, 0xFF, p->size);
    }

    p->blocks_n = 0;
    for (uint32_t i = 0; i < p->info->erase_regions_count; i++)
        p->blocks_n += p->info->erase_regions[i].size / p->info->erase_regions[i].sector;

    p->blocks = g_new0(CFIFlashBlock, p->blocks_n);

    uint32_t block_id = 0;
    uint32_t block_offset = 0;

    for (uint32_t i = 0; i < p->info->erase_regions_count; i++) {
        const CFIFlashEraseRegion *region = &p->info->erase_regions[i];
        uint32_t sectors = region->size / region->sector;
        for (uint32_t j = 0; j < sectors; j++) {
            p->blocks[block_id].offset = block_offset;
            p->blocks[block_id].size = region->sector;
            block_offset += region->sector;
            block_id++;
        }
    }
}

static const CFIFlashCommandSet *cfi_flash_get_cmdset(const CFIFlashChip *chip) {
    switch (lduw_le_p(&chip->cfi[CFI_PRIMARY_ALGORITHM - CFI_ADDR])) {
        case 0x0001:
        case 0x0003:
        case 0x0200:
            return &cfi_flash_intel_cmdset;
        default:
            return NULL;
    }
}

static void cfi_flash_realize(DeviceState *dev, Error **errp) {
    CFIFlashState *flash = CFI_FLASH(dev);
    debug_channel_init_device(&flash->dbg, dev);
    flash->dev = dev;
    flash->otp0_fd = -1;
    flash->otp1_fd = -1;
    flash->efa_fd = -1;

    const CFIFlashChip *chip = cfi_flash_find_chip(flash->vid, flash->pid);
    if (!chip) {
        cfi_flash_error(flash, "unimplemented %04X:%04X", flash->vid, flash->pid);
        exit(1);
    }

    flash->chip = chip;
    flash->cmdset = cfi_flash_get_cmdset(chip);
    if (!flash->cmdset) {
        cfi_flash_error(flash, "unsupported CFI command set %04X",
            lduw_le_p(&chip->cfi[CFI_PRIMARY_ALGORITHM - CFI_ADDR]));
        exit(1);
    }
    if (!flash->size)
        flash->size = chip->size;
    if (flash->size > chip->size) {
        cfi_flash_error(flash, "invalid size 0x%08X, maximum is 0x%08X", flash->size, chip->size);
        exit(1);
    }

    bool complete_partition = false;
    for (size_t i = 0; i < chip->parts_count; i++) {
        const CFIFlashChipPart *part = &chip->parts[i];
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
    flash->writable = flash->storage && cfi_flash_storage_is_rw(flash->storage);
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
    if (chip->otp0_size > 0) {
        flash->otp0_data = g_new(uint16_t, chip->otp0_size / 2);
        memset(flash->otp0_data, 0xFF, chip->otp0_size);
        flash->otp0_data[0] = 0x0002;

        if (!fill_data_from_hex((uint8_t *) flash->otp0_data, chip->otp0_size, flash->hex_otp0_data)) {
            cfi_flash_error(flash, "Invalid OTP0 hex data: %s [max_size=%d, len=%zu]",
                flash->hex_otp0_data, chip->otp0_size, strlen(flash->hex_otp0_data) / 2);
            exit(1);
        }
    }

    // OTP1
    if (chip->otp1_size > 0) {
        flash->otp1_data = g_new(uint16_t, chip->otp1_size / 2);
        memset(flash->otp1_data, 0xFF, chip->otp1_size);

        if (!fill_data_from_hex((uint8_t *) flash->otp1_data, chip->otp1_size, flash->hex_otp1_data)) {
            cfi_flash_error(flash, "Invalid OTP1 hex data: %s [max_size=%d, len=%zu]",
                flash->hex_otp1_data, chip->otp1_size, strlen(flash->hex_otp1_data) / 2);
            exit(1);
        }
    }

    if (chip->otp0_size)
        cfi_flash_load_file(flash, flash->otp0_file, flash->otp0_data, chip->otp0_size, "OTP0");
    if (chip->otp1_size)
        cfi_flash_load_file(flash, flash->otp1_file, flash->otp1_data, chip->otp1_size, "OTP1");

    // Init hw partitions
    cfi_flash_init_efa(flash);
    if (flash->efa_size)
        cfi_flash_load_file(flash, flash->efa_file, flash->efa_storage, flash->efa_size, "EFA");
    for (size_t i = 0; i < chip->parts_count && chip->parts[i].offset < flash->size; i++)
        cfi_flash_init_part(flash, &chip->parts[i]);
    flash->cmdset->init(flash);

    sysbus_init_mmio(SYS_BUS_DEVICE(flash->dev), &flash->mmio);
}

static void cfi_flash_device_reset(DeviceState *dev) {
    CFIFlashState *flash = CFI_FLASH(dev);

    flash->cmdset->reset(flash);
    for (uint32_t i = 0; i < flash->parts_n; i++) {
        CFIFlashPart *p = flash->parts[i];
        p->mmio_array_reads = 0;
        p->romd_threshold = CFI_FLASH_ROMD_THRESHOLD_MIN;
        cfi_flash_romd_map(p);
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
    DEFINE_PROP_LINK("storage", CFIFlashState, storage, TYPE_CFI_FLASH_STORAGE, CFIFlashStorage *),

    DEFINE_PROP_STRING("name", CFIFlashState, name),
    DEFINE_PROP_UINT16("vid", CFIFlashState, vid, 0),
    DEFINE_PROP_UINT16("pid", CFIFlashState, pid, 0),
    DEFINE_PROP_UINT32("offset", CFIFlashState, offset, 0),
    DEFINE_PROP_UINT32("size", CFIFlashState, size, 0),
    /* Refuse program opcodes 0x10 and 0xE8, which command set 0200 does not document. */
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
