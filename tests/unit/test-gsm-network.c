/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/gsm/gsm-network.h"

typedef struct CarrierVector CarrierVector;

struct CarrierVector {
    uint32_t downlink_hz;
    uint32_t uplink_hz;
    uint16_t arfcn;
};

/* 3GPP TS 45.005 2: the first and last channel of each band. */
static const CarrierVector CARRIER_VECTORS[] = {
    { 935200000U, 890200000U, 1 },
    { 959800000U, 914800000U, 124 },
    { 925200000U, 880200000U, 975 },
    { 934800000U, 889800000U, 1023 },
    { 869200000U, 824200000U, 128 },
    { 893800000U, 848800000U, 251 },
    { 1805200000U, 1710200000U, 512 },
    { 1879800000U, 1784800000U, 885 },
    { 1930200000U, 1850200000U, 512 | GSM_ARFCN_PCS1900 },
    { 1989800000U, 1909800000U, 810 | GSM_ARFCN_PCS1900 },
};

static void test_carrier_to_arfcn(void) {
    for (size_t i = 0; i < ARRAY_SIZE(CARRIER_VECTORS); i++) {
        const CarrierVector *vector = &CARRIER_VECTORS[i];
        uint16_t arfcn = 0;

        g_assert_true(gsm_downlink_frequency_to_arfcn(vector->downlink_hz, &arfcn));
        g_assert_cmphex(arfcn, ==, vector->arfcn);
        arfcn = 0;
        g_assert_true(gsm_uplink_frequency_to_arfcn(vector->uplink_hz, &arfcn));
        g_assert_cmphex(arfcn, ==, vector->arfcn);
    }
}

static void test_carrier_off_raster(void) {
    static const uint32_t frequencies_hz[] = {
        935100000U,
        960200000U,
        1880000000U,
        1990000000U,
        2000000000U,
    };
    uint16_t arfcn;

    for (size_t i = 0; i < ARRAY_SIZE(frequencies_hz); i++)
        g_assert_false(gsm_downlink_frequency_to_arfcn(frequencies_hz[i], &arfcn));
}

static void test_arfcn_is_valid(void) {
    static const uint16_t valid[] = { 0, 124, 128, 251, 512, 885, 975, 1023, 512 | GSM_ARFCN_PCS1900,
        810 | GSM_ARFCN_PCS1900 };
    static const uint16_t invalid[] = { 125, 127, 252, 511, 886, 974, 1024, 124 | GSM_ARFCN_PCS1900,
        811 | GSM_ARFCN_PCS1900 };

    for (size_t i = 0; i < ARRAY_SIZE(valid); i++)
        g_assert_true(gsm_arfcn_is_valid(valid[i]));
    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++)
        g_assert_false(gsm_arfcn_is_valid(invalid[i]));
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/gsm-network/carrier-to-arfcn", test_carrier_to_arfcn);
    g_test_add_func("/gsm-network/carrier-off-raster", test_carrier_off_raster);
    g_test_add_func("/gsm-network/arfcn-is-valid", test_arfcn_is_valid);
    return g_test_run();
}
