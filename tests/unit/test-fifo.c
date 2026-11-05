/*
 * Fifo8 and Fifo32 tests
 *
 * Copyright 2024 Mark Cave-Ayland
 *
 * Authors:
 *  Mark Cave-Ayland    <mark.cave-ayland@ilande.co.uk>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qemu/fifo8.h"
#include "qemu/fifo32.h"

const VMStateInfo vmstate_info_uint32;
const VMStateInfo vmstate_info_buffer;


static void test_fifo8_pop_bufptr_wrap(void)
{
    Fifo8 fifo;
    uint8_t data_in1[] = { 0x1, 0x2, 0x3, 0x4 };
    uint8_t data_in2[] = { 0x5, 0x6, 0x7, 0x8, 0x9, 0xa };
    const uint8_t *buf;
    uint32_t count;

    fifo8_create(&fifo, 8);
    /*
     *  head --v-- tail             used = 0
     * FIFO: [ . . . . . . . . ]
     */

    fifo8_push_all(&fifo, data_in1, sizeof(data_in1));
    /*
     *  head --v      ]-- tail      used = 4
     * FIFO: [ 1 2 3 4 . . . . ]
     */
    buf = fifo8_pop_bufptr(&fifo, 2, &count);
    /*
     *      head --v  ]-- tail      used = 2
     * FIFO: [ 1 2 3 4 . . . . ]
     *  buf  --^                    count = 2
     */
    g_assert(count == 2);
    g_assert(buf[0] == 0x1 && buf[1] == 0x2);

    fifo8_push_all(&fifo, data_in2, sizeof(data_in2));
    /*
     *     tail --]v-- head         used = 8
     * FIFO: [ 9 a 3 4 5 6 7 8 ]
     */
    buf = fifo8_pop_bufptr(&fifo, 8, &count);
    /*
     *  head --v  ]-- tail          used = 2
     * FIFO: [ 9 a 3 4 5 6 7 8 ]
     *  buf      --^                count = 6
     */
    g_assert(count == 6);
    g_assert(buf[0] == 0x3 && buf[1] == 0x4 && buf[2] == 0x5 &&
             buf[3] == 0x6 && buf[4] == 0x7 && buf[5] == 0x8);

    g_assert(fifo8_num_used(&fifo) == 2);
    fifo8_destroy(&fifo);
}

static void test_fifo8_pop_bufptr(void)
{
    Fifo8 fifo;
    uint8_t data_in[] = { 0x1, 0x2, 0x3, 0x4 };
    const uint8_t *buf;
    uint32_t count;

    fifo8_create(&fifo, 8);
    /*
     *  head --v-- tail             used = 0
     * FIFO: [ . . . . . . . . ]
     */

    fifo8_push_all(&fifo, data_in, sizeof(data_in));
    /*
     *  head --v      ]-- tail      used = 4
     * FIFO: [ 1 2 3 4 . . . . ]
     */
    buf = fifo8_pop_bufptr(&fifo, 2, &count);
    /*
     *      head --v  ]-- tail      used = 2
     * FIFO: [ 1 2 3 4 . . . . ]
     *  buf  --^                    count = 2
     */
    g_assert(count == 2);
    g_assert(buf[0] == 0x1 && buf[1] == 0x2);

    g_assert(fifo8_num_used(&fifo) == 2);
    fifo8_destroy(&fifo);
}

