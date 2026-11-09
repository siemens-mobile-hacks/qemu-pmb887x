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

typedef struct CFIIntelBuffer CFIIntelBuffer;
typedef struct CFIIntelPart CFIIntelPart;
typedef struct CFIIntelState CFIIntelState;

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
    ip->cmd = 0x70;
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
    const CFIFlashChip *chip = flash->chip;
    uint16_t index = (offset >> 1) & CFI_INDEX_MASK;
    uint32_t value;

    if (cfi_flash_query_read(p, index, &value))
        return value;

    if (index >= chip->otp0_addr && index < chip->otp0_addr + chip->otp0_size / 2) {
        value = flash->otp0_data[index - chip->otp0_addr];
        DEBUG_LOG(&flash->dbg, "<%d> OTP0 %02X: %04X\n", p->n, index - chip->otp0_addr, value);
    } else if (index >= chip->otp1_addr && index < chip->otp1_addr + chip->otp1_size / 2) {
        value = flash->otp1_data[index - chip->otp1_addr];
        DEBUG_LOG(&flash->dbg, "<%d> OTP1 %02X: %04X\n", p->n, index - chip->otp1_addr, value);
    } else {
        switch (index) {
            case 0x00:
                value = flash->vid;
                DEBUG_LOG(&flash->dbg, "<%d> vendor id: %04X\n", p->n, value);
                break;

            case 0x01:
                value = flash->pid;
                DEBUG_LOG(&flash->dbg, "<%d> device id: %04X\n", p->n, value);
                break;

            case 0x02: {
                CFIFlashBlock *blk = cfi_flash_part_find_block(p, offset);
                uint32_t efa_offset = offset - p->offset;
                value = (blk->protected ? CFI_INTEL_LOCK_STATUS_LOCKED : 0) |
                    (ip->locked_down[blk - p->blocks] ? CFI_INTEL_LOCK_STATUS_LOCKED_DOWN : 0);
                if (efa_offset < flash->efa_size) {
                    CFIIntelState *chip_state = flash->cmdset_state;
                    CFIFlashBlock *efa_blk = cfi_flash_efa_find_block(flash, efa_offset);
                    value |= (efa_blk->protected ? CFI_INTEL_EFA_LOCK_STATUS_LOCKED : 0) |
                        (chip_state->efa_locked_down[efa_blk - flash->efa_blocks] ?
                            CFI_INTEL_EFA_LOCK_STATUS_LOCKED_DOWN : 0);
                }
                DEBUG_LOG(&flash->dbg, "<%d> lock status: %02X\n", p->n, value);
                break;
            }

            case 0x05:
                value = chip->cr;
                DEBUG_LOG(&flash->dbg, "<%d> configuration register: %02X\n", p->n, value);
                break;

            case 0x06:
                value = chip->ehcr;
                DEBUG_LOG(&flash->dbg, "<%d> enhanced configuration register: %02X\n", p->n, value);
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
    if (value == 0x01) {
        DEBUG_LOG(&flash->dbg, "<%d> lock EFA block %08X\n", p->n, flash->offset + offset);
        blk->protected = true;
    } else if (value == 0xD0) {
        DEBUG_LOG(&flash->dbg, "<%d> unlock EFA block %08X\n", p->n, flash->offset + offset);
        blk->protected = false;
    } else if (value == 0x2F) {
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
    const CFIFlashChip *chip = flash->chip;
    uint16_t index = (offset >> 1) & CFI_INDEX_MASK;
    uint16_t *otp_data = NULL;
    uint32_t data_index = 0;
    uint16_t lock_mask = 0;

    if (index >= chip->otp0_addr && index < chip->otp0_addr + chip->otp0_size / 2) {
        data_index = index - chip->otp0_addr;
        otp_data = flash->otp0_data;
        if (data_index) {
            uint32_t data_offset = (data_index - 1) * sizeof(uint16_t);
            lock_mask = data_offset < chip->otp0_factory_size ? BIT(0) : BIT(1);
        }
    } else if (index >= chip->otp1_addr && index < chip->otp1_addr + chip->otp1_size / 2) {
        data_index = index - chip->otp1_addr;
        otp_data = flash->otp1_data;
        if (data_index) {
            uint32_t lock_bits = sizeof(otp_data[0]) * CHAR_BIT;
            uint32_t group_size = (chip->otp1_size - sizeof(uint16_t)) / lock_bits;
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

    if (value == 0xFF) {
        cfi_intel_reset_part(ip);
        return true;
    }
    if (value == 0x03) {
        DEBUG_LOG(&flash->dbg, "<%d> program read configuration register at %08X\n",
            p->n, flash->offset + offset);
        cfi_intel_reset_part(ip);
        return true;
    }
    if (value == 0x04) {
        DEBUG_LOG(&flash->dbg, "<%d> program enhanced configuration register at %08X\n",
            p->n, flash->offset + offset);
        cfi_intel_reset_part(ip);
        return true;
    }
    if (value != 0x01 && value != 0xD0 && value != 0x2F)
        return false;

    CFIFlashBlock *blk = cfi_flash_part_find_block(p, offset);
    if (value == 0x01) {
        DEBUG_LOG(&flash->dbg, "<%d> lock block %08X\n", p->n, flash->offset + offset);
        blk->protected = true;
    } else if (value == 0xD0) {
        DEBUG_LOG(&flash->dbg, "<%d> unlock block %08X\n", p->n, flash->offset + offset);
        blk->protected = false;
    } else if (value == 0x2F) {
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
    ip->cmd = 0x70;
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
        case 0x94:
            if (cfi_flash_efa_read(p->flash, offset - p->offset, size, &value) != CFI_FLASH_OK) {
                DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> EFA read outside EFA: %08X\n", p->n,
                    p->flash->offset + offset);
                value = 0;
            }
            break;

        case 0x90:
        case 0x98:
            value = cfi_intel_query_read(ip, offset);
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
            value = ip->status;
            // DEBUG_LOG(&p->flash->dbg, "<%d> %08"PRIX64": status 0x%02X\n", p->n, offset, value);
            break;

        default:
            DEBUG_UNIMP(&p->flash->dbg, "<%d> read in command %02X mode at %08X\n", p->n, ip->cmd,
                p->flash->offset + offset);
            break;
    }

    return value;
}

static void cfi_intel_write(CFIFlashPart *p, uint32_t offset, uint64_t value, uint32_t size) {
    CFIIntelPart *ip = container_of(p, CFIIntelPart, parent);
    bool valid_command = false;

    if (ip->wcycle == 0) {
        valid_command = true;
        ip->cmd_addr = offset;

        switch (value) {
            case 0xFF:
                cfi_intel_reset_part(ip);
                break;

            case 0x00:
            case 0xAA:
            case 0x55:
            case 0xF0:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd AMD probe (%02"PRIX64")\n", p->n, value);
                cfi_intel_reset_part(ip);
                break;

            case 0x70:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd read status (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                break;

            case 0x90:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd read devid (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                break;

            case 0x94:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd read EFA (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                break;

            case 0x98:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd read cfi (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                break;

            case 0x50:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd clear status (%02"PRIX64")\n", p->n, value);
                ip->status &= ~CFI_INTEL_STATUS_ERRORS;
                break;

            case 0x10:
                if (p->flash->strict_opcodes) {
                    DEBUG_LOG(&p->flash->dbg, "<%d> cmd program word (%02"PRIX64") is not documented for command set 0200 -> ignored\n",
                        p->n, value);
                    cfi_intel_reset_part(ip);
                    break;
                }
                QEMU_FALLTHROUGH;
            case 0x41:
            case 0x40:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd program word (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                break;

            case 0x44:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd program EFA word (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                break;

            case 0xE8:
                if (p->flash->strict_opcodes) {
                    DEBUG_LOG(&p->flash->dbg, "<%d> cmd buffered program (%02"PRIX64") is not documented for command set 0200 -> ignored\n",
                        p->n, value);
                    cfi_intel_reset_part(ip);
                    break;
                }
                QEMU_FALLTHROUGH;
            case 0xE9:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd buffered program (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                ip->status |= CFI_INTEL_STATUS_READY;
                break;

            case 0x80:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd buffered EFP (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                break;

            case 0x20:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd block erase (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                ip->status |= CFI_INTEL_STATUS_READY;
                break;

            case 0x24:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd EFA block erase (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                ip->status |= CFI_INTEL_STATUS_READY;
                break;

            case 0xBC:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd blank check (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                break;

            case 0xB0:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd suspend (%02"PRIX64")\n", p->n, value);
                break;

            case 0x60:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd block lock or read configuration (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                break;

            case 0x64:
                DEBUG_LOG(&p->flash->dbg, "<%d> cmd EFA block lock (%02"PRIX64")\n", p->n, value);
                ip->cmd = value;
                ip->wcycle++;
                break;

            case 0xC0:
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
            case 0x70:  // read status
            case 0x90:  // read devid
            case 0x98:  // read cfi
                if (value == 0xFF) {
                    valid_command = true;
                    cfi_intel_reset_part(ip);
                }
                break;

            case 0x60:  // lock or configuration
                valid_command = cfi_intel_lock_command(ip, offset, value);
                break;

            case 0x64:
                valid_command = cfi_intel_efa_lock(ip, offset, value);
                break;

            case 0xBC:
                if (value == 0xD0) {
                    cfi_intel_blank_check(ip, offset);
                    valid_command = true;
                }
                break;

            case 0x20:  // erase
                if (value == 0xD0) {
                    cfi_intel_block_erase(ip, offset);
                    valid_command = true;
                    ip->wcycle = 0;
                    ip->status |= CFI_INTEL_STATUS_READY;
                }
                break;

            case 0x24: // EFA erase
                if (value == 0xD0) {
                    cfi_intel_efa_erase(ip, offset);
                    valid_command = true;
                    ip->wcycle = 0;
                    ip->status |= CFI_INTEL_STATUS_READY;
                }
                break;

            case 0xE9:  // buffered program
            case 0xE8:  // buffered program
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

            case 0x10:  // program word
            case 0x40:  // program word
            case 0x41:  // program word
                valid_command = true;
                cfi_intel_word_program(ip, offset, value, size);
                ip->wcycle = 0;
                ip->status |= CFI_INTEL_STATUS_READY;
                break;

            case 0x44: // EFA program word
                valid_command = true;
                cfi_intel_efa_program(ip, offset, value, size);
                ip->wcycle = 0;
                ip->status |= CFI_INTEL_STATUS_READY;
                break;

            case 0xC0: // Protection register program
                cfi_intel_otp_program(ip, offset, value, size);
                valid_command = true;
                ip->wcycle = 0;
                ip->status |= CFI_INTEL_STATUS_READY;
                break;
        }
    } else if (ip->wcycle == 2) {
        switch (ip->cmd) {
            case 0xE9:  // buffered program
            case 0xE8:  // buffered program
                valid_command = true;
                cfi_intel_buffer_add(ip, offset, value, size);
                break;
        }
    } else if (ip->wcycle == 3) {
        switch (ip->cmd) {
            case 0xE9:  // buffered program
            case 0xE8:  // buffered program
                valid_command = true;
                if (value != 0xD0 || !cfi_intel_buffer_address_in_block(ip, offset, size)) {
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

static void cfi_intel_init(CFIFlashState *flash) {
    CFIIntelState *chip_state = g_new0(CFIIntelState, 1);

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
    .init = cfi_intel_init,
    .read = cfi_intel_read,
    .write = cfi_intel_write,
    .reset = cfi_intel_reset,
};
