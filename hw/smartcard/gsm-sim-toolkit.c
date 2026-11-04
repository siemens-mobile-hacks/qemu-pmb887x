/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/smartcard/gsm11.h"
#include "gsm-sim-toolkit.h"

#define GSM_SIM_TOOLKIT_PROACTIVE_COMMAND_TAG 0xD0
#define GSM_SIM_TOOLKIT_MENU_SELECTION_TAG 0xD3
#define GSM_SIM_TOOLKIT_COMMAND_DETAILS_TAG 0x81
#define GSM_SIM_TOOLKIT_DEVICE_IDENTITIES_TAG 0x82
#define GSM_SIM_TOOLKIT_ALPHA_IDENTIFIER_TAG 0x85
#define GSM_SIM_TOOLKIT_TEXT_STRING_TAG 0x8D
#define GSM_SIM_TOOLKIT_ITEM_TAG 0x8F
#define GSM_SIM_TOOLKIT_ITEM_IDENTIFIER_TAG 0x10
#define GSM_SIM_TOOLKIT_COMPREHENSION_REQUIRED 0x80
#define GSM_SIM_TOOLKIT_SET_UP_MENU 0x25
#define GSM_SIM_TOOLKIT_DISPLAY_TEXT 0x21
#define GSM_SIM_TOOLKIT_WAIT_FOR_USER 0x80
#define GSM_SIM_TOOLKIT_DEVICE_SIM 0x81
#define GSM_SIM_TOOLKIT_DEVICE_ME 0x82
#define GSM_SIM_TOOLKIT_DEVICE_DISPLAY 0x02
#define GSM_SIM_TOOLKIT_DCS_8BIT 0x04
#define GSM_SIM_TOOLKIT_PROACTIVE_COMMAND_SW1 0x91
#define GSM_SIM_TOOLKIT_LONG_LENGTH 0x81
#define GSM_SIM_TOOLKIT_SHORT_LENGTH_MAX 0x7F
#define GSM_SIM_TOOLKIT_MENU_TITLE "Virtual SIM"
#define GSM_SIM_TOOLKIT_INFO_TITLE "Info"

typedef struct GSMSimToolkitWriter {
    uint8_t data[GSM_SIM_TOOLKIT_COMMAND_MAX_SIZE];
    size_t size;
} GSMSimToolkitWriter;

static void gsm_sim_toolkit_write_length(uint8_t *data, size_t *offset,
                                         size_t length)
{
    if (length > GSM_SIM_TOOLKIT_SHORT_LENGTH_MAX) {
        data[(*offset)++] = GSM_SIM_TOOLKIT_LONG_LENGTH;
    }
    data[(*offset)++] = length;
}

static void gsm_sim_toolkit_write(GSMSimToolkitWriter *writer, uint8_t tag,
                                  const uint8_t *value, size_t length)
{
    writer->data[writer->size++] = tag;
    gsm_sim_toolkit_write_length(writer->data, &writer->size, length);
    memcpy(writer->data + writer->size, value, length);
    writer->size += length;
}

static void gsm_sim_toolkit_write_text(GSMSimToolkitWriter *writer,
                                       uint8_t tag, uint8_t prefix,
                                       const char *text)
{
    size_t length = strlen(text);

    writer->data[writer->size++] = tag;
    gsm_sim_toolkit_write_length(writer->data, &writer->size, 1 + length);
    writer->data[writer->size++] = prefix;
    memcpy(writer->data + writer->size, text, length);
    writer->size += length;
}

static void gsm_sim_toolkit_start(GSMSimToolkit *toolkit,
                                  GSMSimToolkitWriter *writer, uint8_t type,
                                  uint8_t qualifier, uint8_t destination)
{
    const uint8_t details[] = { ++toolkit->command_number, type, qualifier };
    const uint8_t devices[] = { GSM_SIM_TOOLKIT_DEVICE_SIM, destination };

    writer->size = 0;
    gsm_sim_toolkit_write(writer, GSM_SIM_TOOLKIT_COMMAND_DETAILS_TAG,
                          details, sizeof(details));
    gsm_sim_toolkit_write(writer, GSM_SIM_TOOLKIT_DEVICE_IDENTITIES_TAG,
                          devices, sizeof(devices));
}

static void gsm_sim_toolkit_queue(GSMSimToolkit *toolkit,
                                  const GSMSimToolkitWriter *writer)
{
    size_t size = 0;

    toolkit->command[size++] = GSM_SIM_TOOLKIT_PROACTIVE_COMMAND_TAG;
    gsm_sim_toolkit_write_length(toolkit->command, &size, writer->size);
    g_assert(size + writer->size <= GSM_SIM_TOOLKIT_COMMAND_MAX_SIZE);
    memcpy(toolkit->command + size, writer->data, writer->size);
    toolkit->command_size = size + writer->size;
    toolkit->command_pending = true;
}

