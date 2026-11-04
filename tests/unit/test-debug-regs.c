/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qemu/debug-regs.h"

static const DebugRegValue en_values[] = {
    { "OFF", 0 },
};

static const DebugRegValue mode_values[] = {
    { "SLOW", 0x00 },
    { "FAST", 0x10 },
};

static const DebugRegField clc_fields[] = {
    { "DISR", 0x0001, 0, NULL, 0 },
    { "EN", 0x0002, 1, en_values, ARRAY_SIZE(en_values) },
    { "MODE", 0x0030, 4, mode_values, ARRAY_SIZE(mode_values) },
    { "RMC", 0xff00, 8, NULL, 0 },
};

static const DebugReg module_reg_list[] = {
    { "CLC", 0x0, clc_fields, ARRAY_SIZE(clc_fields) },
    { "DATA", 0x4, NULL, 0 },
};

static const DebugRegs module_regs = {
    .regs = module_reg_list,
    .count = ARRAY_SIZE(module_reg_list),
    .reg_size = 4,
    .addr_digits = 8,
};

static const DebugReg chip_reg_list[] = {
    { "VOL", 0x1a, NULL, 0 },
};

static const DebugRegs chip_regs = {
    .regs = chip_reg_list,
    .count = ARRAY_SIZE(chip_reg_list),
    .reg_size = 1,
    .addr_digits = 2,
};

static char *print_io(const char *name, const DebugRegs *regs,
                      DebugIoAccess io)
{
    GString *out = g_string_new(NULL);

    debug_regs_format.print(name, regs, &io, out);
    return g_string_free(out, false);
}

static void test_fields(void)
{
    g_autofree char *line = print_io("UNIT0", &module_regs, (DebugIoAccess) {
        .addr = 0xf1000000, .offset = 0x0, .size = 4, .value = 0x10111,
        .is_write = true, .has_context = true,
        .pc = 0x401238, .lr = 0x400738,
    });

    g_assert_cmpstr(line, ==,
                    "[UNIT0] WRITE[4] F1000000: 00010111 (UNIT0_CLC): "
                    "DISR | EN(OFF) | MODE(FAST) | RMC(0x01) | UNK_16 "
                    "(PC: 00401238, LR: 00400738)");
}

static void test_no_fields(void)
{
    g_autofree char *line = print_io("UNIT0", &module_regs, (DebugIoAccess) {
        .addr = 0xf1000004, .offset = 0x4, .size = 4, .value = 0xff,
    });

    g_assert_cmpstr(line, ==, "[UNIT0]  READ[4] F1000004: 000000FF"
                    " (UNIT0_DATA)");
}

static void test_inside_reg(void)
{
    g_autofree char *line = print_io("UNIT0", &module_regs, (DebugIoAccess) {
        .addr = 0xf1000002, .offset = 0x2, .size = 1, .value = 0x5,
    });

    g_assert_cmpstr(line, ==, "[UNIT0]  READ[1] F1000002: 05"
                    " (UNIT0_CLC+0x2)");
}

static void test_unknown_reg(void)
{
    g_autofree char *line = print_io("UNIT0", &module_regs, (DebugIoAccess) {
        .addr = 0xf1000040, .offset = 0x40, .size = 4, .value = 0x1,
        .is_write = true,
    });

    g_assert_cmpstr(line, ==, "[UNIT0] WRITE[4] F1000040: 00000001"
                    " (UNIT0_*)");
}

static void test_chip_addr(void)
{
    g_autofree char *line = print_io("PMIC", &chip_regs, (DebugIoAccess) {
        .addr = 0x1a, .offset = 0x1a, .size = 1, .value = 0x5,
    });

    g_assert_cmpstr(line, ==, "[PMIC]  READ[1] 1A: 05 (PMIC_VOL)");
}

static void test_find(void)
{
    uint64_t delta = 0;

    g_assert_true(debug_regs_find(&module_regs, 0x7, &delta) ==
                  &module_reg_list[1]);
    g_assert_cmpuint(delta, ==, 3);
    g_assert_null(debug_regs_find(&module_regs, 0x8, &delta));
    g_assert_null(debug_regs_find(&chip_regs, 0x1b, &delta));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/debug-regs/fields", test_fields);
    g_test_add_func("/debug-regs/no-fields", test_no_fields);
    g_test_add_func("/debug-regs/inside-reg", test_inside_reg);
    g_test_add_func("/debug-regs/unknown-reg", test_unknown_reg);
    g_test_add_func("/debug-regs/chip-addr", test_chip_addr);
    g_test_add_func("/debug-regs/find", test_find);
    return g_test_run();
}
