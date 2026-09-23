/*
 * DSP
 */
#define PMB887X_TRACE_ID		DSP
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp"

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "hw/core/sysbus.h"
#include "hw/core/hw-error.h"
#include "system/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/atomic.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/ssi/ssi.h"
#include "system/runstate.h"
#include "system/cpus.h"
#include "system/cpu-timers.h"

#include "hw/arm/pmb887x/dsp/runtime.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/gen/dsp_rom.h"
#include "hw/arm/pmb887x/dsp.h"
#include "hw/arm/pmb887x/dsp/config.h"
#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/trace.h"

#define DSP_RAM_SIZE		(DSP_IO_SIZE - DSP_RAM0)
#define DSP_BOOT_DATA_OFFSET	2
#define DSP_RUNTIME_PIPE_OFFSET	5
#define DSP_RUNTIME_PIPE_STRIDE	0x1C
#define DSP_OUTPUT_COUNT	3
#define DSP_SSC_BUS_NAME	"pmb887x-dsp-ssc"
/*
 * This file is compiled twice: once as itself for the core that executes the
 * mask ROM, and once from dsp-stub.c with STUB_DSP set, which answers the
 * firmware's runtime commands from the host instead. Only the PMB8875 ROM
 * (0x0602) runs on the core so far, so board.c still picks the stub for the
 * PMB8876. Everything below is static apart from pmb887x_dsp_set_config, and
 * the two builds share nothing else.
 */
#ifndef STUB_DSP
#define STUB_DSP 0
#endif

#if STUB_DSP
#define TYPE_PMB887X_DSP	TYPE_PMB887X_DSP_STUB
#define pmb887x_dsp_set_config	pmb887x_dsp_stub_set_config
#else
#define TYPE_PMB887X_DSP	"pmb887x-dsp"
#endif
#define PMB887X_DSP(obj)	OBJECT_CHECK(dsp_state_t, (obj), TYPE_PMB887X_DSP)

#if STUB_DSP
#include "hw/arm/pmb887x/dsp/hle.h"

#define DSP_CHAN0_CMD_ADDR	0x0005
#define DSP_CHAN1_CMD_ADDR	0x0021
#define DSP_CHAN2_CMD_ADDR	0x003D
#define DSP_PCMPLAY_SWITCH_END	0
#define DSP_PCMPLAY_SWITCH_INIT	1
#define DSP_PCMPLAY_SWITCH_FEED	2
#define DSP_PCM_BUF_WORD	1952
#define DSP_PCM_MAX_WORDS	1024
#define DSP_PCM_REFILL_PERIOD_NS	(2 * SCALE_MS)
#define DSP_PCM_REFILL_CLOCK		QEMU_CLOCK_HOST
#define DSP_PCM_BLOCK_HINT	80
#define DSP_COM_REFILL_BIT	0x800
#define DSP_COM_PIPE2_BIT	0x4	/* runtime pipe: command pending (ARM sets, DSP clears on consume) */
#define DSP_COM_BUSY_BIT	0x8	/* DSP-owned: runtime pipe busy (a command block is still buffered) */
#define DSP_COM_OVERRUN_BIT	0x10	/* DSP-owned: a runtime command arrived while the pipe was still busy */

/*
 * The mask ROM's PCMPLAY command has two client styles. One feeds samples
 * through the INIT/FEED/END sub-commands above (LG firmware does this). The
 * other (Siemens firmware) stages a chunk of the source stream in shared RAM
 * and raises communication flag 9; the mask ROM decodes the chunk, clears the
 * flag and interrupts the MCU to ask for the next one. Both are driver
 * conventions over the same mask-ROM command.
 *
 * A chunk is a fixed run of equally sized sub-blocks, each a two-word header
 * { type, payload words } followed by its payload. The payloads are the source
 * stream copied verbatim and contiguously, so for the IMA ADPCM streams the
 * melodies use, a 4-byte IMA block header turns up inline every 256 bytes:
 * `type` is the word offset within the payload where one begins, or
 * DSP_CHUNK_SUB_CONT when this sub-block carries none.
 */
#define DSP_COM_CHUNK_STAGED_BIT	0x200	/* communication flag 9: a chunk is staged */
#define DSP_CHUNK_WORDS	0x60
#define DSP_CHUNK_SUB_WORDS	24
#define DSP_CHUNK_SUB_CONT	0xFFFF
/* Every ADPCM payload word carries four samples. */
#define DSP_CHUNK_MAX_SAMPLES	(DSP_CHUNK_WORDS * 4)
/* How much audio to keep buffered in the backend ahead of real time. */
#define DSP_STREAM_BUFFER_NS	(400 * SCALE_MS)
/*
 * Never acknowledge from inside the submit path: the firmware has not finished
 * its bookkeeping yet and would drop the request, stalling the stream. This
 * also covers the empty chunk the driver stages when it (re)starts a stream.
 */
#define DSP_STREAM_MIN_ACK_NS	(5 * SCALE_MS)
#define DSP_STREAM_DEFAULT_RATE	16000
/*
 * For the chunk stream PCMPLAY SWITCH is a bit mask, not the INIT/FEED/END
 * enum: bit 6 selects the ADPCM player, and the upper bits name the player.
 * Whether this is a start or a stop is in the parameters, not the switch - a
 * stop leaves them all clear.
 */
#define DSP_PCMPLAY_SWITCH_ADPCM	0x0040
/*
 * PCMPLAY parameters: nonzero while starting, and the stream rate in Hz. This
 * is the same parameter block the INIT path reads, whose rate field is word 3.
 */
#define DSP_PCMPLAY_PAR_RUN	3
#define DSP_PCMPLAY_PAR_RATE	8
/* MCU interrupt the audio task listens on (SCU service request DSP_SRC1). */
#define DSP_MCU_ACK_IRQ	1

typedef struct dsp_state_t dsp_state_t;

struct dsp_state_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;

	uint32_t com_set;
	uint32_t com_status;
	uint32_t sem_set;
	uint32_t sem_status;
	uint8_t ram[DSP_RAM_SIZE];
	uint32_t revision;
	uint32_t rom_version;
	Clock *gsm_clock;
	qemu_irq mcu_interrupts[PMB887X_DSP_MCU_INT_COUNT];
	qemu_irq outputs[DSP_OUTPUT_COUNT];

	pmb887x_clc_reg_t clc;

	const pmb887x_dsp_config_t *config;
	dsp_device_t *afe;
	bool afe_started;
	bool pcm_armed;
	bool pcm_pending;
	uint16_t pcm_channels;
	uint16_t pcm_len;
	uint16_t pcm_block[DSP_PCM_MAX_WORDS];
	QEMUTimer *refill_timer;

	/* Shared-RAM chunk stream. */
	bool stream_active;
	int16_t ima_pred;
	uint8_t ima_index;
	uint32_t stream_rate;
	/* Flag 9 stays up until the acknowledgement, long after the chunk is read. */
	bool chunk_staged;
	QEMUTimer *stream_timer;
};

static inline uint16_t dsp_read_word(dsp_state_t *p, uint32_t offset) {
	const uint16_t* mem = (const uint16_t*)&p->ram[0];
	return mem[offset];
}

static void dsp_update_state(dsp_state_t *p) {
	// TODO
}

static void dsp_pcm_disarm(dsp_state_t *p);

static void dsp_reset_input(void *opaque, int id, int level) {
	if (level)
		dsp_pcm_disarm(opaque);
}

static void dsp_interrupt_input(void *opaque, int id, int level) {
}

static void dsp_input0(void *opaque, int id, int level) {
}

static void dsp_input1(void *opaque, int id, int level) {
}

static void dsp_gsm_input(void *opaque, int signal, int level) {
}

static uint32_t dsp_ram_read(dsp_state_t *p, uint32_t offset, unsigned size) {
	uint8_t *data = p->ram;
	switch (size) {
		case 1:		return data[offset];
		case 2:		return data[offset] | (data[offset + 1] << 8);
		case 4:		return data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) | (data[offset + 3] << 24);
		default:	abort();
	}
	return 0;
}

static void dsp_ram_write(dsp_state_t *p, uint32_t offset, uint32_t value, unsigned size) {
	uint8_t *data = p->ram;
	switch (size) {
		case 1:
			data[offset] = value & 0xFF;
			break;
		
		case 2:
			data[offset] = value & 0xFF;
			data[offset + 1] = (value >> 8) & 0xFF;
			break;
		
		case 4:
			data[offset] = value & 0xFF;
			data[offset + 1] = (value >> 8) & 0xFF;
			data[offset + 2] = (value >> 16) & 0xFF;
			data[offset + 3] = (value >> 24) & 0xFF;
			break;

		default:
			abort();
	}
}

static inline char hexdump_nibble(unsigned x)
{
	return (x < 10 ? '0' : 'a' - 10) + x;
}

