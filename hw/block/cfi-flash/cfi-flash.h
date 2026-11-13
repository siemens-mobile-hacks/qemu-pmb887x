/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "qemu/timer.h"
#include "system/memory.h"
#include "hw/core/sysbus.h"
#include "system/debug-channel.h"
#include "hw/block/cfi-flash.h"
#include "parts.h"

#define CFI_ADDR 0x10
#define CFI_INDEX_MASK 0xFFF
#define CFI_ERASE_SECTOR_UNIT 256

#define P_ID_INTEL_EXT          0x0001
#define P_ID_INTEL_STD          0x0003
#define P_ID_INTEL_PERFORMANCE  0x0200

OBJECT_DECLARE_SIMPLE_TYPE(CFIFlashState, CFI_FLASH)

typedef struct CFIFlashEraseRegion CFIFlashEraseRegion;
typedef struct CFIFlashChipPart CFIFlashChipPart;
typedef struct CFIFlashOtp CFIFlashOtp;
typedef struct CFIQueryReader CFIQueryReader;
typedef struct CFIFlashPart CFIFlashPart;
typedef struct CFIFlashBlock CFIFlashBlock;
typedef struct CFIFlashDirtyRange CFIFlashDirtyRange;
typedef struct CFIFlashCommandSet CFIFlashCommandSet;
typedef enum CFIFlashResult CFIFlashResult;

enum CFIFlashResult {
    CFI_FLASH_OK,
    CFI_FLASH_PROTECTED,
    CFI_FLASH_OUT_OF_RANGE,
};

struct CFIFlashEraseRegion {
    uint32_t offset;
    uint32_t size;
    uint32_t sector;
};

struct CFIFlashChipPart {
    uint32_t offset;
    uint32_t size;
    const CFIFlashEraseRegion *erase_regions;
    uint32_t erase_regions_count;
};

struct CFIFlashOtp {
    uint32_t addr;
    uint32_t size;
    uint32_t factory_size;
};

struct CFIQueryReader {
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;
    bool overrun;
};

struct CFIFlashBlock {
    uint32_t offset;
    uint32_t size;
    bool protected;
};

struct CFIFlashDirtyRange {
    uint32_t start;
    uint32_t end;
};

struct CFIFlashPart {
    CFIFlashState *flash;
    uint16_t n;
    uint32_t size;
    uint32_t offset;
    const CFIFlashChipPart *info;

    bool read_array;
    uint32_t mmio_array_reads;
    uint32_t romd_threshold;
    int64_t romd_since_ns;

    uint8_t *storage;
    unsigned long *dirty_pages;

    uint32_t blocks_n;
    CFIFlashBlock *blocks;

    MemoryRegion mem;
};

struct CFIFlashCommandSet {
    size_t part_size;
    bool (*decode_pri)(CFIFlashState *flash);
    void (*init)(CFIFlashState *flash);
    uint64_t (*read)(CFIFlashPart *p, uint32_t offset, uint32_t size);
    void (*write)(CFIFlashPart *p, uint32_t offset, uint64_t value, uint32_t size);
    void (*reset)(CFIFlashState *flash);
};

struct CFIFlashState {
    SysBusDevice parent_obj;
    DebugChannel dbg;
    DeviceState *dev;
    MemoryRegion mmio;

    CFIFlashStorage *storage;
    const CFIFlashChip *chip;
    const CFIFlashCommandSet *cmdset;
    void *cmdset_state;

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

    uint32_t offset;

    uint32_t size;
    uint16_t algorithm;
    uint16_t pri_addr;
    uint32_t cfi_erase_regions_n;
    CFIFlashEraseRegion *cfi_erase_regions;
    uint32_t chip_parts_n;
    CFIFlashChipPart *chip_parts;
    uint32_t efa_erase_regions_n;
    CFIFlashEraseRegion *efa_erase_regions;
    CFIFlashOtp otp0;
    CFIFlashOtp otp1;

    uint16_t *otp0_data;
    uint16_t *otp1_data;
    uint8_t *efa_storage;
    uint32_t efa_size;
    uint32_t efa_blocks_n;
    CFIFlashBlock *efa_blocks;

    uint32_t parts_n;
    CFIFlashPart **parts;
};

void cfi_flash_error(CFIFlashState *flash, const char *format, ...) G_GNUC_PRINTF(2, 3);
uint32_t cfi_query_read_le(CFIQueryReader *r, uint32_t bytes);
void cfi_query_skip(CFIQueryReader *r, uint32_t bytes);

CFIFlashBlock *cfi_flash_part_find_block(CFIFlashPart *p, uint32_t offset);
uint32_t cfi_flash_find_sector_size(CFIFlashPart *p, uint32_t offset);
CFIFlashResult cfi_flash_program(CFIFlashPart *p, CFIFlashBlock *blk, uint32_t offset, uint32_t value, uint32_t size);
CFIFlashResult cfi_flash_erase_block(CFIFlashPart *p, CFIFlashBlock *blk);
bool cfi_flash_block_is_blank(CFIFlashPart *p, CFIFlashBlock *blk);
bool cfi_flash_query_read(CFIFlashPart *p, uint16_t index, uint32_t *value);

CFIFlashBlock *cfi_flash_efa_find_block(CFIFlashState *flash, uint32_t efa_offset);
CFIFlashResult cfi_flash_efa_read(CFIFlashState *flash, uint32_t efa_offset, uint32_t size, uint32_t *value);
CFIFlashResult cfi_flash_efa_program(CFIFlashState *flash, uint32_t efa_offset, uint64_t value, uint32_t size);
CFIFlashResult cfi_flash_efa_erase_block(CFIFlashState *flash, CFIFlashBlock *blk);

void cfi_flash_otp_program(CFIFlashState *flash, uint16_t *otp_data, uint32_t data_index, uint16_t value);

extern const CFIFlashCommandSet cfi_flash_intel_cmdset;
