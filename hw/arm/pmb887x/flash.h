#pragma once

#include "qemu/osdep.h"

typedef struct pmb887x_flash_erase_region_t pmb887x_flash_erase_region_t;
typedef struct pmb887x_flash_cfg_part_t pmb887x_flash_cfg_part_t;
typedef struct pmb887x_flash_cfg_t pmb887x_flash_cfg_t;

struct pmb887x_flash_erase_region_t {
	uint32_t offset;
	uint32_t size;
	uint32_t sector;
};

struct pmb887x_flash_cfg_part_t {
	uint32_t offset;
	uint32_t size;
	const pmb887x_flash_erase_region_t *erase_regions;
	uint32_t erase_regions_cnt;
};

struct pmb887x_flash_cfg_t {
	uint16_t vid;
	uint16_t pid;
	uint16_t lock;
	uint16_t cr;
	uint16_t ehcr;
	uint32_t size;
	const uint8_t *cfi;
	uint32_t cfi_size;
	const uint8_t *pri;
	uint32_t pri_size;
	const pmb887x_flash_cfg_part_t *parts;
	uint32_t parts_count;
	uint16_t pri_addr;
	
	uint16_t otp0_addr;
	uint16_t otp0_size;
	uint16_t otp1_addr;
	uint16_t otp1_size;
};

const pmb887x_flash_cfg_t *pmb887x_flash_find(uint16_t vid, uint16_t pid);
