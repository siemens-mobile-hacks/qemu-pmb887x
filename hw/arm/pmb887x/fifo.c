#include "fifo.h"

void pmb887x_fifo_init(pmb887x_fifo_t *fifo, uint32_t size) {
	fifo->buffer = g_new0(uint8_t, size);
	fifo->cursor = 0;
	fifo->size = size;
}

void pmb887x_fifo_reset(pmb887x_fifo_t *fifo) {
	fifo->cursor = 0;
}

void pmb887x_fifo_push(pmb887x_fifo_t *fifo, uint8_t byte) {
	g_assert(fifo->cursor < fifo->size);
	fifo->buffer[fifo->cursor++] = byte;
}

void pmb887x_fifo_push_all(pmb887x_fifo_t *fifo, const uint8_t *src, uint32_t size) {
	g_assert(fifo->cursor + size <= fifo->size);
	memcpy(fifo->buffer + fifo->cursor, src, size);
	fifo->cursor += size;
}

void pmb887x_fifo_cut(pmb887x_fifo_t *fifo, uint32_t size) {
	g_assert(fifo->cursor >= size);
	
	if (fifo->cursor == size) {
		pmb887x_fifo_reset(fifo);
		return;
	}
	
	memmove(fifo->buffer, fifo->buffer + size, fifo->cursor);
	fifo->cursor -= size;
}

uint8_t pmb887x_fifo_pop(pmb887x_fifo_t *fifo) {
	g_assert(fifo->cursor > 0);
	
	uint8_t ret = fifo->buffer[0];
	memmove(fifo->buffer, fifo->buffer + 1, fifo->cursor);
	fifo->cursor--;
	return ret;
}