static void dsp_hexdump(const char *prefix, void *buf, size_t size)
{
	g_autoptr(GString) str = g_string_sized_new(64);
	size_t b, len;

	for (b = 0; b < size; b += len) {
		len = MIN(16, size - b);
		g_string_truncate(str, 0);
		const uint16_t *line = (uint16_t*)buf + b;

		for (size_t i = 0; i < len; i++) {
			uint16_t c = line[i];

			g_string_append_c(str, hexdump_nibble((c >> 12) & 0xf));
			g_string_append_c(str, hexdump_nibble((c >> 8) & 0xf));
			g_string_append_c(str, hexdump_nibble((c >> 4) & 0xf));
			g_string_append_c(str, hexdump_nibble(c & 0xf));

			if (i < len) {
				g_string_append_c(str, ' ');
			}
		}

		DPRINTF("%s: %s\n", prefix, str->str);
	}
}

/*
 * Ask the ARM for the next PCM block the way the real DSP does: raise the 0x800
 * refill bit and pulse the audio/comm IRQ line so APOXI's DSP2 HISR runs, reads
 * the status and feeds a block. Guarded so we never stack duplicate requests.
 */
static void dsp_pcm_request_refill(dsp_state_t *p) {
	if ((p->com_status & DSP_COM_REFILL_BIT) != 0)
		return;
	p->com_status |= DSP_COM_REFILL_BIT;
	qemu_irq_pulse(p->mcu_interrupts[2]);
}

static void dsp_afe_flush_pending(dsp_state_t *p) {
	if (!p->pcm_pending || !p->afe)
		return;
	if (!afe_audio_has_room(p->afe, p->pcm_len))
		return;

	afe_audio_push_samples(p->afe, p->pcm_block, p->pcm_len);
	p->pcm_pending = false;
	p->com_status &= ~(uint32_t) (DSP_COM_PIPE2_BIT | DSP_COM_BUSY_BIT | DSP_COM_OVERRUN_BIT);
	dsp_pcm_request_refill(p);
}

static void dsp_pcm_refill_tick(void *opaque) {
	dsp_state_t *p = opaque;

	if (!p->pcm_armed)
		return;

	if (p->pcm_pending) {
		dsp_afe_flush_pending(p);
	} else if (p->afe && afe_audio_has_room(p->afe, p->pcm_len ? p->pcm_len : DSP_PCM_BLOCK_HINT)) {
		dsp_pcm_request_refill(p);
	}

	timer_mod(p->refill_timer, qemu_clock_get_ns(DSP_PCM_REFILL_CLOCK) + DSP_PCM_REFILL_PERIOD_NS);
}

static uint32_t dsp_pcm_rate_hz(uint16_t rate_field) {
	switch (rate_field) {
		case 8:		return 11025;	/* a1=1: 69 samples/frame */
		case 9:		return 12000;	/* a1=2: 75 */
		case 1:		return 16000;	/* a1=3: 100 */
		case 2:		return 22050;	/* a1=4: 138 */
		case 3:		return 24000;	/* a1=5: 150 */
		case 4:		return 32000;	/* a1=6: 200 */
		case 5:		return 44100;	/* a1=7: 276 */
		case 6:		return 48000;	/* a1=8: 300 */
		default:	return 11025;
	}
}

static uint16_t dsp_pcm_channels(uint16_t chmode)
{
	return chmode != 0 ? 2 : 1;
}

static void dsp_pcm_arm(dsp_state_t *p) {
	p->pcm_armed = true;
	if (p->afe)
		dsp_pcm_request_refill(p);	/* initial pull kick */
	if (p->refill_timer)
		timer_mod(p->refill_timer, qemu_clock_get_ns(DSP_PCM_REFILL_CLOCK) + DSP_PCM_REFILL_PERIOD_NS);
}

static void dsp_pcm_disarm(dsp_state_t *p) {
	p->pcm_armed = false;
	p->pcm_pending = false;
	if (p->refill_timer)
		timer_del(p->refill_timer);
	p->com_status &= ~(uint32_t) (DSP_COM_REFILL_BIT | DSP_COM_PIPE2_BIT | DSP_COM_BUSY_BIT | DSP_COM_OVERRUN_BIT);
}

static const int16_t dsp_ima_step_table[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
	11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767,
};

static const int8_t dsp_ima_index_table[16] = {
	-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

static int16_t dsp_ima_decode(dsp_state_t *p, uint8_t nibble) {
	int step = dsp_ima_step_table[p->ima_index];
	int diff = ((2 * (nibble & 7) + 1) * step) >> 3;
	int pred = p->ima_pred + ((nibble & 8) ? -diff : diff);
	int index = p->ima_index + dsp_ima_index_table[nibble];

	p->ima_pred = MIN(MAX(pred, -32768), 32767);
	p->ima_index = MIN(MAX(index, 0), 88);
	return p->ima_pred;
}

/*
 * Word address of the chunk buffer, as laid out by each mask ROM. The SGOLD
 * ROMs (0x0602/0x0604, buffer at 0x022D) run on the core, not this stub.
 */
static uint32_t dsp_comm_chunk_base(const dsp_state_t *p) {
	switch (p->rom_version) {
		case 0x0801:	return 0x057B;	/* SGOLD2 */
		default:	return 0;
	}
}

/* Decode the staged chunk into the audio backend, returning the sample count. */
static unsigned dsp_chunk_consume(dsp_state_t *p) {
	uint32_t base = dsp_comm_chunk_base(p);
	int16_t samples[DSP_CHUNK_MAX_SAMPLES];
	unsigned count = 0;

	if (!p->afe || base == 0)
		return 0;

	for (unsigned sub = 0; sub + DSP_CHUNK_SUB_WORDS <= DSP_CHUNK_WORDS;
			sub += DSP_CHUNK_SUB_WORDS) {
		uint16_t type = dsp_read_word(p, base + sub);
		uint16_t len = dsp_read_word(p, base + sub + 1);

		if (len == 0 || len > DSP_CHUNK_SUB_WORDS - 2)
			continue;
		for (unsigned w = 0; w < len; w++) {
			uint16_t word = dsp_read_word(p, base + sub + 2 + w);

			/*
			 * An IMA block header (predictor, step index) sits inline at word
			 * offset `type`; its predictor is also the block's first sample.
			 */
			if (w == type && w + 1 < len) {
				p->ima_pred = (int16_t) word;
				p->ima_index = MIN(dsp_read_word(p, base + sub + 3 + w) & 0xFF, 88);
				samples[count++] = p->ima_pred;
				w++;
				continue;
			}
			for (unsigned byte = 0; byte < 2; byte++) {
				uint8_t packed = word >> (8 * byte);

				samples[count++] = dsp_ima_decode(p, packed & 0xF);
				samples[count++] = dsp_ima_decode(p, packed >> 4);
			}
		}
	}

	if (count > 0)
		afe_audio_push_samples(p->afe, (const uint16_t *) samples, count);
	return count;
}

/*
 * Take down the hand-off the firmware is waiting on and interrupt the MCU,
 * which makes its audio task stage the next block.
 */
static void dsp_stream_ack(void *opaque) {
	dsp_state_t *p = opaque;

	if (!p->stream_active)
		return;

	p->com_status &= ~(uint32_t) DSP_COM_CHUNK_STAGED_BIT;
	qemu_irq_pulse(p->mcu_interrupts[DSP_MCU_ACK_IRQ]);
}

/*
 * Hold the acknowledgement until the backend is running low. The real DSP
 * takes the block's own playing time to consume it, but the emulated ARM does
 * not produce blocks at an even rate, so pacing the acknowledgement exactly to
 * real time leaves it no slack and the backend runs dry mid-melody. Letting
 * the firmware build up a cushion instead keeps playback gap-free - and it
 * still must not answer from inside the submit path, where the audio task has
 * not finished its bookkeeping and would drop the request.
 */
static void dsp_stream_pace(dsp_state_t *p) {
	size_t queued = p->afe ? afe_audio_queued_samples(p->afe) : 0;
	int64_t ahead = (int64_t) queued * NANOSECONDS_PER_SECOND / p->stream_rate;
	int64_t wait = MAX(ahead - DSP_STREAM_BUFFER_NS, DSP_STREAM_MIN_ACK_NS);

	timer_mod(p->stream_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST) + wait);
}

/* The ARM raised flag 9: decode the chunk it staged and schedule the answer. */
static void dsp_stream_service(dsp_state_t *p) {
	if (!p->stream_active || (p->com_status & DSP_COM_CHUNK_STAGED_BIT) == 0)
		return;
	if (p->chunk_staged) {
		p->chunk_staged = false;
		dsp_chunk_consume(p);
	}
	dsp_stream_pace(p);
}

