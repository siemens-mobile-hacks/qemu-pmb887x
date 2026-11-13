/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "cfi-flash.h"

#define CFI_INTEL_STATUS_BLOCK_LOCKED           BIT(1)
#define CFI_INTEL_STATUS_PROGRAM_ERROR          BIT(4)
#define CFI_INTEL_STATUS_BLOCK_ERASE_ERROR      BIT(5)
#define CFI_INTEL_STATUS_READY                  BIT(7)
#define CFI_INTEL_STATUS_ERRORS                 (BIT(1) | BIT(3) | BIT(4) | BIT(5) | BIT(8) | BIT(9))
#define CFI_INTEL_STATUS_SEQUENCE_ERROR         (CFI_INTEL_STATUS_PROGRAM_ERROR | CFI_INTEL_STATUS_BLOCK_ERASE_ERROR)

#define CFI_INTEL_LOCK_STATUS_LOCKED            BIT(0)
#define CFI_INTEL_LOCK_STATUS_LOCKED_DOWN       BIT(1)
#define CFI_INTEL_EFA_LOCK_STATUS_LOCKED        BIT(4)
#define CFI_INTEL_EFA_LOCK_STATUS_LOCKED_DOWN   BIT(5)

#define CFI_INTEL_CONFIGURATION_MASK            0xFFFF

#define CFI_INTEL_PRI_MAGIC                     0x495250
#define CFI_INTEL_PRI_FEATURES                  0x05
#define CFI_INTEL_PRI_OTP_FIELDS                0x0E
#define CFI_INTEL_PRI_ERASE_SUSPEND             BIT(1)
#define CFI_INTEL_PRI_PROGRAM_SUSPEND           BIT(2)
#define CFI_INTEL_PRI_PROTECTION_BITS           BIT(6)
#define CFI_INTEL_PRI_SIMULTANEOUS_OPERATIONS   BIT(9)
#define CFI_INTEL_PRI_PAGE_MODE_BYTES           1
#define CFI_INTEL_PRI_V14_RECORD_SIZE_BYTES     2
#define CFI_INTEL_PRI_SIMULTANEOUS_OPS_BYTES    3
#define CFI_INTEL_PRI_ERASE_REGION_INFO_BYTES   4
#define CFI_INTEL_PRI_V14_ERASE_REGION_BYTES    6
#define CFI_INTEL_OTP_LOCK_SIZE                 sizeof(uint16_t)

typedef enum CFIIntelCommand CFIIntelCommand;
typedef enum CFIIntelIdentifier CFIIntelIdentifier;
typedef enum CFIIntelPriVersion CFIIntelPriVersion;

enum CFIIntelPriVersion {
    CFI_INTEL_PRI_VERSION_1_3 = 0x3331,
    CFI_INTEL_PRI_VERSION_1_4 = 0x3431,
};

enum CFIIntelCommand {
    CFI_INTEL_CMD_BLOCK_LOCK_CONFIRM                            = 0x01,
    CFI_INTEL_CMD_SET_CONFIGURATION_REGISTER_CONFIRM            = 0x03,
    CFI_INTEL_CMD_SET_ENHANCED_CONFIGURATION_REGISTER_CONFIRM   = 0x04,
    CFI_INTEL_CMD_WORD_PROGRAM_SETUP_ALTERNATE                  = 0x10,
    CFI_INTEL_CMD_BLOCK_ERASE_SETUP                             = 0x20,
    CFI_INTEL_CMD_EFA_BLOCK_ERASE_SETUP                         = 0x24,
    CFI_INTEL_CMD_BLOCK_LOCK_DOWN_CONFIRM                       = 0x2F,
    CFI_INTEL_CMD_WORD_PROGRAM_SETUP                            = 0x40,
    CFI_INTEL_CMD_PROGRAM_SETUP                                 = 0x41,
    CFI_INTEL_CMD_EFA_PROGRAM_SETUP                             = 0x44,
    CFI_INTEL_CMD_CLEAR_STATUS_REGISTER                         = 0x50,
    CFI_INTEL_CMD_BLOCK_LOCK_SETUP                              = 0x60,
    CFI_INTEL_CMD_EFA_BLOCK_LOCK_SETUP                          = 0x64,
    CFI_INTEL_CMD_READ_STATUS_REGISTER                          = 0x70,
    CFI_INTEL_CMD_BUFFERED_ENHANCED_FACTORY_PROGRAM             = 0x80,
    CFI_INTEL_CMD_READ_DEVICE_IDENTIFIER                        = 0x90,
    CFI_INTEL_CMD_READ_EFA                                      = 0x94,
    CFI_INTEL_CMD_CFI_QUERY                                     = 0x98,
    CFI_INTEL_CMD_PROGRAM_ERASE_SUSPEND                         = 0xB0,
    CFI_INTEL_CMD_BLANK_CHECK_SETUP                             = 0xBC,
    CFI_INTEL_CMD_PROTECTION_REGISTER_PROGRAM                   = 0xC0,
    CFI_INTEL_CMD_CONFIRM                                       = 0xD0,
    CFI_INTEL_CMD_BUFFERED_PROGRAM                              = 0xE8,
    CFI_INTEL_CMD_BUFFER_PROGRAM                                = 0xE9,
    CFI_INTEL_CMD_READ_ARRAY                                    = 0xFF,
};

enum CFIIntelIdentifier {
    CFI_INTEL_ID_MANUFACTURER_CODE                  = 0x00,
    CFI_INTEL_ID_DEVICE_CODE                        = 0x01,
    CFI_INTEL_ID_BLOCK_LOCK_STATUS                  = 0x02,
    CFI_INTEL_ID_CONFIGURATION_REGISTER             = 0x05,
    CFI_INTEL_ID_ENHANCED_CONFIGURATION_REGISTER    = 0x06,
};

