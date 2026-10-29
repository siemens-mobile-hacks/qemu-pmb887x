#pragma once

#include "qemu/fifo8.h"
#include "qemu/thread.h"

#define PMB887X_AUDIO_BUFFER_CAPACITY 512
/*
 * A producer that pushed this recently is mid-stream: an empty FIFO is the
 * guest falling behind real time, not the end of a sound, so it is left as a
 * gap for the backend to absorb instead of being padded into the stream.
 */
#define PMB887X_AUDIO_BUFFER_IDLE_NS (200 * 1000 * 1000)

typedef struct pmb887x_audio_buffer_t pmb887x_audio_buffer_t;

struct pmb887x_audio_buffer_t {
	uint8_t data[PMB887X_AUDIO_BUFFER_CAPACITY];
	size_t length;
	size_t position;
	/* Guarded by the FIFO's lock. */
	int64_t last_push_ns;
};

void pmb887x_audio_buffer_reset(pmb887x_audio_buffer_t *buffer);
const uint8_t *pmb887x_audio_buffer_peek(
	pmb887x_audio_buffer_t *buffer,
	Fifo8 *fifo,
	QemuMutex *lock,
	size_t requested,
	int64_t now_ns,
	size_t *available
);
void pmb887x_audio_buffer_note_push(pmb887x_audio_buffer_t *buffer, int64_t now_ns);
void pmb887x_audio_buffer_consume(pmb887x_audio_buffer_t *buffer, size_t consumed);