static void dsp_stream_pcmplay(dsp_state_t *p, uint32_t base, uint16_t sw) {
	if (dsp_read_word(p, base + DSP_PCMPLAY_PAR_RUN) == 0) {
		p->stream_active = false;
		p->com_status &= ~(uint32_t) DSP_COM_CHUNK_STAGED_BIT;
		timer_del(p->stream_timer);
		DPRINTF("stream: stop\n");
		return;
	}

	uint32_t rate = dsp_read_word(p, base + DSP_PCMPLAY_PAR_RATE);

	if (rate < 4000 || rate > 48000)
		rate = DSP_STREAM_DEFAULT_RATE;
	p->stream_active = true;
	p->ima_pred = 0;
	p->ima_index = 0;
	p->stream_rate = rate;
	if (p->afe)
		afe_audio_set_format(p->afe, rate, 1);
	DPRINTF("stream: start switch=0x%04X %u Hz\n", sw, rate);
}

static void dsp_afe_queue_block(dsp_state_t *p, uint16_t len) {
	uint32_t words;

	if (!p->afe || !p->afe_started)
		return;

	p->pcm_channels = dsp_pcm_channels(dsp_read_word(p, DSP_CHAN2_CMD_ADDR + 4));
	words = (uint32_t) len * p->pcm_channels;
	if (words > DSP_PCM_MAX_WORDS)
		words = DSP_PCM_MAX_WORDS;
	for (uint32_t i = 0; i < words; i++)
		p->pcm_block[i] = dsp_read_word(p, DSP_PCM_BUF_WORD + i);
	p->pcm_len = words;

	/* A runtime command arriving while a block is still buffered = overrun (the ARM should have
	 * waited for DSP_COM_STATUS & 0x18 to clear before enqueuing). Real HW drops it. */
	if ((p->com_status & DSP_COM_BUSY_BIT) != 0)
		p->com_status |= DSP_COM_OVERRUN_BIT;
	p->pcm_pending = true;

	/* Hold the runtime handshake (0x4) and assert DSP-busy (0x8) until the block is buffered. */
	p->com_status |= DSP_COM_PIPE2_BIT | DSP_COM_BUSY_BIT;
	dsp_afe_flush_pending(p);
}

static void dsp_exec_command_ch0(dsp_state_t *p) {
	uint16_t id = dsp_read_word(p, DSP_CHAN0_CMD_ADDR);
	dsp_hexdump("CH0", &p->ram[DSP_CHAN0_CMD_ADDR * 2], 0x1c);

	DPRINTF("CH0 exec command! 0x%x\n", id);
	qemu_irq_pulse(p->mcu_interrupts[0]);
}

static void dsp_exec_command_ch1(dsp_state_t *p) {
	uint16_t id = dsp_read_word(p, DSP_CHAN1_CMD_ADDR);
	dsp_hexdump("CH1", &p->ram[DSP_CHAN1_CMD_ADDR * 2], 0x1c);

	DPRINTF("CH1 exec command! 0x%x\n", id);
	qemu_irq_pulse(p->mcu_interrupts[1]);
}

static void dsp_log_command_ch2(dsp_state_t *p, uint16_t id) {
	uint16_t r[24];
	for (int i = 0; i < 24; i++) {
		r[i] = dsp_read_word(p, DSP_CHAN2_CMD_ADDR + 1 + i);
	}

	DPRINTF("CH2 exec: 0x%02X (%s)\n", id, dsp_cmd_name(id));

	switch (id) {
		case DSP_CMD_VB_ON:
			DPRINTF("    sw=0x%04X, vrx=[0x%04X, 0x%04X], vtx=0x%04X, rate_sw=%u, out_mode=%u, csel=%u\n",
					r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
			DPRINTF("    num0=%u, den0=%u, num1=%u, den1=%u, rxconf=0x%04X, txconf=0x%04X\n",
					r[7], r[8], r[9], r[10], r[11], r[12]);
			break;

		case DSP_CMD_VB_SET_BIQUAD:
			DPRINTF("    in1:  [%04X %04X %04X %04X %04X]  in2:  [%04X %04X %04X %04X %04X]\n",
					r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9]);
			DPRINTF("    out1: [%04X %04X %04X %04X %04X]  out2: [%04X %04X %04X %04X %04X]\n",
					r[10], r[11], r[12], r[13], r[14], r[15], r[16], r[17], r[18], r[19]);
			break;

		case DSP_CMD_VB_SET_GAIN:
			DPRINTF("    Scal_In=0x%04X, Scal_Out=0x%04X, Side_Ton=0x%04X, Mic_Mute=%u, Scal_Mic=0x%04X\n",
					r[0], r[1], r[2], r[3], r[4]);
			DPRINTF("    Gain_Out=0x%04X, Scal_Rec=0x%04X, Speech_Mix_DL=0x%04X, Ton_Mix=0x%04X\n",
					r[5], r[6], r[7], r[8]);
			DPRINTF("    Delta=[0x%04X, 0x%04X], Kappa=[0x%04X, 0x%04X], Lambda=[0x%04X, 0x%04X]\n",
					r[9], r[10], r[11], r[12], r[13], r[14]);
			DPRINTF("    Scal_AFE=0x%04X, Scal_Mic2=0x%04X, AFE_tone=0x%04X, Ton_Mix_UL=0x%04X, Ton_Mix_DL=0x%04X, Speech_Mix_UL=0x%04X\n",
					r[15], r[16], r[17], r[18], r[19], r[20]);
			break;

		case DSP_CMD_VB_SET_CBUF_GAIN:
			DPRINTF("    Scal_SAPP=0x%04X, Scal_Ext=0x%04X, Mix_AFE=0x%04X, Mix_I2Sx=0x%04X, Scal_PCM=0x%04X (vol=%u/32767)\n",
					r[0], r[1], r[2], r[3], r[4], r[4]);
			break;

		case DSP_CMD_VM_CMD:
			DPRINTF("    mode=%u, alpha=[0x%04X, 0x%04X], beta=[0x%04X, 0x%04X], gamma=[0x%04X, 0x%04X]\n",
					r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
			break;

		case DSP_CMD_HF_SET_PAR:
			DPRINTF("    pars: %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X\n",
					r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11]);
			break;

		case DSP_CMD_HF_ON:
			DPRINTF("    switch=0x%04X (EC:%u ADAPT:%u NR:%u AGC:%u)\n",
					r[0], !!(r[0] & 1), !!(r[0] & 2), !!(r[0] & 4), !!(r[0] & 8));
			break;

		case DSP_CMD_PCMPLAY:
			DPRINTF("    switch=0x%04X, par1=0x%04X, rate_field=%u (%u Hz), ch=%u\n",
					r[0], r[1], r[2], dsp_pcm_rate_hz(r[2]), dsp_pcm_channels(r[3]));
			break;

		case DSP_CMD_DTX_ON:
		case DSP_CMD_VB_DAI:
			DPRINTF("    val=0x%04X\n", r[0]);
			break;

		default:
			dsp_hexdump("CH2", &p->ram[DSP_CHAN2_CMD_ADDR * 2], 0x1c);
			break;
	}
}

static void dsp_exec_command_ch2(dsp_state_t *p) {
	uint16_t id = dsp_read_word(p, DSP_CHAN2_CMD_ADDR);

	bool print = true;

	switch (id) {
		case DSP_CMD_VB_ON:
			p->afe_started = true;
			break;

		case DSP_CMD_PCMPLAY: {
			uint16_t sw = dsp_read_word(p, DSP_CHAN2_CMD_ADDR + 1);

			if ((sw & DSP_PCMPLAY_SWITCH_ADPCM) != 0) {
				dsp_stream_pcmplay(p, DSP_CHAN2_CMD_ADDR, sw);
				qemu_irq_pulse(p->mcu_interrupts[2]);
			} else if (sw == DSP_PCMPLAY_SWITCH_INIT) {
				uint16_t rate_field = dsp_read_word(p, DSP_CHAN2_CMD_ADDR + 3);

				p->afe_started = true;
				if (p->afe)
					afe_audio_set_format(p->afe, dsp_pcm_rate_hz(rate_field),
							dsp_pcm_channels(dsp_read_word(p, DSP_CHAN2_CMD_ADDR + 4)));
				dsp_pcm_arm(p);
				DPRINTF("pcm play: arm rate_field=%u (%u Hz)\n", rate_field, dsp_pcm_rate_hz(rate_field));
			} else if (sw == DSP_PCMPLAY_SWITCH_FEED) {
				print = false; // noisy
				dsp_afe_queue_block(p, dsp_read_word(p, DSP_CHAN2_CMD_ADDR + 2));
			} else if (sw == DSP_PCMPLAY_SWITCH_END) {
				dsp_pcm_disarm(p);
				DPRINTF("pcm play: end\n");
				qemu_irq_pulse(p->mcu_interrupts[2]);
			} else {
				DPRINTF("unknown pcmplay subcommand! 0x%x\n", sw);
			}
			break;
		}

		default:
			qemu_irq_pulse(p->mcu_interrupts[2]);
			break;
	}

	if (print) {
		dsp_log_command_ch2(p, id);
	}
}