typedef struct CFIIntelBankRegion CFIIntelBankRegion;
typedef struct CFIIntelBuffer CFIIntelBuffer;
typedef struct CFIIntelPart CFIIntelPart;
typedef struct CFIIntelState CFIIntelState;

struct CFIIntelBankRegion {
    uint32_t banks;
    uint64_t bank_size;
    uint32_t erase_regions_n;
    CFIFlashEraseRegion *erase_regions;
};

struct CFIIntelBuffer {
    uint32_t offset;
    uint32_t value;
    uint8_t size;
};

struct CFIIntelPart {
    CFIFlashPart parent;

    uint8_t wcycle;
    uint8_t cmd;
    uint32_t cmd_addr;
    uint16_t status;

    uint32_t buffer_size;
    uint32_t buffer_index;
    uint32_t buffer_end;
    CFIIntelBuffer *buffer;

    bool *locked_down;
};

struct CFIIntelState {
    uint16_t rcr;
    uint16_t ecr;
    bool *efa_locked_down;
};

static void cfi_intel_buffer_clear(CFIIntelPart *ip) {
    g_clear_pointer(&ip->buffer, g_free);
    ip->buffer_size = 0;
    ip->buffer_index = 0;
    ip->buffer_end = 0;
}

static void cfi_intel_sequence_error(CFIIntelPart *ip) {
    cfi_intel_buffer_clear(ip);
    ip->cmd = CFI_INTEL_CMD_READ_STATUS_REGISTER;
    ip->wcycle = 0;
    ip->status |= CFI_INTEL_STATUS_READY | CFI_INTEL_STATUS_SEQUENCE_ERROR;
}

static void cfi_intel_reset_part(CFIIntelPart *ip) {
    CFIFlashPart *p = &ip->parent;

    DEBUG_LOG(&p->flash->dbg, "<%d> back to read array mode\n", p->n);
    cfi_intel_buffer_clear(ip);
    ip->cmd = 0;
    ip->wcycle = 0;
    p->read_array = true;
}

static uint32_t cfi_intel_query_read(CFIIntelPart *ip, uint32_t offset) {
    CFIFlashPart *p = &ip->parent;
    CFIFlashState *flash = p->flash;
    CFIIntelState *chip_state = flash->cmdset_state;
    uint16_t index = (offset >> 1) & CFI_INDEX_MASK;
    uint32_t value;

    if (cfi_flash_query_read(p, index, &value))
        return value;

    if (index >= flash->otp0.addr && index < flash->otp0.addr + flash->otp0.size / 2) {
        value = flash->otp0_data[index - flash->otp0.addr];
        DEBUG_LOG(&flash->dbg, "<%d> OTP0 %02X: %04X\n", p->n, index - flash->otp0.addr, value);
    } else if (index >= flash->otp1.addr && index < flash->otp1.addr + flash->otp1.size / 2) {
        value = flash->otp1_data[index - flash->otp1.addr];
        DEBUG_LOG(&flash->dbg, "<%d> OTP1 %02X: %04X\n", p->n, index - flash->otp1.addr, value);
    } else {
        switch (index) {
            case CFI_INTEL_ID_MANUFACTURER_CODE:
                value = flash->vid;
                DEBUG_LOG(&flash->dbg, "<%d> vendor id: %04X\n", p->n, value);
                break;

            case CFI_INTEL_ID_DEVICE_CODE:
                value = flash->pid;
                DEBUG_LOG(&flash->dbg, "<%d> device id: %04X\n", p->n, value);
                break;

            case CFI_INTEL_ID_BLOCK_LOCK_STATUS: {
                CFIFlashBlock *blk = cfi_flash_part_find_block(p, offset);
                uint32_t efa_offset = offset - p->offset;
                value = (blk->protected ? CFI_INTEL_LOCK_STATUS_LOCKED : 0) |
                    (ip->locked_down[blk - p->blocks] ? CFI_INTEL_LOCK_STATUS_LOCKED_DOWN : 0);
                if (efa_offset < flash->efa_size) {
                    CFIFlashBlock *efa_blk = cfi_flash_efa_find_block(flash, efa_offset);
                    value |= (efa_blk->protected ? CFI_INTEL_EFA_LOCK_STATUS_LOCKED : 0) |
                        (chip_state->efa_locked_down[efa_blk - flash->efa_blocks] ?
                            CFI_INTEL_EFA_LOCK_STATUS_LOCKED_DOWN : 0);
                }
                DEBUG_LOG(&flash->dbg, "<%d> lock status: %02X\n", p->n, value);
                break;
            }

            case CFI_INTEL_ID_CONFIGURATION_REGISTER:
                value = chip_state->rcr;
                DEBUG_LOG(&flash->dbg, "<%d> configuration register: %04X\n", p->n, value);
                break;

            case CFI_INTEL_ID_ENHANCED_CONFIGURATION_REGISTER:
                value = chip_state->ecr;
                DEBUG_LOG(&flash->dbg, "<%d> enhanced configuration register: %04X\n", p->n, value);
                break;

            default:
                value = 0xFFFF;
                DEBUG_LOG(&flash->dbg, "<%d> %08X: read unknown cfi index 0x%02X\n", p->n, offset, index);
                break;
        }
    }

    return value;
}

static bool cfi_intel_efa_lock(CFIIntelPart *ip, uint32_t offset, uint64_t value) {
    CFIFlashPart *p = &ip->parent;
    CFIFlashState *flash = p->flash;
    CFIIntelState *chip_state = flash->cmdset_state;
    CFIFlashBlock *blk = cfi_flash_efa_find_block(flash, offset - p->offset);

    if (!blk) {
        DEBUG_GUEST_ERROR(&flash->dbg, "<%d> EFA lock outside EFA: %08X\n", p->n, flash->offset + offset);
        cfi_intel_sequence_error(ip);
        return true;
    }
    if (value == CFI_INTEL_CMD_BLOCK_LOCK_CONFIRM) {
        DEBUG_LOG(&flash->dbg, "<%d> lock EFA block %08X\n", p->n, flash->offset + offset);
        blk->protected = true;
    } else if (value == CFI_INTEL_CMD_CONFIRM) {
        DEBUG_LOG(&flash->dbg, "<%d> unlock EFA block %08X\n", p->n, flash->offset + offset);
        blk->protected = false;
    } else if (value == CFI_INTEL_CMD_BLOCK_LOCK_DOWN_CONFIRM) {
        DEBUG_LOG(&flash->dbg, "<%d> lock-down EFA block %08X\n", p->n, flash->offset + offset);
        blk->protected = true;
        chip_state->efa_locked_down[blk - flash->efa_blocks] = true;
    } else {
        return false;
    }

    ip->wcycle = 0;
    ip->status |= CFI_INTEL_STATUS_READY;
    return true;
}

