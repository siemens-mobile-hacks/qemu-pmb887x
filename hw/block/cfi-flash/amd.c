/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "cfi-flash.h"

#define CFI_AMD_UNLOCK1_ADDR                    0x555
#define CFI_AMD_UNLOCK2_ADDR                    0x2AA
#define CFI_AMD_UNLOCK_ADDR_MASK                0x7FF

#define CFI_AMD_STATUS_WRITE_BUFFER_ABORT       BIT(1)
#define CFI_AMD_STATUS_ERASE_TOGGLE             BIT(2)
#define CFI_AMD_STATUS_ERASE_STARTED            BIT(3)
#define CFI_AMD_STATUS_TOGGLE                   BIT(6)
#define CFI_AMD_STATUS_DATA_POLLING             BIT(7)

#define CFI_AMD_INDICATOR_DYB_UNLOCKED          BIT(1)
#define CFI_AMD_INDICATOR_CUSTOMER_LOCKED       BIT(6)
#define CFI_AMD_INDICATOR_FACTORY_LOCKED        BIT(7)

#define CFI_AMD_PROTECTION_CLEAR                0x0001

#define CFI_AMD_LOCK_REGISTER_WORD              0x77
#define CFI_AMD_LOCK_CUSTOMER_SECSI             BIT(0)
#define CFI_AMD_LOCK_PERSISTENT_MODE            BIT(1)
#define CFI_AMD_LOCK_PASSWORD_MODE              BIT(2)
#define CFI_AMD_LOCK_MODE_BITS                  (CFI_AMD_LOCK_PERSISTENT_MODE | CFI_AMD_LOCK_PASSWORD_MODE)

#define CFI_AMD_CONFIGURATION_DEFAULT           0xEFC8
#define CFI_AMD_PASSWORD_WORDS                  4
#define CFI_AMD_PASSWORD_INDEX_MASK             (CFI_AMD_PASSWORD_WORDS - 1)

#define CFI_AMD_PRI_MAGIC                       0x495250
#define CFI_AMD_PRI_UNLOCK_AND_TECHNOLOGY       0x05
#define CFI_AMD_PRI_SECSI_SECTOR_SIZE           0x12
#define CFI_AMD_PRI_BANK_ORGANIZATION           0x17
#define CFI_AMD_PRI_ADDRESS_INSENSITIVE_UNLOCK  (BIT(1) | BIT(0))

typedef enum CFIAmdCommand CFIAmdCommand;
typedef enum CFIAmdAutoselectWord CFIAmdAutoselectWord;
typedef enum CFIAmdPriVersion CFIAmdPriVersion;
typedef enum CFIAmdMode CFIAmdMode;
typedef enum CFIAmdPending CFIAmdPending;
typedef enum CFIAmdOperation CFIAmdOperation;
typedef struct CFIAmdPart CFIAmdPart;
typedef struct CFIAmdState CFIAmdState;

enum CFIAmdPriVersion {
    CFI_AMD_PRI_VERSION_1_3 = 0x3331,
    CFI_AMD_PRI_VERSION_1_4 = 0x3431,
};

enum CFIAmdCommand {
    CFI_AMD_CMD_COMMAND_SET_EXIT_CONFIRM        = 0x00,
    CFI_AMD_CMD_DYB_SET                         = 0x00,
    CFI_AMD_CMD_PPB_PROGRAM                     = 0x00,
    CFI_AMD_CMD_PPB_LOCK_BIT_SET                = 0x00,
    CFI_AMD_CMD_UNLOCK_BYPASS_RESET_CONFIRM     = 0x00,
    CFI_AMD_CMD_DYB_CLEAR                       = 0x01,
    CFI_AMD_CMD_PASSWORD_UNLOCK_WORD_COUNT      = 0x03,
    CFI_AMD_CMD_CHIP_ERASE                      = 0x10,
    CFI_AMD_CMD_UNLOCK_BYPASS_ENTRY             = 0x20,
    CFI_AMD_CMD_WRITE_TO_BUFFER                 = 0x25,
    CFI_AMD_CMD_PASSWORD_UNLOCK                 = 0x25,
    CFI_AMD_CMD_PROGRAM_BUFFER_TO_FLASH         = 0x29,
    CFI_AMD_CMD_PASSWORD_UNLOCK_CONFIRM         = 0x29,
    CFI_AMD_CMD_SECTOR_ERASE                    = 0x30,
    CFI_AMD_CMD_ERASE_RESUME                    = 0x30,
    CFI_AMD_CMD_PROGRAM_RESUME                  = 0x30,
    CFI_AMD_CMD_ALL_PPB_ERASE                   = 0x30,
    CFI_AMD_CMD_LOCK_REGISTER_ENTRY             = 0x40,
    CFI_AMD_CMD_GLOBAL_PPB_LOCK_ENTRY           = 0x50,
    CFI_AMD_CMD_UNLOCK2                         = 0x55,
    CFI_AMD_CMD_PASSWORD_PROTECTION_ENTRY       = 0x60,
    CFI_AMD_CMD_ERASE_SETUP                     = 0x80,
    CFI_AMD_CMD_SECSI_SECTOR_ENTRY              = 0x88,
    CFI_AMD_CMD_AUTOSELECT                      = 0x90,
    CFI_AMD_CMD_COMMAND_SET_EXIT                = 0x90,
    CFI_AMD_CMD_UNLOCK_BYPASS_RESET             = 0x90,
    CFI_AMD_CMD_CFI_QUERY                       = 0x98,
    CFI_AMD_CMD_PROGRAM                         = 0xA0,
    CFI_AMD_CMD_UNLOCK1                         = 0xAA,
    CFI_AMD_CMD_ERASE_SUSPEND                   = 0xB0,
    CFI_AMD_CMD_PROGRAM_SUSPEND                 = 0xB0,
    CFI_AMD_CMD_PPB_ENTRY                       = 0xC0,
    CFI_AMD_CMD_READ_CONFIGURATION_REGISTER     = 0xC6,
    CFI_AMD_CMD_SET_CONFIGURATION_REGISTER      = 0xD0,
    CFI_AMD_CMD_DYB_ENTRY                       = 0xE0,
    CFI_AMD_CMD_RESET                           = 0xF0,
};

enum CFIAmdAutoselectWord {
    CFI_AMD_AUTOSELECT_MANUFACTURER         = 0x00,
    CFI_AMD_AUTOSELECT_DEVICE1              = 0x01,
    CFI_AMD_AUTOSELECT_SECTOR_PROTECTION    = 0x02,
    CFI_AMD_AUTOSELECT_INDICATOR_BITS       = 0x03,
    CFI_AMD_AUTOSELECT_DEVICE2              = 0x0E,
    CFI_AMD_AUTOSELECT_DEVICE3              = 0x0F,
};

enum CFIAmdMode {
    CFI_AMD_MODE_READ_ARRAY,
    CFI_AMD_MODE_AUTOSELECT,
    CFI_AMD_MODE_CFI_QUERY,
    CFI_AMD_MODE_SECSI_SECTOR,
    CFI_AMD_MODE_CONFIGURATION,
    CFI_AMD_MODE_DYB,
    CFI_AMD_MODE_PPB,
    CFI_AMD_MODE_PPB_LOCK,
    CFI_AMD_MODE_LOCK_REGISTER,
    CFI_AMD_MODE_PASSWORD,
};