static uint64_t dsp_io_read(void *opaque, hwaddr haddr, unsigned size) {
	dsp_state_t *p = opaque;
	
	uint64_t value = 0;

	switch (haddr) {
		case DSP_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case DSP_ID:
			value = 0xF022C031;
			break;

		case DSP_COM_SET:
		case DSP_COM_CLEAR:
			value = 0;
			break;

		case DSP_COM_STATUS:
			dsp_afe_flush_pending(p);
			value = p->com_status;
			break;

		case DSP_RAM0 ... (DSP_RAM0 + DSP_RAM_SIZE):
			value = dsp_ram_read(p, haddr - DSP_RAM0, size);
			break;

		default:
			// IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}

	IO_DUMP_READ(haddr + p->mmio.addr, size, value);
	
	return value;
}

static void dsp_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	dsp_state_t *p = opaque;
	
	IO_DUMP_WRITE(haddr + p->mmio.addr, size, value);

	switch (haddr) {
		case DSP_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case DSP_SEM_SET:
			p->sem_set = value;
			p->sem_status = 0;
			break;

		case DSP_SEM_STATUS:
			p->sem_status = value;
			break;

		case DSP_COM_SET:
			p->com_set = value;
			/* Flag 9 stays raised until the staged chunk has actually been
			 * consumed - including the first chunk, which SGOLD2 firmware
			 * stages before it starts the player. */
			p->com_status = (p->com_status | value) & DSP_COM_CHUNK_STAGED_BIT;
			if ((value & DSP_COM_CHUNK_STAGED_BIT) != 0)
				p->chunk_staged = true;
			// if ((value & 1) != 0) dsp_exec_command_ch0(p); // noisy logs
			if ((value & 2) != 0) dsp_exec_command_ch1(p);
			if ((value & 4) != 0) dsp_exec_command_ch2(p);
			dsp_stream_service(p);
			break;

		case DSP_COM_STATUS:
			p->com_status = value;
			break;

		case DSP_COM_CLEAR:
			p->com_status &= ~((uint32_t) value & 0xFFFF);
			break;

		case DSP_RAM0 ... (DSP_RAM0 + DSP_RAM_SIZE):
			dsp_ram_write(p, haddr - DSP_RAM0, value, size);
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}

	dsp_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= dsp_io_read,
	.write			= dsp_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void dsp_init(Object *obj) {
	dsp_state_t *p = PMB887X_DSP(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-dsp", DSP_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_reset_input, "RESET_IN", 1);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_interrupt_input, "INT_IN", PMB887X_DSP_INT_COUNT);
	qdev_init_gpio_out_named(DEVICE(obj), p->mcu_interrupts, "INT_OUT", ARRAY_SIZE(p->mcu_interrupts));
	qdev_init_gpio_in_named(DEVICE(obj), dsp_input0, "DSPIN0_IN", 1);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_input1, "DSPIN1_IN", 1);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_gsm_input, "GSM_IN", PMB887X_DSP_GSM_SIGNAL_COUNT);
	p->gsm_clock = qdev_init_clock_in(DEVICE(obj), "GSM_CLOCK", NULL, p, 0);
	qdev_init_gpio_out_named(DEVICE(obj), &p->outputs[0], "DSPOUT0_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &p->outputs[1], "DSPOUT1_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &p->outputs[2], "DSPOUT2_OUT", 1);
}

static const Property dsp_properties[] = {
	DEFINE_PROP_UINT32("revision", dsp_state_t, revision, 0),
	DEFINE_PROP_UINT32("rom_version", dsp_state_t, rom_version, 0),
};

static const pmb887x_dsp_peripheral_config_t *dsp_find_peripheral(const pmb887x_dsp_config_t *config, pmb887x_dsp_peripheral_type_t type) {
	if (config == NULL)
		return NULL;
	for (size_t i = 0; i < config->peripheral_count; i++)
		if (config->peripherals[i].type == type)
			return &config->peripherals[i];
	return NULL;
}

/*
 * AFE host hooks. The direct playback bridge never drives afe_advance, so the
 * AFE's DAC-ring accessors are unused in practice; back them with the stub's
 * shared RAM (bounds-checked) so the device is nonetheless self-consistent.
 */
static uint16_t dsp_afe_data_read(void *opaque, uint16_t address) {
	dsp_state_t *p = opaque;
	return (size_t) address * sizeof(uint16_t) < DSP_RAM_SIZE ? dsp_read_word(p, address) : 0;
}

static void dsp_afe_data_write(void *opaque, uint16_t address, uint16_t value) {
	dsp_state_t *p = opaque;
	if ((size_t) address * sizeof(uint16_t) < DSP_RAM_SIZE)
		((uint16_t *) p->ram)[address] = value;
}

static void dsp_realize(DeviceState *dev, Error **errp) {
	dsp_state_t *p = PMB887X_DSP(dev);
	const pmb887x_dsp_peripheral_config_t *afe_config;

	pmb887x_clc_init(&p->clc);

	p->sem_set = 0x01;
	p->sem_status = 0x00;
	p->com_set = 0x01;
	p->com_status = 0x00;

	afe_config = dsp_find_peripheral(p->config, PMB887X_DSP_PERIPHERAL_AFE);
	if (afe_config != NULL) {
		dsp_host_t host = {
			.opaque = p,
			.data_read = dsp_afe_data_read,
			.data_write = dsp_afe_data_write,
		};
		p->afe = afe_create(afe_config, NULL, &host);
	}

	p->refill_timer = timer_new_ns(DSP_PCM_REFILL_CLOCK, dsp_pcm_refill_tick, p);
	p->stream_timer = timer_new_ns(QEMU_CLOCK_HOST, dsp_stream_ack, p);

	/* The ARM reads the mask ROM version from shared RAM word 0 and refuses to
	 * boot (ddsphw fatal exit) when it does not match the SoC it expects. */
	if (p->rom_version == 0 && p->config != NULL)
		p->rom_version = p->config->default_rom_version;
	dsp_ram_write(p, 0, p->rom_version, 2);

	dsp_update_state(p);
}

static void dsp_unrealize(DeviceState *dev) {
	dsp_state_t *p = PMB887X_DSP(dev);

	if (p->refill_timer != NULL) {
		timer_free(p->refill_timer);
		p->refill_timer = NULL;
	}
	if (p->stream_timer != NULL) {
		timer_free(p->stream_timer);
		p->stream_timer = NULL;
	}
	if (p->afe != NULL) {
		p->afe->ops->destroy(p->afe);
		g_free(p->afe);
		p->afe = NULL;
	}
}

static void dsp_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, dsp_properties);
	dc->realize = dsp_realize;
	dc->unrealize = dsp_unrealize;
}

void pmb887x_dsp_set_config(DeviceState *dev, const pmb887x_dsp_config_t *config) {
	dsp_state_t *p = PMB887X_DSP(dev);
	p->config = config;
}

static const TypeInfo dsp_info = {
	.name          	= TYPE_PMB887X_DSP,
	.parent        	= TYPE_SYS_BUS_DEVICE,
	.instance_size 	= sizeof(struct dsp_state_t),
	.instance_init 	= dsp_init,
	.class_init    	= dsp_class_init,
};

static void dsp_register_types(void) {
	type_register_static(&dsp_info);
}
type_init(dsp_register_types)

#else

enum {
	DSP_BOOT_PLOAD,
	DSP_BOOT_DLOAD,
	DSP_BOOT_BRANCH,
	DSP_BOOT_PREAD,
	DSP_BOOT_DREAD,
};

/*
 * The DSP runs on its own thread, on the same QEMU_CLOCK_VIRTUAL time line as
 * the ARM: its clock counts core cycles at fDSP. It never runs past the ARM
 * (icount2_get_horizon), and a running ARM gets less than two DSP_QUANTUM_NS
 * ahead of it. Everything the ARM does to the DSP, shared RAM writes included,
 * reaches the DSP at the time it was made, and signals from the DSP reach the
 * ARM through a VIRTUAL timer at the time the DSP made them, or later by as
 * much as the ARM is ahead. Reads of DSP state bring the DSP to the ARM first.
 */
