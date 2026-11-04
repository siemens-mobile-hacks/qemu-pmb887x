/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/smartcard/gsm11.h"
#include "../gsm-sim-fs.h"

#define TEST_DF 0x7F20
#define TEST_EF_PROTECTED 0x6F01
#define TEST_EF_COUNTER 0x6F02
#define TEST_DF_CHARACTERISTICS 13
#define TEST_DF_CHV1_STATUS 18
#define TEST_DF_UNBLOCK_CHV1_STATUS 19
#define TEST_EF_INCREASE 7
#define TEST_EF_FILE_STATUS 11
#define TEST_CHV1 0x01
#define TEST_CHV2 0x02
#define TEST_UNBLOCK_CHV1 0x00

static const uint8_t PIN1[GSM_SIM_FS_CHV_SIZE] = {
    '1', '2', '3', '4', 0xFF, 0xFF, 0xFF, 0xFF
};
static const uint8_t WRONG_PIN[GSM_SIM_FS_CHV_SIZE] = {
    '9', '9', '9', '9', 0xFF, 0xFF, 0xFF, 0xFF
};
static const uint8_t PIN2[GSM_SIM_FS_CHV_SIZE] = {
    '5', '6', '7', '8', 0xFF, 0xFF, 0xFF, 0xFF
};
static const uint8_t PUK1[GSM_SIM_FS_CHV_SIZE] = {
    '1', '1', '1', '1', '2', '2', '2', '2'
};

static const GSMSimFileDefinition TEST_FILES[] = {
    { .id = GSM_SIM_FS_MF_ID, .structure = GSM_SIM_FILE_MF },
    { .id = TEST_DF, .parent_id = GSM_SIM_FS_MF_ID,
      .structure = GSM_SIM_FILE_DF },
    { .id = TEST_EF_PROTECTED, .parent_id = TEST_DF,
      .structure = GSM_SIM_FILE_TRANSPARENT, .size = 2, .fill = 0x55,
      .access = { 0x11, 0xF0, 0x22 } },
    { .id = TEST_EF_COUNTER, .parent_id = TEST_DF,
      .structure = GSM_SIM_FILE_CYCLIC, .size = 6, .record_size = 3,
      .access = { 0x11, 0x10, 0x44 } },
};

typedef struct TestCard {
    GSMSimFS fs;
    ISO7816Response response;
} TestCard;

static void test_card_init(TestCard *card, bool chv1_enabled)
{
    memset(card, 0, sizeof(*card));
    gsm_sim_fs_init(&card->fs, TEST_FILES, ARRAY_SIZE(TEST_FILES));
    memcpy(card->fs.chv1.value, PIN1, sizeof(PIN1));
    memcpy(card->fs.chv1.unblock_value, PUK1, sizeof(PUK1));
    memcpy(card->fs.chv2.value, PIN2, sizeof(PIN2));
    card->fs.chv1.attempts = GSM_SIM_FS_CHV_ATTEMPTS;
    card->fs.chv1.unblock_attempts = GSM_SIM_FS_UNBLOCK_CHV_ATTEMPTS;
    card->fs.chv2.attempts = GSM_SIM_FS_CHV_ATTEMPTS;
    card->fs.chv2.unblock_attempts = GSM_SIM_FS_UNBLOCK_CHV_ATTEMPTS;
    card->fs.chv1_enabled = chv1_enabled;
}

static uint16_t test_card_status(const TestCard *card)
{
    return (card->response.sw1 << 8) | card->response.sw2;
}

static void test_card_select(TestCard *card, uint16_t id)
{
    gsm_sim_fs_select(&card->fs, id, &card->response);
    g_assert_cmphex(card->response.sw1, ==, 0x9F);
    gsm_sim_fs_get_response(&card->fs, card->response.sw2, &card->response);
    g_assert_cmphex(test_card_status(card), ==, GSM11_STATUS_SUCCESS);
}

static uint16_t test_card_read(TestCard *card)
{
    gsm_sim_fs_read_binary(&card->fs, 0, 2, &card->response);
    return test_card_status(card);
}

static uint16_t test_card_verify(TestCard *card, uint8_t chv,
                                 const uint8_t *value)
{
    gsm_sim_fs_verify_chv(&card->fs, chv, value, GSM_SIM_FS_CHV_SIZE,
                          &card->response);
    return test_card_status(card);
}

