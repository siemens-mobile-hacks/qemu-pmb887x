/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/debug-regs.h"

const DebugReg *debug_regs_find(const DebugRegs *regs, uint64_t offset,
                                uint64_t *delta)
{
    for (size_t i = 0; i < regs->count; i++) {
        const DebugReg *reg = &regs->regs[i];

        if (offset >= reg->offset && offset - reg->offset < regs->reg_size) {
            *delta = offset - reg->offset;
            return reg;
        }
    }
    return NULL;
}

static const char *debug_regs_value_name(const DebugRegField *field,
                                         uint64_t bits)
{
    for (size_t i = 0; i < field->values_count; i++) {
        if (field->values[i].bits == bits) {
            return field->values[i].name;
        }
    }
    return NULL;
}

static void debug_regs_separator(bool *is_first, GString *out)
{
    g_string_append(out, *is_first ? ": " : " | ");
    *is_first = false;
}

void debug_regs_print_fields(const DebugReg *reg, uint64_t value,
                             GString *out)
{
    bool is_first = true;
    uint64_t known = 0;

    for (size_t i = 0; i < reg->fields_count; i++) {
        const DebugRegField *field = &reg->fields[i];
        uint64_t field_bits = value & field->mask;
        const char *value_name = debug_regs_value_name(field, field_bits);
        uint64_t field_max = field->mask >> field->shift;
        uint64_t field_value = field_bits >> field->shift;

        known |= field->mask;
        if (field_max == 1 && !field_value && !value_name) {
            continue;
        }
        debug_regs_separator(&is_first, out);
        if (value_name) {
            g_string_append_printf(out, "%s(%s)", field->name, value_name);
        } else if (field_max == 1) {
            g_string_append(out, field->name);
        } else {
            g_string_append_printf(out, "%s(0x%02" PRIX64 ")", field->name,
                                   field_value);
        }
    }
    for (uint64_t unknown = value & ~known; unknown; unknown &= unknown - 1) {
        debug_regs_separator(&is_first, out);
        g_string_append_printf(out, "UNK_%d", ctz64(unknown));
    }
}

static void debug_regs_print(const char *name, const void *opaque,
                             const DebugIoAccess *io, GString *out)
{
    const DebugRegs *regs = opaque;
    uint64_t delta;
    const DebugReg *reg = debug_regs_find(regs, io->offset, &delta);

    debug_io_print_access(name, io, regs->addr_digits, out);
    if (!reg) {
        g_string_append_printf(out, " (%s_*)", name);
    } else if (delta) {
        g_string_append_printf(out, " (%s_%s+0x%" PRIX64 ")", name, reg->name,
                               delta);
    } else {
        g_string_append_printf(out, " (%s_%s)", name, reg->name);
        if (reg->fields_count) {
            debug_regs_print_fields(reg, io->value, out);
        }
    }
    debug_io_print_context(io, out);
}

const DebugIoFormat debug_regs_format = {
    .print = debug_regs_print,
};
