/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/smartcard/gsm11.h"
#include "../gsm-sim-toolkit.h"

static const uint8_t SET_UP_MENU[] = {
    0xD0, 0x1D,
    0x81, 0x03, 0x01, 0x25, 0x00,
    0x82, 0x02, 0x81, 0x82,
    0x85, 0x0B, 'V', 'i', 'r', 't', 'u', 'a', 'l', ' ', 'S', 'I', 'M',
    0x8F, 0x05, 0x01, 'I', 'n', 'f', 'o',
};

static const uint8_t DISPLAY_TEXT[] = {
    0xD0, 0x10,
    0x81, 0x03, 0x02, 0x21, 0x80,
    0x82, 0x02, 0x81, 0x02,
    0x8D, 0x05, 0x04, 'I', 'M', 'S', 'I',
};

static const uint8_t LONG_DISPLAY_TEXT_HEADER[] = {
    0xD0, 0x81, 0x8D,
    0x81, 0x03, 0x01, 0x21, 0x80,
    0x82, 0x02, 0x81, 0x02,
    0x8D, 0x81, 0x81, 0x04,
};

static const uint8_t MENU_SELECTION[] = {
    0xD3, 0x07, 0x82, 0x02, 0x01, 0x81, 0x90, 0x01, 0x01,
};

static uint16_t test_status(const ISO7816Response *response)
{
    return (response->sw1 << 8) | response->sw2;
}

static void test_menu_session(void)
{
    GSMSimToolkit toolkit = { 0 };
    ISO7816Response response = { 0 };

    gsm_sim_toolkit_terminal_profile(&toolkit, &response);
    gsm_sim_toolkit_announce(&toolkit, &response);
    g_assert_cmphex(test_status(&response), ==, 0x9100 | sizeof(SET_UP_MENU));

    gsm_sim_toolkit_fetch(&toolkit, sizeof(SET_UP_MENU), &response);
    g_assert_cmphex(test_status(&response), ==, GSM11_STATUS_SUCCESS);
    g_assert_cmpmem(response.data, response.size, SET_UP_MENU,
                    sizeof(SET_UP_MENU));

    iso7816_response_set_status(&response, GSM11_STATUS_SUCCESS);
    gsm_sim_toolkit_announce(&toolkit, &response);
    g_assert_cmphex(test_status(&response), ==, GSM11_STATUS_SUCCESS);

    g_assert_cmpuint(gsm_sim_toolkit_envelope(MENU_SELECTION,
                                              sizeof(MENU_SELECTION),
                                              &response), ==,
                     GSM_SIM_TOOLKIT_INFO_ITEM);
    gsm_sim_toolkit_display_text(&toolkit, "IMSI");
    gsm_sim_toolkit_announce(&toolkit, &response);
    g_assert_cmphex(test_status(&response), ==, 0x9100 | sizeof(DISPLAY_TEXT));

    gsm_sim_toolkit_fetch(&toolkit, sizeof(DISPLAY_TEXT), &response);
    g_assert_cmphex(test_status(&response), ==, GSM11_STATUS_SUCCESS);
    g_assert_cmpmem(response.data, response.size, DISPLAY_TEXT,
                    sizeof(DISPLAY_TEXT));
}

static void test_long_display_text(void)
{
    GSMSimToolkit toolkit = { 0 };
    ISO7816Response response = { 0 };
    g_autofree char *text = g_strnfill(0x80, 'A');
    size_t size = sizeof(LONG_DISPLAY_TEXT_HEADER) + strlen(text);

    gsm_sim_toolkit_display_text(&toolkit, text);
    gsm_sim_toolkit_fetch(&toolkit, size, &response);
    g_assert_cmphex(test_status(&response), ==, GSM11_STATUS_SUCCESS);
    g_assert_cmpuint(response.size, ==, size);
    g_assert_cmpmem(response.data, sizeof(LONG_DISPLAY_TEXT_HEADER),
                    LONG_DISPLAY_TEXT_HEADER,
                    sizeof(LONG_DISPLAY_TEXT_HEADER));
}

static void test_fetch_errors(void)
{
    GSMSimToolkit toolkit = { 0 };
    ISO7816Response response = { 0 };

    gsm_sim_toolkit_fetch(&toolkit, 0x10, &response);
    g_assert_cmphex(test_status(&response), ==,
                    GSM11_STATUS_TECHNICAL_PROBLEM);

    gsm_sim_toolkit_terminal_profile(&toolkit, &response);
    gsm_sim_toolkit_fetch(&toolkit, 0x10, &response);
    g_assert_cmphex(test_status(&response), ==,
                    GSM11_STATUS_CORRECT_LENGTH | sizeof(SET_UP_MENU));
}

static void test_announce_keeps_other_statuses(void)
{
    GSMSimToolkit toolkit = { 0 };
    ISO7816Response response = { 0 };

    gsm_sim_toolkit_terminal_profile(&toolkit, &response);
    iso7816_response_set_status(&response,
                                GSM11_STATUS_RESPONSE_AVAILABLE | 0x0F);
    gsm_sim_toolkit_announce(&toolkit, &response);
    g_assert_cmphex(test_status(&response), ==, 0x9F0F);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/smartcard/gsm-sim-toolkit/menu-session",
                    test_menu_session);
    g_test_add_func("/smartcard/gsm-sim-toolkit/long-display-text",
                    test_long_display_text);
    g_test_add_func("/smartcard/gsm-sim-toolkit/fetch-errors",
                    test_fetch_errors);
    g_test_add_func("/smartcard/gsm-sim-toolkit/announce-keeps-other-statuses",
                    test_announce_keeps_other_statuses);
    return g_test_run();
}
