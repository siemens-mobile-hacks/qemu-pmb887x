/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "../gsm-test-algorithm.h"

static void test_default_ki(void)
{
    static const uint8_t ki[GSM_TEST_ALGORITHM_KI_SIZE] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
    };
    static const uint8_t rand[GSM_TEST_ALGORITHM_RAND_SIZE] = {
        0x3A, 0x5F, 0x91, 0xC2, 0x07, 0xE4, 0x68, 0xBD,
        0x14, 0xF9, 0x2C, 0x83, 0xD6, 0x70, 0xAB, 0x45
    };
    static const uint8_t expected_sres[GSM_TEST_ALGORITHM_SRES_SIZE] = {
        0xFF, 0x32, 0x7E, 0xB9
    };
    static const uint8_t expected_kc[GSM_TEST_ALGORITHM_KC_SIZE] = {
        0x1B, 0xFC, 0x90, 0x45, 0x57, 0x3B, 0xD6, 0x88
    };
    uint8_t sres[GSM_TEST_ALGORITHM_SRES_SIZE];
    uint8_t kc[GSM_TEST_ALGORITHM_KC_SIZE];

    gsm_test_algorithm_run(ki, rand, sres, kc);
    g_assert_cmpmem(sres, sizeof(sres), expected_sres, sizeof(expected_sres));
    g_assert_cmpmem(kc, sizeof(kc), expected_kc, sizeof(expected_kc));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/smartcard/gsm-test-algorithm/default-ki",
                    test_default_ki);
    return g_test_run();
}
