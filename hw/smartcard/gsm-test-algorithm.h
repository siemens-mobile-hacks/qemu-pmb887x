/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SMARTCARD_GSM_TEST_ALGORITHM_H
#define HW_SMARTCARD_GSM_TEST_ALGORITHM_H

#define GSM_TEST_ALGORITHM_KI_SIZE 16
#define GSM_TEST_ALGORITHM_RAND_SIZE 16
#define GSM_TEST_ALGORITHM_SRES_SIZE 4
#define GSM_TEST_ALGORITHM_KC_SIZE 8

void gsm_test_algorithm_run(const uint8_t ki[GSM_TEST_ALGORITHM_KI_SIZE],
                            const uint8_t rand[GSM_TEST_ALGORITHM_RAND_SIZE],
                            uint8_t sres[GSM_TEST_ALGORITHM_SRES_SIZE],
                            uint8_t kc[GSM_TEST_ALGORITHM_KC_SIZE]);

#endif