enum CFIAmdPending {
    CFI_AMD_PENDING_NONE,
    CFI_AMD_PENDING_BUFFER_COUNT,
    CFI_AMD_PENDING_BUFFER_LOAD,
    CFI_AMD_PENDING_BUFFER_CONFIRM,
    CFI_AMD_PENDING_COMMAND_SET_PROGRAM,
    CFI_AMD_PENDING_COMMAND_SET_EXIT,
    CFI_AMD_PENDING_SET_CONFIGURATION,
    CFI_AMD_PENDING_PPB_ERASE,
    CFI_AMD_PENDING_PASSWORD_UNLOCK,
    CFI_AMD_PENDING_UNLOCK_BYPASS_RESET,
};

enum CFIAmdOperation {
    CFI_AMD_OPERATION_NONE,
    CFI_AMD_OPERATION_PROGRAM,
    CFI_AMD_OPERATION_ERASE,
    CFI_AMD_OPERATION_BUFFER_ABORT,
};

struct CFIAmdPart {
    CFIFlashPart parent;

    CFIAmdMode mode;
    CFIAmdPending pending;
    CFIAmdOperation operation;
    uint16_t polling_data;
    bool toggle;
    QEMUTimer *timer;

    bool unlock_bypass;
    bool *dyb_protected;
    bool *ppb_protected;

    CFIFlashBlock *erase_block;
    bool erase_suspended;
    int64_t erase_remaining_ns;
    bool program_suspended;
    int64_t program_remaining_ns;

    CFIFlashBlock *buffer_block;
    uint32_t buffer_page;
    uint32_t buffer_loads;
    uint16_t *buffer;
};

struct CFIAmdState {
    bool address_sensitive_unlock;
    bool erase_suspend;
    uint8_t unlock_cycles;
    bool program_setup;
    bool erase_setup;

    uint16_t configuration;
    uint16_t lock_register;
    bool ppb_lock;
    uint16_t password[CFI_AMD_PASSWORD_WORDS];
    uint16_t password_entered[CFI_AMD_PASSWORD_WORDS];
    uint8_t password_unlock_cycle;
};

static const char *const CFI_AMD_MODE_NAMES[] = {
    [CFI_AMD_MODE_READ_ARRAY]       = "read array",
    [CFI_AMD_MODE_AUTOSELECT]       = "autoselect",
    [CFI_AMD_MODE_CFI_QUERY]        = "CFI query",
    [CFI_AMD_MODE_SECSI_SECTOR]     = "SecSi sector",
    [CFI_AMD_MODE_CONFIGURATION]    = "configuration register",
    [CFI_AMD_MODE_DYB]              = "DYB",
    [CFI_AMD_MODE_PPB]              = "PPB",
    [CFI_AMD_MODE_PPB_LOCK]         = "PPB lock",
    [CFI_AMD_MODE_LOCK_REGISTER]    = "lock register",
    [CFI_AMD_MODE_PASSWORD]         = "password",
};

static void cfi_amd_update_read_array(CFIAmdPart *ap) {
    ap->parent.read_array = ap->mode == CFI_AMD_MODE_READ_ARRAY && ap->operation == CFI_AMD_OPERATION_NONE &&
        !ap->erase_suspended && !ap->program_suspended;
}

static void cfi_amd_set_mode(CFIAmdPart *ap, CFIAmdMode mode) {
    CFIFlashPart *p = &ap->parent;

    if (ap->mode != mode)
        DEBUG_LOG(&p->flash->dbg, "<%d> %s mode\n", p->n, CFI_AMD_MODE_NAMES[mode]);
    ap->mode = mode;
    ap->pending = CFI_AMD_PENDING_NONE;
    cfi_amd_update_read_array(ap);
}

static void cfi_amd_start_operation(
    CFIAmdPart *ap,
    CFIAmdOperation operation,
    uint16_t polling_data,
    int64_t duration_ns
) {
    ap->operation = operation;
    ap->polling_data = polling_data;
    ap->pending = CFI_AMD_PENDING_NONE;
    timer_mod(ap->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + duration_ns);
    cfi_amd_update_read_array(ap);
}

static void cfi_amd_operation_done(void *opaque) {
    CFIAmdPart *ap = opaque;
    CFIFlashPart *p = &ap->parent;

    DEBUG_LOG(&p->flash->dbg, "<%d> embedded operation finished\n", p->n);
    if (ap->operation == CFI_AMD_OPERATION_ERASE)
        ap->erase_block = NULL;
    ap->operation = CFI_AMD_OPERATION_NONE;
    cfi_amd_update_read_array(ap);
}

static bool cfi_amd_unlock_addr_is(CFIAmdState *chip_state, uint32_t offset, uint32_t addr) {
    if (!chip_state->address_sensitive_unlock)
        return true;
    return ((offset >> 1) & CFI_AMD_UNLOCK_ADDR_MASK) == addr;
}

static bool cfi_amd_block_contains(CFIFlashPart *p, CFIFlashBlock *blk, uint32_t offset) {
    uint32_t start = p->offset + blk->offset;
    return offset >= start && offset < start + blk->size;
}

static uint16_t cfi_amd_toggle(CFIAmdPart *ap, uint16_t bits) {
    ap->toggle = !ap->toggle;
    return ap->toggle ? bits : 0;
}

static uint16_t cfi_amd_read_status(CFIAmdPart *ap, uint32_t offset) {
    CFIFlashPart *p = &ap->parent;
    uint16_t data_polling = ~ap->polling_data & CFI_AMD_STATUS_DATA_POLLING;

    switch (ap->operation) {
        case CFI_AMD_OPERATION_PROGRAM:
            return data_polling | cfi_amd_toggle(ap, CFI_AMD_STATUS_TOGGLE);

        case CFI_AMD_OPERATION_BUFFER_ABORT:
            return data_polling | CFI_AMD_STATUS_WRITE_BUFFER_ABORT | cfi_amd_toggle(ap, CFI_AMD_STATUS_TOGGLE);

        case CFI_AMD_OPERATION_ERASE: {
            uint16_t toggle_bits = CFI_AMD_STATUS_TOGGLE;
            if (!ap->erase_block || cfi_amd_block_contains(p, ap->erase_block, offset))
                toggle_bits |= CFI_AMD_STATUS_ERASE_TOGGLE;
            return CFI_AMD_STATUS_ERASE_STARTED | cfi_amd_toggle(ap, toggle_bits);
        }

        case CFI_AMD_OPERATION_NONE:
            break;
    }
    g_assert_not_reached();
}

static uint16_t cfi_amd_read_autoselect(CFIAmdPart *ap, uint32_t offset) {
    CFIFlashPart *p = &ap->parent;
    CFIFlashState *flash = p->flash;
    uint16_t index = (offset >> 1) & CFI_INDEX_MASK;

    switch (index) {
        case CFI_AMD_AUTOSELECT_MANUFACTURER:
            return flash->vid;

        case CFI_AMD_AUTOSELECT_DEVICE1:
            return flash->pid[0];

        case CFI_AMD_AUTOSELECT_SECTOR_PROTECTION:
            return cfi_flash_part_find_block(p, offset)->protected;

        case CFI_AMD_AUTOSELECT_INDICATOR_BITS: {
            CFIAmdState *chip_state = flash->cmdset_state;
            uint16_t indicator = flash->chip->indicator_bits & ~CFI_AMD_INDICATOR_CUSTOMER_LOCKED;
            if (!flash->dyb_protected)
                indicator |= CFI_AMD_INDICATOR_DYB_UNLOCKED;
            if (!(chip_state->lock_register & CFI_AMD_LOCK_CUSTOMER_SECSI))
                indicator |= CFI_AMD_INDICATOR_CUSTOMER_LOCKED;
            return indicator;
        }

        case CFI_AMD_AUTOSELECT_DEVICE2:
            return flash->pid[1];

        case CFI_AMD_AUTOSELECT_DEVICE3:
            return flash->pid[2];

        default:
            DEBUG_UNIMP(&flash->dbg, "<%d> autoselect read %02X\n", p->n, index);
            return 0;
    }
}