static void cfi_intel_efa_erase(CFIIntelPart *ip, uint32_t offset) {
    CFIFlashPart *p = &ip->parent;
    CFIFlashState *flash = p->flash;
    CFIFlashBlock *blk = cfi_flash_efa_find_block(flash, offset - p->offset);

    if (!blk) {
        DEBUG_GUEST_ERROR(&flash->dbg, "<%d> EFA erase outside EFA: %08X\n", p->n, flash->offset + offset);
        cfi_intel_sequence_error(ip);
        return;
    }
    DEBUG_LOG(&flash->dbg, "<%d> erase EFA block %08X...%08X\n", p->n, flash->offset + p->offset + blk->offset,
        flash->offset + p->offset + blk->offset + blk->size - 1);
    if (cfi_flash_efa_erase_block(flash, blk) == CFI_FLASH_PROTECTED)
        ip->status |= CFI_INTEL_STATUS_BLOCK_LOCKED;
}

static void cfi_intel_efa_program(CFIIntelPart *ip, uint32_t offset, uint64_t value, uint32_t size) {
    CFIFlashPart *p = &ip->parent;
    CFIFlashState *flash = p->flash;

    DEBUG_LOG(&flash->dbg, "<%d> program EFA word [%d]: %08"PRIX64" to %08X\n",
        p->n, size, value, flash->offset + offset);
    switch (cfi_flash_efa_program(flash, offset - p->offset, value, size)) {
        case CFI_FLASH_OUT_OF_RANGE:
            DEBUG_GUEST_ERROR(&flash->dbg, "<%d> EFA program outside EFA: %08X\n", p->n, flash->offset + offset);
            cfi_intel_sequence_error(ip);
            break;
        case CFI_FLASH_PROTECTED:
            ip->status |= CFI_INTEL_STATUS_BLOCK_LOCKED;
            break;
        case CFI_FLASH_OK:
            break;
    }
}

static void cfi_intel_otp_program(CFIIntelPart *ip, uint32_t offset, uint64_t value, uint32_t size) {
    CFIFlashPart *p = &ip->parent;
    CFIFlashState *flash = p->flash;
    uint16_t index = (offset >> 1) & CFI_INDEX_MASK;
    uint16_t *otp_data = NULL;
    uint32_t data_index = 0;
    uint16_t lock_mask = 0;

    if (index >= flash->otp0.addr && index < flash->otp0.addr + flash->otp0.size / 2) {
        data_index = index - flash->otp0.addr;
        otp_data = flash->otp0_data;
        if (data_index) {
            uint32_t data_offset = (data_index - 1) * sizeof(uint16_t);
            lock_mask = data_offset < flash->otp0.factory_size ? BIT(0) : BIT(1);
        }
    } else if (index >= flash->otp1.addr && index < flash->otp1.addr + flash->otp1.size / 2) {
        data_index = index - flash->otp1.addr;
        otp_data = flash->otp1_data;
        if (data_index) {
            uint32_t lock_bits = sizeof(otp_data[0]) * CHAR_BIT;
            uint32_t group_size = (flash->otp1.size - CFI_INTEL_OTP_LOCK_SIZE) / lock_bits;
            uint32_t group = (data_index - 1) * sizeof(uint16_t) / group_size;
            lock_mask = BIT(group);
        }
    }

    DEBUG_LOG(&flash->dbg, "<%d> program protection word [%d]: %08"PRIX64" to %08X\n",
        p->n, size, value, flash->offset + offset);
    if (!otp_data || size != sizeof(uint16_t) || (data_index && ((otp_data[0] & lock_mask) == 0))) {
        ip->status |= CFI_INTEL_STATUS_PROGRAM_ERROR;
        return;
    }

    cfi_flash_otp_program(flash, otp_data, data_index, value);
}

static bool cfi_intel_lock_command(CFIIntelPart *ip, uint32_t offset, uint64_t value) {
    CFIFlashPart *p = &ip->parent;
    CFIFlashState *flash = p->flash;
    CFIIntelState *chip_state = flash->cmdset_state;

    if (value == CFI_INTEL_CMD_READ_ARRAY) {
        cfi_intel_reset_part(ip);
        return true;
    }

    uint16_t configuration = (offset >> 1) & CFI_INTEL_CONFIGURATION_MASK;
    if (value == CFI_INTEL_CMD_SET_CONFIGURATION_REGISTER_CONFIRM) {
        chip_state->rcr = configuration;
        DEBUG_LOG(&flash->dbg, "<%d> program read configuration register: %04X\n", p->n, chip_state->rcr);
        cfi_intel_reset_part(ip);
        return true;
    }
    if (value == CFI_INTEL_CMD_SET_ENHANCED_CONFIGURATION_REGISTER_CONFIRM) {
        bool has_ecr = flash->algorithm == P_ID_INTEL_PERFORMANCE;
        if (has_ecr)
            chip_state->ecr = configuration;
        DEBUG_LOG(&flash->dbg, "<%d> program enhanced configuration register %04X: %04X\n",
            p->n, configuration, chip_state->ecr);
        cfi_intel_reset_part(ip);
        return true;
    }
    bool block_lock_confirm = (
        value == CFI_INTEL_CMD_BLOCK_LOCK_CONFIRM ||
        value == CFI_INTEL_CMD_CONFIRM ||
        value == CFI_INTEL_CMD_BLOCK_LOCK_DOWN_CONFIRM
    );
    if (!block_lock_confirm)
        return false;

    CFIFlashBlock *blk = cfi_flash_part_find_block(p, offset);
    if (value == CFI_INTEL_CMD_BLOCK_LOCK_CONFIRM) {
        DEBUG_LOG(&flash->dbg, "<%d> lock block %08X\n", p->n, flash->offset + offset);
        blk->protected = true;
    } else if (value == CFI_INTEL_CMD_CONFIRM) {
        DEBUG_LOG(&flash->dbg, "<%d> unlock block %08X\n", p->n, flash->offset + offset);
        blk->protected = false;
    } else if (value == CFI_INTEL_CMD_BLOCK_LOCK_DOWN_CONFIRM) {
        DEBUG_LOG(&flash->dbg, "<%d> lock-down block %08X\n", p->n, flash->offset + offset);
        blk->protected = true;
        ip->locked_down[blk - p->blocks] = true;
    }

    ip->wcycle = 0;
    ip->status |= CFI_INTEL_STATUS_READY;
    return true;
}

