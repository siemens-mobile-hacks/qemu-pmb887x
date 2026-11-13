/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#define CFI_FLASH_PID_WORDS 3

typedef struct CFIFlashChip CFIFlashChip;

struct CFIFlashChip {
    uint16_t vid;
    uint16_t pid[CFI_FLASH_PID_WORDS];
    const uint8_t *cfi;
    uint32_t cfi_size;
    const uint8_t *pri;
    uint32_t pri_size;
    uint16_t rcr;
    uint16_t ecr;
    uint16_t indicator_bits;
};

const CFIFlashChip *cfi_flash_find_chip(uint16_t vid, const uint16_t pid[CFI_FLASH_PID_WORDS]);