static bool cfi_amd_secsi_find(CFIFlashState *flash, uint32_t offset, uint16_t **otp_data, uint32_t *data_index) {
    uint32_t index = offset >> 1;

    if (index >= flash->otp0.addr && index < flash->otp0.addr + flash->otp0.size / 2) {
        *otp_data = flash->otp0_data;
        *data_index = index - flash->otp0.addr;
        return true;
    }
    if (index >= flash->otp1.addr && index < flash->otp1.addr + flash->otp1.size / 2) {
        *otp_data = flash->otp1_data;
        *data_index = index - flash->otp1.addr;
        return true;
    }
    return false;
}

static void cfi_amd_update_protection(CFIAmdPart *ap, uint32_t index) {
    ap->parent.blocks[index].protected = ap->dyb_protected[index] || ap->ppb_protected[index];
}

static uint16_t cfi_amd_read_lock_register(CFIAmdPart *ap, uint32_t offset) {
    CFIFlashPart *p = &ap->parent;
    CFIAmdState *chip_state = p->flash->cmdset_state;
    uint16_t index = (offset >> 1) & CFI_INDEX_MASK;

    if (index != CFI_AMD_LOCK_REGISTER_WORD) {
        DEBUG_UNIMP(&p->flash->dbg, "<%d> lock register mode read %02X\n", p->n, index);
        return 0;
    }
    return chip_state->lock_register;
}

static uint16_t cfi_amd_read_password(CFIAmdPart *ap, uint32_t offset) {
    CFIFlashPart *p = &ap->parent;
    CFIAmdState *chip_state = p->flash->cmdset_state;

    if (!(chip_state->lock_register & CFI_AMD_LOCK_PASSWORD_MODE)) {
        DEBUG_UNIMP(&p->flash->dbg, "<%d> password read after the password mode lock\n", p->n);
        return 0;
    }
    return chip_state->password[(offset >> 1) & CFI_AMD_PASSWORD_INDEX_MASK];
}

static uint64_t cfi_amd_read(CFIFlashPart *p, uint32_t offset, uint32_t size) {
    CFIAmdPart *ap = container_of(p, CFIAmdPart, parent);
    CFIFlashState *flash = p->flash;
    CFIAmdState *chip_state = flash->cmdset_state;

    if (ap->operation != CFI_AMD_OPERATION_NONE)
        return cfi_amd_read_status(ap, offset);

    switch (ap->mode) {
        case CFI_AMD_MODE_READ_ARRAY:
            if (ap->erase_suspended && cfi_amd_block_contains(p, ap->erase_block, offset)) {
                return CFI_AMD_STATUS_DATA_POLLING | CFI_AMD_STATUS_TOGGLE |
                    cfi_amd_toggle(ap, CFI_AMD_STATUS_ERASE_TOGGLE);
            }
            return cfi_flash_array_read(p, offset, size);

        case CFI_AMD_MODE_AUTOSELECT:
            return cfi_amd_read_autoselect(ap, offset);

        case CFI_AMD_MODE_CFI_QUERY: {
            uint32_t value = 0;
            if (!cfi_flash_query_read(p, (offset >> 1) & CFI_INDEX_MASK, &value))
                DEBUG_UNIMP(&flash->dbg, "<%d> CFI query read outside the tables at %08X\n", p->n, offset);
            return value;
        }

        case CFI_AMD_MODE_SECSI_SECTOR: {
            uint16_t *otp_data;
            uint32_t data_index;
            if (cfi_amd_secsi_find(flash, offset, &otp_data, &data_index))
                return otp_data[data_index];
            return cfi_flash_array_read(p, offset, size);
        }

        case CFI_AMD_MODE_CONFIGURATION:
            return chip_state->configuration;

        case CFI_AMD_MODE_DYB: {
            uint32_t index = cfi_flash_part_find_block(p, offset) - p->blocks;
            return ap->dyb_protected[index] ? 0 : CFI_AMD_PROTECTION_CLEAR;
        }

        case CFI_AMD_MODE_PPB: {
            uint32_t index = cfi_flash_part_find_block(p, offset) - p->blocks;
            return ap->ppb_protected[index] ? 0 : CFI_AMD_PROTECTION_CLEAR;
        }

        case CFI_AMD_MODE_PPB_LOCK:
            return chip_state->ppb_lock ? 0 : CFI_AMD_PROTECTION_CLEAR;

        case CFI_AMD_MODE_LOCK_REGISTER:
            return cfi_amd_read_lock_register(ap, offset);

        case CFI_AMD_MODE_PASSWORD:
            return cfi_amd_read_password(ap, offset);
    }
    g_assert_not_reached();
}

static void cfi_amd_secsi_program(CFIAmdPart *ap, uint16_t *otp_data, uint32_t data_index, uint16_t value) {
    CFIFlashPart *p = &ap->parent;
    CFIFlashState *flash = p->flash;
    CFIAmdState *chip_state = flash->cmdset_state;
    bool locked = otp_data == flash->otp0_data ?
        (flash->chip->indicator_bits & CFI_AMD_INDICATOR_FACTORY_LOCKED) != 0 :
        (chip_state->lock_register & CFI_AMD_LOCK_CUSTOMER_SECSI) == 0;

    if (locked) {
        DEBUG_LOG(&flash->dbg, "<%d> program in a locked SecSi region ignored\n", p->n);
        return;
    }
    DEBUG_LOG(&flash->dbg, "<%d> program SecSi word %02X: %04X\n", p->n, data_index, value);
    cfi_flash_otp_program(flash, otp_data, data_index, value);
    cfi_amd_start_operation(ap, CFI_AMD_OPERATION_PROGRAM, value,
        (int64_t) flash->word_program_typical_us * SCALE_US);
}

static void cfi_amd_program_word(CFIAmdPart *ap, uint32_t offset, uint16_t value) {
    CFIFlashPart *p = &ap->parent;

    uint16_t *otp_data;
    uint32_t data_index;
    if (ap->mode == CFI_AMD_MODE_SECSI_SECTOR && cfi_amd_secsi_find(p->flash, offset, &otp_data, &data_index)) {
        cfi_amd_secsi_program(ap, otp_data, data_index, value);
        return;
    }

    CFIFlashBlock *blk = cfi_flash_part_find_block(p, offset);
    if (ap->erase_suspended && blk == ap->erase_block) {
        DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> program in the erase-suspended sector at %08X\n", p->n,
            p->flash->offset + offset);
        return;
    }
    DEBUG_LOG(&p->flash->dbg, "<%d> program word %04X to %08X\n", p->n, value, p->flash->offset + offset);
    if (cfi_flash_program(p, blk, offset, value, sizeof(value)) == CFI_FLASH_PROTECTED) {
        DEBUG_LOG(&p->flash->dbg, "<%d> program in a protected sector ignored\n", p->n);
        return;
    }
    cfi_amd_start_operation(ap, CFI_AMD_OPERATION_PROGRAM, value,
        (int64_t) p->flash->word_program_typical_us * SCALE_US);
}