static void test_fifo8_peek_bufptr_wrap(void)
{
    Fifo8 fifo;
    uint8_t data_in1[] = { 0x1, 0x2, 0x3, 0x4 };
    uint8_t data_in2[] = { 0x5, 0x6, 0x7, 0x8, 0x9, 0xa };
    const uint8_t *buf;
    uint32_t count;

    fifo8_create(&fifo, 8);
    /*
     *  head --v-- tail             used = 0
     * FIFO: { . . . . . . . . }
     */

    fifo8_push_all(&fifo, data_in1, sizeof(data_in1));
    /*
     *  head --v      ]-- tail      used = 4
     * FIFO: { 1 2 3 4 . . . . }
     */
    buf = fifo8_peek_bufptr(&fifo, 2, &count);
    /*
     *  head --v      ]-- tail      used = 4
     * FIFO: { 1 2 3 4 . . . . }
     *  buf: [ 1 2 ]                count = 2
     */
    g_assert(count == 2);
    g_assert(buf[0] == 0x1 && buf[1] == 0x2);

    buf = fifo8_pop_bufptr(&fifo, 2, &count);
    /*
     *     head  --v  ]-- tail      used = 2
     * FIFO: { 1 2 3 4 . . . . }
     *  buf: [ 1 2 ]                count = 2
     */
    g_assert(count == 2);
    g_assert(buf[0] == 0x1 && buf[1] == 0x2);
    fifo8_push_all(&fifo, data_in2, sizeof(data_in2));
    /*
     *  tail   ---]v-- head         used = 8
     * FIFO: { 9 a 3 4 5 6 7 8 }
     */

    buf = fifo8_peek_bufptr(&fifo, 8, &count);
    /*
     *     tail --]v-- head         used = 8
     * FIFO: { 9 a 3 4 5 6 7 8 }
     *  buf:     [ 3 4 5 6 7 8 ]    count = 6
     */
    g_assert(count == 6);
    g_assert(buf[0] == 0x3 && buf[1] == 0x4 && buf[2] == 0x5 &&
             buf[3] == 0x6 && buf[4] == 0x7 && buf[5] == 0x8);

    g_assert(fifo8_num_used(&fifo) == 8);
    fifo8_destroy(&fifo);
}

static void test_fifo8_peek_bufptr(void)
{
    Fifo8 fifo;
    uint8_t data_in[] = { 0x1, 0x2, 0x3, 0x4 };
    const uint8_t *buf;
    uint32_t count;

    fifo8_create(&fifo, 8);
    /*
     *  head --v-- tail             used = 0
     * FIFO: { . . . . . . . . }
     */

    fifo8_push_all(&fifo, data_in, sizeof(data_in));
    /*
     *  head --v      ]-- tail      used = 4
     * FIFO: { 1 2 3 4 . . . . }
     */
    buf = fifo8_peek_bufptr(&fifo, 2, &count);
    /*
     *  head --v      ]-- tail      used = 4
     * FIFO: { 1 2 3 4 . . . . }
     *  buf: [ 1 2 ]                count = 2
     */
    g_assert(count == 2);
    g_assert(buf[0] == 0x1 && buf[1] == 0x2);

    g_assert(fifo8_num_used(&fifo) == 4);
    fifo8_destroy(&fifo);
}

static void test_fifo8_pop_buf_wrap(void)
{
    Fifo8 fifo;
    uint8_t data_in1[] = { 0x1, 0x2, 0x3, 0x4 };
    uint8_t data_in2[] = { 0x5, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc };
    uint8_t data_out[4];
    int count;

    fifo8_create(&fifo, 8);
    /*
     *  head --v-- tail             used = 0
     * FIFO: { . . . . . . . . }
     */

    fifo8_push_all(&fifo, data_in1, sizeof(data_in1));
    /*
     *  head --v      ]-- tail      used = 4
     * FIFO: { 1 2 3 4 . . . . }
     */
    fifo8_pop_buf(&fifo, NULL, 4);
    /*
     *         tail --]v-- head     used = 0
     * FIFO: [ 1 2 3 4 . . . . ]
     */

    fifo8_push_all(&fifo, data_in2, sizeof(data_in2));
    /*
     *         tail --]v-- head     used = 8
     * FIFO: { 9 a b c 5 6 7 8 }
     */
    count = fifo8_pop_buf(&fifo, NULL, 4);
    /*
     * head  --v      ]-- tail      used = 4
     * FIFO: { 9 a b c 5 6 7 8 }
     */
    g_assert(count == 4);
    count = fifo8_pop_buf(&fifo, data_out, 4);
    /*
     *         tail --]v-- head     used = 0
     * FIFO: { 9 a b c 5 6 7 8 }
     */
    g_assert(count == 4);
    g_assert(data_out[0] == 0x9 && data_out[1] == 0xa &&
             data_out[2] == 0xb && data_out[3] == 0xc);

    g_assert(fifo8_num_used(&fifo) == 0);
    fifo8_destroy(&fifo);
}