#define DSP_QUANTUM_NS		(20 * SCALE_US)
/* While the ARM runs, the DSP follows it in steps no smaller than this. */
#define DSP_MIN_STEP_NS		(2 * SCALE_US)
#define DSP_SPIN_NS		(100 * SCALE_US)
/* While the ARM sleeps nobody waits on a busy DSP: it keeps up with the host in steps this long. */
#define DSP_SLEEP_STEP_NS	(200 * SCALE_US)
#define DSP_FLUSH_POLL_MS	1
/* Applied events are dropped from the front of the queue in batches of at least this many. */
#define DSP_EVENTS_COMPACT	256
#define DSP_SLOW_SYNC_NS	(10 * SCALE_MS)
#define DSP_OUTPUT_MASK		MAKE_64BIT_MASK(0, DSP_OUTPUT_COUNT)

typedef struct dsp_state_t dsp_state_t;
typedef struct dsp_event_t dsp_event_t;
typedef struct dsp_output_t dsp_output_t;
typedef struct dsp_worker_t dsp_worker_t;

typedef enum {
	DSP_EVENT_RESET,
	DSP_EVENT_CLOCK,
	DSP_EVENT_FREQUENCY,
	DSP_EVENT_COM_SET,
	DSP_EVENT_COM_CLEAR,
	DSP_EVENT_SEM_SET,
	DSP_EVENT_SEM_CLEAR,
	DSP_EVENT_REQUEST,
	DSP_EVENT_INPUT,
	DSP_EVENT_GSM_SIGNAL,
	DSP_EVENT_SHARED_WRITE,
} dsp_event_type_t;

/* Something the ARM did to the DSP. */
struct dsp_event_t {
	int64_t time;
	dsp_event_type_t type;
	uint32_t index;
	uint32_t value;
	uint32_t frequency;
	uint8_t size;
};

/* Something the DSP did that the ARM sees. */
struct dsp_output_t {
	int64_t time;
	uint16_t interrupts;
	uint16_t output_events;
	uint16_t outputs;
};

struct dsp_worker_t {
	QemuThread thread;
	QemuMutex mutex;
	QemuCond cond;
	QemuCond progress;
	GArray *events;
	size_t events_head;
	GArray *outputs;
	uint64_t posted;
	uint64_t applied;
	int64_t last_event_time;
	/* DSP time with every event up to it applied. */
	int64_t now;
	/* Unless the ARM does something, nothing happens on the DSP before this. */
	int64_t quiet_until;
	/*
	 * ARM writes to the shared RAM that the DSP has yet to take, byte by
	 * byte: the latest value and how many are outstanding.
	 */
	uint8_t *shared_pending;
	uint32_t *shared_pending_count;
	/* Sleeping vCPUs wait for the DSP to reach this, or -1. */
	int64_t want;
	uint32_t waiters;
	bool output_added;
	bool kicked;
	bool running;
	bool stop;
	bool created;
};

struct dsp_state_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	MemoryRegion regs;
	MemoryRegion ram;
	uint32_t revision;
	uint32_t rom_version;
	const pmb887x_dsp_config_t *config;
	pmb887x_cgu_t *cgu;
	bool trace_boot_mode;
	pmb887x_clc_reg_t clc;
	dsp_runtime_t *runtime;
	dsp_worker_t worker;
	QEMUTimer *delivery_timer;
	QEMUTimer *lead_timer;
	QEMUTimer *wake_timer;
	VMChangeStateEntry *vmstate;
	Clock *gsm_clock;
	uint32_t fdsp;
	/* Owned by the worker: the clock the core was last given. */
	uint32_t core_fdsp;
	bool core_clock_enabled;
	qemu_irq mcu_interrupts[PMB887X_DSP_MCU_INT_COUNT];
	qemu_irq outputs[DSP_OUTPUT_COUNT];
	SSIBus *ssc_bus;
};

static uint32_t dsp_ssc_transfer(void *opaque, uint32_t value) {
	dsp_state_t *p = opaque;
	bool locked = bql_locked();
	uint32_t received;

	if (!locked)
		bql_lock();
	received = ssi_transfer(p->ssc_bus, value);
	if (!locked)
		bql_unlock();
	return received;
}

static int64_t dsp_horizon(void) {
	if (icount2_enabled())
		return icount2_get_horizon();
	return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static bool dsp_arm_sleeping(void) {
	return icount2_enabled() && icount2_get_horizon_delay(INT64_MAX) >= 0;
}

static void dsp_worker_kick_locked(dsp_worker_t *w) {
	w->kicked = true;
	qemu_cond_signal(&w->cond);
}

static void dsp_worker_kick(dsp_state_t *p) {
	qemu_mutex_lock(&p->worker.mutex);
	dsp_worker_kick_locked(&p->worker);
	qemu_mutex_unlock(&p->worker.mutex);
}

static int64_t dsp_first_output_time(dsp_worker_t *w) {
	if (w->outputs->len == 0)
		return INT64_MAX;
	return g_array_index(w->outputs, dsp_output_t, 0).time;
}

static bool dsp_has_events(dsp_worker_t *w) {
	return w->events_head < w->events->len;
}

static int64_t dsp_first_event_time(dsp_worker_t *w) {
	if (!dsp_has_events(w))
		return INT64_MAX;
	return g_array_index(w->events, dsp_event_t, w->events_head).time;
}

static dsp_event_t dsp_pop_event(dsp_worker_t *w) {
	dsp_event_t event = g_array_index(w->events, dsp_event_t, w->events_head++);

	if (w->events_head == w->events->len) {
		g_array_set_size(w->events, 0);
		w->events_head = 0;
	} else if (w->events_head >= DSP_EVENTS_COMPACT && w->events_head * 2 >= w->events->len) {
		g_array_remove_range(w->events, 0, w->events_head);
		w->events_head = 0;
	}
	return event;
}

/*
 * How far sleeping vCPUs may go: not past what the DSP has yet to tell them,
 * nor past where it could next do something.
 */
static int64_t dsp_limit_locked(dsp_worker_t *w) {
	int64_t now = qatomic_read(&w->now);

	if (!w->running)
		return INT64_MAX;
	return MIN(dsp_first_output_time(w), MAX(now, MIN(w->quiet_until, dsp_first_event_time(w))));
}

static int64_t dsp_icount_limit(void *opaque, int64_t want) {
	dsp_state_t *p = opaque;
	dsp_worker_t *w = &p->worker;
	int64_t limit;

	qemu_mutex_lock(&w->mutex);
	limit = dsp_limit_locked(w);
	if (want >= 0 && limit < want) {
		w->want = w->want < 0 ? want : MIN(w->want, want);
		dsp_worker_kick_locked(w);
	}
	qemu_mutex_unlock(&w->mutex);
	return limit;
}

static void dsp_post_event(dsp_state_t *p, dsp_event_t *event) {
	dsp_worker_t *w = &p->worker;

	event->time = MAX(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), w->last_event_time);
	w->last_event_time = event->time;
	g_array_append_val(w->events, *event);
	w->posted++;
	dsp_worker_kick_locked(w);
}

static void dsp_post(dsp_state_t *p, dsp_event_type_t type, uint32_t index, uint32_t value, uint32_t frequency) {
	dsp_event_t event = {
		.type = type,
		.index = index,
		.value = value,
		.frequency = frequency,
	};

	qemu_mutex_lock(&p->worker.mutex);
	dsp_post_event(p, &event);
	qemu_mutex_unlock(&p->worker.mutex);
}

static void dsp_post_shared_write(dsp_state_t *p, size_t offset, uint32_t value, unsigned size) {
	dsp_worker_t *w = &p->worker;
	dsp_event_t event = {
		.type = DSP_EVENT_SHARED_WRITE,
		.index = offset,
		.value = value,
		.size = size,
	};

	qemu_mutex_lock(&w->mutex);
	for (unsigned i = 0; i < size; i++) {
		w->shared_pending[offset + i] = value >> (i * 8);
		w->shared_pending_count[offset + i]++;
	}
	dsp_post_event(p, &event);
	qemu_mutex_unlock(&w->mutex);
}

/* On the worker, once the write is in the shared RAM. */
static void dsp_shared_write_done(dsp_worker_t *w, const dsp_event_t *event) {
	for (unsigned i = 0; i < event->size; i++)
		w->shared_pending_count[event->index + i]--;
}

/* The shared RAM as the ARM sees it: with its own writes the DSP has yet to take. */
static uint64_t dsp_shared_read(dsp_state_t *p, size_t offset, unsigned size) {
	dsp_worker_t *w = &p->worker;
	uint64_t value;

	qemu_mutex_lock(&w->mutex);
	value = dsp_runtime_shared_read_bytes(p->runtime, offset, size);
	for (unsigned i = 0; i < size; i++) {
		if (w->shared_pending_count[offset + i] != 0) {
			value &= ~((uint64_t) UINT8_MAX << (i * 8));
			value |= (uint64_t) w->shared_pending[offset + i] << (i * 8);
		}
	}
	qemu_mutex_unlock(&w->mutex);
	return value;
}

static uint16_t dsp_shared_read_word(dsp_state_t *p, size_t offset) {
	return dsp_shared_read(p, offset * sizeof(uint16_t), sizeof(uint16_t));
}