static void cfi_amd_buffer_abort(CFIAmdPart *ap, uint32_t offset, uint16_t value) {
    CFIFlashPart *p = &ap->parent;

    DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> write buffer abort at %08X\n", p->n, p->flash->offset + offset);
    ap->operation = CFI_AMD_OPERATION_BUFFER_ABORT;
    ap->polling_data = value;
    ap->pending = CFI_AMD_PENDING_NONE;
    cfi_amd_update_read_array(ap);
}

static void cfi_amd_buffer_start(CFIAmdPart *ap, uint32_t offset) {
    CFIFlashPart *p = &ap->parent;
    uint32_t page_size = p->flash->write_buffer_size;

    ap->buffer_block = cfi_flash_part_find_block(p, offset);
    ap->buffer_page = offset & ~(page_size - 1);
    memset(ap->buffer, 0xFF, page_size);
    ap->pending = CFI_AMD_PENDING_BUFFER_COUNT;
    DEBUG_LOG(&p->flash->dbg, "<%d> write to buffer at page %08X\n", p->n, p->flash->offset + ap->buffer_page);
}

static void cfi_amd_buffer_write(CFIAmdPart *ap, uint32_t offset, uint16_t value) {
    CFIFlashPart *p = &ap->parent;
    uint32_t page_size = p->flash->write_buffer_size;

    switch (ap->pending) {
        case CFI_AMD_PENDING_BUFFER_COUNT:
            if (!cfi_amd_block_contains(p, ap->buffer_block, offset) || value >= page_size / 2) {
                cfi_amd_buffer_abort(ap, offset, value);
                return;
            }
            ap->buffer_loads = value + 1;
            ap->pending = CFI_AMD_PENDING_BUFFER_LOAD;
            break;

        case CFI_AMD_PENDING_BUFFER_LOAD:
            if ((offset & ~(page_size - 1)) != ap->buffer_page) {
                cfi_amd_buffer_abort(ap, offset, value);
                return;
            }
            ap->buffer[(offset - ap->buffer_page) >> 1] = value;
            ap->polling_data = value;
            if (--ap->buffer_loads == 0)
                ap->pending = CFI_AMD_PENDING_BUFFER_CONFIRM;
            break;

        case CFI_AMD_PENDING_BUFFER_CONFIRM: {
            bool confirmed = value == CFI_AMD_CMD_PROGRAM_BUFFER_TO_FLASH &&
                cfi_amd_block_contains(p, ap->buffer_block, offset);
            if (!confirmed) {
                cfi_amd_buffer_abort(ap, offset, ap->polling_data);
                return;
            }
            DEBUG_LOG(&p->flash->dbg, "<%d> program buffer to flash at page %08X\n", p->n,
                p->flash->offset + ap->buffer_page);
            if (ap->erase_suspended && ap->buffer_block == ap->erase_block) {
                DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> buffer program in the erase-suspended sector\n", p->n);
                ap->pending = CFI_AMD_PENDING_NONE;
                return;
            }
            if (ap->buffer_block->protected) {
                DEBUG_LOG(&p->flash->dbg, "<%d> program in a protected sector ignored\n", p->n);
                ap->pending = CFI_AMD_PENDING_NONE;
                return;
            }
            for (uint32_t i = 0; i < page_size / 2; i++)
                cfi_flash_program(p, ap->buffer_block, ap->buffer_page + i * 2, ap->buffer[i], sizeof(ap->buffer[i]));
            cfi_amd_start_operation(ap, CFI_AMD_OPERATION_PROGRAM, ap->polling_data,
                (int64_t) p->flash->buffer_program_typical_us * SCALE_US);
            break;
        }

        default:
            g_assert_not_reached();
    }
}

static void cfi_amd_sector_erase(CFIAmdPart *ap, uint32_t offset) {
    CFIFlashPart *p = &ap->parent;
    CFIFlashBlock *blk = cfi_flash_part_find_block(p, offset);

    DEBUG_LOG(&p->flash->dbg, "<%d> erase sector %08X...%08X\n", p->n, p->flash->offset + p->offset + blk->offset,
        p->flash->offset + p->offset + blk->offset + blk->size - 1);
    if (ap->erase_suspended) {
        DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> sector erase during erase suspend\n", p->n);
        return;
    }
    if (cfi_flash_erase_block(p, blk) == CFI_FLASH_PROTECTED) {
        DEBUG_LOG(&p->flash->dbg, "<%d> erase of a protected sector ignored\n", p->n);
        return;
    }
    ap->erase_block = blk;
    cfi_amd_start_operation(ap, CFI_AMD_OPERATION_ERASE, 0, (int64_t) p->flash->block_erase_typical_ms * SCALE_MS);
}

static void cfi_amd_erase_suspend(CFIAmdPart *ap) {
    CFIFlashPart *p = &ap->parent;

    DEBUG_LOG(&p->flash->dbg, "<%d> erase suspend\n", p->n);
    ap->erase_remaining_ns = timer_expire_time_ns(ap->timer) - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_del(ap->timer);
    ap->operation = CFI_AMD_OPERATION_NONE;
    ap->erase_suspended = true;
    cfi_amd_update_read_array(ap);
}

static void cfi_amd_erase_resume(CFIAmdPart *ap) {
    CFIFlashPart *p = &ap->parent;

    DEBUG_LOG(&p->flash->dbg, "<%d> erase resume\n", p->n);
    ap->erase_suspended = false;
    cfi_amd_start_operation(ap, CFI_AMD_OPERATION_ERASE, 0, MAX(ap->erase_remaining_ns, 0));
}

static void cfi_amd_chip_erase(CFIAmdPart *ap) {
    CFIFlashState *flash = ap->parent.flash;

    DEBUG_LOG(&flash->dbg, "<%d> chip erase\n", ap->parent.n);
    for (uint32_t i = 0; i < flash->parts_n; i++) {
        CFIAmdPart *part = container_of(flash->parts[i], CFIAmdPart, parent);
        if (part->erase_suspended) {
            DEBUG_GUEST_ERROR(&flash->dbg, "<%d> chip erase during erase suspend\n", ap->parent.n);
            return;
        }
    }

    uint32_t erased_blocks = 0;
    for (uint32_t i = 0; i < flash->parts_n; i++) {
        CFIFlashPart *p = flash->parts[i];
        for (uint32_t j = 0; j < p->blocks_n; j++) {
            if (cfi_flash_erase_block(p, &p->blocks[j]) == CFI_FLASH_OK)
                erased_blocks++;
        }
    }

    int64_t duration_ns = (int64_t) erased_blocks * flash->block_erase_typical_ms * SCALE_MS;
    for (uint32_t i = 0; i < flash->parts_n; i++) {
        CFIAmdPart *part = container_of(flash->parts[i], CFIAmdPart, parent);
        part->erase_block = NULL;
        cfi_amd_start_operation(part, CFI_AMD_OPERATION_ERASE, 0, duration_ns);
    }
}

