/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/debug-regs.h"

static const DebugReg *debug_regs_find(const DebugRegs *regs, uint64_t offset,
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

static void debug_regs_print_fields(const DebugReg *reg, uint64_t value,
                                    GString *out)
{
    if (!reg->fields_count) {
        return;
    }

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

static const DebugRegion *debug_regs_find_region(const DebugRegs *regs,
                                                 uint64_t offset)
{
    for (size_t i = 0; i < regs->regions_count; i++) {
        const DebugRegion *region = &regs->regions[i];

        if (offset >= region->offset && offset - region->offset < region->size) {
            return region;
        }
    }
    return NULL;
}

static void debug_regs_print_region(const char *name, const DebugRegs *regs,
                                    uint64_t offset, GString *out)
{
    const DebugRegion *region = debug_regs_find_region(regs, offset);

    if (!region) {
        g_string_append_printf(out, " (%s_*)", name);
    } else if (offset != region->offset) {
        g_string_append_printf(out, " (%s_%s+0x%" PRIX64 ")", name,
                               region->name, offset - region->offset);
    } else {
        g_string_append_printf(out, " (%s_%s)", name, region->name);
    }
}

static void debug_regs_print(const DebugChannel *ch, const DebugIoAccess *io,
                             GString *out)
{
    const DebugRegs *regs = ch->io_format_opaque;
    uint64_t delta;
    const DebugReg *reg = debug_regs_find(regs, io->offset, &delta);

    debug_io_print_access(ch->name, io, regs->addr_digits, out);
    if (!reg) {
        debug_regs_print_region(ch->name, regs, io->offset, out);
    } else if (delta) {
        g_string_append_printf(out, " (%s_%s+0x%" PRIX64 ")", ch->name,
                               reg->name, delta);
    } else {
        const char *suffix = ch->get_reg_suffix ?
                             ch->get_reg_suffix(ch, io->offset) : NULL;

        if (suffix) {
            g_string_append_printf(out, " (%s_%s_%s)", ch->name, reg->name,
                                   suffix);
        } else {
            g_string_append_printf(out, " (%s_%s)", ch->name, reg->name);
        }
        debug_regs_print_fields(reg, io->value, out);
    }
    debug_io_print_context(io, out);
}

const DebugIoFormat debug_regs_format = {
    .print = debug_regs_print,
};