static void cfi_intel_blank_check(CFIIntelPart *ip, uint32_t offset) {
    CFIFlashPart *p = &ip->parent;
    CFIFlashBlock *blk = cfi_flash_part_find_block(p, offset);
    bool blank = cfi_flash_block_is_blank(p, blk);

    DEBUG_LOG(&p->flash->dbg, "<%d> blank check block %08X...%08X: %s\n", p->n,
        p->flash->offset + p->offset + blk->offset,
        p->flash->offset + p->offset + blk->offset + blk->size - 1,
        blank ? "blank" : "not blank");
    ip->status &= ~CFI_INTEL_STATUS_BLOCK_ERASE_ERROR;
    if (!blank)
        ip->status |= CFI_INTEL_STATUS_BLOCK_ERASE_ERROR;
    ip->status |= CFI_INTEL_STATUS_READY;
    ip->cmd = CFI_INTEL_CMD_READ_STATUS_REGISTER;
    ip->wcycle = 0;
}

static void cfi_intel_block_erase(CFIIntelPart *ip, uint32_t offset) {
    CFIFlashPart *p = &ip->parent;
    CFIFlashBlock *blk = cfi_flash_part_find_block(p, offset);
    uint32_t sector_size = cfi_flash_find_sector_size(p, offset);
    uint32_t mask = ~(sector_size - 1);
    uint32_t base = ip->cmd_addr & mask;

    DEBUG_LOG(&p->flash->dbg, "<%d> confirm erase block %08X...%08X (sector: %08X)\n", p->n, p->flash->offset + base,
        p->flash->offset + base + sector_size - 1, sector_size);
    if ((offset & mask) != (ip->cmd_addr & mask)) {
        DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> erase confirm at %08X, setup at %08X\n", p->n,
            p->flash->offset + offset, p->flash->offset + ip->cmd_addr);
        cfi_intel_sequence_error(ip);
        return;
    }
    if (cfi_flash_erase_block(p, blk) == CFI_FLASH_PROTECTED)
        ip->status |= CFI_INTEL_STATUS_BLOCK_LOCKED;
}

static void cfi_intel_word_program(CFIIntelPart *ip, uint32_t offset, uint64_t value, uint32_t size) {
    CFIFlashPart *p = &ip->parent;
    CFIFlashBlock *blk = cfi_flash_part_find_block(p, offset);

    DEBUG_LOG(&p->flash->dbg, "<%d> program single word [%d]: %08"PRIX64" to %08X\n",
        p->n, size, value, p->flash->offset + offset);
    if (cfi_flash_program(p, blk, offset, value, size) == CFI_FLASH_PROTECTED)
        ip->status |= CFI_INTEL_STATUS_BLOCK_LOCKED;
}

static bool cfi_intel_buffer_address_in_block(CFIIntelPart *ip, uint32_t offset, uint32_t size) {
    CFIFlashPart *p = &ip->parent;
    CFIFlashBlock *command_block = cfi_flash_part_find_block(p, ip->cmd_addr);
    uint32_t block_start = p->offset + command_block->offset;
    uint32_t block_end = block_start + command_block->size;

    return offset >= block_start && offset < block_end && size <= block_end - offset;
}

static void cfi_intel_buffer_abort(CFIIntelPart *ip, uint32_t offset) {
    CFIFlashPart *p = &ip->parent;

    DEBUG_LOG(&p->flash->dbg, "<%d> abort buffered program at %08X (setup at %08X)\n", p->n,
        p->flash->offset + offset, p->flash->offset + ip->cmd_addr);
    cfi_intel_sequence_error(ip);
}

static void cfi_intel_buffer_add(CFIIntelPart *ip, uint32_t offset, uint64_t value, uint32_t size) {
    CFIFlashPart *p = &ip->parent;

    DEBUG_LOG(&p->flash->dbg, "<%d> program word [%d]: %08"PRIX64" to %08X\n",
        p->n, size, value, p->flash->offset + offset);
    if (size != 2 && size != 4) {
        cfi_intel_buffer_abort(ip, offset);
        return;
    }
    if (!cfi_intel_buffer_address_in_block(ip, offset, size)) {
        cfi_intel_buffer_abort(ip, offset);
        return;
    }

    for (uint32_t i = 0; i < size; i += 2) {
        CFIIntelBuffer *buffer_entry = NULL;
        if (offset + i < ip->buffer_end) {
            for (uint32_t j = 0; j < ip->buffer_index; j++) {
                if (ip->buffer[j].offset == offset + i && ip->buffer[j].size == 2) {
                    buffer_entry = &ip->buffer[j];
                    break;
                }
            }
        }

        if (!buffer_entry) {
            buffer_entry = &ip->buffer[ip->buffer_index];
            buffer_entry->offset = offset + i;
            buffer_entry->size = 2;
            ip->buffer_index++;
            ip->buffer_end = MAX(ip->buffer_end, offset + i + 2);
        }
        buffer_entry->value = (value >> (i * 8)) & 0xFFFF;

        if (ip->buffer_index == ip->buffer_size)
            break;
    }

    if (ip->buffer_index == ip->buffer_size) {
        DEBUG_LOG(&p->flash->dbg, "<%d> buffered program finished\n", p->n);
        ip->wcycle++;
    }
}