static void gsm_sim_toolkit_queue_menu(GSMSimToolkit *toolkit)
{
    GSMSimToolkitWriter writer;

    gsm_sim_toolkit_start(toolkit, &writer, GSM_SIM_TOOLKIT_SET_UP_MENU, 0,
                          GSM_SIM_TOOLKIT_DEVICE_ME);
    gsm_sim_toolkit_write(&writer, GSM_SIM_TOOLKIT_ALPHA_IDENTIFIER_TAG,
                          (const uint8_t *)GSM_SIM_TOOLKIT_MENU_TITLE,
                          strlen(GSM_SIM_TOOLKIT_MENU_TITLE));
    gsm_sim_toolkit_write_text(&writer, GSM_SIM_TOOLKIT_ITEM_TAG,
                               GSM_SIM_TOOLKIT_INFO_ITEM,
                               GSM_SIM_TOOLKIT_INFO_TITLE);
    gsm_sim_toolkit_queue(toolkit, &writer);
}

void gsm_sim_toolkit_display_text(GSMSimToolkit *toolkit, const char *text)
{
    GSMSimToolkitWriter writer;

    gsm_sim_toolkit_start(toolkit, &writer, GSM_SIM_TOOLKIT_DISPLAY_TEXT,
                          GSM_SIM_TOOLKIT_WAIT_FOR_USER,
                          GSM_SIM_TOOLKIT_DEVICE_DISPLAY);
    gsm_sim_toolkit_write_text(&writer, GSM_SIM_TOOLKIT_TEXT_STRING_TAG,
                               GSM_SIM_TOOLKIT_DCS_8BIT, text);
    gsm_sim_toolkit_queue(toolkit, &writer);
}

static void gsm_sim_toolkit_set_status(ISO7816Response *response,
                                       uint16_t status)
{
    response->size = 0;
    iso7816_response_set_status(response, status);
}

void gsm_sim_toolkit_reset(GSMSimToolkit *toolkit)
{
    toolkit->command_size = 0;
    toolkit->command_pending = false;
    toolkit->command_number = 0;
}

void gsm_sim_toolkit_terminal_profile(GSMSimToolkit *toolkit,
                                      ISO7816Response *response)
{
    gsm_sim_toolkit_queue_menu(toolkit);
    gsm_sim_toolkit_set_status(response, GSM11_STATUS_SUCCESS);
}

void gsm_sim_toolkit_fetch(GSMSimToolkit *toolkit, uint8_t length,
                           ISO7816Response *response)
{
    if (!toolkit->command_pending) {
        gsm_sim_toolkit_set_status(response, GSM11_STATUS_TECHNICAL_PROBLEM);
        return;
    }
    if (length != toolkit->command_size) {
        gsm_sim_toolkit_set_status(response, GSM11_STATUS_CORRECT_LENGTH |
                                             toolkit->command_size);
        return;
    }
    memcpy(response->data, toolkit->command, toolkit->command_size);
    response->size = toolkit->command_size;
    toolkit->command_pending = false;
    iso7816_response_set_status(response, GSM11_STATUS_SUCCESS);
}

static const uint8_t *gsm_sim_toolkit_find(const uint8_t *data,
                                           uint16_t length, uint8_t tag)
{
    uint16_t offset = 0;

    while (offset + 2 <= length) {
        uint8_t current =
            data[offset] & ~GSM_SIM_TOOLKIT_COMPREHENSION_REQUIRED;
        uint8_t size = data[offset + 1];

        if (offset + 2 + size > length) {
            return NULL;
        }
        if (current == tag) {
            return data + offset;
        }
        offset += 2 + size;
    }
    return NULL;
}

uint8_t gsm_sim_toolkit_envelope(const uint8_t *data, uint16_t length,
                                 ISO7816Response *response)
{
    const uint8_t *item;

    gsm_sim_toolkit_set_status(response, GSM11_STATUS_SUCCESS);
    if (length < 2 || data[0] != GSM_SIM_TOOLKIT_MENU_SELECTION_TAG ||
        data[1] > length - 2) {
        return 0;
    }
    item = gsm_sim_toolkit_find(data + 2, data[1],
                                GSM_SIM_TOOLKIT_ITEM_IDENTIFIER_TAG);
    return item && item[1] == 1 ? item[2] : 0;
}

void gsm_sim_toolkit_announce(const GSMSimToolkit *toolkit,
                              ISO7816Response *response)
{
    bool success = response->sw1 == (GSM11_STATUS_SUCCESS >> 8) &&
                   response->sw2 == 0;

    if (success && toolkit->command_pending) {
        response->sw1 = GSM_SIM_TOOLKIT_PROACTIVE_COMMAND_SW1;
        response->sw2 = toolkit->command_size;
    }
}