static void cfi_amd_program_suspend(CFIAmdPart *ap) {
    CFIFlashPart *p = &ap->parent;

    DEBUG_LOG(&p->flash->dbg, "<%d> program suspend\n", p->n);
    ap->program_remaining_ns = timer_expire_time_ns(ap->timer) - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_del(ap->timer);
    ap->operation = CFI_AMD_OPERATION_NONE;
    ap->program_suspended = true;
    cfi_amd_update_read_array(ap);
}

static void cfi_amd_program_resume(CFIAmdPart *ap) {
    CFIFlashPart *p = &ap->parent;

    DEBUG_LOG(&p->flash->dbg, "<%d> program resume\n", p->n);
    ap->program_suspended = false;
    cfi_amd_start_operation(ap, CFI_AMD_OPERATION_PROGRAM, ap->polling_data, MAX(ap->program_remaining_ns, 0));
}

static void cfi_amd_dyb_write(CFIAmdPart *ap, uint32_t offset, uint16_t value) {
    CFIFlashPart *p = &ap->parent;
    CFIFlashBlock *blk = cfi_flash_part_find_block(p, offset);
    uint32_t index = blk - p->blocks;

    switch (value) {
        case CFI_AMD_CMD_DYB_SET:
            ap->dyb_protected[index] = true;
            break;

        case CFI_AMD_CMD_DYB_CLEAR:
            ap->dyb_protected[index] = false;
            break;

        default:
            DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> invalid DYB data %04X\n", p->n, value);
            return;
    }
    cfi_amd_update_protection(ap, index);
    DEBUG_LOG(&p->flash->dbg, "<%d> DYB %08X: %s\n", p->n, p->flash->offset + p->offset + blk->offset,
        ap->dyb_protected[index] ? "protected" : "unprotected");
}

static void cfi_amd_ppb_program(CFIAmdPart *ap, uint32_t offset, uint16_t value) {
    CFIFlashPart *p = &ap->parent;
    CFIAmdState *chip_state = p->flash->cmdset_state;
    CFIFlashBlock *blk = cfi_flash_part_find_block(p, offset);
    uint32_t index = blk - p->blocks;

    if (value != CFI_AMD_CMD_PPB_PROGRAM) {
        DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> invalid PPB program data %04X\n", p->n, value);
        return;
    }
    if (chip_state->ppb_lock) {
        DEBUG_LOG(&p->flash->dbg, "<%d> PPB program with the PPB lock bit set ignored\n", p->n);
        return;
    }
    DEBUG_LOG(&p->flash->dbg, "<%d> PPB %08X: protected\n", p->n, p->flash->offset + p->offset + blk->offset);
    ap->ppb_protected[index] = true;
    cfi_amd_update_protection(ap, index);
    cfi_amd_start_operation(ap, CFI_AMD_OPERATION_PROGRAM, value,
        (int64_t) p->flash->word_program_typical_us * SCALE_US);
}

static void cfi_amd_ppb_erase_all(CFIAmdPart *ap) {
    CFIFlashState *flash = ap->parent.flash;
    CFIAmdState *chip_state = flash->cmdset_state;

    if (chip_state->ppb_lock) {
        DEBUG_LOG(&flash->dbg, "<%d> all PPB erase with the PPB lock bit set ignored\n", ap->parent.n);
        return;
    }
    DEBUG_LOG(&flash->dbg, "<%d> all PPB erase\n", ap->parent.n);
    for (uint32_t i = 0; i < flash->parts_n; i++) {
        CFIAmdPart *part = container_of(flash->parts[i], CFIAmdPart, parent);
        for (uint32_t j = 0; j < part->parent.blocks_n; j++) {
            part->ppb_protected[j] = false;
            cfi_amd_update_protection(part, j);
        }
    }
    cfi_amd_start_operation(ap, CFI_AMD_OPERATION_PROGRAM, 0, (int64_t) flash->block_erase_typical_ms * SCALE_MS);
}

static void cfi_amd_lock_register_program(CFIAmdPart *ap, uint16_t value) {
    CFIFlashPart *p = &ap->parent;
    CFIAmdState *chip_state = p->flash->cmdset_state;
    uint16_t lock_register = chip_state->lock_register & value;

    if ((lock_register & CFI_AMD_LOCK_MODE_BITS) == 0) {
        DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> lock register program of both protection modes aborted\n", p->n);
        return;
    }
    DEBUG_LOG(&p->flash->dbg, "<%d> lock register: %04X\n", p->n, lock_register);
    chip_state->lock_register = lock_register;
    cfi_amd_start_operation(ap, CFI_AMD_OPERATION_PROGRAM, value,
        (int64_t) p->flash->word_program_typical_us * SCALE_US);
}

static void cfi_amd_ppb_lock_set(CFIAmdPart *ap, uint16_t value) {
    CFIFlashPart *p = &ap->parent;
    CFIAmdState *chip_state = p->flash->cmdset_state;

    if (value != CFI_AMD_CMD_PPB_LOCK_BIT_SET) {
        DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> invalid PPB lock bit data %04X\n", p->n, value);
        return;
    }
    DEBUG_LOG(&p->flash->dbg, "<%d> PPB lock bit set\n", p->n);
    chip_state->ppb_lock = true;
}

static void cfi_amd_password_program(CFIAmdPart *ap, uint32_t offset, uint16_t value) {
    CFIFlashPart *p = &ap->parent;
    CFIAmdState *chip_state = p->flash->cmdset_state;
    uint32_t index = (offset >> 1) & CFI_AMD_PASSWORD_INDEX_MASK;

    if (!(chip_state->lock_register & CFI_AMD_LOCK_PASSWORD_MODE)) {
        DEBUG_LOG(&p->flash->dbg, "<%d> password program after the password mode lock ignored\n", p->n);
        return;
    }
    chip_state->password[index] &= value;
    DEBUG_LOG(&p->flash->dbg, "<%d> password word %u: %04X\n", p->n, index, chip_state->password[index]);
    cfi_amd_start_operation(ap, CFI_AMD_OPERATION_PROGRAM, value,
        (int64_t) p->flash->word_program_typical_us * SCALE_US);
}

static void cfi_amd_password_unlock_write(CFIAmdPart *ap, uint32_t offset, uint16_t value) {
    CFIFlashPart *p = &ap->parent;
    CFIAmdState *chip_state = p->flash->cmdset_state;
    uint8_t cycle = chip_state->password_unlock_cycle++;

    if (cycle == 0) {
        if (value != CFI_AMD_CMD_PASSWORD_UNLOCK_WORD_COUNT) {
            DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> invalid password unlock word count %04X\n", p->n, value);
            ap->pending = CFI_AMD_PENDING_NONE;
        }
        return;
    }
    if (cycle <= CFI_AMD_PASSWORD_WORDS) {
        chip_state->password_entered[(offset >> 1) & CFI_AMD_PASSWORD_INDEX_MASK] = value;
        return;
    }

    ap->pending = CFI_AMD_PENDING_NONE;
    if (value != CFI_AMD_CMD_PASSWORD_UNLOCK_CONFIRM) {
        DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> invalid password unlock confirm %04X\n", p->n, value);
        return;
    }
    bool password_mode = (chip_state->lock_register & CFI_AMD_LOCK_PASSWORD_MODE) == 0;
    bool matches = memcmp(chip_state->password_entered, chip_state->password, sizeof(chip_state->password)) == 0;
    DEBUG_LOG(&p->flash->dbg, "<%d> password unlock: %s\n", p->n, password_mode && matches ? "done" : "rejected");
    if (password_mode && matches)
        chip_state->ppb_lock = false;
}

