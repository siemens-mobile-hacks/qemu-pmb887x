#include "qemu/osdep.h"

#include "hw/arm/pmb887x/audio/buffer.h"

/* Covers DSP output that arrives in bursts of about 20 ms plus the 10 ms audio mixer period. */
#define AUDIO_BUFFER_PREFILL_MS	60

void pmb887x_audio_buffer_configure(pmb887x_audio_buffer_t *buffer, size_t frame_bytes, uint32_t rate_hz) {
	buffer->prefill_bytes = frame_bytes * rate_hz * AUDIO_BUFFER_PREFILL_MS / 1000;
	pmb887x_audio_buffer_reset(buffer);
}

void pmb887x_audio_buffer_reset(pmb887x_audio_buffer_t *buffer) {
	buffer->length = 0;
	buffer->position = 0;
	buffer->playing = false;
}

const uint8_t *pmb887x_audio_buffer_peek(
	pmb887x_audio_buffer_t *buffer,
	Fifo8 *fifo,
	QemuMutex *lock,
	size_t requested,
	size_t *available
) {
	g_assert(requested > 0);

	requested = MIN(requested, sizeof(buffer->data));
	if (buffer->position == buffer->length) {
		qemu_mutex_lock(lock);
		size_t queued = fifo8_num_used(fifo);
		buffer->playing = queued != 0 && (buffer->playing || queued >= buffer->prefill_bytes);
		buffer->length = buffer->playing ? fifo8_pop_buf(fifo, buffer->data, MIN(requested, queued)) : 0;
		qemu_mutex_unlock(lock);
		buffer->position = 0;
	}

	*available = MIN(requested, buffer->length - buffer->position);
	return buffer->data + buffer->position;
}

void pmb887x_audio_buffer_consume(pmb887x_audio_buffer_t *buffer, size_t consumed) {
	g_assert(consumed <= buffer->length - buffer->position);
	buffer->position += consumed;
}