static void cfi_intel_buffer_commit(CFIIntelPart *ip) {
    CFIFlashPart *p = &ip->parent;
    CFIFlashBlock *blk = cfi_flash_part_find_block(p, ip->cmd_addr);

    if (blk->protected) {
        ip->status |= CFI_INTEL_STATUS_BLOCK_LOCKED;
    } else {
        for (uint32_t i = 0; i < ip->buffer_size; i++)
            cfi_flash_program(p, blk, ip->buffer[i].offset, ip->buffer[i].value, ip->buffer[i].size);
    }

    cfi_intel_buffer_clear(ip);
    DEBUG_LOG(&p->flash->dbg, "<%d> confirm buffered program\n", p->n);
    ip->wcycle = 0;
    ip->status |= CFI_INTEL_STATUS_READY;
}

static uint64_t cfi_intel_read(CFIFlashPart *p, uint32_t offset, uint32_t size) {
    CFIIntelPart *ip = container_of(p, CFIIntelPart, parent);
    uint32_t value = 0;

    switch (ip->cmd) {
        case CFI_INTEL_CMD_READ_EFA:
            if (cfi_flash_efa_read(p->flash, offset - p->offset, size, &value) != CFI_FLASH_OK) {
                DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> EFA read outside EFA: %08X\n", p->n,
                    p->flash->offset + offset);
                value = 0;
            }
            break;

        case CFI_INTEL_CMD_READ_DEVICE_IDENTIFIER:
        case CFI_INTEL_CMD_CFI_QUERY:
            value = cfi_intel_query_read(ip, offset);
            break;

        case CFI_INTEL_CMD_BLOCK_ERASE_SETUP:
        case CFI_INTEL_CMD_EFA_BLOCK_ERASE_SETUP:
        case CFI_INTEL_CMD_EFA_PROGRAM_SETUP:
        case CFI_INTEL_CMD_BLOCK_LOCK_SETUP:
        case CFI_INTEL_CMD_EFA_BLOCK_LOCK_SETUP:
        case CFI_INTEL_CMD_READ_STATUS_REGISTER:
        case CFI_INTEL_CMD_PROTECTION_REGISTER_PROGRAM:
        case CFI_INTEL_CMD_BUFFERED_PROGRAM:
        case CFI_INTEL_CMD_BUFFER_PROGRAM:
        case CFI_INTEL_CMD_PROGRAM_SETUP:
        case CFI_INTEL_CMD_WORD_PROGRAM_SETUP:
        case CFI_INTEL_CMD_WORD_PROGRAM_SETUP_ALTERNATE:
            value = ip->status;
            break;

        default:
            DEBUG_UNIMP(&p->flash->dbg, "<%d> read in command %02X mode at %08X\n", p->n, ip->cmd,
                p->flash->offset + offset);
            break;
    }

    return value;
}

static bool cfi_intel_command_is_declared(const CFIFlashState *flash, uint64_t command) {
    bool performance = flash->algorithm == P_ID_INTEL_PERFORMANCE;

    switch (command) {
        case CFI_INTEL_CMD_READ_ARRAY:
        case CFI_INTEL_CMD_READ_STATUS_REGISTER:
        case CFI_INTEL_CMD_CLEAR_STATUS_REGISTER:
        case CFI_INTEL_CMD_READ_DEVICE_IDENTIFIER:
        case CFI_INTEL_CMD_CFI_QUERY:
        case CFI_INTEL_CMD_BLOCK_ERASE_SETUP:
        case CFI_INTEL_CMD_BLOCK_LOCK_SETUP:
        case CFI_INTEL_CMD_BUFFERED_ENHANCED_FACTORY_PROGRAM:
            return true;

        case CFI_INTEL_CMD_WORD_PROGRAM_SETUP:
        case CFI_INTEL_CMD_WORD_PROGRAM_SETUP_ALTERNATE:
        case CFI_INTEL_CMD_BUFFERED_PROGRAM:
            return !performance;

        case CFI_INTEL_CMD_PROGRAM_SETUP:
        case CFI_INTEL_CMD_BUFFER_PROGRAM:
        case CFI_INTEL_CMD_BLANK_CHECK_SETUP:
            return performance;

        case CFI_INTEL_CMD_PROGRAM_ERASE_SUSPEND:
        case CFI_INTEL_CMD_CONFIRM:
            return (flash->pri_features & (CFI_INTEL_PRI_ERASE_SUSPEND | CFI_INTEL_PRI_PROGRAM_SUSPEND)) != 0;

        case CFI_INTEL_CMD_PROTECTION_REGISTER_PROGRAM:
            return (flash->pri_features & CFI_INTEL_PRI_PROTECTION_BITS) != 0;

        case CFI_INTEL_CMD_READ_EFA:
        case CFI_INTEL_CMD_EFA_PROGRAM_SETUP:
        case CFI_INTEL_CMD_EFA_BLOCK_ERASE_SETUP:
        case CFI_INTEL_CMD_EFA_BLOCK_LOCK_SETUP:
            return flash->efa_erase_regions_n != 0;

        default:
            return false;
    }
}

