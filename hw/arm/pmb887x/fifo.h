#pragma once

#include "qemu/osdep.h"

typedef struct {
	uint8_t *buffer;
	uint32_t cursor;
	uint32_t size;
} pmb887x_fifo_t;

void pmb887x_fifo_init(pmb887x_fifo_t *fifo, uint32_t size);
void pmb887x_fifo_reset(pmb887x_fifo_t *fifo);
void pmb887x_fifo_push(pmb887x_fifo_t *fifo, uint8_t byte);
void pmb887x_fifo_push_all(pmb887x_fifo_t *fifo, const uint8_t *src, uint32_t size);
uint8_t pmb887x_fifo_pop(pmb887x_fifo_t *fifo);
void pmb887x_fifo_cut(pmb887x_fifo_t *fifo, uint32_t size);

static inline bool pmb887x_fifo_is_full(pmb887x_fifo_t *fifo) {
	return fifo->cursor >= fifo->size;
}

static inline bool pmb887x_fifo_is_empty(pmb887x_fifo_t *fifo) {
	return !fifo->cursor;
}

static inline uint32_t pmb887x_fifo_count(pmb887x_fifo_t *fifo) {
	return fifo->cursor;
}

static inline uint32_t pmb887x_fifo_free_count(pmb887x_fifo_t *fifo) {
	return fifo->size - fifo->cursor;
}

inline static const uint8_t *pmb887x_fifo_data(pmb887x_fifo_t *fifo) {
	return fifo->buffer;
}
