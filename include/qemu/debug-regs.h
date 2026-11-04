/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef QEMU_DEBUG_REGS_H
#define QEMU_DEBUG_REGS_H

#include "qemu/debug-channel.h"

typedef struct DebugRegValue {
    const char *name;
    uint64_t bits;
} DebugRegValue;

typedef struct DebugRegField {
    const char *name;
    uint64_t mask;
    uint8_t shift;
    const DebugRegValue *values;
    size_t values_count;
} DebugRegField;

typedef struct DebugReg {
    const char *name;
    uint64_t offset;
    const DebugRegField *fields;
    size_t fields_count;
} DebugReg;

typedef struct DebugRegion {
    const char *name;
    uint64_t offset;
    uint64_t size;
} DebugRegion;

typedef struct DebugRegs {
    const DebugReg *regs;
    size_t count;
    const DebugRegion *regions;
    size_t regions_count;
    uint8_t reg_size;
    uint8_t addr_digits;
} DebugRegs;

extern const DebugIoFormat debug_regs_format;

const DebugReg *debug_regs_find(const DebugRegs *regs, uint64_t offset,
                                uint64_t *delta);
void debug_regs_print_fields(const DebugReg *reg, uint64_t value,
                             GString *out);

#endif