static void cfi_amd_command_set_program(CFIAmdPart *ap, uint32_t offset, uint16_t value) {
    switch (ap->mode) {
        case CFI_AMD_MODE_DYB:
            cfi_amd_dyb_write(ap, offset, value);
            break;

        case CFI_AMD_MODE_PPB:
            cfi_amd_ppb_program(ap, offset, value);
            break;

        case CFI_AMD_MODE_PPB_LOCK:
            cfi_amd_ppb_lock_set(ap, value);
            break;

        case CFI_AMD_MODE_LOCK_REGISTER:
            cfi_amd_lock_register_program(ap, value);
            break;

        case CFI_AMD_MODE_PASSWORD:
            cfi_amd_password_program(ap, offset, value);
            break;

        default:
            g_assert_not_reached();
    }
}

static bool cfi_amd_command_set_write(CFIAmdPart *ap, uint32_t offset, uint16_t value) {
    CFIFlashPart *p = &ap->parent;
    CFIAmdState *chip_state = p->flash->cmdset_state;

    switch (ap->pending) {
        case CFI_AMD_PENDING_COMMAND_SET_PROGRAM:
            ap->pending = CFI_AMD_PENDING_NONE;
            cfi_amd_command_set_program(ap, offset, value);
            return true;

        case CFI_AMD_PENDING_PPB_ERASE:
            ap->pending = CFI_AMD_PENDING_NONE;
            if (value == CFI_AMD_CMD_ALL_PPB_ERASE) {
                cfi_amd_ppb_erase_all(ap);
            } else {
                DEBUG_GUEST_ERROR(&p->flash->dbg, "<%d> invalid all PPB erase command %04X\n", p->n, value);
            }
            return true;

        case CFI_AMD_PENDING_PASSWORD_UNLOCK:
            cfi_amd_password_unlock_write(ap, offset, value);
            return true;

        default:
            break;
    }

    switch (value) {
        case CFI_AMD_CMD_COMMAND_SET_EXIT:
            ap->pending = CFI_AMD_PENDING_COMMAND_SET_EXIT;
            return true;

        case CFI_AMD_CMD_PROGRAM:
            ap->pending = CFI_AMD_PENDING_COMMAND_SET_PROGRAM;
            return true;

        case CFI_AMD_CMD_ERASE_SETUP:
            if (ap->mode != CFI_AMD_MODE_PPB)
                return false;
            ap->pending = CFI_AMD_PENDING_PPB_ERASE;
            return true;

        case CFI_AMD_CMD_PASSWORD_UNLOCK:
            if (ap->mode != CFI_AMD_MODE_PASSWORD)
                return false;
            chip_state->password_unlock_cycle = 0;
            ap->pending = CFI_AMD_PENDING_PASSWORD_UNLOCK;
            return true;

        default:
            return false;
    }
}

static bool cfi_amd_unlock_bypass_write(CFIAmdPart *ap, uint16_t value) {
    CFIAmdState *chip_state = ap->parent.flash->cmdset_state;

    switch (value) {
        case CFI_AMD_CMD_PROGRAM:
            chip_state->program_setup = true;
            return true;

        case CFI_AMD_CMD_UNLOCK_BYPASS_RESET:
            ap->pending = CFI_AMD_PENDING_UNLOCK_BYPASS_RESET;
            return true;

        default:
            return false;
    }
}

static void cfi_amd_unlocked_command(CFIAmdPart *ap, uint32_t offset, uint16_t value) {
    CFIFlashPart *p = &ap->parent;
    CFIFlashState *flash = p->flash;
    CFIAmdState *chip_state = flash->cmdset_state;

    bool at_unlock1_addr = cfi_amd_unlock_addr_is(chip_state, offset, CFI_AMD_UNLOCK1_ADDR);
    if (ap->operation == CFI_AMD_OPERATION_BUFFER_ABORT) {
        if (value == CFI_AMD_CMD_RESET && at_unlock1_addr) {
            DEBUG_LOG(&flash->dbg, "<%d> write to buffer abort reset\n", p->n);
            ap->operation = CFI_AMD_OPERATION_NONE;
            cfi_amd_set_mode(ap, CFI_AMD_MODE_READ_ARRAY);
        } else {
            DEBUG_LOG(&flash->dbg, "<%d> command %04X in write buffer abort ignored\n", p->n, value);
        }
        return;
    }

    if (chip_state->erase_setup) {
        chip_state->erase_setup = false;
        if (value == CFI_AMD_CMD_SECTOR_ERASE) {
            cfi_amd_sector_erase(ap, offset);
        } else if (value == CFI_AMD_CMD_CHIP_ERASE && at_unlock1_addr) {
            cfi_amd_chip_erase(ap);
        } else {
            DEBUG_GUEST_ERROR(&flash->dbg, "<%d> invalid erase command %04X at %08X\n", p->n, value,
                flash->offset + offset);
        }
        return;
    }

    if (value == CFI_AMD_CMD_WRITE_TO_BUFFER) {
        cfi_amd_buffer_start(ap, offset);
        return;
    }

    if (!at_unlock1_addr) {
        DEBUG_LOG(&flash->dbg, "<%d> command %04X at %08X: wrong unlock address, ignored\n", p->n, value,
            flash->offset + offset);
        return;
    }

    switch (value) {
        case CFI_AMD_CMD_RESET:
            cfi_amd_set_mode(ap, CFI_AMD_MODE_READ_ARRAY);
            break;

        case CFI_AMD_CMD_AUTOSELECT:
            if (ap->mode == CFI_AMD_MODE_SECSI_SECTOR) {
                ap->pending = CFI_AMD_PENDING_COMMAND_SET_EXIT;
            } else {
                cfi_amd_set_mode(ap, CFI_AMD_MODE_AUTOSELECT);
            }
            break;

        case CFI_AMD_CMD_PROGRAM:
            chip_state->program_setup = true;
            break;

        case CFI_AMD_CMD_ERASE_SETUP:
            if (ap->erase_suspended) {
                DEBUG_GUEST_ERROR(&flash->dbg, "<%d> erase setup during erase suspend\n", p->n);
                break;
            }
            chip_state->erase_setup = true;
            break;

        case CFI_AMD_CMD_SECSI_SECTOR_ENTRY:
            cfi_amd_set_mode(ap, CFI_AMD_MODE_SECSI_SECTOR);
            break;

        case CFI_AMD_CMD_DYB_ENTRY:
            cfi_amd_set_mode(ap, CFI_AMD_MODE_DYB);
            break;

        case CFI_AMD_CMD_PPB_ENTRY:
            cfi_amd_set_mode(ap, CFI_AMD_MODE_PPB);
            break;

        case CFI_AMD_CMD_GLOBAL_PPB_LOCK_ENTRY:
            cfi_amd_set_mode(ap, CFI_AMD_MODE_PPB_LOCK);
            break;

        case CFI_AMD_CMD_LOCK_REGISTER_ENTRY:
            cfi_amd_set_mode(ap, CFI_AMD_MODE_LOCK_REGISTER);
            break;

        case CFI_AMD_CMD_PASSWORD_PROTECTION_ENTRY:
            cfi_amd_set_mode(ap, CFI_AMD_MODE_PASSWORD);
            break;

        case CFI_AMD_CMD_READ_CONFIGURATION_REGISTER:
            cfi_amd_set_mode(ap, CFI_AMD_MODE_CONFIGURATION);
            break;

        case CFI_AMD_CMD_SET_CONFIGURATION_REGISTER:
            ap->pending = CFI_AMD_PENDING_SET_CONFIGURATION;
            break;

        case CFI_AMD_CMD_UNLOCK_BYPASS_ENTRY:
            DEBUG_LOG(&flash->dbg, "<%d> unlock bypass\n", p->n);
            ap->unlock_bypass = true;
            break;

        default:
            DEBUG_GUEST_ERROR(&flash->dbg, "<%d> unknown command %04X at %08X\n", p->n, value,
                flash->offset + offset);
            break;
    }
}

