/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

typedef struct CFIFlashChip CFIFlashChip;

struct CFIFlashChip {
    uint16_t vid;
    uint16_t pid;
    const uint8_t *cfi;
    uint32_t cfi_size;
    const uint8_t *pri;
    uint32_t pri_size;
    uint16_t rcr;
    uint16_t ecr;
};

const CFIFlashChip *cfi_flash_find_chip(uint16_t vid, uint16_t pid);