static void dsp_queue_output(dsp_state_t *p, const dsp_output_t *output) {
	dsp_worker_t *w = &p->worker;

	qemu_mutex_lock(&w->mutex);
	g_array_append_val(w->outputs, *output);
	w->output_added = true;
	qemu_mutex_unlock(&w->mutex);
	timer_mod_anticipate(p->delivery_timer, output->time);
}

/* On the worker, at the DSP time the core changed TOMCU or DSPOUT. */
static void dsp_worker_events_changed(void *opaque) {
	dsp_state_t *p = opaque;
	dsp_output_t output = {
		.time = dsp_runtime_get_time(p->runtime),
		.interrupts = dsp_runtime_take_mcu_irqs(p->runtime) & MAKE_64BIT_MASK(0, PMB887X_DSP_MCU_INT_COUNT),
		.output_events = dsp_runtime_take_output_events(p->runtime) & DSP_OUTPUT_MASK,
		.outputs = dsp_runtime_get_outputs(p->runtime),
	};

	dsp_queue_output(p, &output);
}

/* Under the BQL: hand the ARM what the DSP did up to @time. */
static void dsp_deliver(dsp_state_t *p, int64_t time) {
	dsp_worker_t *w = &p->worker;
	bool delivered = false;
	int64_t next;

	for (;;) {
		dsp_output_t output;

		qemu_mutex_lock(&w->mutex);
		next = dsp_first_output_time(w);
		if (next > time) {
			if (delivered)
				dsp_worker_kick_locked(w);
			qemu_mutex_unlock(&w->mutex);
			break;
		}
		output = g_array_index(w->outputs, dsp_output_t, 0);
		g_array_remove_index(w->outputs, 0);
		qemu_mutex_unlock(&w->mutex);

		for (size_t i = 0; i < ARRAY_SIZE(p->mcu_interrupts); i++)
			if ((output.interrupts & BIT(i)) != 0)
				qemu_irq_raise(p->mcu_interrupts[i]);
		for (size_t i = 0; i < ARRAY_SIZE(p->outputs); i++)
			if ((output.output_events & BIT(i)) != 0)
				qemu_set_irq(p->outputs[i], (output.outputs & BIT(i)) != 0);
		delivered = true;
	}

	if (next != INT64_MAX)
		timer_mod(p->delivery_timer, next);
}