static bool cfi_amd_unlock_cycle(CFIAmdState *chip_state, uint32_t offset, uint16_t value) {
    switch (chip_state->unlock_cycles) {
        case 0:
            if (value == CFI_AMD_CMD_UNLOCK1 && cfi_amd_unlock_addr_is(chip_state, offset, CFI_AMD_UNLOCK1_ADDR)) {
                chip_state->unlock_cycles = 1;
                return true;
            }
            return false;

        case 1:
            if (value == CFI_AMD_CMD_UNLOCK2 && cfi_amd_unlock_addr_is(chip_state, offset, CFI_AMD_UNLOCK2_ADDR)) {
                chip_state->unlock_cycles = 2;
                return true;
            }
            chip_state->unlock_cycles = 0;
            return false;

        default:
            return false;
    }
}

static void cfi_amd_write(CFIFlashPart *p, uint32_t offset, uint64_t value64, uint32_t size) {
    CFIAmdPart *ap = container_of(p, CFIAmdPart, parent);
    CFIFlashState *flash = p->flash;
    CFIAmdState *chip_state = flash->cmdset_state;
    uint16_t value = value64;

    if (size == 4) {
        cfi_amd_write(p, offset, value64 & 0xFFFF, 2);
        cfi_amd_write(p, offset + 2, value64 >> 16, 2);
        return;
    }
    if (size != 2) {
        DEBUG_GUEST_ERROR(&flash->dbg, "<%d> %u-byte write %08"PRIX64" at %08X\n", p->n, size, value64,
            flash->offset + offset);
        return;
    }

    switch (ap->operation) {
        case CFI_AMD_OPERATION_PROGRAM:
            if (value == CFI_AMD_CMD_PROGRAM_SUSPEND) {
                cfi_amd_program_suspend(ap);
            } else {
                DEBUG_LOG(&flash->dbg, "<%d> write %04X during program ignored\n", p->n, value);
            }
            return;

        case CFI_AMD_OPERATION_ERASE:
            if (value == CFI_AMD_CMD_ERASE_SUSPEND && chip_state->erase_suspend && ap->erase_block) {
                cfi_amd_erase_suspend(ap);
            } else {
                DEBUG_LOG(&flash->dbg, "<%d> write %04X during erase ignored\n", p->n, value);
            }
            return;

        case CFI_AMD_OPERATION_BUFFER_ABORT:
        case CFI_AMD_OPERATION_NONE:
            break;
    }

    if (chip_state->program_setup) {
        chip_state->program_setup = false;
        cfi_amd_program_word(ap, offset, value);
        return;
    }

    switch (ap->pending) {
        case CFI_AMD_PENDING_BUFFER_COUNT:
        case CFI_AMD_PENDING_BUFFER_LOAD:
        case CFI_AMD_PENDING_BUFFER_CONFIRM:
            cfi_amd_buffer_write(ap, offset, value);
            return;

        case CFI_AMD_PENDING_COMMAND_SET_EXIT:
            if (value == CFI_AMD_CMD_COMMAND_SET_EXIT_CONFIRM) {
                cfi_amd_set_mode(ap, CFI_AMD_MODE_READ_ARRAY);
            } else {
                ap->pending = CFI_AMD_PENDING_NONE;
            }
            return;

        case CFI_AMD_PENDING_SET_CONFIGURATION:
            DEBUG_LOG(&flash->dbg, "<%d> configuration register: %04X\n", p->n, value);
            chip_state->configuration = value;
            ap->pending = CFI_AMD_PENDING_NONE;
            return;

        case CFI_AMD_PENDING_UNLOCK_BYPASS_RESET:
            ap->pending = CFI_AMD_PENDING_NONE;
            if (value == CFI_AMD_CMD_UNLOCK_BYPASS_RESET_CONFIRM) {
                DEBUG_LOG(&flash->dbg, "<%d> unlock bypass reset\n", p->n);
                ap->unlock_bypass = false;
            }
            return;

        default:
            break;
    }

    bool command_set_mode = (
        ap->mode == CFI_AMD_MODE_DYB ||
        ap->mode == CFI_AMD_MODE_PPB ||
        ap->mode == CFI_AMD_MODE_PPB_LOCK ||
        ap->mode == CFI_AMD_MODE_LOCK_REGISTER ||
        ap->mode == CFI_AMD_MODE_PASSWORD
    );
    if (command_set_mode && cfi_amd_command_set_write(ap, offset, value))
        return;

    bool unlock_bypass_ready = ap->unlock_bypass && ap->mode == CFI_AMD_MODE_READ_ARRAY;
    if (unlock_bypass_ready && cfi_amd_unlock_bypass_write(ap, value))
        return;

    if (chip_state->unlock_cycles == 2) {
        chip_state->unlock_cycles = 0;
        cfi_amd_unlocked_command(ap, offset, value);
        return;
    }
    if (cfi_amd_unlock_cycle(chip_state, offset, value))
        return;

    if (ap->operation == CFI_AMD_OPERATION_BUFFER_ABORT) {
        DEBUG_LOG(&flash->dbg, "<%d> write %04X in write buffer abort ignored\n", p->n, value);
        return;
    }

    bool query_allowed = ap->mode == CFI_AMD_MODE_READ_ARRAY || ap->mode == CFI_AMD_MODE_AUTOSELECT;
    bool query_command = value == CFI_AMD_CMD_CFI_QUERY &&
        cfi_amd_unlock_addr_is(chip_state, offset, CFI_AMD_UNLOCK1_ADDR);
    if (query_command && query_allowed) {
        cfi_amd_set_mode(ap, CFI_AMD_MODE_CFI_QUERY);
        return;
    }
    if (value == CFI_AMD_CMD_PROGRAM_RESUME && ap->program_suspended) {
        cfi_amd_program_resume(ap);
        return;
    }
    if (value == CFI_AMD_CMD_ERASE_RESUME && ap->erase_suspended) {
        cfi_amd_erase_resume(ap);
        return;
    }

    bool identification_mode = ap->mode == CFI_AMD_MODE_AUTOSELECT || ap->mode == CFI_AMD_MODE_CFI_QUERY;
    bool resets_mode = value == CFI_AMD_CMD_RESET && ap->mode != CFI_AMD_MODE_SECSI_SECTOR;
    if (identification_mode || resets_mode) {
        DEBUG_LOG(&flash->dbg, "<%d> write %04X at %08X returns to read array\n", p->n, value, flash->offset + offset);
        cfi_amd_set_mode(ap, CFI_AMD_MODE_READ_ARRAY);
        return;
    }
    DEBUG_LOG(&flash->dbg, "<%d> write %04X at %08X ignored\n", p->n, value, flash->offset + offset);
}

