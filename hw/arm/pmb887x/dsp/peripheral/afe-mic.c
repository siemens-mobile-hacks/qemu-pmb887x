/*
 * PMB887X-WEB FEATURE (mic): what the AFE's microphone ADC hears, a file of the
 * fork. Upstream's afe.c feeds the ADC silence; its one hook here takes
 * pmb887x_afe_mic_next() instead.
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"

#include "hw/arm/pmb887x/dsp/afe-mic.h"

/*
 * Test microphone: PMB887X_MIC_FILE = raw signed 16-bit little-endian mono samples at the ADC rate
 * (8 kHz), looped into the mic-ADC ring instead of silence. Unset = silence as before.
 */
static int16_t *afe_mic_samples;
static size_t afe_mic_count, afe_mic_pos;
static bool afe_mic_loaded;

/*
 * Live microphone (the browser page's getUserMedia, ui/wasm.c wasm_mic_put): 8 kHz samples pushed by
 * another thread, one producer and one consumer. Used when there is no PMB887X_MIC_FILE; empty = silence.
 * The page sends at wall-clock pace and the ADC takes them at guest pace: more than AFE_MIC_LIVE_MAX
 * waiting (a guest slower than real time) and new ones are dropped, so the delay stays under 0.25 s.
 */
#define AFE_MIC_LIVE_SIZE 4096      /* power of two */
#define AFE_MIC_LIVE_MAX 2000
static int16_t afe_mic_live[AFE_MIC_LIVE_SIZE];
static unsigned int afe_mic_live_head, afe_mic_live_tail;
static unsigned int afe_mic_live_flush;    /* the producer's ask: drop what waits up to here */

/* the mic stopped: what it said is not for the next call (the ADC may not be reading now) */
void pmb887x_afe_mic_flush(void) {
	qatomic_store_release(&afe_mic_live_flush, qatomic_read(&afe_mic_live_tail));
}

size_t pmb887x_afe_mic_push(const int16_t *samples, size_t count) {
	unsigned int tail = qatomic_read(&afe_mic_live_tail);
	unsigned int head = qatomic_load_acquire(&afe_mic_live_head);
	unsigned int flush = qatomic_read(&afe_mic_live_flush);
	unsigned int fill = tail - ((int) (flush - head) > 0 ? flush : head);
	size_t n = 0;

	while (n < count && fill + n < AFE_MIC_LIVE_MAX) {
		afe_mic_live[(tail + n) & (AFE_MIC_LIVE_SIZE - 1)] = samples[n];
		n++;
	}
	qatomic_store_release(&afe_mic_live_tail, tail + n);
	return n;
}

static bool afe_mic_live_pop(int16_t *v) {
	unsigned int head = qatomic_read(&afe_mic_live_head);
	unsigned int flush = qatomic_load_acquire(&afe_mic_live_flush);

	if ((int) (flush - head) > 0)
		head = flush;
	if (qatomic_load_acquire(&afe_mic_live_tail) == head)
		return false;
	*v = afe_mic_live[head & (AFE_MIC_LIVE_SIZE - 1)];
	qatomic_store_release(&afe_mic_live_head, head + 1);
	return true;
}

uint16_t pmb887x_afe_mic_next(void) {
	int16_t live;

	if (!afe_mic_loaded) {
		const char *path = getenv("PMB887X_MIC_FILE");
		gchar *data = NULL;
		gsize len = 0;
		afe_mic_loaded = true;
		if (path && g_file_get_contents(path, &data, &len, NULL) && len >= 2) {
			afe_mic_samples = (int16_t *) data;
			afe_mic_count = len / 2;
		}
	}
	if (!afe_mic_count)
		return afe_mic_live_pop(&live) ? (uint16_t) live : 0;
	const uint8_t *b = (const uint8_t *) &afe_mic_samples[afe_mic_pos];
	int16_t v = (int16_t) (b[0] | (b[1] << 8));
	afe_mic_pos = (afe_mic_pos + 1) % afe_mic_count;
	return (uint16_t) v;
}