static void test_chv1_block_and_unblock(void)
{
    TestCard card;
    uint8_t unblock[2 * GSM_SIM_FS_CHV_SIZE];

    test_card_init(&card, true);
    test_card_select(&card, TEST_DF);
    test_card_select(&card, TEST_EF_PROTECTED);
    g_assert_cmphex(test_card_read(&card), ==, GSM11_STATUS_ACCESS_DENIED);

    g_assert_cmphex(test_card_verify(&card, TEST_CHV1, WRONG_PIN), ==,
                    GSM11_STATUS_ACCESS_DENIED);
    g_assert_cmphex(test_card_verify(&card, TEST_CHV1, WRONG_PIN), ==,
                    GSM11_STATUS_ACCESS_DENIED);
    test_card_select(&card, TEST_DF);
    g_assert_cmphex(card.response.data[TEST_DF_CHV1_STATUS], ==, 0x81);
    g_assert_cmphex(test_card_verify(&card, TEST_CHV1, WRONG_PIN), ==,
                    GSM11_STATUS_CHV_BLOCKED);
    g_assert_cmphex(test_card_verify(&card, TEST_CHV1, PIN1), ==,
                    GSM11_STATUS_CHV_BLOCKED);

    memcpy(unblock, WRONG_PIN, GSM_SIM_FS_CHV_SIZE);
    memcpy(unblock + GSM_SIM_FS_CHV_SIZE, PIN2, GSM_SIM_FS_CHV_SIZE);
    gsm_sim_fs_unblock_chv(&card.fs, TEST_UNBLOCK_CHV1, unblock,
                           sizeof(unblock), &card.response);
    g_assert_cmphex(test_card_status(&card), ==, GSM11_STATUS_ACCESS_DENIED);
    test_card_select(&card, TEST_DF);
    g_assert_cmphex(card.response.data[TEST_DF_UNBLOCK_CHV1_STATUS], ==, 0x89);

    memcpy(unblock, PUK1, GSM_SIM_FS_CHV_SIZE);
    gsm_sim_fs_unblock_chv(&card.fs, TEST_UNBLOCK_CHV1, unblock,
                           sizeof(unblock), &card.response);
    g_assert_cmphex(test_card_status(&card), ==, GSM11_STATUS_SUCCESS);
    test_card_select(&card, TEST_DF);
    g_assert_cmphex(card.response.data[TEST_DF_CHV1_STATUS], ==, 0x83);
    g_assert_cmphex(card.response.data[TEST_DF_UNBLOCK_CHV1_STATUS], ==, 0x8A);
    test_card_select(&card, TEST_EF_PROTECTED);
    g_assert_cmphex(test_card_read(&card), ==, GSM11_STATUS_SUCCESS);
    g_assert_cmphex(test_card_verify(&card, TEST_CHV1, PIN2), ==,
                    GSM11_STATUS_SUCCESS);
    gsm_sim_fs_destroy(&card.fs);
}

static void test_chv1_disable_and_enable(void)
{
    TestCard card;

    test_card_init(&card, true);
    gsm_sim_fs_disable_chv(&card.fs, TEST_CHV1, PIN1, GSM_SIM_FS_CHV_SIZE,
                           &card.response);
    g_assert_cmphex(test_card_status(&card), ==, GSM11_STATUS_SUCCESS);
    test_card_select(&card, TEST_DF);
    g_assert_cmphex(card.response.data[TEST_DF_CHARACTERISTICS] & 0x80, ==,
                    0x80);
    g_assert_cmphex(test_card_verify(&card, TEST_CHV1, PIN1), ==,
                    GSM11_STATUS_CHV_CONTRADICTION);
    test_card_select(&card, TEST_EF_PROTECTED);
    g_assert_cmphex(test_card_read(&card), ==, GSM11_STATUS_SUCCESS);

    gsm_sim_fs_enable_chv(&card.fs, TEST_CHV1, WRONG_PIN, GSM_SIM_FS_CHV_SIZE,
                          &card.response);
    g_assert_cmphex(test_card_status(&card), ==, GSM11_STATUS_ACCESS_DENIED);
    gsm_sim_fs_enable_chv(&card.fs, TEST_CHV1, PIN1, GSM_SIM_FS_CHV_SIZE,
                          &card.response);
    g_assert_cmphex(test_card_status(&card), ==, GSM11_STATUS_SUCCESS);
    test_card_select(&card, TEST_DF);
    g_assert_cmphex(card.response.data[TEST_DF_CHARACTERISTICS] & 0x80, ==, 0);
    g_assert_cmphex(card.response.data[TEST_DF_CHV1_STATUS], ==, 0x83);
    gsm_sim_fs_destroy(&card.fs);
}

