#include "hw/arm/pmb887x/fifo.h"

void pmb887x_fifo_base_write(pmb887x_fifo_base_t *fifo, void *buffer, const void *items, uint32_t items_count, uint32_t item_size) {
	g_assert(fifo->count + items_count <= fifo->total);
	
	uint32_t free_after = fifo->write >= fifo->read ? fifo->total - fifo->write : fifo->read - fifo->write;
	
	if (free_after > 0) {
		uint32_t chunk = MIN(items_count, free_after);
		memcpy(buffer + (fifo->write * item_size), items, chunk * item_size);
		fifo->write = (fifo->write + chunk) % fifo->total;
		fifo->count += chunk;
		items_count -= chunk;
		items = items + chunk * item_size;
	}
	
	if (items_count > 0) {
		memcpy(buffer + (fifo->write * item_size), items, items_count * item_size);
		fifo->write = (fifo->write + items_count) % fifo->total;
		fifo->count += items_count;
	}
}

void pmb887x_fifo_base_read(pmb887x_fifo_base_t *fifo, void *buffer, void *items, uint32_t items_count, uint32_t item_size) {
	g_assert(items_count <= fifo->count);
	
	uint32_t read_after = fifo->write >= fifo->read ? fifo->write - fifo->read : fifo->total - fifo->read;
	
	if (read_after > 0) {
		uint32_t chunk = MIN(items_count, read_after);
		memcpy(items, buffer + (fifo->read * item_size), chunk * item_size);
		fifo->read = (fifo->read + chunk) % fifo->total;
		fifo->count -= chunk;
		items_count -= chunk;
		items = items + chunk * item_size;
	}
	
	if (items_count > 0) {
		memcpy(items, buffer + (fifo->read * item_size), items_count * item_size);
		fifo->read = (fifo->read + items_count) % fifo->total;
		fifo->count -= items_count;
	}
}
