/*
 * Generic FIFO32 component.
 *
 * Copyright (c) 2016 Jean-Christophe Dubois
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef FIFO32_H
#define FIFO32_H

/* Existing users get Fifo8 through this header */
#include "qemu/fifo8.h"

typedef struct {
    /* All fields are private */
    uint32_t *data;
    uint32_t capacity;
    uint32_t head;
    uint32_t num;
} Fifo32;

/**
 * fifo32_create:
 * @fifo: struct Fifo32 to initialise with new FIFO
 * @capacity: capacity of the newly created FIFO expressed in 32 bit words
 *
 * Create a FIFO of the specified size. Clients should call fifo32_destroy()
 * when finished using the fifo. The FIFO is initially empty.
 */

static inline void fifo32_create(Fifo32 *fifo, uint32_t capacity)
{
    fifo->data = g_new(uint32_t, capacity);
    fifo->capacity = capacity;
    fifo->head = 0;
    fifo->num = 0;
}

/**
 * fifo32_destroy:
 * @fifo: FIFO to cleanup
 *
 * Cleanup a FIFO created with fifo32_create(). Frees memory created for FIFO
 * storage. The FIFO is no longer usable after this has been called.
 */

static inline void fifo32_destroy(Fifo32 *fifo)
{
    g_free(fifo->data);
}

/**
 * fifo32_num_free:
 * @fifo: FIFO to check
 *
 * Return the number of free uint32_t slots in the FIFO.
 *
 * Returns: Number of free 32 bit words.
 */

static inline uint32_t fifo32_num_free(const Fifo32 *fifo)
{
    return fifo->capacity - fifo->num;
}

/**
 * fifo32_num_used:
 * @fifo: FIFO to check
 *
 * Return the number of used uint32_t slots in the FIFO.
 *
 * Returns: Number of used 32 bit words.
 */

static inline uint32_t fifo32_num_used(const Fifo32 *fifo)
{
    return fifo->num;
}

static inline uint32_t fifo32_index(const Fifo32 *fifo, uint32_t pos)
{
    uint32_t index = fifo->head + pos;

    return index >= fifo->capacity ? index - fifo->capacity : index;
}

/**
 * fifo32_push:
 * @fifo: FIFO to push to
 * @data: 32 bits data word to push
 *
 * Push a 32 bits data word to the FIFO. Behaviour is undefined if the FIFO
 * is full. Clients are responsible for checking for fullness using
 * fifo32_is_full().
 */

static inline void fifo32_push(Fifo32 *fifo, uint32_t data)
{
    assert(fifo->num < fifo->capacity);
    fifo->data[fifo32_index(fifo, fifo->num)] = data;
    fifo->num++;
}

/**
 * fifo32_push_all:
 * @fifo: FIFO to push to
 * @data: data to push
 * @size: number of 32 bit words to push
 *
 * Push a 32 bit word array to the FIFO. Behaviour is undefined if the FIFO
 * is full. Clients are responsible for checking the space left in the FIFO
 * using fifo32_num_free().
 */

static inline void fifo32_push_all(Fifo32 *fifo, const uint32_t *data,
                                   uint32_t num)
{
    for (uint32_t i = 0; i < num; i++) {
        fifo32_push(fifo, data[i]);
    }
}

/**
 * fifo32_pop:
 * @fifo: fifo to pop from
 *
 * Pop a 32 bits data word from the FIFO. Behaviour is undefined if the FIFO
 * is empty. Clients are responsible for checking for emptiness using
 * fifo32_is_empty().
 *
 * Returns: The popped 32 bits data word.
 */

static inline uint32_t fifo32_pop(Fifo32 *fifo)
{
    uint32_t ret;

    assert(fifo->num > 0);
    ret = fifo->data[fifo->head];
    fifo->head = fifo32_index(fifo, 1);
    fifo->num--;
    return ret;
}

/**
 * fifo32_peek:
 * @fifo: fifo to peek from
 *
 * Return the oldest 32 bits data word without removing it. Behaviour is
 * undefined if the FIFO is empty.
 */

static inline uint32_t fifo32_peek(const Fifo32 *fifo)
{
    assert(fifo->num > 0);
    return fifo->data[fifo->head];
}

/**
 * fifo32_replace_last:
 * @fifo: FIFO to update
 * @data: 32 bits data word to store
 *
 * Overwrite the most recently pushed word. Behaviour is undefined if the
 * FIFO is empty.
 */

static inline void fifo32_replace_last(Fifo32 *fifo, uint32_t data)
{
    assert(fifo->num > 0);
    fifo->data[fifo32_index(fifo, fifo->num - 1)] = data;
}

/**
 * fifo32_reset:
 * @fifo: FIFO to reset
 *
 * Reset a FIFO. All data is discarded and the FIFO is emptied.
 */

static inline void fifo32_reset(Fifo32 *fifo)
{
    fifo->head = 0;
    fifo->num = 0;
}

/**
 * fifo32_is_empty:
 * @fifo: FIFO to check
 *
 * Check if a FIFO is empty.
 *
 * Returns: True if the fifo is empty, false otherwise.
 */

static inline bool fifo32_is_empty(const Fifo32 *fifo)
{
    return fifo->num == 0;
}

/**
 * fifo32_is_full:
 * @fifo: FIFO to check
 *
 * Check if a FIFO is full.
 *
 * Returns: True if the fifo is full, false otherwise.
 */

static inline bool fifo32_is_full(const Fifo32 *fifo)
{
    return fifo->num == fifo->capacity;
}

extern const VMStateDescription vmstate_fifo32;

#define VMSTATE_FIFO32(_field, _state) {                             \
    .name         = (stringify(_field)),                             \
    .size         = sizeof(Fifo32),                                  \
    .vmsd         = &vmstate_fifo32,                                 \
    .flags        = VMS_STRUCT,                                      \
    .offset       = vmstate_offset_value(_state, _field, Fifo32),    \
}

#endif /* FIFO32_H */