static void dsp_delivery_timer(void *opaque) {
	dsp_deliver(opaque, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static bool dsp_synced(dsp_worker_t *w, int64_t time, uint64_t seq) {
	return qatomic_read(&w->now) >= time && qatomic_read(&w->applied) >= seq;
}

/*
 * Wait for the DSP to reach @time with every event up to @seq applied. The
 * BQL is dropped meanwhile: the worker may need it for the SSC.
 *
 * Returns false when it gave up because the DSP is waiting for the vCPU to
 * flush the shared code buffer, which the vCPU can only do from its own loop.
 * The lead timer retries; a register or RAM read gets the DSP state as it
 * stands, which the flush, a rare event, makes a few microseconds stale.
 */
static bool dsp_wait(dsp_state_t *p, int64_t time, uint64_t seq) {
	dsp_worker_t *w = &p->worker;
	int64_t start;
	int64_t spin_end;
	bool synced;
	bool bql;

	qemu_mutex_lock(&w->mutex);
	if (dsp_synced(w, time, seq) || !w->running || w->stop) {
		qemu_mutex_unlock(&w->mutex);
		return true;
	}
	w->waiters++;
	dsp_worker_kick_locked(w);
	qemu_mutex_unlock(&w->mutex);

	bql = bql_locked();
	if (bql)
		bql_unlock();

	start = get_clock();
	spin_end = start + DSP_SPIN_NS;
	while (!dsp_synced(w, time, seq) && !dsp_runtime_code_flush_pending() && get_clock() < spin_end)
		cpu_relax();

	qemu_mutex_lock(&w->mutex);
	while (!(synced = dsp_synced(w, time, seq)) && w->running && !w->stop && !dsp_runtime_code_flush_pending())
		qemu_cond_timedwait(&w->progress, &w->mutex, DSP_FLUSH_POLL_MS);
	synced = synced || !w->running || w->stop;
	w->waiters--;
	qemu_mutex_unlock(&w->mutex);

	if (get_clock() - start > DSP_SLOW_SYNC_NS)
		DPRINTF("slow sync: time=%" PRId64 " host=%" PRId64 " us dsp_pc=%05X\n", time,
			(get_clock() - start) / SCALE_US, dsp_runtime_get_pc(p->runtime));
	if (!synced)
		DPRINTF("sync deferred by code flush: time=%" PRId64 " dsp=%" PRId64 "\n", time, qatomic_read(&w->now));

	if (bql)
		bql_lock();
	return synced;
}

/* The ARM reads DSP state that only the DSP changes: bring the DSP to the ARM's time first. */
static void dsp_sync_exact(dsp_state_t *p) {
	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

	dsp_wait(p, now, qatomic_read(&p->worker.posted));
	dsp_deliver(p, now);
}

static void dsp_lead_timer(void *opaque) {
	dsp_state_t *p = opaque;
	int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

	dsp_worker_kick(p);
	if (qemu_in_vcpu_thread() && !dsp_wait(p, now - DSP_QUANTUM_NS, 0)) {
		/* Catch up as soon as the vCPU is back from flushing the code buffer. */
		timer_mod(p->lead_timer, now + 1);
		return;
	}
	timer_mod(p->lead_timer, now + DSP_QUANTUM_NS);
}

static void dsp_wake_timer(void *opaque) {
	dsp_worker_kick(opaque);
}

static void dsp_apply_clock(dsp_state_t *p) {
	dsp_runtime_set_clock(p->runtime, p->core_clock_enabled);
	dsp_runtime_set_frequency(p->runtime, p->core_clock_enabled ? p->core_fdsp : 0);
}

/* On the worker, with the DSP at the event's time. */
static void dsp_apply_event(dsp_state_t *p, const dsp_event_t *event) {
	switch (event->type) {
		case DSP_EVENT_RESET: {
			dsp_output_t output = {
				.time = event->time,
				.output_events = DSP_OUTPUT_MASK,
			};

			dsp_runtime_take_mcu_irqs(p->runtime);
			dsp_runtime_take_output_events(p->runtime);
			dsp_runtime_reset(p->runtime);
			dsp_apply_clock(p);
			dsp_queue_output(p, &output);
			DPRINTF("core reset: time=%" PRId64 "\n", event->time);
			break;
		}

		case DSP_EVENT_CLOCK:
			p->core_clock_enabled = event->value != 0;
			dsp_apply_clock(p);
			break;

		case DSP_EVENT_FREQUENCY:
			p->core_fdsp = event->frequency;
			dsp_apply_clock(p);
			break;

		case DSP_EVENT_COM_SET:
			dsp_runtime_set_comm(p->runtime, event->value);
			break;

		case DSP_EVENT_COM_CLEAR:
			dsp_runtime_clear_comm(p->runtime, event->value);
			break;

		case DSP_EVENT_SEM_SET:
			dsp_runtime_request_mcu_semaphores(p->runtime, event->value);
			break;

		case DSP_EVENT_SEM_CLEAR:
			dsp_runtime_release_mcu_semaphores(p->runtime, event->value);
			break;

		case DSP_EVENT_REQUEST:
			dsp_runtime_set_request(p->runtime, event->index, true);
			break;

		case DSP_EVENT_INPUT:
			dsp_runtime_set_input(p->runtime, event->index, event->value != 0);
			break;

		case DSP_EVENT_GSM_SIGNAL:
			dsp_runtime_set_gsm_clock(p->runtime, event->frequency);
			dsp_runtime_set_gsm_signal(p->runtime, event->index, event->value != 0);
			break;

		case DSP_EVENT_SHARED_WRITE:
			dsp_runtime_shared_write_bytes(p->runtime, event->index, event->value, event->size);
			break;
	}
}

static void dsp_worker_publish(dsp_state_t *p) {
	dsp_worker_t *w = &p->worker;
	bool advanced;

	qatomic_set(&w->now, dsp_runtime_get_time(p->runtime));
	w->quiet_until = dsp_runtime_next_event_time(p->runtime);
	if (w->waiters != 0)
		qemu_cond_broadcast(&w->progress);

	advanced = w->want >= 0 && (w->output_added || dsp_limit_locked(w) >= w->want);
	w->output_added = false;
	if (advanced) {
		w->want = -1;
		icount2_limit_advanced();
	}
}

static void dsp_worker_sleep(dsp_state_t *p, int64_t now, int64_t event_time, bool arm_sleeping) {
	dsp_worker_t *w = &p->worker;

	if (arm_sleeping) {
		int64_t wake = MIN(dsp_runtime_next_event_time(p->runtime), event_time);
		int64_t delay = icount2_get_horizon_delay(MAX(wake, now + DSP_SLEEP_STEP_NS));

		if (delay < 0)
			return;
		/*
		 * Delivering an output kicks the worker, and so does a vCPU that
		 * wants to go further; the timer only paces a DSP that has work.
		 */
		if (wake != INT64_MAX && dsp_first_output_time(w) > now) {
			timer_mod(p->wake_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
				(delay == 0 ? DSP_SLEEP_STEP_NS : delay));
		}
	} else {
		int64_t spin_end = get_clock() + DSP_SPIN_NS;

		qemu_mutex_unlock(&w->mutex);
		while (!qatomic_read(&w->kicked) && dsp_horizon() < now + DSP_MIN_STEP_NS && get_clock() < spin_end)
			cpu_relax();
		qemu_mutex_lock(&w->mutex);
		if (dsp_horizon() >= now + DSP_MIN_STEP_NS)
			return;
	}

	while (!w->kicked && w->running && !w->stop)
		qemu_cond_wait(&w->cond, &w->mutex);
}

static void *dsp_worker(void *opaque) {
	dsp_state_t *p = opaque;
	dsp_worker_t *w = &p->worker;

	dsp_runtime_thread_enter();

	qemu_mutex_lock(&w->mutex);
	while (!w->stop) {
		int64_t now = dsp_runtime_get_time(p->runtime);
		int64_t event_time = dsp_first_event_time(w);
		int64_t target;
		bool arm_sleeping;
		bool urgent;

		if (!w->running) {
			if (w->waiters != 0)
				qemu_cond_broadcast(&w->progress);
			qemu_cond_wait(&w->cond, &w->mutex);
			continue;
		}
		w->kicked = false;

		if (event_time <= now) {
			dsp_event_t event = dsp_pop_event(w);

			w->quiet_until = INT64_MIN;
			qemu_mutex_unlock(&w->mutex);
			dsp_apply_event(p, &event);
			qemu_mutex_lock(&w->mutex);
			if (event.type == DSP_EVENT_SHARED_WRITE)
				dsp_shared_write_done(w, &event);
			w->applied++;
			dsp_worker_publish(p);
			continue;
		}

		arm_sleeping = dsp_arm_sleeping();
		target = MIN(MIN(dsp_horizon(), event_time), now + DSP_QUANTUM_NS);
		if (arm_sleeping)
			target = MIN(target, dsp_first_output_time(w));
		urgent = arm_sleeping || w->waiters != 0 || w->want >= 0 || target == event_time;

		if (target > now && (urgent || target - now >= DSP_MIN_STEP_NS)) {
			qemu_mutex_unlock(&w->mutex);
			dsp_runtime_run_until(p->runtime, target);
			qemu_mutex_lock(&w->mutex);
			dsp_worker_publish(p);
			continue;
		}

		dsp_worker_sleep(p, now, event_time, arm_sleeping);
	}
	qemu_mutex_unlock(&w->mutex);

	dsp_runtime_thread_exit();
	return NULL;
}

static void dsp_vm_state_change(void *opaque, bool running, RunState state) {
	dsp_state_t *p = opaque;

	qemu_mutex_lock(&p->worker.mutex);
	p->worker.running = running;
	dsp_worker_kick_locked(&p->worker);
	qemu_mutex_unlock(&p->worker.mutex);

	if (running)
		timer_mod(p->lead_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DSP_QUANTUM_NS);
}

static void dsp_fdsp_changed(void *opaque) {
	dsp_state_t *p = opaque;
	uint32_t fdsp = pmb887x_pll_get_fdsp(p->cgu);

	if (fdsp == p->fdsp)
		return;
	DPRINTF("fDSP: %u -> %u Hz\n", p->fdsp, fdsp);
	p->fdsp = fdsp;
	dsp_post(p, DSP_EVENT_FREQUENCY, 0, 0, fdsp);
}

static void dsp_reset_core(dsp_state_t *p) {
	p->trace_boot_mode = true;
	dsp_post(p, DSP_EVENT_RESET, 0, 0, 0);
	/* The reset leaves the ROM version in the first word, over anything the ARM wrote there before it. */
	dsp_post_shared_write(p, 0, p->rom_version, sizeof(uint16_t));
}

static void dsp_reset_input(void *opaque, int id, int level) {
	if (level)
		dsp_reset_core(opaque);
}

static const char *dsp_boot_command_name(uint16_t command) {
	switch (command) {
		case DSP_BOOT_PLOAD:
			return "PLOAD";
		case DSP_BOOT_DLOAD:
			return "DLOAD";
		case DSP_BOOT_BRANCH:
			return "BRANCH";
		case DSP_BOOT_PREAD:
			return "PREAD";
		case DSP_BOOT_DREAD:
			return "DREAD";
		default:
			return "UNKNOWN";
	}
}

static void dsp_trace_command(dsp_state_t *p, size_t pipe) {
	if (p->trace_boot_mode) {
		uint16_t command = dsp_shared_read_word(p, DSP_BOOT_DATA_OFFSET);
		uint16_t address = dsp_shared_read_word(p, DSP_BOOT_DATA_OFFSET + 1);
		uint16_t words = 0;

		if (command != DSP_BOOT_BRANCH)
			words = dsp_shared_read_word(p, DSP_BOOT_DATA_OFFSET + 2);

		DPRINTF("boot command: %s(%u) address=%04X words=%u\n", dsp_boot_command_name(command), command, address, words);

		if (command == DSP_BOOT_BRANCH)
			p->trace_boot_mode = false;
		return;
	}

	uint16_t offset = DSP_RUNTIME_PIPE_OFFSET + pipe * DSP_RUNTIME_PIPE_STRIDE;
	uint16_t command = dsp_shared_read_word(p, offset);
	DPRINTF("runtime command: pipe=%zu command=%u (0x%04X)\n", pipe, command, command);
}

static void dsp_interrupt_input(void *opaque, int id, int level) {
	dsp_state_t *p = opaque;

	if (!level)
		return;

	if (pmb887x_trace_log_enabled(PMB887X_TRACE_DSP))
		dsp_trace_command(p, id);
	dsp_post(p, DSP_EVENT_REQUEST, id, 0, 0);
}

static void dsp_input0(void *opaque, int id, int level) {
	dsp_post(opaque, DSP_EVENT_INPUT, 0, level != 0, 0);
}

static void dsp_input1(void *opaque, int id, int level) {
	dsp_post(opaque, DSP_EVENT_INPUT, 1, level != 0, 0);
}

static void dsp_gsm_input(void *opaque, int signal, int level) {
	dsp_state_t *p = opaque;
	dsp_post(p, DSP_EVENT_GSM_SIGNAL, signal, level != 0, clock_get_hz(p->gsm_clock));
}

static uint64_t dsp_io_read(void *opaque, hwaddr haddr, unsigned size) {
	dsp_state_t *p = opaque;
	uint64_t value = 0;

	switch (haddr) {
		case DSP_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case DSP_ID:
			value = 0xF022C000 | p->revision;
			break;

		case DSP_COM_STATUS:
			dsp_sync_exact(p);
			value = dsp_runtime_get_comm(p->runtime);
			break;

		case DSP_COM_SET:
		case DSP_COM_CLEAR:
		case DSP_SEM_SET:
		case DSP_SEM_CLEAR:
			break;

		case DSP_SEM_STATUS:
			dsp_sync_exact(p);
			value = dsp_runtime_get_mcu_semaphores(p->runtime);
			break;

		default:
			IO_DUMP_READ(haddr + p->mmio.addr, size, 0xFFFFFFFF);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}

	IO_DUMP_READ(haddr + p->mmio.addr, size, value);
	return value;
}

static void dsp_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	dsp_state_t *p = opaque;

	IO_DUMP_WRITE(haddr + p->mmio.addr, size, value);

	switch (haddr) {
		case DSP_CLC:
			pmb887x_clc_set(&p->clc, value);
			dsp_post(p, DSP_EVENT_CLOCK, 0, pmb887x_clc_is_enabled(&p->clc), 0);
			break;

		case DSP_COM_SET:
			dsp_post(p, DSP_EVENT_COM_SET, 0, value & DSP_COM_SET_FLAGS, 0);
			break;

		case DSP_COM_CLEAR:
			dsp_post(p, DSP_EVENT_COM_CLEAR, 0, value & DSP_COM_CLEAR_FLAGS, 0);
			break;

		case DSP_SEM_SET:
			dsp_post(p, DSP_EVENT_SEM_SET, 0, value & DSP_SEM_SET_FLAGS, 0);
			break;

		case DSP_SEM_CLEAR:
			dsp_post(p, DSP_EVENT_SEM_CLEAR, 0, value & DSP_SEM_CLEAR_FLAGS, 0);
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}
}

static const MemoryRegionOps io_ops = {
	.read			= dsp_io_read,
	.write			= dsp_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 4,
		.max_access_size	= 4,
	},
};