static void test_fifo8_pop_buf(void)
{
    Fifo8 fifo;
    uint8_t data_in[] = { 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8 };
    uint8_t data_out[] = { 0xff, 0xff, 0xff, 0xff };
    int count;

    fifo8_create(&fifo, 8);
    /*
     *  head --v-- tail             used = 0
     * FIFO: { . . . . . . . . }
     */

    fifo8_push_all(&fifo, data_in, sizeof(data_in));
    /*
     *  head --v      ]-- tail      used = 4
     * FIFO: { 1 2 3 4 . . . . }
     */
    count = fifo8_pop_buf(&fifo, NULL, 4);
    /*
     *         tail --]v-- head     used = 0
     * FIFO: { 1 2 3 4 . . . . }
     */
    g_assert(count == 4);
    count = fifo8_pop_buf(&fifo, data_out, 4);
    g_assert(data_out[0] == 0x5 && data_out[1] == 0x6 &&
             data_out[2] == 0x7 && data_out[3] == 0x8);

    g_assert(fifo8_num_used(&fifo) == 0);
    fifo8_destroy(&fifo);
}

static void test_fifo8_peek_buf_wrap(void)
{
    Fifo8 fifo;
    uint8_t data_in1[] = { 0x1, 0x2, 0x3, 0x4 };
    uint8_t data_in2[] = { 0x5, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc };
    uint8_t data_out[8];
    int count;

    fifo8_create(&fifo, 8);
    /*
     *  head --v-- tail             used = 0
     * FIFO: { . . . . . . . . }
     */

    fifo8_push_all(&fifo, data_in1, sizeof(data_in1));
    /*
     *  head --v      ]-- tail      used = 4
     * FIFO: { 1 2 3 4 . . . . }
     */
    fifo8_pop_buf(&fifo, NULL, 4);
    /*
     *         tail --]v-- head     used = 0
     * FIFO: { 1 2 3 4 . . . . }
     */

    fifo8_push_all(&fifo, data_in2, sizeof(data_in2));
    /*
     *         tail --]v-- head     used = 8
     * FIFO: { 9 a b c 5 6 7 8 }
     */
    count = fifo8_peek_buf(&fifo, NULL, 4);
    g_assert(count == 4);
    count = fifo8_peek_buf(&fifo, data_out, 4);
    /*
     *         tail --]v-- head     used = 8
     * FIFO: { 9 a b c 5 6 7 8 }
     *  buf:         [ 5 6 7 8 ]    count = 4
     */
    g_assert(count == 4);
    g_assert(data_out[0] == 0x5 && data_out[1] == 0x6 &&
             data_out[2] == 0x7 && data_out[3] == 0x8);

    count = fifo8_peek_buf(&fifo, data_out, 8);
    /*
     *         tail --]v-- head     used = 8
     * FIFO: { 9 a b c 5 6 7 8 }
     *  buf:         [ 5 6 7 8 9 a b c ] count = 8
     */
    g_assert(count == 8);
    g_assert(data_out[0] == 0x5 && data_out[1] == 0x6 &&
             data_out[2] == 0x7 && data_out[3] == 0x8);
    g_assert(data_out[4] == 0x9 && data_out[5] == 0xa &&
             data_out[6] == 0xb && data_out[7] == 0xc);

    g_assert(fifo8_num_used(&fifo) == 8);
    fifo8_destroy(&fifo);
}

