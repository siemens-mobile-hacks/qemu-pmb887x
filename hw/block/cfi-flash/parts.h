/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

typedef struct CFIFlashEraseRegion CFIFlashEraseRegion;
typedef struct CFIFlashChipPart CFIFlashChipPart;
typedef struct CFIFlashChip CFIFlashChip;

struct CFIFlashEraseRegion {
    uint32_t offset;
    uint32_t size;
    uint32_t sector;
};

struct CFIFlashChipPart {
    uint32_t offset;
    uint32_t size;
    const CFIFlashEraseRegion *erase_regions;
    uint32_t erase_regions_cnt;
};

struct CFIFlashChip {
    uint16_t vid;
    uint16_t pid;
    uint16_t cr;
    uint16_t ehcr;
    uint32_t size;
    const uint8_t *cfi;
    uint32_t cfi_size;
    const uint8_t *pri;
    uint32_t pri_size;
    const CFIFlashChipPart *parts;
    uint32_t parts_count;
    const CFIFlashEraseRegion *efa_erase_regions;
    uint32_t efa_erase_regions_count;
    uint16_t pri_addr;

    uint16_t otp0_addr;
    uint16_t otp0_size;
    uint16_t otp0_factory_size;
    uint16_t otp1_addr;
    uint16_t otp1_size;
};

const CFIFlashChip *cfi_flash_find_chip(uint16_t vid, uint16_t pid);
