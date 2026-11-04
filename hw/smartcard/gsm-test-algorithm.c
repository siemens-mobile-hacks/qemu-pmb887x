/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "gsm-test-algorithm.h"

#define GSM_TEST_ALGORITHM_RES_WORD_SIZE 4
#define GSM_TEST_ALGORITHM_CK_SHIFT 1
#define GSM_TEST_ALGORITHM_IK_SHIFT 2

void gsm_test_algorithm_run(const uint8_t ki[GSM_TEST_ALGORITHM_KI_SIZE],
                            const uint8_t rand[GSM_TEST_ALGORITHM_RAND_SIZE],
                            uint8_t sres[GSM_TEST_ALGORITHM_SRES_SIZE],
                            uint8_t kc[GSM_TEST_ALGORITHM_KC_SIZE])
{
    uint8_t xdout[GSM_TEST_ALGORITHM_RAND_SIZE];
    uint8_t ck[GSM_TEST_ALGORITHM_RAND_SIZE];
    uint8_t ik[GSM_TEST_ALGORITHM_RAND_SIZE];

    for (size_t i = 0; i < GSM_TEST_ALGORITHM_RAND_SIZE; i++) {
        xdout[i] = ki[i] ^ rand[i];
    }
    for (size_t i = 0; i < GSM_TEST_ALGORITHM_RAND_SIZE; i++) {
        ck[i] = xdout[(i + GSM_TEST_ALGORITHM_CK_SHIFT) %
                      GSM_TEST_ALGORITHM_RAND_SIZE];
        ik[i] = xdout[(i + GSM_TEST_ALGORITHM_IK_SHIFT) %
                      GSM_TEST_ALGORITHM_RAND_SIZE];
    }
    for (size_t i = 0; i < GSM_TEST_ALGORITHM_SRES_SIZE; i++) {
        sres[i] = xdout[i] ^ xdout[i + GSM_TEST_ALGORITHM_RES_WORD_SIZE] ^
                  xdout[i + 2 * GSM_TEST_ALGORITHM_RES_WORD_SIZE] ^
                  xdout[i + 3 * GSM_TEST_ALGORITHM_RES_WORD_SIZE];
    }
    for (size_t i = 0; i < GSM_TEST_ALGORITHM_KC_SIZE; i++) {
        size_t second = i + GSM_TEST_ALGORITHM_KC_SIZE;

        kc[i] = ck[i] ^ ck[second] ^ ik[i] ^ ik[second];
    }
}