static void test_fifo8_peek_buf(void)
{
    Fifo8 fifo;
    uint8_t data_in[] = { 0x1, 0x2, 0x3, 0x4 };
    uint8_t data_out[] = { 0xff, 0xff, 0xff, 0xff };
    int count;

    fifo8_create(&fifo, 8);
    /*
     *  head --v-- tail             used = 0
     * FIFO: { . . . . . . . . }
     */

    fifo8_push_all(&fifo, data_in, sizeof(data_in));
    /*
     *  head --v      ]-- tail      used = 4
     * FIFO: { 1 2 3 4 . . . . }
     */
    count = fifo8_peek_buf(&fifo, NULL, 4);
    g_assert(count == 4);

    g_assert(data_out[0] == 0xff && data_out[1] == 0xff &&
             data_out[2] == 0xff && data_out[3] == 0xff);
    count = fifo8_peek_buf(&fifo, data_out, 4);
    /*
     *  head --v      ]-- tail      used = 4
     * FIFO: { 1 2 3 4 . . . . }
     *  buf: [ 1 2 3 4 ]            count = 4
     */
    g_assert(count == 4);
    g_assert(data_out[0] == 0x1 && data_out[1] == 0x2 &&
             data_out[2] == 0x3 && data_out[3] == 0x4);

    g_assert(fifo8_num_used(&fifo) == 4);
    fifo8_destroy(&fifo);
}

static void test_fifo8_peek(void)
{
    Fifo8 fifo;
    uint8_t c;

    fifo8_create(&fifo, 8);
    /*
     *  head --v-- tail             used = 0
     * FIFO: { . . . . . . . . }
     */
    fifo8_push(&fifo, 0x1);
    /*
     *  head --v]-- tail            used = 1
     * FIFO: { 1 . . . . . . . }
     */
    fifo8_push(&fifo, 0x2);
    /*
     *  head --v  ]-- tail          used = 2
     * FIFO: { 1 2 . . . . . . }
     */

    c = fifo8_peek(&fifo);
    g_assert(c == 0x1);
    fifo8_pop(&fifo);
    /*
     *    head --v]-- tail          used = 1
     * FIFO: { 1 2 . . . . . . }
     */
    c = fifo8_peek(&fifo);
    g_assert(c == 0x2);

    g_assert(fifo8_num_used(&fifo) == 1);
    fifo8_destroy(&fifo);
}

static void test_fifo8_pushpop(void)
{
    Fifo8 fifo;
    uint8_t c;

    fifo8_create(&fifo, 8);
    /*
     *  head --v-- tail             used = 0
     * FIFO: { . . . . . . . . }
     */
    fifo8_push(&fifo, 0x1);
    /*
     *  head --v]-- tail            used = 1
     * FIFO: { 1 . . . . . . . }
     */
    fifo8_push(&fifo, 0x2);
    /*
     *  head --v  ]-- tail          used = 2
     * FIFO: { 1 2 . . . . . . }
     */

    c = fifo8_pop(&fifo);
    /*
     *    head --v]-- tail          used = 1
     * FIFO: { 1 2 . . . . . . }
     */
    g_assert(c == 0x1);
    c = fifo8_pop(&fifo);
    /*
     *     tail --]v-- head         used = 0
     * FIFO: { 1 2 . . . . . . }
     */
    g_assert(c == 0x2);

    g_assert(fifo8_num_used(&fifo) == 0);
    fifo8_destroy(&fifo);
}

static void test_fifo8_pushpop_wrap(void)
{
    Fifo8 fifo;

    fifo8_create(&fifo, 4);
    for (uint8_t i = 0; i < 3; i++) {
        fifo8_push(&fifo, i);
        g_assert_cmpuint(fifo8_pop(&fifo), ==, i);
    }
    for (uint8_t i = 0; i < 4; i++) {
        fifo8_push(&fifo, 0x10 + i);
    }
    g_assert_true(fifo8_is_full(&fifo));
    g_assert_cmpuint(fifo8_peek(&fifo), ==, 0x10);
    for (uint8_t i = 0; i < 4; i++) {
        g_assert_cmpuint(fifo8_pop(&fifo), ==, 0x10 + i);
    }
    g_assert_true(fifo8_is_empty(&fifo));
    fifo8_destroy(&fifo);
}

