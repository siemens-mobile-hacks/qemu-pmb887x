#include "qemu/osdep.h"

#include "hw/arm/pmb887x/fifo.h"

static void test_full_wrapped_read(void) {
	static const uint16_t FIRST[] = { 1, 2, 3 };
	static const uint16_t SECOND[] = { 4, 5, 6, 7, 8, 9, 10, 11 };
	pmb887x_fifo16_t fifo;
	uint16_t items[8];

	pmb887x_fifo16_init(&fifo, 8);
	pmb887x_fifo16_write(&fifo, FIRST, ARRAY_SIZE(FIRST));
	pmb887x_fifo16_read(&fifo, items, ARRAY_SIZE(FIRST));
	pmb887x_fifo16_write(&fifo, SECOND, ARRAY_SIZE(SECOND));
	g_assert_true(pmb887x_fifo_is_full(&fifo));

	pmb887x_fifo16_read(&fifo, items, ARRAY_SIZE(SECOND));
	g_assert_cmpmem(items, sizeof(items), SECOND, sizeof(SECOND));
	g_assert_true(pmb887x_fifo_is_empty(&fifo));
	g_free(fifo.buffer);
}

int main(int argc, char **argv) {
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/pmb887x/fifo/full-wrapped-read", test_full_wrapped_read);
	return g_test_run();
}
