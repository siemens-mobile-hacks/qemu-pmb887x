#pragma once

#include "qemu/osdep.h"

typedef struct {
	uint32_t total;
	uint32_t read;
	uint32_t write;
	uint32_t count;
} pmb887x_fifo_base_t;

#define pmb887x_fifo_is_full(_fifo)		pmb887x_fifo_base_is_full(&(_fifo)->base)
#define pmb887x_fifo_is_empty(_fifo)	pmb887x_fifo_base_is_empty(&(_fifo)->base)
#define pmb887x_fifo_count(_fifo)		pmb887x_fifo_base_count(&(_fifo)->base)
#define pmb887x_fifo_free_count(_fifo)	pmb887x_fifo_base_free_count(&(_fifo)->base)
#define pmb887x_fifo_reset(_fifo)		pmb887x_fifo_base_reset(&(_fifo)->base)

/*
 * Base
 * */
void pmb887x_fifo_base_write(pmb887x_fifo_base_t *fifo, void *buffer, const void *items, uint32_t items_count, uint32_t item_size);
void pmb887x_fifo_base_read(pmb887x_fifo_base_t *fifo, void *buffer, void *items, uint32_t items_count, uint32_t item_size);

static inline bool pmb887x_fifo_base_is_full(pmb887x_fifo_base_t *fifo) {
	return fifo->count >= fifo->total;
}

static inline bool pmb887x_fifo_base_is_empty(pmb887x_fifo_base_t *fifo) {
	return !fifo->count;
}

static inline uint32_t pmb887x_fifo_base_count(pmb887x_fifo_base_t *fifo) {
	return fifo->count;
}

static inline uint32_t pmb887x_fifo_base_free_count(pmb887x_fifo_base_t *fifo) {
	return fifo->total - fifo->count;
}

static inline void pmb887x_fifo_base_push(pmb887x_fifo_base_t *fifo) {
	g_assert(fifo->count < fifo->total);
	fifo->write = (fifo->write + 1) % fifo->total;
	fifo->count++;
}

static inline void pmb887x_fifo_base_pop(pmb887x_fifo_base_t *fifo) {
	g_assert(fifo->count > 0);
	fifo->read = (fifo->read + 1) % fifo->total;
	fifo->count--;
}

static inline void pmb887x_fifo_base_reset(pmb887x_fifo_base_t *fifo) {
	fifo->read = 0;
	fifo->write = 0;
	fifo->count = 0;
}

/*
 * FIFO8
 * */
typedef struct {
	pmb887x_fifo_base_t base;
	uint8_t *buffer;
} pmb887x_fifo8_t;

static inline void pmb887x_fifo8_init(pmb887x_fifo8_t *fifo, uint32_t total) {
	fifo->buffer = g_new0(uint8_t, total);
	fifo->base.total = total;
	pmb887x_fifo_base_reset(&fifo->base);
}

static inline void pmb887x_fifo8_push(pmb887x_fifo8_t *fifo, uint8_t value) {
	fifo->buffer[fifo->base.write] = value;
	pmb887x_fifo_base_push(&fifo->base);
}

static inline uint8_t pmb887x_fifo8_pop(pmb887x_fifo8_t *fifo) {
	uint8_t value = fifo->buffer[fifo->base.read];
	pmb887x_fifo_base_pop(&fifo->base);
	return value;
}

static inline void pmb887x_fifo8_write(pmb887x_fifo8_t *fifo, const uint8_t *items, uint32_t items_count) {
	pmb887x_fifo_base_write(&fifo->base, fifo->buffer, items, items_count, sizeof(uint8_t));
}

static inline void pmb887x_fifo8_read(pmb887x_fifo8_t *fifo, uint8_t *items, uint8_t items_count) {
	pmb887x_fifo_base_read(&fifo->base, fifo->buffer, items, items_count, sizeof(uint8_t));
}

static inline void pmb887x_fifo8_free(pmb887x_fifo8_t *fifo) {
	g_free(fifo->buffer);
	fifo->buffer = NULL;
}

/*
 * FIFO16
 * */
typedef struct {
	pmb887x_fifo_base_t base;
	uint16_t *buffer;
} pmb887x_fifo16_t;

static inline void pmb887x_fifo16_init(pmb887x_fifo16_t *fifo, uint32_t total) {
	fifo->buffer = g_new0(uint16_t, total);
	fifo->base.total = total;
	pmb887x_fifo_base_reset(&fifo->base);
}

static inline void pmb887x_fifo16_push(pmb887x_fifo16_t *fifo, uint16_t value) {
	fifo->buffer[fifo->base.write] = value;
	pmb887x_fifo_base_push(&fifo->base);
}

static inline uint16_t pmb887x_fifo16_pop(pmb887x_fifo16_t *fifo) {
	uint16_t value = fifo->buffer[fifo->base.read];
	pmb887x_fifo_base_pop(&fifo->base);
	return value;
}

static inline void pmb887x_fifo16_write(pmb887x_fifo16_t *fifo, const uint16_t *items, uint32_t items_count) {
	pmb887x_fifo_base_write(&fifo->base, fifo->buffer, items, items_count, sizeof(uint16_t));
}

static inline void pmb887x_fifo16_read(pmb887x_fifo16_t *fifo, uint16_t *items, uint16_t items_count) {
	pmb887x_fifo_base_read(&fifo->base, fifo->buffer, items, items_count, sizeof(uint16_t));
}

static inline void pmb887x_fifo16_free(pmb887x_fifo16_t *fifo) {
	g_free(fifo->buffer);
	fifo->buffer = NULL;
}

/*
 * FIFO32
 * */
typedef struct {
	pmb887x_fifo_base_t base;
	uint32_t *buffer;
} pmb887x_fifo32_t;

static inline void pmb887x_fifo32_init(pmb887x_fifo32_t *fifo, uint32_t total) {
	fifo->buffer = g_new0(uint32_t, total);
	fifo->base.total = total;
	pmb887x_fifo_base_reset(&fifo->base);
}

static inline void pmb887x_fifo32_push(pmb887x_fifo32_t *fifo, uint32_t value) {
	fifo->buffer[fifo->base.write] = value;
	pmb887x_fifo_base_push(&fifo->base);
}

static inline uint32_t pmb887x_fifo32_pop(pmb887x_fifo32_t *fifo) {
	uint32_t value = fifo->buffer[fifo->base.read];
	pmb887x_fifo_base_pop(&fifo->base);
	return value;
}

static inline void pmb887x_fifo32_write(pmb887x_fifo32_t *fifo, const uint32_t *items, uint32_t items_count) {
	pmb887x_fifo_base_write(&fifo->base, fifo->buffer, items, items_count, sizeof(uint32_t));
}

static inline void pmb887x_fifo32_read(pmb887x_fifo32_t *fifo, uint32_t *items, uint32_t items_count) {
	pmb887x_fifo_base_read(&fifo->base, fifo->buffer, items, items_count, sizeof(uint32_t));
}

static inline void pmb887x_fifo32_free(pmb887x_fifo32_t *fifo) {
	g_free(fifo->buffer);
	fifo->buffer = NULL;
}