static void cfi_amd_init_bank(
    CFIFlashState *flash,
    CFIFlashChipPart *bank,
    uint32_t offset,
    uint32_t first_sector,
    uint32_t sectors
) {
    CFIFlashEraseRegion *regions = g_new(CFIFlashEraseRegion, flash->cfi_erase_regions_n);
    uint32_t regions_n = 0;
    uint32_t bank_size = 0;
    uint32_t region_first_sector = 0;

    for (uint32_t i = 0; i < flash->cfi_erase_regions_n; i++) {
        const CFIFlashEraseRegion *erase = &flash->cfi_erase_regions[i];
        uint32_t region_end_sector = region_first_sector + erase->size / erase->sector;
        uint32_t start = MAX(first_sector, region_first_sector);
        uint32_t end = MIN(first_sector + sectors, region_end_sector);
        if (start < end) {
            uint32_t size = (end - start) * erase->sector;
            regions[regions_n++] = (CFIFlashEraseRegion) { bank_size, size, erase->sector };
            bank_size += size;
        }
        region_first_sector = region_end_sector;
    }
    *bank = (CFIFlashChipPart) { offset, bank_size, regions, regions_n };
}

static bool cfi_amd_add_banks(CFIFlashState *flash, CFIQueryReader *r) {
    uint32_t banks_n = cfi_query_read_le(r, 1);
    if (r->overrun || banks_n == 0) {
        cfi_flash_error(flash, "invalid PRI bank organization");
        return false;
    }

    flash->chip_parts = g_new(CFIFlashChipPart, banks_n);
    uint32_t first_sector = 0;
    uint32_t offset = 0;
    for (uint32_t i = 0; i < banks_n; i++) {
        uint32_t sectors = cfi_query_read_le(r, 1);
        CFIFlashChipPart *bank = &flash->chip_parts[flash->chip_parts_n++];
        cfi_amd_init_bank(flash, bank, offset, first_sector, sectors);
        if (r->overrun || bank->size == 0) {
            cfi_flash_error(flash, "PRI bank %u does not match the CFI erase regions", i);
            return false;
        }
        first_sector += sectors;
        offset += bank->size;
    }
    if (offset != flash->size) {
        cfi_flash_error(flash, "PRI banks cover 0x%08X bytes, device size is 0x%08X", offset, flash->size);
        return false;
    }
    return true;
}

static bool cfi_amd_decode_pri(CFIFlashState *flash) {
    const CFIFlashChip *chip = flash->chip;
    CFIQueryReader r = { .data = chip->pri, .size = chip->pri_size };

    uint32_t magic = cfi_query_read_le(&r, 3);
    uint32_t version = cfi_query_read_le(&r, 2);
    bool known_version = version == CFI_AMD_PRI_VERSION_1_3 || version == CFI_AMD_PRI_VERSION_1_4;
    if (r.overrun || magic != CFI_AMD_PRI_MAGIC || !known_version) {
        cfi_flash_error(flash, "unsupported PRI");
        return false;
    }
    if (!flash->write_buffer_size) {
        cfi_flash_error(flash, "CFI declares no write buffer");
        return false;
    }

    r.pos = CFI_AMD_PRI_UNLOCK_AND_TECHNOLOGY;
    uint32_t unlock_and_technology = cfi_query_read_le(&r, 1);
    uint32_t erase_suspend = cfi_query_read_le(&r, 1);
    if (r.overrun) {
        cfi_flash_error(flash, "truncated PRI");
        return false;
    }
    CFIAmdState *chip_state = g_new0(CFIAmdState, 1);
    chip_state->address_sensitive_unlock = (unlock_and_technology & CFI_AMD_PRI_ADDRESS_INSENSITIVE_UNLOCK) == 0;
    chip_state->erase_suspend = erase_suspend != 0;
    chip_state->lock_register = UINT16_MAX;
    if (chip->indicator_bits & CFI_AMD_INDICATOR_CUSTOMER_LOCKED)
        chip_state->lock_register &= ~CFI_AMD_LOCK_CUSTOMER_SECSI;
    memset(chip_state->password, 0xFF, sizeof(chip_state->password));
    flash->cmdset_state = chip_state;

    if (version == CFI_AMD_PRI_VERSION_1_4) {
        r.pos = CFI_AMD_PRI_SECSI_SECTOR_SIZE;
        uint32_t secsi_region_shift = cfi_query_read_le(&r, 1);
        if (secsi_region_shift >= 16) {
            cfi_flash_error(flash, "invalid PRI SecSi sector size 2^%u", secsi_region_shift);
            return false;
        }
        uint32_t secsi_region_words = 1U << secsi_region_shift;
        flash->otp0 = (CFIFlashOtp) {
            .addr = 0,
            .size = secsi_region_words * 2,
            .factory_size = secsi_region_words * 2,
        };
        flash->otp1 = (CFIFlashOtp) { .addr = secsi_region_words, .size = secsi_region_words * 2 };
        r.pos = CFI_AMD_PRI_BANK_ORGANIZATION;
        return cfi_amd_add_banks(flash, &r);
    }
    flash->chip_parts = g_new(CFIFlashChipPart, 1);
    flash->chip_parts[0] = (CFIFlashChipPart) { 0, flash->size, flash->cfi_erase_regions, flash->cfi_erase_regions_n };
    flash->chip_parts_n = 1;
    return true;
}

static void cfi_amd_init(CFIFlashState *flash) {
    for (uint32_t i = 0; i < flash->parts_n; i++) {
        CFIAmdPart *ap = container_of(flash->parts[i], CFIAmdPart, parent);
        ap->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cfi_amd_operation_done, ap);
        ap->buffer = g_malloc(flash->write_buffer_size);
        ap->dyb_protected = g_new0(bool, flash->parts[i]->blocks_n);
        ap->ppb_protected = g_new0(bool, flash->parts[i]->blocks_n);
    }
}

static void cfi_amd_reset(CFIFlashState *flash) {
    CFIAmdState *chip_state = flash->cmdset_state;

    chip_state->unlock_cycles = 0;
    chip_state->program_setup = false;
    chip_state->erase_setup = false;
    chip_state->configuration = CFI_AMD_CONFIGURATION_DEFAULT;
    chip_state->ppb_lock = (chip_state->lock_register & CFI_AMD_LOCK_PASSWORD_MODE) == 0;
    for (uint32_t i = 0; i < flash->parts_n; i++) {
        CFIFlashPart *p = flash->parts[i];
        CFIAmdPart *ap = container_of(p, CFIAmdPart, parent);

        timer_del(ap->timer);
        ap->operation = CFI_AMD_OPERATION_NONE;
        ap->unlock_bypass = false;
        ap->erase_block = NULL;
        ap->erase_suspended = false;
        ap->program_suspended = false;
        for (uint32_t j = 0; j < p->blocks_n; j++) {
            ap->dyb_protected[j] = flash->dyb_protected;
            cfi_amd_update_protection(ap, j);
        }
        cfi_amd_set_mode(ap, CFI_AMD_MODE_READ_ARRAY);
    }
}

const CFIFlashCommandSet cfi_flash_amd_cmdset = {
    .part_size = sizeof(CFIAmdPart),
    .decode_pri = cfi_amd_decode_pri,
    .init = cfi_amd_init,
    .read = cfi_amd_read,
    .write = cfi_amd_write,
    .reset = cfi_amd_reset,
};