static void test_fifo32_full_wrapped(void)
{
    static const uint32_t first[] = { 1, 2, 3 };
    static const uint32_t second[] = {
        0x10000004, 5, 6, 7, 8, 9, 10, 0xffffffff
    };
    Fifo32 fifo;

    fifo32_create(&fifo, 8);
    fifo32_push_all(&fifo, first, ARRAY_SIZE(first));
    for (int i = 0; i < ARRAY_SIZE(first); i++) {
        g_assert_cmphex(fifo32_pop(&fifo), ==, first[i]);
    }
    fifo32_push_all(&fifo, second, ARRAY_SIZE(second));
    g_assert_true(fifo32_is_full(&fifo));
    g_assert_cmpuint(fifo32_num_used(&fifo), ==, 8);
    g_assert_cmpuint(fifo32_num_free(&fifo), ==, 0);

    for (int i = 0; i < ARRAY_SIZE(second); i++) {
        g_assert_cmphex(fifo32_pop(&fifo), ==, second[i]);
    }
    g_assert_true(fifo32_is_empty(&fifo));
    g_assert_cmpuint(fifo32_num_free(&fifo), ==, 8);
    fifo32_destroy(&fifo);
}

static void test_fifo32_peek(void)
{
    Fifo32 fifo;

    fifo32_create(&fifo, 2);
    fifo32_push(&fifo, 0x11111111);
    fifo32_push(&fifo, 0x22222222);
    g_assert_cmphex(fifo32_peek(&fifo), ==, 0x11111111);
    g_assert_cmpuint(fifo32_num_used(&fifo), ==, 2);
    g_assert_cmphex(fifo32_pop(&fifo), ==, 0x11111111);
    g_assert_cmphex(fifo32_peek(&fifo), ==, 0x22222222);
    fifo32_destroy(&fifo);
}

static void test_fifo32_replace_last(void)
{
    Fifo32 fifo;

    fifo32_create(&fifo, 3);
    fifo32_push(&fifo, 1);
    fifo32_push(&fifo, 2);
    fifo32_pop(&fifo);
    fifo32_pop(&fifo);
    /* head and tail are at the last slot, the next push wraps */
    fifo32_push(&fifo, 3);
    fifo32_push(&fifo, 4);
    fifo32_replace_last(&fifo, 5);
    g_assert_cmpuint(fifo32_num_used(&fifo), ==, 2);
    g_assert_cmphex(fifo32_pop(&fifo), ==, 3);
    fifo32_replace_last(&fifo, 6);
    g_assert_cmphex(fifo32_pop(&fifo), ==, 6);
    fifo32_push(&fifo, 7);
    fifo32_replace_last(&fifo, 8);
    g_assert_cmphex(fifo32_pop(&fifo), ==, 8);
    fifo32_destroy(&fifo);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/fifo8/pushpop", test_fifo8_pushpop);
    g_test_add_func("/fifo8/pushpop_wrap", test_fifo8_pushpop_wrap);
    g_test_add_func("/fifo8/peek", test_fifo8_peek);
    g_test_add_func("/fifo8/peek_buf", test_fifo8_peek_buf);
    g_test_add_func("/fifo8/peek_buf_wrap", test_fifo8_peek_buf_wrap);
    g_test_add_func("/fifo8/pop_buf", test_fifo8_pop_buf);
    g_test_add_func("/fifo8/pop_buf_wrap", test_fifo8_pop_buf_wrap);
    g_test_add_func("/fifo8/peek_bufptr", test_fifo8_peek_bufptr);
    g_test_add_func("/fifo8/peek_bufptr_wrap", test_fifo8_peek_bufptr_wrap);
    g_test_add_func("/fifo8/pop_bufptr", test_fifo8_pop_bufptr);
    g_test_add_func("/fifo8/pop_bufptr_wrap", test_fifo8_pop_bufptr_wrap);
    g_test_add_func("/fifo32/full_wrapped", test_fifo32_full_wrapped);
    g_test_add_func("/fifo32/peek", test_fifo32_peek);
    g_test_add_func("/fifo32/replace_last", test_fifo32_replace_last);
    return g_test_run();
}