static uint64_t dsp_ram_read(void *opaque, hwaddr haddr, unsigned size) {
	dsp_state_t *p = opaque;
	uint64_t value = 0;

	if (pmb887x_clc_is_enabled(&p->clc)) {
		dsp_sync_exact(p);
		value = dsp_shared_read(p, haddr, size);
	}

	IO_DUMP_READ(haddr + p->mmio.addr + DSP_RAM0, size, value);
	return value;
}

static void dsp_ram_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	dsp_state_t *p = opaque;

	IO_DUMP_WRITE(haddr + p->mmio.addr + DSP_RAM0, size, value);

	if (!pmb887x_clc_is_enabled(&p->clc))
		return;

	dsp_post_shared_write(p, haddr, value, size);
}

static const MemoryRegionOps ram_io_ops = {
	.read			= dsp_ram_read,
	.write			= dsp_ram_write,
	.endianness		= DEVICE_LITTLE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4,
		.unaligned		= true,
	},
	.impl			= {
		.min_access_size	= 1,
		.max_access_size	= 4,
		.unaligned		= true,
	},
};

static void dsp_init(Object *obj) {
	dsp_state_t *p = PMB887X_DSP(obj);
	p->ssc_bus = ssi_create_bus(DEVICE(obj), DSP_SSC_BUS_NAME);
	memory_region_init(&p->mmio, obj, "pmb887x-dsp", DSP_IO_SIZE);
	memory_region_init_io(&p->regs, obj, &io_ops, p, "pmb887x-dsp-regs", DSP_RAM0);
	memory_region_init_io(&p->ram, obj, &ram_io_ops, p, "pmb887x-dsp-ram", DSP_RAM_SIZE);
	memory_region_add_subregion(&p->mmio, 0, &p->regs);
	memory_region_add_subregion(&p->mmio, DSP_RAM0, &p->ram);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_reset_input, "RESET_IN", 1);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_interrupt_input, "INT_IN", PMB887X_DSP_INT_COUNT);
	qdev_init_gpio_out_named(DEVICE(obj), p->mcu_interrupts, "INT_OUT", ARRAY_SIZE(p->mcu_interrupts));
	qdev_init_gpio_in_named(DEVICE(obj), dsp_input0, "DSPIN0_IN", 1);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_input1, "DSPIN1_IN", 1);
	qdev_init_gpio_in_named(DEVICE(obj), dsp_gsm_input, "GSM_IN", PMB887X_DSP_GSM_SIGNAL_COUNT);
	p->gsm_clock = qdev_init_clock_in(DEVICE(obj), "GSM_CLOCK", NULL, p, 0);
	qdev_init_gpio_out_named(DEVICE(obj), &p->outputs[0], "DSPOUT0_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &p->outputs[1], "DSPOUT1_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &p->outputs[2], "DSPOUT2_OUT", 1);
}

static void dsp_reset(DeviceState *dev) {
	dsp_state_t *p = PMB887X_DSP(dev);

	qemu_mutex_lock(&p->worker.mutex);
	g_array_set_size(p->worker.outputs, 0);
	qemu_mutex_unlock(&p->worker.mutex);
	for (size_t i = 0; i < ARRAY_SIZE(p->outputs); i++)
		qemu_irq_lower(p->outputs[i]);

	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	dsp_post(p, DSP_EVENT_CLOCK, 0, false, 0);
	dsp_reset_core(p);
}

void pmb887x_dsp_set_config(DeviceState *dev, const pmb887x_dsp_config_t *config) {
	dsp_state_t *p = PMB887X_DSP(dev);
	p->config = config;
}

static const Property dsp_properties[] = {
	DEFINE_PROP_UINT32("revision", dsp_state_t, revision, 0),
	DEFINE_PROP_UINT32("rom_version", dsp_state_t, rom_version, 0),
	DEFINE_PROP_LINK("bus_ssc", dsp_state_t, ssc_bus, "SSI", SSIBus *),
	DEFINE_PROP_LINK("cgu", dsp_state_t, cgu, "pmb887x-cgu", pmb887x_cgu_t *),
};

static void dsp_realize(DeviceState *dev, Error **errp) {
	dsp_state_t *p = PMB887X_DSP(dev);
	const pmb887x_dsp_config_t *config;
	const pmb887x_dsp_rom_t *rom;
	size_t shared_ram_size;

	if (p->config == NULL) {
		error_setg(errp, "DSP configuration is not set");
		return;
	}

	config = p->config;

	shared_ram_size = config->shared_size * sizeof(uint16_t);
	memory_region_set_size(&p->ram, shared_ram_size);
	memory_region_set_size(&p->mmio, DSP_RAM0 + shared_ram_size);
	if (p->rom_version == 0)
		p->rom_version = config->default_rom_version;

	rom = pmb887x_dsp_rom_find(p->rom_version);
	if (rom == NULL) {
		error_setg(errp, "DSP MASK ROM version %04X is not embedded", p->rom_version);
		return;
	}

	if (p->cgu == NULL) {
		error_setg(errp, "DSP clock generator is not set");
		return;
	}

	p->runtime = dsp_runtime_create(config, p->rom_version, rom->program_rom, rom->data_rom,
		p, dsp_worker_events_changed, dsp_ssc_transfer);

	qemu_mutex_init(&p->worker.mutex);
	qemu_cond_init(&p->worker.cond);
	qemu_cond_init(&p->worker.progress);
	p->worker.events = g_array_new(false, false, sizeof(dsp_event_t));
	p->worker.outputs = g_array_new(false, false, sizeof(dsp_output_t));
	p->worker.shared_pending = g_new0(uint8_t, shared_ram_size);
	p->worker.shared_pending_count = g_new0(uint32_t, shared_ram_size);
	p->worker.quiet_until = INT64_MIN;
	p->worker.want = -1;
	p->delivery_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dsp_delivery_timer, p);
	p->lead_timer = timer_new_full(NULL, QEMU_CLOCK_VIRTUAL, SCALE_NS, QEMU_TIMER_ATTR_EXTERNAL, dsp_lead_timer, p);
	p->wake_timer = timer_new_ns(QEMU_CLOCK_REALTIME, dsp_wake_timer, p);
	icount2_set_limit(dsp_icount_limit, p);
	qemu_thread_create(&p->worker.thread, "pmb887x-dsp", dsp_worker, p, QEMU_THREAD_JOINABLE);
	p->worker.created = true;

	p->vmstate = qdev_add_vm_change_state_handler(dev, dsp_vm_state_change, NULL, p);
	pmb887x_pll_add_freq_update_callback(p->cgu, dsp_fdsp_changed, p);
	dsp_fdsp_changed(p);
	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	dsp_reset_core(p);
	DPRINTF("core initialized: cpu=%s revision=%02X rom_version=%04X\n", config->name, p->revision, p->rom_version);
}

static void dsp_unrealize(DeviceState *dev) {
	dsp_state_t *p = PMB887X_DSP(dev);

	qemu_del_vm_change_state_handler(p->vmstate);
	p->vmstate = NULL;
	if (p->worker.created) {
		qemu_mutex_lock(&p->worker.mutex);
		p->worker.stop = true;
		dsp_worker_kick_locked(&p->worker);
		qemu_cond_broadcast(&p->worker.progress);
		qemu_mutex_unlock(&p->worker.mutex);
		qemu_thread_join(&p->worker.thread);
		p->worker.created = false;
		icount2_set_limit(NULL, NULL);
		timer_free(p->delivery_timer);
		timer_free(p->lead_timer);
		timer_free(p->wake_timer);
		g_array_free(p->worker.events, true);
		g_array_free(p->worker.outputs, true);
		g_free(p->worker.shared_pending);
		g_free(p->worker.shared_pending_count);
		qemu_cond_destroy(&p->worker.progress);
		qemu_cond_destroy(&p->worker.cond);
		qemu_mutex_destroy(&p->worker.mutex);
	}

	dsp_runtime_destroy(p->runtime);
	p->runtime = NULL;
}

static void dsp_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, dsp_properties);
	device_class_set_legacy_reset(dc, dsp_reset);
	dc->realize = dsp_realize;
	dc->unrealize = dsp_unrealize;
}

static const TypeInfo dsp_info = {
	.name			= TYPE_PMB887X_DSP,
	.parent			= TYPE_SYS_BUS_DEVICE,
	.instance_size	= sizeof(struct dsp_state_t),
	.instance_init	= dsp_init,
	.class_init		= dsp_class_init,
};

static void dsp_register_types(void) {
	type_register_static(&dsp_info);
}
type_init(dsp_register_types)

#endif
