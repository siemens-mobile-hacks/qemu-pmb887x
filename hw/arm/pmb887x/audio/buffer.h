#pragma once

#include "qemu/fifo8.h"
#include "qemu/thread.h"

#define PMB887X_AUDIO_BUFFER_CAPACITY 512

typedef struct pmb887x_audio_buffer_t pmb887x_audio_buffer_t;

struct pmb887x_audio_buffer_t {
	uint8_t data[PMB887X_AUDIO_BUFFER_CAPACITY];
	size_t length;
	size_t position;
};

void pmb887x_audio_buffer_reset(pmb887x_audio_buffer_t *buffer);
const uint8_t *pmb887x_audio_buffer_peek(
	pmb887x_audio_buffer_t *buffer,
	Fifo8 *fifo,
	QemuMutex *lock,
	size_t requested,
	size_t *available
);
void pmb887x_audio_buffer_consume(pmb887x_audio_buffer_t *buffer, size_t consumed);