static void test_change_chv2(void)
{
    TestCard card;
    uint8_t change[2 * GSM_SIM_FS_CHV_SIZE];

    test_card_init(&card, false);
    memcpy(change, WRONG_PIN, GSM_SIM_FS_CHV_SIZE);
    memcpy(change + GSM_SIM_FS_CHV_SIZE, PIN1, GSM_SIM_FS_CHV_SIZE);
    gsm_sim_fs_change_chv(&card.fs, TEST_CHV2, change, sizeof(change),
                          &card.response);
    g_assert_cmphex(test_card_status(&card), ==, GSM11_STATUS_ACCESS_DENIED);

    memcpy(change, PIN2, GSM_SIM_FS_CHV_SIZE);
    gsm_sim_fs_change_chv(&card.fs, TEST_CHV2, change, sizeof(change),
                          &card.response);
    g_assert_cmphex(test_card_status(&card), ==, GSM11_STATUS_SUCCESS);
    g_assert_cmphex(test_card_verify(&card, TEST_CHV2, PIN2), ==,
                    GSM11_STATUS_ACCESS_DENIED);
    g_assert_cmphex(test_card_verify(&card, TEST_CHV2, PIN1), ==,
                    GSM11_STATUS_SUCCESS);
    gsm_sim_fs_destroy(&card.fs);
}

static void test_invalidate_and_rehabilitate(void)
{
    TestCard card;

    test_card_init(&card, false);
    test_card_select(&card, TEST_DF);
    gsm_sim_fs_invalidate(&card.fs, &card.response);
    g_assert_cmphex(test_card_status(&card), ==, GSM11_STATUS_NO_EF_SELECTED);
    test_card_select(&card, TEST_EF_PROTECTED);
    gsm_sim_fs_invalidate(&card.fs, &card.response);
    g_assert_cmphex(test_card_status(&card), ==, GSM11_STATUS_ACCESS_DENIED);

    g_assert_cmphex(test_card_verify(&card, TEST_CHV2, PIN2), ==,
                    GSM11_STATUS_SUCCESS);
    gsm_sim_fs_invalidate(&card.fs, &card.response);
    g_assert_cmphex(test_card_status(&card), ==, GSM11_STATUS_SUCCESS);
    test_card_select(&card, TEST_EF_PROTECTED);
    g_assert_cmphex(card.response.data[TEST_EF_FILE_STATUS], ==, 0x00);
    g_assert_cmphex(test_card_read(&card), ==,
                    GSM11_STATUS_INVALIDATION_CONTRADICTION);

    gsm_sim_fs_rehabilitate(&card.fs, &card.response);
    g_assert_cmphex(test_card_status(&card), ==, GSM11_STATUS_SUCCESS);
    test_card_select(&card, TEST_EF_PROTECTED);
    g_assert_cmphex(card.response.data[TEST_EF_FILE_STATUS], ==, 0x01);
    g_assert_cmphex(test_card_read(&card), ==, GSM11_STATUS_SUCCESS);
    gsm_sim_fs_destroy(&card.fs);
}

static void test_increase_indicator(void)
{
    TestCard card;

    test_card_init(&card, false);
    test_card_select(&card, TEST_DF);
    test_card_select(&card, TEST_EF_COUNTER);
    g_assert_cmphex(card.response.data[TEST_EF_INCREASE], ==, 0x40);
    test_card_select(&card, TEST_EF_PROTECTED);
    g_assert_cmphex(card.response.data[TEST_EF_INCREASE], ==, 0x00);
    gsm_sim_fs_destroy(&card.fs);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/smartcard/gsm-sim-fs/chv1-block-and-unblock",
                    test_chv1_block_and_unblock);
    g_test_add_func("/smartcard/gsm-sim-fs/chv1-disable-and-enable",
                    test_chv1_disable_and_enable);
    g_test_add_func("/smartcard/gsm-sim-fs/change-chv2", test_change_chv2);
    g_test_add_func("/smartcard/gsm-sim-fs/invalidate-and-rehabilitate",
                    test_invalidate_and_rehabilitate);
    g_test_add_func("/smartcard/gsm-sim-fs/increase-indicator",
                    test_increase_indicator);
    return g_test_run();
}
