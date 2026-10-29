#include "qemu/osdep.h"

#include "migration/vmstate.h"
#include "hw/arm/pmb887x/audio/buffer.h"

const VMStateInfo vmstate_info_uint32;
const VMStateInfo vmstate_info_buffer;

#define IDLE PMB887X_AUDIO_BUFFER_IDLE_NS

static void test_short_write(void) {
	static const uint8_t FIRST[] = { 1, 2, 3, 4 };
	static const uint8_t SECOND[] = { 5, 6 };
	static const uint8_t FIRST_TAIL[] = { 3, 4 };
	static const uint8_t SILENCE[] = { 0, 0, 0 };
	pmb887x_audio_buffer_t buffer = {};
	QemuMutex lock;
	Fifo8 fifo;
	size_t size;
	const uint8_t *data;

	qemu_mutex_init(&lock);
	fifo8_create(&fifo, 8);
	fifo8_push_all(&fifo, FIRST, sizeof(FIRST));

	data = pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, sizeof(FIRST), IDLE, &size);
	g_assert_cmpuint(size, ==, sizeof(FIRST));
	g_assert_cmpmem(data, size, FIRST, sizeof(FIRST));
	pmb887x_audio_buffer_consume(&buffer, 2);
	fifo8_push_all(&fifo, SECOND, sizeof(SECOND));

	data = pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, sizeof(FIRST), IDLE, &size);
	g_assert_cmpuint(size, ==, sizeof(FIRST_TAIL));
	g_assert_cmpmem(data, size, FIRST_TAIL, sizeof(FIRST_TAIL));
	pmb887x_audio_buffer_consume(&buffer, size);

	data = pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, sizeof(FIRST), IDLE, &size);
	g_assert_cmpuint(size, ==, sizeof(SECOND));
	g_assert_cmpmem(data, size, SECOND, sizeof(SECOND));
	pmb887x_audio_buffer_consume(&buffer, size);

	data = pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, sizeof(SILENCE), IDLE, &size);
	g_assert_cmpuint(size, ==, sizeof(SILENCE));
	g_assert_cmpmem(data, size, SILENCE, sizeof(SILENCE));

	fifo8_destroy(&fifo);
	qemu_mutex_destroy(&lock);
}

static void test_underrun_mid_stream(void) {
	static const uint8_t SAMPLES[] = { 1, 2 };
	static const uint8_t SILENCE[] = { 0, 0 };
	pmb887x_audio_buffer_t buffer = {};
	QemuMutex lock;
	Fifo8 fifo;
	size_t size;
	const uint8_t *data;

	qemu_mutex_init(&lock);
	fifo8_create(&fifo, 8);
	fifo8_push_all(&fifo, SAMPLES, sizeof(SAMPLES));
	pmb887x_audio_buffer_note_push(&buffer, 1000);

	data = pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, 4, 1000, &size);
	g_assert_cmpmem(data, size, SAMPLES, sizeof(SAMPLES));
	pmb887x_audio_buffer_consume(&buffer, size);

	pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, 2, 1000 + IDLE - 1, &size);
	g_assert_cmpuint(size, ==, 0);

	data = pmb887x_audio_buffer_peek(&buffer, &fifo, &lock, 2, 1000 + IDLE, &size);
	g_assert_cmpmem(data, size, SILENCE, sizeof(SILENCE));

	fifo8_destroy(&fifo);
	qemu_mutex_destroy(&lock);
}

int main(int argc, char **argv) {
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/pmb887x/audio-buffer/short-write", test_short_write);
	g_test_add_func("/pmb887x/audio-buffer/underrun-mid-stream", test_underrun_mid_stream);
	return g_test_run();
}
