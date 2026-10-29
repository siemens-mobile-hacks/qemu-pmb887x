#include "qemu/osdep.h"

#include "hw/arm/pmb887x/audio/buffer.h"

void pmb887x_audio_buffer_reset(pmb887x_audio_buffer_t *buffer) {
	buffer->length = 0;
	buffer->position = 0;
}

const uint8_t *pmb887x_audio_buffer_peek(
	pmb887x_audio_buffer_t *buffer,
	Fifo8 *fifo,
	QemuMutex *lock,
	size_t requested,
	int64_t now_ns,
	size_t *available
) {
	bool streaming = false;

	g_assert(requested > 0);

	requested = MIN(requested, sizeof(buffer->data));
	if (buffer->position == buffer->length) {
		qemu_mutex_lock(lock);
		buffer->length = fifo8_pop_buf(fifo, buffer->data,
			MIN(requested, (size_t) fifo8_num_used(fifo)));
		streaming = now_ns - buffer->last_push_ns < PMB887X_AUDIO_BUFFER_IDLE_NS;
		qemu_mutex_unlock(lock);
		buffer->position = 0;
	}
	if (buffer->length == 0 && !streaming) {
		memset(buffer->data, 0, requested);
		buffer->length = requested;
	}

	*available = MIN(requested, buffer->length - buffer->position);
	return buffer->data + buffer->position;
}

void pmb887x_audio_buffer_note_push(pmb887x_audio_buffer_t *buffer, int64_t now_ns) {
	buffer->last_push_ns = now_ns;
}

void pmb887x_audio_buffer_consume(pmb887x_audio_buffer_t *buffer, size_t consumed) {
	g_assert(consumed <= buffer->length - buffer->position);
	buffer->position += consumed;
}
