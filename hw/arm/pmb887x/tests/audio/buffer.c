#include "qemu/osdep.h"

#include "migration/vmstate.h"
#include "hw/arm/pmb887x/audio/buffer.h"

const VMStateInfo vmstate_info_uint32;
const VMStateInfo vmstate_info_buffer;

static void test_short_write(void) {
	static const uint8_t FIRST[] = { 1, 2, 3, 4 };
	static const uint8_t SECOND[] = { 5, 6 };
	static const uint8_t FIRST_TAIL[] = { 3, 4 };
	pmb887x_audio_buffer_t buffer = {};
	QemuMutex lock;
	Fifo8 fifo;
	size_t size;
	const uint8_t *data;

	qemu_mutex_init(&lock);
	fifo8_create(&fifo, 8);
	fifo8_push_all(&fifo, FIRST, sizeof(FIRST));

	data = pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, sizeof(FIRST), &size);
	g_assert_cmpuint(size, ==, sizeof(FIRST));
	g_assert_cmpmem(data, size, FIRST, sizeof(FIRST));
	pmb887x_audio_buffer_consume(&buffer, 2);
	fifo8_push_all(&fifo, SECOND, sizeof(SECOND));

	data = pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, sizeof(FIRST), &size);
	g_assert_cmpuint(size, ==, sizeof(FIRST_TAIL));
	g_assert_cmpmem(data, size, FIRST_TAIL, sizeof(FIRST_TAIL));
	pmb887x_audio_buffer_consume(&buffer, size);

	data = pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, sizeof(FIRST), &size);
	g_assert_cmpuint(size, ==, sizeof(SECOND));
	g_assert_cmpmem(data, size, SECOND, sizeof(SECOND));
	pmb887x_audio_buffer_consume(&buffer, size);

	pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, sizeof(FIRST), &size);
	g_assert_cmpuint(size, ==, 0);

	fifo8_destroy(&fifo);
	qemu_mutex_destroy(&lock);
}

static void test_prefill(void) {
	static const uint8_t SAMPLES[] = { 1, 2, 3, 4, 5, 6 };
	pmb887x_audio_buffer_t buffer = {};
	QemuMutex lock;
	Fifo8 fifo;
	size_t size;

	qemu_mutex_init(&lock);
	fifo8_create(&fifo, 8);
	/* 2-byte frames at 50 Hz: 60 ms is three frames, six bytes. */
	pmb887x_audio_buffer_configure(&buffer, 2, 50);
	fifo8_push_all(&fifo, SAMPLES, 4);

	pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, sizeof(SAMPLES), &size);
	g_assert_cmpuint(size, ==, 0);
	fifo8_push_all(&fifo, SAMPLES + 4, 2);
	const uint8_t *data = pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, sizeof(SAMPLES), &size);
	g_assert_cmpmem(data, size, SAMPLES, sizeof(SAMPLES));
	pmb887x_audio_buffer_consume(&buffer, size);

	fifo8_push_all(&fifo, SAMPLES, 2);
	pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, sizeof(SAMPLES), &size);
	g_assert_cmpuint(size, ==, 2);
	pmb887x_audio_buffer_consume(&buffer, size);
	pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, sizeof(SAMPLES), &size);
	g_assert_cmpuint(size, ==, 0);
	fifo8_push_all(&fifo, SAMPLES, 2);
	pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, sizeof(SAMPLES), &size);
	g_assert_cmpuint(size, ==, 0);

	fifo8_destroy(&fifo);
	qemu_mutex_destroy(&lock);
}

int main(int argc, char **argv) {
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/pmb887x/audio-buffer/short-write", test_short_write);
	g_test_add_func("/pmb887x/audio-buffer/prefill", test_prefill);
	return g_test_run();
}