static void cfi_intel_write(CFIFlashPart *p, uint32_t offset, uint64_t value, uint32_t size) {
    CFIIntelPart *ip = container_of(p, CFIIntelPart, parent);
    bool valid_command = false;

    if (ip->wcycle == 0) {
        if (!cfi_intel_command_is_declared(p->flash, value)) {
            DEBUG_LOG(&p->flash->dbg, "<%d> command %02"PRIX64" is not declared by CFI, ignored\n", p->n, value);
            return;
        }
        valid_command = true;
        ip->cmd_addr = offset;

        switch (value) {
            case CFI_INTEL_CMD_READ_ARRAY:
                cfi_intel_reset_part(ip);
                break;

            case CFI_INTEL_CMD_READ_STATUS_REGISTER:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd read status (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                break;

            case CFI_INTEL_CMD_READ_DEVICE_IDENTIFIER:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd read devid (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                break;

            case CFI_INTEL_CMD_READ_EFA:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd read EFA (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                break;

            case CFI_INTEL_CMD_CFI_QUERY:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd read cfi (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                break;

            case CFI_INTEL_CMD_CLEAR_STATUS_REGISTER:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd clear status (%02"PRIX64")\n", p->n, value);
                ip->status &= ~CFI_INTEL_STATUS_ERRORS;
                break;

            case CFI_INTEL_CMD_WORD_PROGRAM_SETUP_ALTERNATE:
            case CFI_INTEL_CMD_PROGRAM_SETUP:
            case CFI_INTEL_CMD_WORD_PROGRAM_SETUP:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd program word (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                break;

            case CFI_INTEL_CMD_EFA_PROGRAM_SETUP:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd program EFA word (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                break;

            case CFI_INTEL_CMD_BUFFERED_PROGRAM:
            case CFI_INTEL_CMD_BUFFER_PROGRAM:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd buffered program (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                ip->status |= CFI_INTEL_STATUS_READY;
                break;

            case CFI_INTEL_CMD_BUFFERED_ENHANCED_FACTORY_PROGRAM:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd buffered EFP (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                break;

            case CFI_INTEL_CMD_BLOCK_ERASE_SETUP:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd block erase (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                ip->status |= CFI_INTEL_STATUS_READY;
                break;

            case CFI_INTEL_CMD_EFA_BLOCK_ERASE_SETUP:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd EFA block erase (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                ip->status |= CFI_INTEL_STATUS_READY;
                break;

            case CFI_INTEL_CMD_BLANK_CHECK_SETUP:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd blank check (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                break;

            case CFI_INTEL_CMD_PROGRAM_ERASE_SUSPEND:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd suspend (%02"PRIX64")\n", p->n, value);
                break;

            case CFI_INTEL_CMD_BLOCK_LOCK_SETUP:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd block lock or read configuration (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                break;

            case CFI_INTEL_CMD_EFA_BLOCK_LOCK_SETUP:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd EFA block lock (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                break;

            case CFI_INTEL_CMD_PROTECTION_REGISTER_PROGRAM:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd protection program (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                break;

            default:
                DEBUG_UNIMP(&p->flash->dbg, "<%d> command %02"PRIX64" at %08X\n",
                    p->n, value, p->flash->offset + offset);
                cfi_intel_reset_part(ip);
                break;
        }
    } else if (ip->wcycle == 1) {
        switch (ip->cmd) {
            case CFI_INTEL_CMD_READ_STATUS_REGISTER:
            case CFI_INTEL_CMD_READ_DEVICE_IDENTIFIER:
            case CFI_INTEL_CMD_CFI_QUERY:
                if (value == CFI_INTEL_CMD_READ_ARRAY) {
                    valid_command = true;
                    cfi_intel_reset_part(ip);
                }
                break;

            case CFI_INTEL_CMD_BLOCK_LOCK_SETUP:
                valid_command = cfi_intel_lock_command(ip, offset, value);
                break;

            case CFI_INTEL_CMD_EFA_BLOCK_LOCK_SETUP:
                valid_command = cfi_intel_efa_lock(ip, offset, value);
                break;

            case CFI_INTEL_CMD_BLANK_CHECK_SETUP:
                if (value == CFI_INTEL_CMD_CONFIRM) {
                    cfi_intel_blank_check(ip, offset);
                    valid_command = true;
                }
                break;

            case CFI_INTEL_CMD_BLOCK_ERASE_SETUP:
                if (value == CFI_INTEL_CMD_CONFIRM) {
                    cfi_intel_block_erase(ip, offset);
                    valid_command = true;
                    ip->wcycle = 0;
                    ip->status |= CFI_INTEL_STATUS_READY;
                }
                break;

            case CFI_INTEL_CMD_EFA_BLOCK_ERASE_SETUP:
                if (value == CFI_INTEL_CMD_CONFIRM) {
                    cfi_intel_efa_erase(ip, offset);
                    valid_command = true;
                    ip->wcycle = 0;
                    ip->status |= CFI_INTEL_STATUS_READY;
                }
                break;

            case CFI_INTEL_CMD_BUFFER_PROGRAM:
            case CFI_INTEL_CMD_BUFFERED_PROGRAM:
                valid_command = true;
                if (!cfi_intel_buffer_address_in_block(ip, offset, size)) {
                    cfi_intel_buffer_abort(ip, offset);
                } else {
                    ip->buffer_size = (value & 0xFFFF) + 1;
                    ip->buffer_index = 0;
                    ip->buffer_end = 0;
                    ip->buffer = g_new0(CFIIntelBuffer, ip->buffer_size);
                    DEBUG_LOG(&p->flash->dbg, "<%d> buffered program %d words\n", p->n, ip->buffer_size);
                    ip->wcycle++;
                }
                break;

            case CFI_INTEL_CMD_WORD_PROGRAM_SETUP_ALTERNATE:
            case CFI_INTEL_CMD_WORD_PROGRAM_SETUP:
            case CFI_INTEL_CMD_PROGRAM_SETUP:
                valid_command = true;
                cfi_intel_word_program(ip, offset, value, size);
                ip->wcycle = 0;
                ip->status |= CFI_INTEL_STATUS_READY;
                break;

            case CFI_INTEL_CMD_EFA_PROGRAM_SETUP:
                valid_command = true;
                cfi_intel_efa_program(ip, offset, value, size);
                ip->wcycle = 0;
                ip->status |= CFI_INTEL_STATUS_READY;
                break;

            case CFI_INTEL_CMD_PROTECTION_REGISTER_PROGRAM:
                cfi_intel_otp_program(ip, offset, value, size);
                valid_command = true;
                ip->wcycle = 0;
                ip->status |= CFI_INTEL_STATUS_READY;
                break;
        }
    } else if (ip->wcycle == 2) {
        switch (ip->cmd) {
            case CFI_INTEL_CMD_BUFFER_PROGRAM:
            case CFI_INTEL_CMD_BUFFERED_PROGRAM:
                valid_command = true;
                cfi_intel_buffer_add(ip, offset, value, size);
                break;
        }
    } else if (ip->wcycle == 3) {
        switch (ip->cmd) {
            case CFI_INTEL_CMD_BUFFER_PROGRAM:
            case CFI_INTEL_CMD_BUFFERED_PROGRAM:
                valid_command = true;
                if (value != CFI_INTEL_CMD_CONFIRM || !cfi_intel_buffer_address_in_block(ip, offset, size)) {
                    cfi_intel_buffer_abort(ip, offset);
                } else {
                    cfi_intel_buffer_commit(ip);
                }
                break;
        }
    }

    if (!valid_command) {
        DEBUG_UNIMP(&p->flash->dbg, "<%d> command %02X cycle %d: %08"PRIX64" at %08X\n", p->n, ip->cmd,
            ip->wcycle, value, p->flash->offset + offset);
        cfi_intel_sequence_error(ip);
    }

    p->read_array = ip->cmd == 0;
}

static bool cfi_intel_decode_otp(CFIFlashState *flash, CFIQueryReader *r) {
    uint32_t fields_n = cfi_query_read_le(r, 1);

    for (uint32_t i = 0; i < fields_n; i++) {
        uint32_t addr, factory_groups, factory_shift, user_groups, user_shift;
        if (i == 0) {
            addr = cfi_query_read_le(r, 2);
            factory_groups = 1;
            factory_shift = cfi_query_read_le(r, 1);
            user_groups = 1;
            user_shift = cfi_query_read_le(r, 1);
        } else {
            addr = cfi_query_read_le(r, 4);
            factory_groups = cfi_query_read_le(r, 2);
            factory_shift = cfi_query_read_le(r, 1);
            user_groups = cfi_query_read_le(r, 2);
            user_shift = cfi_query_read_le(r, 1);
        }

        if (factory_shift >= 16 || user_shift >= 16) {
            cfi_flash_error(flash, "invalid PRI protection field %u size", i);
            return false;
        }
        uint32_t factory_size = factory_groups << factory_shift;
        CFIFlashOtp otp = {
            .addr = addr,
            .size = factory_size + (user_groups << user_shift) + CFI_INTEL_OTP_LOCK_SIZE,
            .factory_size = factory_size,
        };
        if (i == 0) {
            flash->otp0 = otp;
        } else if (i == 1) {
            flash->otp1 = otp;
        }
    }
    return true;
}

static CFIIntelBankRegion *cfi_intel_decode_bank_regions(CFIQueryReader *r, bool pri_v14, uint32_t *regions_n) {
    *regions_n = cfi_query_read_le(r, 1);
    CFIIntelBankRegion *regions = g_new0(CFIIntelBankRegion, *regions_n);

    for (uint32_t i = 0; i < *regions_n; i++) {
        CFIIntelBankRegion *region = &regions[i];
        if (pri_v14)
            cfi_query_skip(r, CFI_INTEL_PRI_V14_RECORD_SIZE_BYTES);
        region->banks = cfi_query_read_le(r, 2);
        cfi_query_skip(r, CFI_INTEL_PRI_SIMULTANEOUS_OPS_BYTES);
        region->erase_regions_n = cfi_query_read_le(r, 1);
        region->erase_regions = g_new(CFIFlashEraseRegion, region->erase_regions_n);

        for (uint32_t j = 0; j < region->erase_regions_n; j++) {
            uint32_t blocks = cfi_query_read_le(r, 2) + 1;
            uint32_t sector = cfi_query_read_le(r, 2) * CFI_ERASE_SECTOR_UNIT;
            cfi_query_skip(r, CFI_INTEL_PRI_ERASE_REGION_INFO_BYTES);
            if (pri_v14)
                cfi_query_skip(r, CFI_INTEL_PRI_V14_ERASE_REGION_BYTES);

            uint64_t size = (uint64_t) blocks * sector;
            region->erase_regions[j] = (CFIFlashEraseRegion) { region->bank_size, size, sector };
            region->bank_size += size;
        }
    }
    return regions;
}

static bool cfi_intel_add_chip_parts(CFIFlashState *flash, const CFIIntelBankRegion *regions, uint32_t regions_n) {
    uint64_t total_size = 0;
    uint32_t banks_n = 0;
    for (uint32_t i = 0; i < regions_n; i++) {
        total_size += regions[i].banks * regions[i].bank_size;
        banks_n += regions[i].banks;
    }
    if (total_size != flash->size) {
        cfi_flash_error(flash, "PRI bank regions cover 0x%"PRIX64" bytes, device size is 0x%08X",
            total_size, flash->size);
        return false;
    }

    flash->chip_parts = g_new(CFIFlashChipPart, banks_n);
    uint32_t offset = 0;
    for (uint32_t i = 0; i < regions_n; i++) {
        for (uint32_t j = 0; j < regions[i].banks; j++) {
            flash->chip_parts[flash->chip_parts_n++] = (CFIFlashChipPart) {
                offset, regions[i].bank_size, regions[i].erase_regions, regions[i].erase_regions_n
            };
            offset += regions[i].bank_size;
        }
    }
    return true;
}

static void cfi_intel_add_efa_regions(CFIFlashState *flash, const CFIIntelBankRegion *regions, uint32_t regions_n) {
    uint32_t erase_regions_n = 0;
    for (uint32_t i = 0; i < regions_n; i++)
        erase_regions_n += regions[i].banks * regions[i].erase_regions_n;

    flash->efa_erase_regions = g_new(CFIFlashEraseRegion, erase_regions_n);
    uint32_t offset = 0;
    for (uint32_t i = 0; i < regions_n; i++) {
        for (uint32_t j = 0; j < regions[i].banks; j++) {
            for (uint32_t k = 0; k < regions[i].erase_regions_n; k++) {
                const CFIFlashEraseRegion *region = &regions[i].erase_regions[k];
                flash->efa_erase_regions[flash->efa_erase_regions_n++] = (CFIFlashEraseRegion) {
                    offset, region->size, region->sector
                };
                offset += region->size;
            }
        }
    }
}

static bool cfi_intel_decode_pri(CFIFlashState *flash) {
    const CFIFlashChip *chip = flash->chip;
    bool pri_v14 = flash->algorithm == P_ID_INTEL_PERFORMANCE;
    uint32_t supported_version = pri_v14 ? CFI_INTEL_PRI_VERSION_1_4 : CFI_INTEL_PRI_VERSION_1_3;
    CFIQueryReader r = { .data = chip->pri, .size = chip->pri_size };

    uint32_t magic = cfi_query_read_le(&r, 3);
    uint32_t version = cfi_query_read_le(&r, 2);
    if (r.overrun || magic != CFI_INTEL_PRI_MAGIC || version != supported_version) {
        cfi_flash_error(flash, "unsupported PRI");
        return false;
    }

    r.pos = CFI_INTEL_PRI_FEATURES;
    uint32_t features = cfi_query_read_le(&r, 4);
    flash->pri_features = features;
    r.pos = CFI_INTEL_PRI_OTP_FIELDS;
    if ((features & CFI_INTEL_PRI_PROTECTION_BITS) && !cfi_intel_decode_otp(flash, &r))
        return false;
    cfi_query_skip(&r, CFI_INTEL_PRI_PAGE_MODE_BYTES);
    uint32_t sync_modes_n = cfi_query_read_le(&r, 1);
    cfi_query_skip(&r, sync_modes_n);

    g_autofree CFIIntelBankRegion *banks = NULL;
    g_autofree CFIIntelBankRegion *efa_banks = NULL;
    uint32_t banks_n = 0;
    uint32_t efa_banks_n = 0;
    if (features & CFI_INTEL_PRI_SIMULTANEOUS_OPERATIONS) {
        banks = cfi_intel_decode_bank_regions(&r, pri_v14, &banks_n);
        if (pri_v14)
            efa_banks = cfi_intel_decode_bank_regions(&r, pri_v14, &efa_banks_n);
    }
    if (r.overrun) {
        cfi_flash_error(flash, "truncated PRI");
        return false;
    }

    if (banks_n) {
        if (!cfi_intel_add_chip_parts(flash, banks, banks_n))
            return false;
    } else {
        flash->chip_parts = g_new(CFIFlashChipPart, 1);
        flash->chip_parts[0] = (CFIFlashChipPart) {
            0, flash->size, flash->cfi_erase_regions, flash->cfi_erase_regions_n
        };
        flash->chip_parts_n = 1;
    }
    cfi_intel_add_efa_regions(flash, efa_banks, efa_banks_n);
    for (uint32_t i = 0; i < efa_banks_n; i++)
        g_free(efa_banks[i].erase_regions);
    return true;
}

static void cfi_intel_init(CFIFlashState *flash) {
    CFIIntelState *chip_state = g_new0(CFIIntelState, 1);

    chip_state->rcr = flash->chip->rcr;
    chip_state->ecr = flash->chip->ecr;

    chip_state->efa_locked_down = g_new0(bool, flash->efa_blocks_n);
    for (uint32_t i = 0; i < flash->efa_blocks_n; i++)
        flash->efa_blocks[i].protected = true;
    flash->cmdset_state = chip_state;

    for (uint32_t i = 0; i < flash->parts_n; i++) {
        CFIFlashPart *p = flash->parts[i];
        CFIIntelPart *ip = container_of(p, CFIIntelPart, parent);

        ip->status = CFI_INTEL_STATUS_READY;
        ip->locked_down = g_new0(bool, p->blocks_n);
        for (uint32_t j = 0; j < p->blocks_n; j++)
            p->blocks[j].protected = true;
    }
}

static void cfi_intel_reset(CFIFlashState *flash) {
    CFIIntelState *chip_state = flash->cmdset_state;

    chip_state->rcr = flash->chip->rcr;
    chip_state->ecr = flash->chip->ecr;

    for (uint32_t i = 0; i < flash->parts_n; i++) {
        CFIFlashPart *p = flash->parts[i];
        CFIIntelPart *ip = container_of(p, CFIIntelPart, parent);

        ip->status = CFI_INTEL_STATUS_READY;
        for (uint32_t j = 0; j < p->blocks_n; j++) {
            if (ip->locked_down[j]) {
                p->blocks[j].protected = true;
                ip->locked_down[j] = false;
            }
        }
        cfi_intel_reset_part(ip);
    }

    for (uint32_t i = 0; i < flash->efa_blocks_n; i++) {
        if (chip_state->efa_locked_down[i]) {
            flash->efa_blocks[i].protected = true;
            chip_state->efa_locked_down[i] = false;
        }
    }
}

const CFIFlashCommandSet cfi_flash_intel_cmdset = {
    .part_size = sizeof(CFIIntelPart),
    .decode_pri = cfi_intel_decode_pri,
    .init = cfi_intel_init,
    .read = cfi_intel_read,
    .write = cfi_intel_write,
    .reset = cfi_intel_reset,
};
