/*
 * DSP
 */
#define PMB887X_TRACE_ID		DSP
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp"

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include <math.h>
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
#include "hw/arm/pmb887x/board/startup.h"

#include "hw/arm/pmb887x/dsp/runtime.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/gen/dsp_rom.h"
#include "hw/arm/pmb887x/dsp.h"
#include "hw/arm/pmb887x/dsp/config.h"
#include "hw/arm/pmb887x/dsp/peripheral/internal.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define DSP_RAM_SIZE		(DSP_IO_SIZE - DSP_RAM0)
#define DSP_BOOT_DATA_OFFSET	2
#define DSP_RUNTIME_PIPE_OFFSET	5
#define DSP_RUNTIME_PIPE_STRIDE	0x1C
#define DSP_OUTPUT_COUNT	3
#define DSP_BASEBAND_SYNC_TIMEOUT_MS	50
#define DSP_BASEBAND_SPIN_NS	(100 * SCALE_US)
#define DSP_BASEBAND_IRQ_MASK	(TEAK_INT_FINTA0_BBHI | TEAK_INT_FINTA0_BBLO | TEAK_INT_FINTA0_BB_FULL)
#define DSP_COMM_SYNC_TIMEOUT_MS	50
#define DSP_COMM_SPIN_NS	(100 * SCALE_US)
#define DSP_SSC_BUS_NAME	"pmb887x-dsp-ssc"
#define TYPE_PMB887X_DSP	"pmb887x-dsp"
#define PMB887X_DSP(obj)	OBJECT_CHECK(dsp_state_t, (obj), TYPE_PMB887X_DSP)

/* 1 = HLE stub DSP, 0 = LLE (real Teak core + mask ROM firmware). */
#ifndef STUB_DSP
#define STUB_DSP 1
#endif
#define DSP_RAMTAP_WRITE	0
#define DSP_RAMTAP_READ		1
#define DSP_RAMTAP_MARK		2
#define DSP_RAMTAP_COM_READ	3
#define DSP_RAMTAP_COM_WRITE	4
#define DSP_RAMTAP_READ_PC	5
#define DSP_RAMTAP_WRITE_PC	6
#define DSP_RAMTAP_COM_READ_PC	7
#define DSP_RAMTAP_COM_WRITE_PC	8
/* The staging copier is a shared halfword memmove, so its own PC names nothing.
 * LR names the caller that is actually staging the block. */
#define DSP_RAMTAP_WRITE_LR	9

/*
 * Reads are far too frequent to log in full, so keep the most recent events in
 * a ring and dump them at exit: the interesting window is the steady-state
 * polling while playback is running.
 */
#define DSP_RAMTAP_RING		(4u << 20)

typedef struct {
	uint8_t kind;
	uint8_t size;
	uint32_t offset;
	uint32_t value;
} dsp_ramtap_entry_t;

static dsp_ramtap_entry_t *dsp_ramtap_ring;
static uint64_t dsp_ramtap_seq;
static const char *dsp_ramtap_path;
static bool dsp_ramtap_writes_only;
static bool dsp_ramtap_allpc;

/*
 * Sampling profiler: PMB887X_PCPROF=<path> histograms the ARM PC once per
 * millisecond, which is the only way to see where a stalled firmware path is
 * spinning when it touches neither shared RAM nor SDRAM. Run QEMU with
 * -d nochain, or env.regs[15] goes stale inside chained TB loops and the
 * histogram collapses onto a handful of TB entry points.
 */
static GHashTable *pcprof_hist;
static QEMUTimer *pcprof_timer;
static const char *pcprof_path;
static bool pcprof_stack;
static const char *pcprof_stackdump;
static uint32_t pcprof_stackdump_pc;
static uint32_t pcprof_stackdump_lr;

static void pcprof_dump(void) {
	GList *keys, *it;
	FILE *out;

	if (!pcprof_hist || !pcprof_path)
		return;
	out = fopen(pcprof_path, "w");
	if (!out)
		return;
	keys = g_hash_table_get_keys(pcprof_hist);
	for (it = keys; it; it = it->next) {
		guint64 key = *(guint64 *) it->data;
		uint64_t *count = g_hash_table_lookup(pcprof_hist, it->data);

		fprintf(out, "%08X %08X %" PRIu64 "\n", (uint32_t) (key >> 32),
			(uint32_t) key, count ? *count : 0);
	}
	g_list_free(keys);
	fclose(out);
}

static void pcprof_tick(void *opaque) {
	if (first_cpu) {
		CPUARMState *env = &ARM_CPU(first_cpu)->env;
		/* The hot delay helpers are leaves, so LR names the caller that is
		 * actually spending the time. */
		uint32_t caller = 0;
		guint64 key;
		uint64_t *count;

		/*
		 * The hot loop is entered by a branch, not a call, so LR only names
		 * its own leaf callee. Its frame ([sp+4] after `push {r4, lr}`) holds
		 * whoever branched in.
		 */
		if (pcprof_stack)
			cpu_memory_rw_debug(first_cpu, env->regs[13] + 4,
				(uint8_t *) &caller, 4, false);
		key = pcprof_stack
			? ((guint64) env->regs[15] << 32) | caller
			: ((guint64) env->regs[15] << 32) | env->regs[14];
		count = g_hash_table_lookup(pcprof_hist, &key);

		if (count) {
			(*count)++;
		} else {
			guint64 *stored = g_new(guint64, 1);

			count = g_new0(uint64_t, 1);
			*stored = key;
			*count = 1;
			g_hash_table_insert(pcprof_hist, stored, count);
		}
		if (pcprof_stackdump && env->regs[15] == pcprof_stackdump_pc &&
				(!pcprof_stackdump_lr || env->regs[14] == pcprof_stackdump_lr)) {
			FILE *out = fopen(pcprof_stackdump, "w");

			if (out) {
				uint32_t sp = env->regs[13];
				uint8_t buf[1024];

				fprintf(out, "pc=%08X lr=%08X sp=%08X\n",
					env->regs[15], env->regs[14], sp);
				for (int r = 0; r < 13; r++)
					fprintf(out, "r%-2d=%08X\n", r, env->regs[r]);
				cpu_memory_rw_debug(first_cpu, sp, buf, sizeof(buf), false);
				for (unsigned i = 0; i < sizeof(buf); i += 4) {
					uint32_t v;

					memcpy(&v, buf + i, 4);
					fprintf(out, "[sp+%04X] %08X\n", i, v);
				}
				fclose(out);
			}
			pcprof_stackdump = NULL;
		}
		cpu_exit(first_cpu);
	}
	timer_mod(pcprof_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST) + 1000000);
}

static void pcprof_init(void) {
	pcprof_path = getenv("PMB887X_PCPROF");
	if (!pcprof_path)
		return;
	pcprof_stack = getenv("PMB887X_PCPROF_STACK") != NULL;
	pcprof_stackdump = getenv("PMB887X_STACKDUMP");
	if (pcprof_stackdump) {
		const char *pc = getenv("PMB887X_STACKDUMP_PC");

		const char *lr = getenv("PMB887X_STACKDUMP_LR");

		pcprof_stackdump_pc = pc ? strtoul(pc, NULL, 16) : 0x0008E048;
		pcprof_stackdump_lr = lr ? strtoul(lr, NULL, 16) : 0;
	}
	pcprof_hist = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	pcprof_timer = timer_new_ns(QEMU_CLOCK_HOST, pcprof_tick, NULL);
	timer_mod(pcprof_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST) + 1000000);
	atexit(pcprof_dump);
}

/*
 * PMB887X_DSP_COMM_TRACE=1 logs every communication-flag edge from both sides.
 * The flags are the media player's whole hand-off protocol, so which side
 * clears which bit, and when, is the ground truth the HLE stub has to match.
 */
static bool dsp_comm_trace_enabled(void) {
	static int enabled = -1;

	if (enabled < 0)
		enabled = getenv("PMB887X_DSP_COMM_TRACE") != NULL;
	return enabled != 0;
}

static void dsp_ramtap_dump(void) {
	FILE *tap;
	uint64_t total, start;

	if (!dsp_ramtap_ring || !dsp_ramtap_path)
		return;
	tap = fopen(dsp_ramtap_path, "wb");
	if (!tap)
		return;

	total = dsp_ramtap_seq;
	start = total > DSP_RAMTAP_RING ? total - DSP_RAMTAP_RING : 0;
	for (uint64_t i = start; i < total; i++) {
		const dsp_ramtap_entry_t *entry = &dsp_ramtap_ring[i % DSP_RAMTAP_RING];
		uint8_t record[10];

		record[0] = entry->kind;
		record[1] = entry->offset & 0xFF;
		record[2] = (entry->offset >> 8) & 0xFF;
		record[3] = (entry->offset >> 16) & 0xFF;
		record[4] = (entry->offset >> 24) & 0xFF;
		record[5] = entry->size;
		for (unsigned b = 0; b < 4; b++)
			record[6 + b] = (entry->value >> (8 * b)) & 0xFF;
		fwrite(record, 1, 10, tap);
	}
	fclose(tap);
}

static void dsp_ramtap(uint8_t kind, uint32_t offset, uint32_t value, unsigned size) {
	static int tried;
	dsp_ramtap_entry_t *entry;

	if (!tried) {
		tried = 1;
		dsp_ramtap_path = getenv("PMB887X_DSP_RAMTAP");
		dsp_ramtap_writes_only = getenv("PMB887X_DSP_RAMTAP_WRITES") != NULL;
		dsp_ramtap_allpc = getenv("PMB887X_DSP_RAMTAP_ALLPC") != NULL;
		if (dsp_ramtap_path) {
			dsp_ramtap_ring = g_new0(dsp_ramtap_entry_t, DSP_RAMTAP_RING);
			atexit(dsp_ramtap_dump);
		}
	}
	if (!dsp_ramtap_ring)
		return;
	/* Reads outnumber writes by ~1000:1 and would wrap the ring before the
	 * interesting submit-time writes are ever read back. */
	if (dsp_ramtap_writes_only && (kind == DSP_RAMTAP_READ || kind == DSP_RAMTAP_READ_PC))
		return;

	entry = &dsp_ramtap_ring[dsp_ramtap_seq++ % DSP_RAMTAP_RING];
	entry->kind = kind;
	entry->size = size;
	entry->offset = offset;
	entry->value = value;
}

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
#define DSP_COM_MP_REQUEST_BIT	0x400	/* ARM-raised request; DSP clears it on accept */

/*
 * PMB887X_DSP_EXP=<hex> switches off individually speculative HLE behaviours.
 * Several DSP->ARM interrupt pulses here were guessed rather than derived, and
 * a spurious one makes the firmware's AudioRouting task receive a message it
 * does not expect, which it treats as fatal ("AudioRouting_MsgProc: [%x]=
 * Unexpected message ID") and halts the phone.
 */
#define DSP_EXP_NO_CF10		0x01
#define DSP_EXP_NO_CF10_INT	0x02
#define DSP_EXP_NO_MPTIMER	0x04
#define DSP_EXP_NO_PACE_INT	0x08
#define DSP_EXP_NO_SUBMIT_INT	0x10
#define DSP_EXP_NO_DEFAULT_INT	0x20
#define DSP_EXP_NO_DRAIN_INT	0x40
/* Opt-in experiment: treat the CF10 ring as PCM (it is not; see PROGRESS.md). */
#define DSP_EXP_MP_CONSUME	0x80
/*
 * Candidate hand-backs for the media player's staged block. The ARM stages a
 * 64-word slot whose header word has bit 15 set and then waits; which signal
 * releases it is still unknown, so both are switchable.
 */
#define DSP_EXP_MP_HDR_ACK	0x100	/* drop bit 15 of the staged slot header */
#define DSP_EXP_MP_CF10_ACK	0x200	/* deferred CF10 clear, media player only */
/* Do not run the ADPCM chunk engine for the media player's linear stream. */
#define DSP_EXP_MP_NO_ADPCM	0x400
/* Answer each CF10 request by filling the 56-word window the ARM reads. */
#define DSP_EXP_MP_WINDOW	0x800
/*
 * Consume the block when its header is written rather than when CF10 is
 * raised. The stager writes the payload first and the header last, so the
 * header write is the only instant at which count and samples are coherent;
 * by CF10 time the mixer has often staged a different, shorter block.
 */
#define DSP_EXP_MP_ON_HEADER	0x1000
/*
 * After taking a block, ask for the next one. The ARM polls COM_STATUS & 0x7F00
 * (0xA09A33E6) for the DSP's "audio status", so a DSP-raised flag in 8..14 is
 * how the real part would say it wants more; nothing in the stub ever sets one
 * for the media path, which may be why the renderer stops after one burst.
 */
#define DSP_EXP_MP_REFILL	0x2000

/* How long the modelled DSP takes to accept a CF10 request. */
#define DSP_CF10_ACK_DELAY_NS	(200 * SCALE_US)

static uint32_t dsp_exp_mask(void) {
	static uint32_t mask;
	static bool initialized;

	if (!initialized) {
		const char *value = getenv("PMB887X_DSP_EXP");

		mask = value ? (uint32_t) strtoul(value, NULL, 16) : 0;
		initialized = true;
		if (mask)
			fprintf(stderr, "[pmb887x-dsp] experiment mask %02X\n", mask);
	}
	return mask;
}
#define DSP_COM_PIPE2_BIT	0x4	/* runtime pipe: command pending (ARM sets, DSP clears on consume) */
#define DSP_COM_BUSY_BIT	0x8	/* DSP-owned: runtime pipe busy (a command block is still buffered) */
#define DSP_COM_OVERRUN_BIT	0x10	/* DSP-owned: a runtime command arrived while the pipe was still busy */

/*
 * Siemens (SGold2 x75/elka family) PCM player protocol, recovered from the
 * S75 v40 ARM firmware and the mask ROM map in bsp/rom/dsp/0801.md:
 *
 * - Runtime pipe 2 command 45 (PCMPLAY) uses a different ABI than the LG
 *   phones: SWITCH is a bit mask (bit 8 = start, bit 9 = stop; bit 6 is
 *   always set by the ADPCM player) and PAR3 encodes the sample rate as
 *   rate/16 + 1 (0x3E9 -> 16000 Hz; 1 on the stop command).
 * - The ARM does not feed data through PCMPLAY sub-commands. It copies a
 *   0x60-word chunk of the source stream into shared RAM at word 0x57B and
 *   raises communication flag 9 (0x200). The DSP consumes the chunk, clears
 *   flag 9, and asks for the next one by raising an MCU interrupt together
 *   with an "audio status" bit (communication flags 8..14).
 * - The streamed data is IMA ADPCM (16 kHz mono for the boot jingle) in
 *   48-byte blocks: a 4-byte header (predictor, step index, padding) followed
 *   by 44 data bytes (two samples per byte, high nibble first), split across
 *   the submitted chunks. The stream starts with a block header at byte 0.
 */
#define DSP_SIEMENS_SWITCH_START	0x100
#define DSP_SIEMENS_SWITCH_STOP		0x200
/*
 * Recovered from the command pipe: the boot chime starts with SWITCH 0x0140
 * and the media player with 0x0500. Bit 6 is the ADPCM player; the media
 * player leaves it clear and streams linear PCM instead.
 */
#define DSP_SIEMENS_SWITCH_ADPCM	0x040
#define DSP_SIEMENS_PCM_BUF_WORD		0x57B
#define DSP_SIEMENS_PCM_WORDS		0x60
#define DSP_SIEMENS_DATA_FLAG		0x200
/*
 * The source stream is IMA ADPCM in 48-byte blocks: a 4-byte header
 * (predictor [16 bit LE], step index, padding) followed by 44 data bytes
 * (two samples per byte, high nibble first). One 0x60-word chunk is 192
 * bytes = exactly four blocks.
 */
#define DSP_SIEMENS_IMA_BLOCK		256	/* standard IMA block size of the source WAV stream */
#define DSP_SIEMENS_ADPCM_BLOCK		48	/* submission sub-block size */
#define DSP_SIEMENS_ADPCM_HEADER		4
#define DSP_SIEMENS_BLOCK_SAMPLES	((DSP_SIEMENS_ADPCM_BLOCK - DSP_SIEMENS_ADPCM_HEADER) * 2)
#define DSP_SIEMENS_CHUNK_SAMPLES	(DSP_SIEMENS_PCM_WORDS * 2) /* upper bound of samples per chunk */
#define DSP_SIEMENS_PACE_PERIOD_NS	(5 * SCALE_MS)
#define DSP_SIEMENS_PACE_CLOCK		QEMU_CLOCK_HOST
/* Media-player sample-based PCM task (armed by PCM_SUBMIT): the ARM copies a
 * 16-bit mono block (1 header word + N samples) into Shared RAM and polls the
 * output region for the DSP result. We feed the samples straight to the AFE
 * and echo them back to the output region to release the per-block pacing. */
#define DSP_SIEMENS_MP_IN_WORD		0x5DB	/* 0xBB6/2, input block start */
#define DSP_SIEMENS_MP_OUT_WORD		0x87	/* 0x10E/2, output region ARM polls */
#define DSP_SIEMENS_MP_MAX		256
/* Staged-block header: bit 15 = ready, low bits = sample count. */
#define DSP_SIEMENS_MP_READY		0x8000
#define DSP_SIEMENS_MP_COUNT_MASK	0x01FF

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
	bool pcm_armed;	int assist_done;
	int afe_forced;
	QEMUTimer *afe_force_timer;
	QEMUTimer *mpfeed_timer;
	QEMUTimer *cf10_timer;
	uint64_t mp_blocks;
	uint32_t mpfeed_pos;
	bool pcm_pending;
	uint16_t pcm_channels;
	uint16_t pcm_len;
	uint16_t pcm_block[DSP_PCM_MAX_WORDS];
	QEMUTimer *refill_timer;

	/* Siemens PCM player state. */
	bool siemens_active;
	/* PCMPLAY SWITCH bit 10 with bit 6 clear: the media player's linear-PCM
	 * stream, as opposed to the boot chime's ADPCM stream (bit 6 set). */
	bool siemens_linear;
	unsigned siemens_submits;
	uint64_t siemens_samples;
	uint32_t siemens_rate;
	QEMUTimer *siemens_timer;
	/* Sample-based PCM task (PCM_SUBMIT): output buffer the ARM polls, and a
	 * wall-clock timer that clears it once the AFE has drained the block. */
	QEMUTimer *mp_timer;
	uint32_t mp_out_w;
	unsigned mp_clears;
	/* The submitted 255-sample block, streamed into the 56-word window. */
	uint16_t mp_block[255];
	uint32_t mp_block_pos;
	uint32_t mp_nonce;
	/* IMA ADPCM decoder state. */
	uint32_t siemens_stream_pos;	/* submission-buffer bytes consumed so far */
	uint32_t siemens_data_pos;	/* stream data bytes decoded (skipping control/pad) */
	int16_t siemens_pred;
	int siemens_index;
	uint8_t siemens_hdr[2];
};

static inline uint16_t dsp_read_word(dsp_state_t *p, uint32_t offset) {
	const uint16_t* mem = (const uint16_t*)&p->ram[0];
	return mem[offset];
}

static void dsp_update_state(dsp_state_t *p) {
	// TODO
}

static void dsp_pcm_disarm(dsp_state_t *p);
static void dsp_siemens_pcm_stop(dsp_state_t *p);

static void dsp_reset_input(void *opaque, int id, int level) {
	if (level) {
		dsp_pcm_disarm(opaque);
		dsp_siemens_pcm_stop(opaque);
	}
}

static void dsp_interrupt_input(void *opaque, int id, int level) {
}

static void dsp_input0(void *opaque, int id, int level) {
}

static void dsp_input1(void *opaque, int id, int level) {
}

static void dsp_gsm_input(void *opaque, int signal, int level) {
}

/*
 * Diagnostic: record shared-RAM traffic as {u8 kind, u32 offset, u8 size,
 * payload} so a host-side scan can locate the compressed-audio bitstream and
 * see which addresses the ARM polls while waiting for the DSP.
 */

static uint32_t dsp_ram_read(dsp_state_t *p, uint32_t offset, unsigned size) {
	uint8_t *data = p->ram;
	uint32_t value;

	switch (size) {
		case 1:		value = data[offset]; break;
		case 2:		value = data[offset] | (data[offset + 1] << 8); break;
		case 4:		value = data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) | (data[offset + 3] << 24); break;
		default:	abort();
	}
	dsp_ramtap(DSP_RAMTAP_READ, offset, value, size);
	if (dsp_ramtap_allpc && current_cpu)
		dsp_ramtap(DSP_RAMTAP_READ_PC, offset, ARM_CPU(current_cpu)->env.regs[15], 4);
	/*
	 * The media-player output window is reached through accessor functions
	 * that compute base + offset, so no literal identifies its reader.
	 * Record the ARM PC instead: that names the code actually polling it.
	 */
	if (offset >= DSP_SIEMENS_MP_OUT_WORD * 2 &&
			offset < (DSP_SIEMENS_MP_OUT_WORD + 65) * 2 && current_cpu)
		dsp_ramtap(DSP_RAMTAP_READ_PC, offset,
			ARM_CPU(current_cpu)->env.regs[15], 4);
	return value;
}

static void dsp_siemens_mp_consume(dsp_state_t *p);

static void dsp_ram_write(dsp_state_t *p, uint32_t offset, uint32_t value, unsigned size) {
	uint8_t *data = p->ram;
	dsp_ramtap(DSP_RAMTAP_WRITE, offset, value, size);
	/* Name the ARM code that builds commands and stages PCM blocks; the
	 * builders use computed addresses, so no literal identifies them. */
	if (current_cpu &&
			((offset >= DSP_CHAN2_CMD_ADDR * 2 && offset < (DSP_CHAN2_CMD_ADDR + 28) * 2) ||
			 (offset >= (DSP_SIEMENS_MP_IN_WORD - 8) * 2 && offset < (DSP_SIEMENS_MP_IN_WORD + 300) * 2))) {
		dsp_ramtap(DSP_RAMTAP_WRITE_PC, offset, ARM_CPU(current_cpu)->env.regs[15], 4);
		dsp_ramtap(DSP_RAMTAP_WRITE_LR, offset, ARM_CPU(current_cpu)->env.regs[14], 4);
	}
	/*
	 * The staging copier is reached through a generic wcopy() wrapper, so both
	 * PC and LR name library code. Walk a few stack words to reach the caller
	 * that actually owns the block.
	 */
	if (getenv("PMB887X_DSP_STAGE_TRACE") != NULL && current_cpu &&
			offset >= (DSP_SIEMENS_MP_IN_WORD + 1) * 2 &&
			offset < (DSP_SIEMENS_MP_IN_WORD + 256) * 2) {
		static unsigned traced;

		CPUARMState *env = &ARM_CPU(current_cpu)->env;

		/* Only the descending copy loop stages real data; the ascending one at
		 * 0xA02BFBDA is the driver's fill and floods this trace otherwise. */
		/*
		 * Log the source pointer, not the destination: whether it walks a long
		 * SDRAM buffer or circles a few words is what says if a real decoded
		 * stream exists behind the mixer. The copy descends, so its final store
		 * lands on the block's first sample word -- one record per staging.
		 */
		if (env->regs[15] == 0xA02BFBCC &&
				offset == (DSP_SIEMENS_MP_IN_WORD + 1) * 2) {
			traced++;
			if (traced <= 40)
				DPRINTF("stage #%u: src=%08X dst=%08X val=%04X\n",
					traced, env->regs[1], env->regs[2], (uint16_t) value);
		}
	}
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

	/* Runs after the store so the header word is already in place. */
	if ((dsp_exp_mask() & DSP_EXP_MP_ON_HEADER) != 0 &&
			offset == DSP_SIEMENS_MP_IN_WORD * 2 &&
			(value & DSP_SIEMENS_MP_READY) != 0 &&
			(value & DSP_SIEMENS_MP_COUNT_MASK) != 0)
		dsp_siemens_mp_consume(p);
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
	if (!afe_audio_has_room(p->afe, p->pcm_len)) {
		DPRINTF("pcm flush: no room (len=%u)\n", p->pcm_len);
		return;
	}
	DPRINTF("pcm flush: pushing %u samples to afe\n", p->pcm_len);

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

static void dsp_afe_force_tick(void *opaque) {
	dsp_state_t *p = opaque;
	if (p->afe != NULL) {
		afe_force_start(p->afe);
		qatomic_set(&p->afe_forced, 1);
	}
	timer_mod(p->afe_force_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + 1000000000LL);
}

/*
 * Take the media player's block request. CF10 is to the media player what CF9
 * is to the boot chime: the ARM raises it once the staged block at w1499 is
 * marked full, and waits for the DSP to take it. It must not be cleared inside
 * the COM_SET write itself -- the firmware reads the flag back to confirm its
 * own write (0xA09A33DA) and would spin forever -- so the clear is deferred.
 */
static void dsp_cf10_ack(void *opaque) {
	dsp_state_t *p = opaque;

	if ((p->com_status & DSP_COM_MP_REQUEST_BIT) == 0)
		return;
	p->com_status &= ~(uint32_t) DSP_COM_MP_REQUEST_BIT;
	if (!(dsp_exp_mask() & DSP_EXP_NO_CF10_INT))
		qemu_irq_pulse(p->mcu_interrupts[2]);
}

static void dsp_mpfeed_tick(void *opaque) {
	dsp_state_t *p = opaque;
	uint16_t *mem = (uint16_t *) &p->ram[0];

	for (unsigned i = 0; i < 56; i++) {
		double phase = 2.0 * G_PI * 1000.0 * (double) p->mpfeed_pos++ / 16000.0;
		mem[DSP_SIEMENS_MP_OUT_WORD + i] = (uint16_t) (int16_t) (12000.0 * sinf(phase));
	}
	/*
	 * Words 0xC0..0xC7 are an 8-slot sample FIFO (ARM side: 0xA0A5B5EA).
	 * Bit 15 is the marker the ARM ORs in when it consumes a slot, and 0xFF80
	 * is the "silence" sentinel, so a supplied sample must be positive and not
	 * 0xFF80 to be taken as real audio.
	 */
	for (unsigned slot = 0; slot < 8; slot++) {
		double phase = 2.0 * G_PI * 1000.0 * (double) (p->mpfeed_pos + slot) / 16000.0;
		uint16_t sample = (uint16_t) (int16_t) (8000.0 * sinf(phase)) & 0x7FFF;
		mem[0xC0 + slot] = sample == 0xFF80 ? 0x0100 : sample;
	}

	timer_mod(p->mpfeed_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST) +
		(int64_t) 56 * NANOSECONDS_PER_SECOND / 16000);
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

/* ------------------------------------------------------------------
 * Siemens PCM player (SGold2 x75/elka family)
 * ------------------------------------------------------------------ */

static const int16_t dsp_ima_step_table[89] = {
		7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34,
		37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
		157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
		544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
	1712, 1884, 2076, 2282, 2511, 2761, 3037, 3341, 3677, 4045, 4447,
		4895, 5385, 5923, 6516, 7168, 7885, 8674, 9540, 10483, 11519,
		12667, 13932, 15320, 16836, 18509, 20350, 22373, 24592, 27041,
		29737, 32700
};

static const int8_t dsp_ima_index_table[16] = {
		-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

static int16_t dsp_ima_decode_nibble(dsp_state_t *p, uint8_t nibble) {
	int32_t step = dsp_ima_step_table[p->siemens_index];
	int32_t diff = step >> 3;

	if (nibble & 4)
		diff += step;
	if (nibble & 2)
		diff += step >> 1;
	if (nibble & 1)
		diff += step >> 2;

	/* Accumulate in 32 bits and clamp: the predictor is 16-bit but the step
	 * can push it past the range for one sample. */
	int32_t pred = (int32_t) p->siemens_pred + ((nibble & 8) ? -diff : diff);
	if (pred > 32767)
		pred = 32767;
	else if (pred < -32768)
		pred = -32768;
	p->siemens_pred = (int16_t) pred;

	p->siemens_index += dsp_ima_index_table[nibble];
	if (p->siemens_index > 88)
		p->siemens_index = 88;
	else if (p->siemens_index < 0)
		p->siemens_index = 0;

	return p->siemens_pred;
}

/*
 * Decode one byte of the source stream. The reconstructed stream is the WAV
 * data payload of the reference chime file: every DSP_SIEMENS_IMA_BLOCK bytes
 * a 4-byte IMA block header (predictor, step index) restarts the decoder;
 * every other byte holds two samples, high nibble first.
 */
static void dsp_siemens_feed_byte(dsp_state_t *p, uint8_t byte, uint16_t **out, size_t *out_left) {
	uint32_t block_pos = p->siemens_data_pos % DSP_SIEMENS_IMA_BLOCK;

	if (block_pos < DSP_SIEMENS_ADPCM_HEADER) {
		/* Accumulate the block header: predictor low, high, then index. */
		switch (block_pos) {
				case 0:	p->siemens_hdr[0] = byte; break;
				case 1:	p->siemens_hdr[1] = byte; break;
				case 2:
					p->siemens_pred = (int16_t) (p->siemens_hdr[0] | (p->siemens_hdr[1] << 8));
					p->siemens_index = byte <= 88 ? byte : 0;
					break;
		}
	} else if (out_left && *out_left >= 2) {
		*(*out)++ = dsp_ima_decode_nibble(p, byte >> 4);
		*(*out)++ = dsp_ima_decode_nibble(p, byte & 0xF);
		(*out_left) -= 2;
	}
	p->siemens_data_pos++;
}

/*
 * Decode a submitted chunk into samples and push them to the audio backend.
 *
 * Submission framing (reverse-engineered against the reference chime,
 * "Siemens on.wav"): the 96-word buffer is four 24-word sub-blocks, each
 * { 2 control words, N data words, zero pad }, where N = word[1] (22, 20, ...
 * seen). The data words (low byte first) concatenate across sub-blocks and
 * chunks into the exact WAV data stream, so the IMA state and the 256-byte
 * block framing carry on across chunk boundaries.
 */
static void dsp_siemens_decode_chunk(dsp_state_t *p, const uint16_t *words, size_t word_count) {
	uint16_t samples[DSP_SIEMENS_CHUNK_SAMPLES];
	uint16_t *out = samples;
	size_t out_left = ARRAY_SIZE(samples);

	for (size_t base = 0; base + 2 <= word_count; base += 24) {
		unsigned data_words = words[base + 1];
		if (data_words > 22)
			data_words = 22;
		for (unsigned w = 0; w < data_words && base + 2 + w < word_count; w++) {
			dsp_siemens_feed_byte(p, words[base + 2 + w] & 0xFF, &out, &out_left);
			dsp_siemens_feed_byte(p, words[base + 2 + w] >> 8, &out, &out_left);
		}
	}

	p->siemens_samples += (uint64_t) (out - samples);
	if (p->afe && out != samples)
		afe_audio_push_samples(p->afe, samples, out - samples);
}

static void dsp_siemens_pcm_stop(dsp_state_t *p) {
	if (!p->siemens_active)
		return;

	p->siemens_active = false;
	p->siemens_linear = false;
	if (p->siemens_timer)
		timer_del(p->siemens_timer);
	p->com_status &= ~(uint32_t) DSP_SIEMENS_DATA_FLAG;
	DPRINTF("siemens pcm: stop (total %llu samples) virt=%llus rt=%.2fs\n",
		(unsigned long long) p->siemens_samples,
		(long long) (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000),
		(double) qemu_clock_get_ns(QEMU_CLOCK_REALTIME) / 1e9);
	/* The boot chime plays at the IDLE; open the media player right after. */
	pmb887x_keyseq_bootchime_stop();
}

static void dsp_siemens_pcm_start(dsp_state_t *p, uint32_t rate) {
	p->siemens_active = true;
	p->siemens_submits = 0;
	p->siemens_samples = 0;
	p->siemens_stream_pos = 0;
	p->siemens_data_pos = 0;
	p->siemens_pred = 0;
	p->siemens_index = 0;
	p->siemens_rate = rate;
	if (p->afe)
		afe_audio_set_format(p->afe, rate, 1);
	if (p->siemens_timer)
		timer_mod(p->siemens_timer, qemu_clock_get_ns(DSP_SIEMENS_PACE_CLOCK) + DSP_SIEMENS_PACE_PERIOD_NS);
	DPRINTF("siemens pcm: start %u Hz virt=%llus rt=%.2fs\n", rate,
		(long long) (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000),
		(double) qemu_clock_get_ns(QEMU_CLOCK_REALTIME) / 1e9);
}

/*
 * The ARM submitted a chunk (communication flag 9). Consume it the way the
 * real DSP does: decode the data into the audio backend and clear the flag.
 * While the audio backend is saturated the flag stays set - that is the
 * backpressure which stops the firmware from overwriting the buffer. The
 * firmware paces itself with a periodic task which only submits a new chunk
 * while flag 9 is clear.
 */
static void dsp_siemens_try_consume(dsp_state_t *p) {
	if (!p->siemens_active || !(p->com_status & DSP_SIEMENS_DATA_FLAG))
		return;

	if (!p->afe || !afe_audio_has_room(p->afe, DSP_SIEMENS_CHUNK_SAMPLES))
		return;

	uint16_t words[DSP_SIEMENS_PCM_WORDS];
	for (unsigned i = 0; i < DSP_SIEMENS_PCM_WORDS; i++)
		words[i] = dsp_read_word(p, DSP_SIEMENS_PCM_BUF_WORD + i);

	if (p->siemens_submits < 6) {
		DPRINTF("siemens pcm: chunk %u:\n", p->siemens_submits + 1);
		for (unsigned wi = 0; wi < DSP_SIEMENS_PCM_WORDS; wi += 12) {
			DPRINTF("  %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X\n",
				words[wi], words[wi+1], words[wi+2], words[wi+3], words[wi+4], words[wi+5],
				words[wi+6], words[wi+7], words[wi+8], words[wi+9], words[wi+10], words[wi+11]);
		}
	}

	dsp_siemens_decode_chunk(p, words, DSP_SIEMENS_PCM_WORDS);
	p->siemens_submits++;
	p->com_status &= ~(uint32_t) DSP_SIEMENS_DATA_FLAG;

	/*
	 * The PCM player tick interrupts the MCU (mask ROM MCU_INT), which makes
	 * the firmware's audio task check for and submit the next chunk.
	 */
	qemu_irq_pulse(p->mcu_interrupts[1]);
	qemu_irq_pulse(p->mcu_interrupts[2]);
}

/*
 * Drain timer: keep consuming chunks while the backend has room, so the
 * stream keeps flowing when the firmware is faster than real time.
 */
static void dsp_siemens_pace_timer(void *opaque) {
	dsp_state_t *p = opaque;

	if (!p->siemens_active)
		return;

	/* Keep the firmware's audio task ticking so it submits the next chunk. */
	if (!(dsp_exp_mask() & DSP_EXP_NO_PACE_INT))
		qemu_irq_pulse(p->mcu_interrupts[1]);
	dsp_siemens_try_consume(p);
	dsp_siemens_try_consume(p);

	timer_mod(p->siemens_timer, qemu_clock_get_ns(DSP_SIEMENS_PACE_CLOCK) + DSP_SIEMENS_PACE_PERIOD_NS);
}

/*
 * The firmware reads the sample-based PCM task's 56-word output window and
 * then polls it until the DSP has drained the block.  Clear the window after
 * the AFE has had wall-clock time to consume the block (255 samples at the
 * task rate) so the firmware sees the "empty" window and submits the next
 * block, keeping the stream flowing in real time.
 */
/* Stream the next 56-sample window of the submitted block into the output
 * buffer.  Once the whole block is streamed, clear the window: that "empty"
 * window is the "consumed" signal that releases the firmware to submit the
 * next block. */
/*
 * Write the 4-word transform/nonce the ARM polls to know a window is fresh.
 * The real DSP emits a fresh random nonce per window; the ARM waits for it to
 * change before consuming the next window.  Derive deterministic random-
 * looking values from a counter.
 */
static void dsp_siemens_mp_write_nonce(dsp_state_t *p) {
	uint16_t *outmem = (uint16_t *) &p->ram[0];
	uint32_t n = ++p->mp_nonce;
	outmem[p->mp_out_w + 61] = (0xB1E2u + n * 2654435761u) & 0xFFFF;
	outmem[p->mp_out_w + 62] = (0xADE7u + n * 40503u) & 0xFFFF;
	outmem[p->mp_out_w + 63] = (0x94D0u + n * 69069u) & 0xFFFF;
	outmem[p->mp_out_w + 64] = (0xAC34u + n * 1315423911u) & 0xFFFF;
}

static void dsp_siemens_mp_drain(void *opaque) {
	dsp_state_t *p = opaque;
	uint16_t *outmem = (uint16_t *) &p->ram[0];
	uint32_t pos = p->mp_block_pos;

	if (pos + 56 <= 255) {
		for (uint32_t i = 0; i < 56; i++)
			outmem[p->mp_out_w + i] = p->mp_block[pos + i];
		outmem[p->mp_out_w + 56] = 4;
		dsp_siemens_mp_write_nonce(p);
		p->mp_block_pos = pos + 56;
		/* Re-arm: next window after one window's worth of wall-clock time. */
		int64_t period_ns = (int64_t) 56 * NANOSECONDS_PER_SECOND /
			(p->siemens_rate ? p->siemens_rate : 16000);
		timer_mod(p->mp_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST) + period_ns);
		return;
	}
	/* Block fully streamed: clear the window (the "consumed" signal) and
	 * tick the MCU so its audio task submits the next block. */
	for (uint32_t i = 0; i < 56; i++)
		outmem[p->mp_out_w + i] = 0;
	p->mp_block_pos = 0;
	p->mp_clears++;
	if (!(dsp_exp_mask() & DSP_EXP_NO_DRAIN_INT)) {
		qemu_irq_pulse(p->mcu_interrupts[2]);
		qemu_irq_pulse(p->mcu_interrupts[1]);
	}
	if (p->mp_clears <= 8) {
		DPRINTF("siemens mp: block done (window cleared) virt=%llus\n",
			(long long) (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000));
	}
}

/*
 * Media-player block hand-off. The player stages one 255-sample PCM block at
 * DSP_SIEMENS_MP_IN_WORD and raises comm flag 10 for every block it produces;
 * only the first block or two also arrive as an explicit PCM_SUBMIT command.
 * Listening for the command alone therefore drops the whole stream after the
 * first block: CF10 is to the media player what CF9 is to the boot chime.
 */
/*
 * Samples per staged block. The header's low byte only carries a usable count
 * for the priming block (0x80FF); in the streaming state it reads B800/0A00/
 * 1801, so the length has to come from elsewhere. PMB887X_DSP_MP_COUNT=<n>
 * overrides it while the real layout is being pinned down.
 */
static uint32_t dsp_mp_count_override(void) {
	static uint32_t count;
	static bool initialized;

	if (!initialized) {
		const char *value = getenv("PMB887X_DSP_MP_COUNT");

		count = value ? (uint32_t) strtoul(value, NULL, 0) : 0;
		initialized = true;
	}
	return count;
}

/*
 * Fill the 56-word window the ARM reads after every CF10 request, streaming
 * the staged block through it a window at a time and wrapping at its end.
 */
static void dsp_siemens_mp_window(dsp_state_t *p) {
	uint16_t *outmem = (uint16_t *) &p->ram[0];
	uint32_t pos = p->mp_block_pos;

	for (uint32_t i = 0; i < 56; i++) {
		uint32_t src = pos + i;

		outmem[DSP_SIEMENS_MP_OUT_WORD + i] =
			src < ARRAY_SIZE(p->mp_block) ? p->mp_block[src] : 0;
	}
	pos += 56;
	p->mp_block_pos = pos + 56 <= ARRAY_SIZE(p->mp_block) ? pos : 0;
	p->mp_out_w = DSP_SIEMENS_MP_OUT_WORD;
	dsp_siemens_mp_write_nonce(p);
}

static void dsp_siemens_mp_consume(dsp_state_t *p) {
	uint32_t in_w = DSP_SIEMENS_MP_IN_WORD;
	uint16_t header = dsp_read_word(p, in_w);
	uint32_t count = dsp_mp_count_override() ? dsp_mp_count_override()
					       : (header & DSP_SIEMENS_MP_COUNT_MASK);
	uint16_t block[DSP_SIEMENS_MP_MAX];

	/*
	 * The stager at 0xA030EF7C copies `count` samples to slot+2 and only then
	 * writes the header at slot+0 (0xA030EF82), so the header is the sample
	 * count with bit 15 as the ready flag. The synth writes 0xB800 into the
	 * same word when it has nothing to play (0xA030EE8C, 0x17 << 11), which is
	 * "ready, zero samples" -- consuming a fixed 255 words on that marker is
	 * what turned the stream into clipping noise.
	 */
	if ((header & DSP_SIEMENS_MP_READY) == 0 || count == 0)
		return;
	/* A slot holds at most 255 samples, so a larger count is not a block at
	 * all: 0xFFFF is the driver's "slot cleared" marker. */
	if (count > DSP_SIEMENS_MP_MAX - 1)
		return;
	if (in_w + count + 1 > DSP_RAM_SIZE / 2)
		count = DSP_RAM_SIZE / 2 - in_w - 1;

	for (uint32_t i = 0; i < count; i++)
		block[i] = dsp_read_word(p, in_w + 1 + i);

	if (!p->afe || !p->afe_started)
		return;

	afe_audio_push_samples(p->afe, block, count);
	/*
	 * PMB887X_DSP_MP_DUMP=<path> appends every consumed block. The audio device
	 * runs in real time and fills underruns with silence, so if the emulated
	 * ARM decodes slower than real time the captured WAV is shredded even when
	 * the samples are perfectly good. Concatenating the blocks tells the two
	 * apart: coherent audio here means decode works and only pacing is wrong.
	 */
	{
		static FILE *dump;
		static int tried;

		if (!tried) {
			const char *path = getenv("PMB887X_DSP_MP_DUMP");

			tried = 1;
			if (path)
				dump = fopen(path, "wb");
		}
		if (dump) {
			fwrite(block, sizeof(block[0]), count, dump);
			fflush(dump);
		}
	}
	if ((dsp_exp_mask() & DSP_EXP_MP_HDR_ACK) != 0) {
		uint16_t *mem = (uint16_t *) &p->ram[0];

		mem[in_w] = header & (uint16_t) ~0x8000;
	}
	p->siemens_samples += count;
	p->mp_blocks++;
	if ((dsp_exp_mask() & DSP_EXP_MP_REFILL) != 0)
		dsp_pcm_request_refill(p);
	if (p->mp_blocks <= 3 || (p->mp_blocks % 200) == 0) {
		DPRINTF("media block %llu: %u samples @%u Hz hdr=%04X data=%04X %04X %04X %04X\n",
			(unsigned long long) p->mp_blocks, count,
			p->siemens_rate ? p->siemens_rate : 16000, header,
			block[0], block[1], block[2], block[3]);
	}
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
	DPRINTF("pcm submit: %u samples\n", len);
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

/*
 * Runtime-command acknowledgement: on acceptance the DSP dispatcher replaces
 * pipe[0] with the one's complement of the command (-command); on rejection
 * it writes zero (see bsp/rom/dsp/0801.md). The ARM polls this word to see
 * whether the command was accepted.
 */
static void dsp_ack_command(dsp_state_t *p, uint32_t pipe_addr, uint16_t id, bool accepted) {
	((uint16_t *) p->ram)[pipe_addr] = accepted ? (uint16_t) -id : 0;
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

	dsp_ramtap(DSP_RAMTAP_MARK, id, 2, 2);

	switch (id) {
		case DSP_CMD_VB_ON:
			p->afe_started = true;
			break;

		case DSP_CMD_PCMPLAY: {
			uint16_t sw = dsp_read_word(p, DSP_CHAN2_CMD_ADDR + 1);

			if (sw == DSP_PCMPLAY_SWITCH_INIT) {
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
			} else if (sw & DSP_SIEMENS_SWITCH_START) {
				/* Siemens ABI: SWITCH is a bit mask, PAR3 = rate/16 + 1. */
				uint16_t rate_word = dsp_read_word(p, DSP_CHAN2_CMD_ADDR + 4);
				uint32_t rate = rate_word > 1 ? (uint32_t) (rate_word - 1) * 16 : 0;

				p->afe_started = true;
				dsp_siemens_pcm_start(p, rate ? rate : 16000);
				p->siemens_linear = (sw & DSP_SIEMENS_SWITCH_ADPCM) == 0;
				if (p->siemens_linear && (dsp_exp_mask() & DSP_EXP_MP_NO_ADPCM) != 0 &&
						p->siemens_timer)
					timer_del(p->siemens_timer);
				DPRINTF("siemens pcm: start switch=%04X (%s) %u Hz\n", sw,
					p->siemens_linear ? "linear/media" : "adpcm/chime",
					rate ? rate : 16000);
				print = false;
			} else if (sw & DSP_SIEMENS_SWITCH_STOP) {
				dsp_siemens_pcm_stop(p);
				print = false;
			} else {
				DPRINTF("unknown pcmplay subcommand! 0x%x\n", sw);
			}
			break;
		}

		case DSP_CMD_PCM_SUBMIT: {
			/* Sample-based PCM task (media player / voice path). The ARM has
			 * copied a PCM block into Shared RAM: a header word (0x8000 | count)
			 * followed by 16-bit mono samples. Feed the samples to the AFE and
			 * write them back to the output region the ARM polls, which releases
			 * the per-block pacing so the next block gets submitted. */
			uint32_t in_w  = DSP_SIEMENS_MP_IN_WORD;
			uint32_t out_w = DSP_SIEMENS_MP_OUT_WORD;
			uint32_t count = (uint32_t) (dsp_read_word(p, in_w) & 0xFF);
			if (count == 0)
				count = DSP_SIEMENS_MP_MAX - 1;
			if (count > DSP_SIEMENS_MP_MAX - 1)
				count = DSP_SIEMENS_MP_MAX - 1;
			if (in_w + count + 1 > DSP_RAM_SIZE / 2)
				count = DSP_RAM_SIZE / 2 - in_w - 1;
			uint16_t block[DSP_SIEMENS_MP_MAX];
			for (uint32_t i = 0; i < count; i++)
				block[i] = dsp_read_word(p, in_w + 1 + i);
			if (p->afe && p->afe_started) {
				afe_audio_push_samples(p->afe, block, count);
				p->siemens_samples += count;
			}
			uint16_t *outmem = (uint16_t *) &p->ram[0];
			/*
			 * The ARM's block reader copies a fixed 56-word window (0x10E..0x17C)
			 * then a count word (0x17E) and a 4-word transform (0x188..0x18E), and
			 * polls until the window is updated with fresh samples.  Reflect the
			 * first window now and let the wall-clock timer stream the rest.
			 */
			for (uint32_t i = 0; i < count && i < (uint32_t) ARRAY_SIZE(p->mp_block); i++)
				p->mp_block[i] = block[i];
			p->mp_block_pos = 0;
			for (uint32_t i = 0; i < 56; i++)
				outmem[out_w + i] = i < count ? block[i] : 0;
			outmem[out_w + 56] = 4;                 /* 0x17E count word */
			p->mp_out_w = out_w;
			dsp_siemens_mp_write_nonce(p);           /* 0x188 transform nonce */
			if (!p->mp_timer && !(dsp_exp_mask() & DSP_EXP_NO_MPTIMER))
				p->mp_timer = timer_new_ns(QEMU_CLOCK_HOST, dsp_siemens_mp_drain, p);
			int64_t period_ns = (int64_t) 56 * NANOSECONDS_PER_SECOND /
				(p->siemens_rate ? p->siemens_rate : 16000);
			if (p->mp_timer)
				timer_mod(p->mp_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST) + period_ns);
			p->siemens_submits++;
			if ((dsp_exp_mask() & DSP_EXP_MP_HDR_ACK) != 0) {
				uint16_t *mem = (uint16_t *) &p->ram[0];

				mem[in_w] = dsp_read_word(p, in_w) & (uint16_t) ~0x8000;
			}
			/* The media player is open; stop the continuous keyseq fire. */
			pmb887x_keyseq_stop_continuous();
			DPRINTF("siemens PCM_SUBMIT: %u samples @%u Hz (in w%u, out w%u) hdr=%04X data=%04X %04X %04X %04X %04X %04X %04X %04X\n",
				count, p->siemens_rate, in_w, out_w, dsp_read_word(p, in_w),
				block[0], block[1], block[2], block[3], block[4], block[5], block[6], block[7]);
			if (!(dsp_exp_mask() & DSP_EXP_NO_SUBMIT_INT))
				qemu_irq_pulse(p->mcu_interrupts[2]);
			break;
		}

		default:
			if (!(dsp_exp_mask() & DSP_EXP_NO_DEFAULT_INT))
				qemu_irq_pulse(p->mcu_interrupts[2]);
			break;
	}

	/*
	 * Pipe 2 flag: cleared once the command is accepted, except when the
	 * LG PCMPLAY feed path is holding it while a block waits for FIFO space.
	 */
	if (id != DSP_CMD_PCMPLAY || dsp_read_word(p, DSP_CHAN2_CMD_ADDR + 1) != DSP_PCMPLAY_SWITCH_FEED)
		p->com_status &= ~(uint32_t) DSP_COM_PIPE2_BIT;

	dsp_ack_command(p, DSP_CHAN2_CMD_ADDR, id, true);

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
			dsp_ramtap(DSP_RAMTAP_COM_READ, DSP_COM_STATUS, value, 4);
			if (current_cpu)
				dsp_ramtap(DSP_RAMTAP_COM_READ_PC, DSP_COM_STATUS,
					ARM_CPU(current_cpu)->env.regs[15], 4);
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
			if (dsp_comm_trace_enabled())
				DPRINTF("COM_SET %04X (status=%04X) pc=%08X\n", (uint16_t) value,
					(uint16_t) p->com_status,
					current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
			dsp_ramtap(DSP_RAMTAP_COM_WRITE, DSP_COM_SET, value, 4);
			if (current_cpu)
				dsp_ramtap(DSP_RAMTAP_COM_WRITE_PC, DSP_COM_SET,
					ARM_CPU(current_cpu)->env.regs[15], 4);
			p->com_set = value;
			/*
			 * Communication flags are level-shared state: the ARM sets them,
			 * the DSP clears the pipe/data flags once it accepted the payload.
			 * The audio-status flags 8..14 (DSP-to-ARM events) must survive this
			 * write, so never zero the whole register here.
			 */
			p->com_status |= value & 0xFFFF;
			if ((value & DSP_SIEMENS_DATA_FLAG) != 0)
				DPRINTF("CF9 data flag set (active=%u submits=%u)\n",
					p->siemens_active, p->siemens_submits);
			// if ((value & 1) != 0) dsp_exec_command_ch0(p); // noisy logs
			if ((value & 1) != 0)
				p->com_status &= ~(uint32_t) 1;
			if ((value & 2) != 0) {
				dsp_exec_command_ch1(p);
				dsp_ack_command(p, DSP_CHAN1_CMD_ADDR, dsp_read_word(p, DSP_CHAN1_CMD_ADDR), true);
				p->com_status &= ~(uint32_t) 2;
			}
			if ((value & 4) != 0)
				dsp_exec_command_ch2(p);
			/*
			 * CF10 is a request the ARM raises and then waits on the DSP to
			 * take. Nothing cleared it, so the media player's audio task spun
			 * on it forever (status reads returned 0x400 indefinitely) and
			 * never streamed a second block, for every format.
			 */
			/*
			 * CF10 is deliberately left alone. The firmware asserts it and then
			 * reads it back to confirm it is set (0xA09A33DA), so clearing it in
			 * this write makes that read-back spin forever; clearing it later
			 * instead silences the boot chime. Neither models the real DSP, and
			 * the ring CF10 guards carries handshake words, not PCM, so there is
			 * nothing to consume here yet. See PROGRESS.md.
			 */
			if ((value & DSP_COM_MP_REQUEST_BIT) != 0 &&
					(dsp_exp_mask() & DSP_EXP_MP_CONSUME) != 0)
				dsp_siemens_mp_consume(p);
			/*
			 * Deferred variant, scoped to the media player's linear-PCM stream
			 * so the chime's ADPCM path keeps its untouched CF9 handshake. The
			 * delay has to outlast the firmware's read-back at 0xA09A33DA.
			 */
			/*
			 * Each CF10 request is preceded by a token at w196..w199 and a
			 * count at w191, and followed by the ARM reading 56 words from
			 * w135. Answer it: stream the staged block through that window so
			 * the request actually returns something.
			 */
			if ((value & DSP_COM_MP_REQUEST_BIT) != 0 &&
					(dsp_exp_mask() & DSP_EXP_MP_WINDOW) != 0)
				dsp_siemens_mp_window(p);
			if ((value & DSP_COM_MP_REQUEST_BIT) != 0 && p->siemens_linear &&
					(dsp_exp_mask() & DSP_EXP_MP_CF10_ACK) != 0)
				timer_mod(p->cf10_timer,
					qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500000);
			/* Consumption is paced by the 10 ms pace timer (1 chunk = 10 ms of audio). */
			break;

		case DSP_COM_STATUS:
			p->com_status = value;
			break;

		case DSP_COM_CLEAR:
			dsp_ramtap(DSP_RAMTAP_COM_WRITE, DSP_COM_CLEAR, value, 4);
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
	if (getenv("PMB887X_DSP_AFE_FORCE")) {
		p->afe_force_timer = timer_new_ns(QEMU_CLOCK_REALTIME, dsp_afe_force_tick, p);
		timer_mod(p->afe_force_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + 8 * 1000000000LL);
	}
	p->siemens_timer = timer_new_ns(DSP_SIEMENS_PACE_CLOCK, dsp_siemens_pace_timer, p);
	p->cf10_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dsp_cf10_ack, p);
	pcprof_init();
	if (getenv("PMB887X_DSP_MPFEED")) {
		p->mpfeed_timer = timer_new_ns(QEMU_CLOCK_HOST, dsp_mpfeed_tick, p);
		timer_mod(p->mpfeed_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST) + NANOSECONDS_PER_SECOND);
	}

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
	if (p->siemens_timer != NULL) {
		timer_free(p->siemens_timer);
		p->siemens_timer = NULL;
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

typedef struct dsp_state_t dsp_state_t;
typedef struct dsp_events_t dsp_events_t;
typedef struct dsp_worker_t dsp_worker_t;

struct dsp_events_t {
	uint16_t interrupts;
	uint16_t output_events;
	uint16_t outputs;
};

struct dsp_worker_t {
	QEMUBH *bh;
	QemuThread thread;
	QemuMutex mutex;
	QemuCond cond;
	QemuCond idle_cond;
	QemuEvent event;
	uint16_t interrupt_events;
	uint16_t output_events;
	uint16_t outputs;
	bool enabled;
	bool busy;
	bool sync_requested;
	bool reset;
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
	bool trace_boot_mode;
	pmb887x_clc_reg_t clc;
	dsp_runtime_t *runtime;
	dsp_worker_t worker;
	bool runtime_running;
	QEMUTimer *afe_timer;
	VMChangeStateEntry *vmstate;
	uint16_t comm_status;
	uint16_t comm_pending;
	uint16_t reset_comm_flags;
	uint16_t reset_requests;
	uint16_t baseband_timeout_flags;
	Clock *gsm_clock;
	bool reset_pending;
	bool vm_running;
	qemu_irq mcu_interrupts[PMB887X_DSP_MCU_INT_COUNT];
	qemu_irq outputs[DSP_OUTPUT_COUNT];
	SSIBus *ssc_bus;
	int assist_done;
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

static void dsp_run(dsp_state_t *p, dsp_events_t *events) {
	p->runtime_running = dsp_runtime_run(p->runtime);
	events->interrupts = dsp_runtime_take_mcu_irqs(p->runtime);
	events->output_events = dsp_runtime_take_output_events(p->runtime);
	events->outputs = dsp_runtime_get_outputs(p->runtime);
}

static bool dsp_runnable(dsp_state_t *p) {
	if (!p->runtime_running)
		return false;
	return !dsp_runtime_is_idle(p->runtime);
}

static void dsp_worker_bh(void *opaque) {
	dsp_state_t *p = opaque;
	uint16_t events = qatomic_xchg(&p->worker.interrupt_events, 0);
	uint16_t output_events = qatomic_xchg(&p->worker.output_events, 0);
	uint16_t outputs = qatomic_read(&p->worker.outputs);
	bool locked = bql_locked();

	events &= MAKE_64BIT_MASK(0, PMB887X_DSP_MCU_INT_COUNT);

	if (!locked)
		bql_lock();
	for (size_t i = 0; i < ARRAY_SIZE(p->mcu_interrupts); i++)
		if ((events & BIT(i)) != 0)
			qemu_irq_raise(p->mcu_interrupts[i]);

	for (size_t i = 0; i < ARRAY_SIZE(p->outputs); i++)
		if ((output_events & BIT(i)) != 0)
			qemu_set_irq(p->outputs[i], (outputs & BIT(i)) != 0);
	if (!locked)
		bql_unlock();
}

static void dsp_worker_publish_events(dsp_state_t *p, const dsp_events_t *events) {
	if (events->interrupts == 0 && events->output_events == 0)
		return;

	qatomic_or(&p->worker.interrupt_events, events->interrupts);
	qatomic_set(&p->worker.outputs, events->outputs);
	qatomic_or(&p->worker.output_events, events->output_events);
	qemu_bh_schedule(p->worker.bh);
}

static void *dsp_worker(void *opaque) {
	dsp_state_t *p = opaque;

	dsp_runtime_thread_enter();

	qemu_mutex_lock(&p->worker.mutex);
	while (!p->worker.stop) {
		dsp_events_t events = {};
		bool runnable;

		while (!p->worker.enabled && !p->worker.reset && !p->worker.stop)
			qemu_cond_wait(&p->worker.cond, &p->worker.mutex);

		if (p->worker.stop)
			break;

		if (p->worker.reset) {
			bool run_startup = p->worker.enabled;
			uint16_t comm_flags;
			uint16_t requests;

			p->worker.busy = true;
			p->worker.reset = false;
			qemu_mutex_unlock(&p->worker.mutex);
			dsp_runtime_reset(p->runtime);
			p->runtime_running = true;
			DPRINTF("core reset: startup=%d\n", run_startup);

			if (run_startup)
				dsp_run(p, &events);

			qemu_mutex_lock(&p->worker.mutex);
			p->worker.busy = false;
			p->worker.sync_requested = false;
			qemu_cond_broadcast(&p->worker.idle_cond);
			if (p->worker.reset)
				continue;

			comm_flags = qatomic_read(&p->reset_comm_flags);
			requests = qatomic_read(&p->reset_requests);
			dsp_runtime_set_comm(p->runtime, comm_flags);
			qatomic_set(&p->comm_status, dsp_runtime_get_comm(p->runtime));
			for (size_t i = 0; i < PMB887X_DSP_INT_COUNT; i++)
				if ((requests & BIT(i)) != 0)
					dsp_runtime_set_request(p->runtime, i, true);

			qatomic_set(&p->reset_pending, false);
			qatomic_set(&p->reset_comm_flags, 0);
			qatomic_set(&p->reset_requests, 0);
			qemu_cond_broadcast(&p->worker.idle_cond);
			dsp_worker_publish_events(p, &events);
			continue;
		}

		/*
		 * Sleep only when truly idle. If a real-time peripheral (the AFE) is
		 * active, fall through and run one iteration even though the core is
		 * idle: dsp_run() -> dsp_runtime_pace_afe() feeds the AFE its wall-clock
		 * due samples, which may raise VBRX/VBTX and wake the core. We pace
		 * rather than busy-spin because we sleep again at the end of the loop
		 * until the AFE timer re-kicks us (see below).
		 */
		runnable = dsp_runnable(p);
		if (!runnable && !dsp_runtime_realtime_active(p->runtime)) {
			qemu_event_reset(&p->worker.event);
			if (!dsp_runnable(p)) {
				qemu_mutex_unlock(&p->worker.mutex);
				qemu_event_wait(&p->worker.event);
				qemu_mutex_lock(&p->worker.mutex);
				continue;
			}
		}

		p->worker.busy = true;
		qemu_mutex_unlock(&p->worker.mutex);

		dsp_run(p, &events);

		qemu_mutex_lock(&p->worker.mutex);
		qatomic_set(&p->comm_status, dsp_runtime_get_comm(p->runtime));
		p->worker.busy = false;
		p->worker.sync_requested = false;
		qemu_cond_broadcast(&p->worker.idle_cond);

		if (p->worker.reset)
			continue;
		dsp_worker_publish_events(p, &events);

		/*
		 * Idle but a real-time peripheral (AFE) is still active: sleep until the
		 * AFE timer ticks (or a command kick arrives) so we advance the sample
		 * clock at its real 8 kHz rate instead of spinning this thread at 100%.
		 */
		if (!dsp_runnable(p) && dsp_runtime_realtime_active(p->runtime)) {
			qemu_event_reset(&p->worker.event);
			if (!dsp_runnable(p) && dsp_runtime_realtime_active(p->runtime)) {
				qemu_mutex_unlock(&p->worker.mutex);
				qemu_event_wait(&p->worker.event);
				qemu_mutex_lock(&p->worker.mutex);
			}
		}
	}

	qemu_mutex_unlock(&p->worker.mutex);
	dsp_runtime_thread_exit();
	return NULL;
}

/*
 * Wall-clock heartbeat for the real-time DSP peripherals (the AFE sample clock).
 * While the DSP worker is enabled this fires every DSP_AFE_TICK_NS and kicks the
 * worker so the paced idle loop in dsp_runtime_run feeds the AFE the samples that
 * are due. Without it the worker would sleep between samples and the AFE clock
 * would stop (see the paced advance in runtime.c). Only kicks when a real-time
 * peripheral is actually running, so it costs nothing when there is no audio.
 */
#define DSP_AFE_TICK_NS	(1 * SCALE_MS)
/*
 * The AFE sample clock / DSP timer pacing runs on QEMU_CLOCK_HOST (true
 * wall-clock) deliberately. The DSP executes on its own worker thread in real
 * time, and the ARM<->DSP handshake (dsp_wait_comm_clear) blocks the vCPU while
 * the worker makes progress. Under -icount, blocking the vCPU freezes
 * QEMU_CLOCK_VIRTUAL, which would in turn freeze the AFE/timers and deadlock the
 * DSP -- so the pacing must stay on a clock that keeps advancing while the vCPU
 * is parked. QEMU_CLOCK_HOST does; QEMU_CLOCK_VIRTUAL does not.
 */
#define DSP_AFE_CLOCK	QEMU_CLOCK_HOST

static void dsp_worker_kick(void *opaque);

/*
 * The DSP's scheduler is driven by the TDMA frame interrupt. On this board the
 * TPU does emit one, and PMB887X_DSP_INT_SUPPRESS is what hides it: its usual
 * value 0x1C10 includes 0x0010, which is FRAME. This free-runs the frame
 * instead, for a startup mode whose TPU drives none; "1" selects the GSM rate
 * of 1/(60/13 ms). Off unless set, and it is not needed on siemens-s75.
 */
#define DSP_FRAME_TICK_DEFAULT_HZ	217

static void dsp_frame_tick(dsp_state_t *p) {
	static int64_t period_ns = -1;
	static int64_t next;
	static int64_t start;
	int64_t now;

	if (period_ns < 0) {
		const char *value = getenv("PMB887X_DSP_FRAME_TICK");
		const char *delay = getenv("PMB887X_DSP_FRAME_TICK_DELAY");
		unsigned long hz = value ? strtoul(value, NULL, 0) : 0;

		if (hz <= 1)
			hz = value ? DSP_FRAME_TICK_DEFAULT_HZ : 0;
		period_ns = hz ? (int64_t) (NANOSECONDS_PER_SECOND / hz) : 0;
		/*
		 * Ticking from reset lets the GSM L1 make progress it cannot finish
		 * with PMB887X_STARTUP=OFFLINE, and the phone never leaves the splash.
		 * Delaying the start lets the phone boot first and only then gives the
		 * DSP the scheduler it needs for audio.
		 */
		start = qemu_clock_get_ns(DSP_AFE_CLOCK) +
			(delay ? (int64_t) strtoul(delay, NULL, 0) * NANOSECONDS_PER_SECOND : 0);
		if (period_ns)
			DPRINTF("free-running frame tick at %lu Hz, start delay %s s\n",
				hz, delay ? delay : "0");
	}
	if (!period_ns)
		return;

	now = qemu_clock_get_ns(DSP_AFE_CLOCK);
	if (now < start || now < next)
		return;
	next = now + period_ns;
	dsp_runtime_set_gsm_signal(p->runtime, PMB887X_DSP_GSM_SIGNAL_FRAME, true);
	dsp_runtime_set_gsm_signal(p->runtime, PMB887X_DSP_GSM_SIGNAL_FRAME, false);
	dsp_worker_kick(p);
}

static void dsp_afe_timer_cb(void *opaque) {
	dsp_state_t *p = opaque;
	bool active = dsp_runtime_realtime_active(p->runtime);

	dsp_frame_tick(p);

#if 0	/* AFE timer debug */
	static uint32_t tn;
	if ((tn++ & 0x3FF) == 0)
		fprintf(stderr, "[afe-timer] n=%u active=%d\n", tn, active);
#endif

	if (getenv("PMB887X_DSP_INT_RATE") != NULL) {
		/* Periodic view of the DSP interrupt groups: g3 carries the RTOS event
		 * dispatch (INT2), g1 the AFE/VB interrupts. */
		static int64_t int_deadline;
		int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

		if (now >= int_deadline) {
			uint8_t ie = 0, mask = 0, lines = 0;

			int_deadline = now + NANOSECONDS_PER_SECOND;
			dsp_runtime_get_irq_debug(p->runtime, &ie, &mask, &lines);
			DPRINTF("int groups: g0 f=%04X p=%04X g1 f=%04X p=%04X g3 f=%04X p=%04X pc=%05X ie=%u mask=%X lines=%X\n",
				dsp_runtime_get_irq_flags(p->runtime, 0), dsp_runtime_get_irq_pending_flags(p->runtime, 0),
				dsp_runtime_get_irq_flags(p->runtime, 1), dsp_runtime_get_irq_pending_flags(p->runtime, 1),
				dsp_runtime_get_irq_flags(p->runtime, 3), dsp_runtime_get_irq_pending_flags(p->runtime, 3),
				dsp_runtime_get_pc(p->runtime), ie, mask, lines);
		}
	}

	if (getenv("PMB887X_DSP_AFE_FORCE") != NULL) {
		/* Diagnostic: the firmware's audio path may render into the AFE DAC ring
		 * without ever setting BCON.MODE|TXSTART here. Forcing the transmit on
		 * reveals whether samples are actually being produced. */
		static int64_t afe_deadline;
		int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

		if (now >= afe_deadline) {
			afe_deadline = now + NANOSECONDS_PER_SECOND;
			dsp_runtime_force_afe_start(p->runtime);
		}
	}

	if (getenv("PMB887X_DSP_PCWATCH") != NULL) {
		/* Report the watched DSP PC counters once a second so they are visible
		 * during playback, not only when a comm stall happens to fire. */
		extern void teak_tcg_report_pcwatch(void);
		static int64_t deadline;
		int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

		if (now >= deadline) {
			deadline = now + NANOSECONDS_PER_SECOND;
			teak_tcg_report_pcwatch();
		}
	}

	if (active)
		dsp_worker_kick(p);
	timer_mod(p->afe_timer, qemu_clock_get_ns(DSP_AFE_CLOCK) + DSP_AFE_TICK_NS);
}

static void dsp_worker_set_enabled(dsp_state_t *p, bool enabled) {
	qemu_mutex_lock(&p->worker.mutex);
	p->worker.enabled = enabled;
	if (enabled) {
		qemu_cond_signal(&p->worker.cond);
		qemu_event_set(&p->worker.event);
	}
	qemu_mutex_unlock(&p->worker.mutex);

	if (enabled)
		timer_mod(p->afe_timer, qemu_clock_get_ns(DSP_AFE_CLOCK) + DSP_AFE_TICK_NS);
	else
		timer_del(p->afe_timer);
}

static void dsp_vm_state_change(void *opaque, bool running, RunState state) {
	dsp_state_t *p = opaque;
	bool enabled = running && pmb887x_clc_is_enabled(&p->clc);

	p->vm_running = running;

	qemu_mutex_lock(&p->worker.mutex);
	p->worker.enabled = enabled;
	if (enabled) {
		qemu_cond_signal(&p->worker.cond);
		qemu_event_set(&p->worker.event);
	}

	while (!running && (p->worker.busy || p->worker.reset))
		qemu_cond_wait(&p->worker.idle_cond, &p->worker.mutex);
	qemu_mutex_unlock(&p->worker.mutex);
}

static void dsp_worker_kick(void *opaque) {
	dsp_state_t *p = opaque;

	if (qemu_thread_is_self(&p->worker.thread)) {
		qemu_event_set(&p->worker.event);
		return;
	}

	qemu_mutex_lock(&p->worker.mutex);
	if (p->worker.busy) {
		dsp_runtime_kick(p->runtime);
	} else {
		dsp_runtime_wake(p->runtime);
	}
	qemu_event_set(&p->worker.event);
	qemu_mutex_unlock(&p->worker.mutex);
}

static void dsp_worker_notify_activity(void *opaque) {
	dsp_state_t *p = opaque;

	if (qemu_thread_is_self(&p->worker.thread)) {
		qemu_event_set(&p->worker.event);
		return;
	}

	qemu_mutex_lock(&p->worker.mutex);
	dsp_runtime_wake(p->runtime);
	qemu_event_set(&p->worker.event);
	qemu_mutex_unlock(&p->worker.mutex);
}

static void dsp_worker_notify_comm(void *opaque, uint16_t flags, bool set) {
	dsp_state_t *p = opaque;

	if (dsp_comm_trace_enabled())
		DPRINTF("DSP %s %04X (status=%04X) dsp_pc=%05X\n", set ? "SET  " : "CLEAR",
			flags, (uint16_t) qatomic_read(&p->comm_status),
			dsp_runtime_get_pc(p->runtime));

	if (set) {
		qatomic_or(&p->comm_status, flags);
		return;
	}

	qatomic_and(&p->comm_status, (uint16_t) ~flags);
	qatomic_and(&p->comm_pending, (uint16_t) ~flags);
}

static void dsp_worker_synchronize_cold_program(dsp_state_t *p) {
	int64_t start = qemu_clock_get_ns(QEMU_CLOCK_HOST);
	uint64_t cache_compiles = dsp_runtime_get_cache_compiles(p->runtime);
	bool waited = false;

	qemu_mutex_lock(&p->worker.mutex);
	if (p->worker.enabled && !p->worker.stop && dsp_runnable(p)) {
		p->worker.sync_requested = true;
		qemu_event_set(&p->worker.event);
	}
	while (p->worker.sync_requested && p->worker.enabled && !p->worker.stop) {
		waited = true;
		qemu_cond_wait(&p->worker.idle_cond, &p->worker.mutex);
	}
	qemu_mutex_unlock(&p->worker.mutex);

	cache_compiles = dsp_runtime_get_cache_compiles(p->runtime) - cache_compiles;
	if (cache_compiles == 0 && dsp_runtime_get_comm(p->runtime) == 0)
		dsp_runtime_finish_program_warmup(p->runtime);

	DPRINTF("cold program sync: waited=%u host_delay=%" PRId64 " ns compile=%" PRIu64 " warming=%u\n",
		waited, qemu_clock_get_ns(QEMU_CLOCK_HOST) - start, cache_compiles,
		dsp_runtime_is_program_warming(p->runtime));
}

static void dsp_reset_internal_state(dsp_state_t *p) {
	qemu_bh_cancel(p->worker.bh);

	qemu_mutex_lock(&p->worker.mutex);
	p->worker.reset = true;
	qatomic_set(&p->reset_pending, true);
	p->worker.enabled = p->vm_running && pmb887x_clc_is_enabled(&p->clc);
	qatomic_set(&p->worker.interrupt_events, 0);
	qatomic_set(&p->worker.output_events, 0);
	qatomic_set(&p->assist_done, 0);
	qatomic_set(&p->worker.outputs, 0);
	qatomic_set(&p->comm_status, 0);
	qatomic_set(&p->comm_pending, 0);
	qatomic_set(&p->reset_comm_flags, 0);
	qatomic_set(&p->reset_requests, 0);
	p->baseband_timeout_flags = 0;
	for (size_t i = 0; i < ARRAY_SIZE(p->outputs); i++)
		qemu_irq_lower(p->outputs[i]);
	p->trace_boot_mode = true;
	qemu_cond_signal(&p->worker.cond);
	qemu_event_set(&p->worker.event);
	qemu_mutex_unlock(&p->worker.mutex);
}

static void dsp_reset_input(void *opaque, int id, int level) {
	if (level)
		dsp_reset_internal_state(opaque);
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
		uint16_t command = dsp_runtime_shared_read(p->runtime, DSP_BOOT_DATA_OFFSET);
		uint16_t address = dsp_runtime_shared_read(p->runtime, DSP_BOOT_DATA_OFFSET + 1);
		uint16_t words = 0;

		if (command != DSP_BOOT_BRANCH)
			words = dsp_runtime_shared_read(p->runtime, DSP_BOOT_DATA_OFFSET + 2);

		DPRINTF("boot command: %s(%u) address=%04X words=%u\n", dsp_boot_command_name(command), command, address, words);

		if (command == DSP_BOOT_BRANCH)
			p->trace_boot_mode = false;
		return;
	}

	uint16_t offset = DSP_RUNTIME_PIPE_OFFSET + pipe * DSP_RUNTIME_PIPE_STRIDE;
	uint16_t command = dsp_runtime_shared_read(p->runtime, offset);
	DPRINTF("runtime command: pipe=%zu command=%u (0x%04X)\n", pipe, command, command);
}

static bool dsp_comm_handshake_pending(dsp_state_t *p);
static void dsp_wait_comm_clear(dsp_state_t *p);

static void dsp_interrupt_input(void *opaque, int id, int level) {
	dsp_state_t *p = opaque;

	if (!level) {
		/* Falling edge: the MCU comms line is level-sensitive, propagate it so the
		 * firmware's ISR wait loop (0801 ROM 0x204F) observes the drop. */
		if (!qatomic_read(&p->reset_pending))
			dsp_runtime_set_request(p->runtime, id, false);
		return;
	}

	if (pmb887x_trace_log_enabled(PMB887X_TRACE_DSP))
		dsp_trace_command(p, id);

	if (!qatomic_read(&p->reset_pending)) {
		dsp_runtime_set_request(p->runtime, id, true);
		dsp_worker_kick(p);
		/*
		 * SCU_DSP_INT is the causal trigger for a runtime command: the ARM has
		 * already staged the request in DSP_COM_SET and now pulses this IRQ.
		 * Rendezvous here -- drive the DSP until it acks (clears the comm flag)
		 * so the ensuing dwd_dsp_com_set poll of DSP_COM_STATUS finds the answer
		 * ready on its first iteration, inside the firmware's 1000-poll budget.
		 * The DSP's real-time peripherals run on wall-clock (DSP_AFE_CLOCK) so it
		 * keeps making progress even under -icount while the vCPU parks here.
		 */
		if (dsp_comm_handshake_pending(p))
			dsp_wait_comm_clear(p);
		return;
	}

	qemu_mutex_lock(&p->worker.mutex);
	if (qatomic_read(&p->reset_pending)) {
		qatomic_or(&p->reset_requests, BIT(id));
	} else {
		dsp_runtime_set_request(p->runtime, id, true);
	}
	qemu_mutex_unlock(&p->worker.mutex);
	dsp_worker_kick(p);
}

static void dsp_set_input(dsp_state_t *p, size_t index, int level) {
	dsp_runtime_set_input(p->runtime, index, level != 0);
}

static void dsp_input0(void *opaque, int id, int level) {
	dsp_set_input(opaque, 0, level);
}

static void dsp_input1(void *opaque, int id, int level) {
	dsp_set_input(opaque, 1, level);
}

static bool dsp_baseband_event_blocked(dsp_state_t *p) {
	uint16_t pending = dsp_runtime_get_irq_pending_flags(p->runtime, 0);
	uint16_t pending_baseband = pending & DSP_BASEBAND_IRQ_MASK;

	if (dsp_runtime_is_maskable_interrupt_active(p->runtime))
		return true;

	if (pending_baseband == 0) {
		p->baseband_timeout_flags = 0;
		return false;
	}

	return pending_baseband != p->baseband_timeout_flags;
}

static void dsp_wait_baseband_irq(dsp_state_t *p, int signal, int level) {
	if (!dsp_baseband_event_blocked(p))
		return;

	int64_t start = qemu_clock_get_ns(QEMU_CLOCK_HOST);
	int64_t deadline = start + DSP_BASEBAND_SYNC_TIMEOUT_MS * SCALE_MS;
	int64_t spin_deadline = start + DSP_BASEBAND_SPIN_NS;
	uint32_t sleeps = 0;
	bool timed_out = false;

	dsp_worker_kick(p);
	while (dsp_baseband_event_blocked(p) && qemu_clock_get_ns(QEMU_CLOCK_HOST) < spin_deadline)
		cpu_relax();

	qemu_mutex_lock(&p->worker.mutex);
	while (dsp_baseband_event_blocked(p)) {
		bool worker_stopped = !p->worker.enabled || !p->runtime_running || p->worker.stop;

		if (worker_stopped)
			break;

		int64_t remaining = deadline - qemu_clock_get_ns(QEMU_CLOCK_HOST);

		if (remaining <= 0) {
			timed_out = true;
			break;
		}
		sleeps++;
		qemu_cond_timedwait(&p->worker.idle_cond, &p->worker.mutex, DIV_ROUND_UP(remaining, SCALE_MS));
	}
	qemu_mutex_unlock(&p->worker.mutex);

	if (timed_out)
		p->baseband_timeout_flags = dsp_runtime_get_irq_pending_flags(p->runtime, 0) & DSP_BASEBAND_IRQ_MASK;

	if (sleeps == 0)
		return;

	int64_t host_wait_us = (qemu_clock_get_ns(QEMU_CLOCK_HOST) - start) / SCALE_US;
	uint16_t flags = dsp_runtime_get_irq_flags(p->runtime, 0) & DSP_BASEBAND_IRQ_MASK;
	uint32_t pc = dsp_runtime_get_pc(p->runtime);

	DPRINTF("ARM wait: sig=%d/%d irq=%04X active=%u pc=%05X wait=%" PRId64 " us sleeps=%u timeout=%u\n",
		signal, level, flags, dsp_runtime_is_maskable_interrupt_active(p->runtime), pc, host_wait_us, sleeps, timed_out);
}

static void dsp_gsm_input(void *opaque, int signal, int level) {
	dsp_state_t *p = opaque;
	uint32_t gsm_frequency = clock_get_hz(p->gsm_clock);
	bool baseband_irq = signal < PMB887X_DSP_GSM_SIGNAL_RXON;

	dsp_runtime_set_gsm_clock(p->runtime, gsm_frequency);
	if (baseband_irq)
		dsp_wait_baseband_irq(p, signal, level);

	dsp_runtime_set_gsm_signal(p->runtime, signal, level != 0);
}

static bool dsp_comm_handshake_pending(dsp_state_t *p) {
	return (qatomic_read(&p->comm_status) & qatomic_read(&p->comm_pending)) != 0;
}

static void dsp_wait_comm_clear(dsp_state_t *p) {
	int64_t start;
	int64_t deadline;
	int64_t spin_deadline;

	if (!dsp_comm_handshake_pending(p))
		return;

	start = qemu_clock_get_ns(QEMU_CLOCK_HOST);
	deadline = start + DSP_COMM_SYNC_TIMEOUT_MS * SCALE_MS;
	spin_deadline = start + DSP_COMM_SPIN_NS;

	/*
	 * This runs inside the ARM's DSP_COM_STATUS MMIO read, which holds the
	 * BQL. The DSP worker's notify callbacks (dsp_worker_notify_comm etc.)
	 * take the BQL to publish events, so waiting for the worker while holding
	 * it would deadlock: the worker could never clear the flag we wait on.
	 * Drop the BQL for the duration of the wait and reacquire it after.
	 */
	bool bql = bql_locked();
	if (bql)
		bql_unlock();

	dsp_worker_kick(p);
	while (dsp_comm_handshake_pending(p) && qemu_clock_get_ns(QEMU_CLOCK_HOST) < spin_deadline)
		cpu_relax();

	qemu_mutex_lock(&p->worker.mutex);
	while (dsp_comm_handshake_pending(p)) {
		int64_t remaining;

		if (!p->worker.enabled || !p->runtime_running || p->worker.stop)
			break;
		/*
		 * If the core is idle keep waiting only while a real-time peripheral
		 * (the AFE) is active: it is clocked by the AFE timer, which will soon
		 * advance it, raise its IRQ and wake the core to service the pending
		 * command. Without this, a core momentarily idle between 8 kHz AFE
		 * samples makes every ARM poll return instantly, so the firmware burns
		 * its 1000-poll budget in microseconds and panics before the AFE ticks.
		 */
		if (!dsp_runnable(p) && !dsp_runtime_realtime_active(p->runtime))
			break;

		remaining = deadline - qemu_clock_get_ns(QEMU_CLOCK_HOST);
		if (remaining <= 0)
			break;
		qemu_cond_timedwait(&p->worker.idle_cond, &p->worker.mutex, DIV_ROUND_UP(remaining, SCALE_MS));
	}
	qemu_mutex_unlock(&p->worker.mutex);

	if (dsp_comm_handshake_pending(p)) {
		uint8_t ie = 0, mask = 0, lines = 0;

		dsp_runtime_get_irq_debug(p->runtime, &ie, &mask, &lines);
		if (getenv("PMB887X_DSP_ASSIST") && qatomic_read(&p->assist_done) == 0) {
			dsp_runtime_set_ie(p->runtime, 1);
			dsp_runtime_set_int_mask(p->runtime, 0x7);
			dsp_runtime_poke(p->runtime, 0x7D8A, 1);
			/* Clear any stale CF flags so the handshake can proceed. */
			dsp_runtime_clear_comm(p->runtime, 0xFFFF);
			qatomic_set(&p->comm_status, 0);
			qatomic_set(&p->comm_pending, 0);
			qatomic_set(&p->assist_done, 1);
			DPRINTF("kernel assist applied: ie=1, [7D8A]=1\n");
		}
		{
			/* PMB887X_DSP_RING_DUMP=<path>[,<stall index>] dumps the DSP
			 * instruction ring at the Nth comm stall (default: the first). */
			static uint64_t stall_count;
			const char *rd = getenv("PMB887X_DSP_RING_DUMP");

			stall_count++;
			if (rd != NULL) {
				extern void teak_tcg_dump_trace_ring(const char *path);
				char path[512];
				uint64_t want = 1;
				const char *comma = strchr(rd, ',');

				if (comma != NULL) {
					size_t len = MIN((size_t) (comma - rd), sizeof(path) - 1);
					memcpy(path, rd, len);
					path[len] = '\0';
					want = strtoull(comma + 1, NULL, 0);
				} else {
					snprintf(path, sizeof(path), "%s", rd);
				}
				if (stall_count == want)
					teak_tcg_dump_trace_ring(path);
			}
		}
		{
			const char *pd = getenv("PMB887X_DSP_PDUMP");
			if (pd && access(pd, F_OK) != 0) {
				FILE *f = fopen(pd, "wb");
				if (f) {
					for (uint32_t a = 0; a < 0x10000; a++) {
						uint16_t w = dsp_runtime_peek_program(p->runtime, a);
						fwrite(&w, 2, 1, f);
					}
					fclose(f);
					DPRINTF("program dumped to %s\n", pd);
				}
			}
		}
		DPRINTF("comm handshake STALL: status=%04X pending=%04X dsp_pc=%05X int0_flags=%04X int0_pending=%04X "
			"ie=%d mask=%X lines=%X isr_active=%d running=%d idle=%d realtime=%d mcs_comm=%04X warming=%d page=%02X\n",
			qatomic_read(&p->comm_status), qatomic_read(&p->comm_pending),
			dsp_runtime_get_pc(p->runtime), dsp_runtime_get_irq_flags(p->runtime, 0),
			dsp_runtime_get_irq_pending_flags(p->runtime, 0), ie, mask, lines,
			dsp_runtime_is_maskable_interrupt_active(p->runtime), p->runtime_running,
			dsp_runtime_is_idle(p->runtime), dsp_runtime_realtime_active(p->runtime),
			dsp_runtime_get_comm(p->runtime), dsp_runtime_is_program_warming(p->runtime),
			dsp_runtime_get_page(p->runtime));

		/* If stuck in the mask-ROM timer-queue insert (0x2340-0x2346), dump the
		 * event-queue region: 0x7c54 sentinel, 0x7c55 head, 0x7c56.. node pool. */
		uint32_t pc = dsp_runtime_get_pc(p->runtime);
		if (pc >= 0x2340 && pc <= 0x2346) {
			char buf[512];
			int n = 0;
			for (uint16_t a = 0x7C54; a <= 0x7C7F; a++)
				n += snprintf(buf + n, sizeof(buf) - n, "%04X ", dsp_runtime_peek(p->runtime, a));
			DPRINTF("evq[7C54..7C7F]: %s\n", buf);
			{
				uint8_t page = dsp_runtime_get_page(p->runtime);
				uint16_t base = ((uint16_t) page) << 8;
				n = 0;
				for (uint16_t a = 0x40; a <= 0x7F; a++)
					n += snprintf(buf + n, sizeof(buf) - n, "%04X ", dsp_runtime_peek(p->runtime, base | a));
				DPRINTF("kern[p%02X:40..7F]: %s\n", page, buf);
			}
			{
				/* Follow the event-queue chain: node = {marker, next, fn, time},
				 * head var at 0x7C55, sentinel 0xFFFF at 0x7C54. Detect cycles. */
				uint16_t cur = dsp_runtime_peek(p->runtime, 0x7C55);
				uint16_t seen[512];
				int count = 0, cycle_at = -1;
				uint16_t tail_word = 0, tail_addr = 0;
				for (int hop = 0; hop < 2000; hop++) {
					for (int k = 0; k < count; k++)
						if (seen[k] == cur) { cycle_at = hop; goto done_walk; }
					if (count < 512) seen[count++] = cur;
					uint16_t next = dsp_runtime_peek(p->runtime, (uint16_t)(cur + 1));
					tail_addr = cur; tail_word = next;
					if (next == cur) break;
					cur = next;
				}
				done_walk:
				DPRINTF("chain: nodes=%d cycle_at=%d tail=[%04X next=%04X w0=%04X w2=%04X w3=%04X]\n",
					count, cycle_at, tail_addr, tail_word,
					dsp_runtime_peek(p->runtime, (uint16_t)(tail_addr + 2)),
					dsp_runtime_peek(p->runtime, (uint16_t)(tail_addr + 3)));
			}
		}
			{
				char buf[512];
				int n = 0;
				for (uint16_t a = 0x00; a <= 0x3F; a++)
					n += snprintf(buf + n, sizeof(buf) - n, "%04X ", dsp_runtime_peek_program(p->runtime, a));
				DPRINTF("vecs[P00..3F]: %s\n", buf);
				/* The loaded kernel page: dump around the current PC. */
				{
					uint16_t pc = dsp_runtime_peek_pc(p->runtime);
					char buf2[512];
					int n2 = 0;
					for (uint16_t a = pc - 0x10; a != pc + 0x10; a++)
						n2 += snprintf(buf2 + n2, sizeof(buf2) - n2, "%04X ", dsp_runtime_peek_program(p->runtime, a));
					DPRINTF("kern[P%04X-30]: %s\n", pc - 0x10, buf2);
				}
			}
			{
				{
					char buf3[1024];
					int n3 = 0;
					for (uint16_t a = 0x7770; a <= 0x77D0; a++)
						n3 += snprintf(buf3 + n3, sizeof(buf3) - n3, "%04X ", dsp_runtime_peek(p->runtime, a));
					{
					uint16_t sp = dsp_runtime_peek_sp(p->runtime);
					char buf4[256];
					int n4 = 0;
					for (uint16_t a = sp; a != (uint16_t)(sp + 16); a++)
						n4 += snprintf(buf4 + n4, sizeof(buf4) - n4, "%04X ", dsp_runtime_peek(p->runtime, a));
					DPRINTF("stack[sp=%04X]: %s\n", sp, buf4);
				}
				{
					uint16_t ra = dsp_runtime_peek(p->runtime, dsp_runtime_peek_sp(p->runtime));
					char buf5[512];
					int n5 = 0;
					for (uint16_t a = ra - 0x18; a != (uint16_t)(ra + 0x8); a++)
						n5 += snprintf(buf5 + n5, sizeof(buf5) - n5, "%04X ", dsp_runtime_peek_program(p->runtime, a));
					DPRINTF("caller[P%04X-18]: %s\n", ra - 0x18, buf5);
				}
				DPRINTF("regs: r0=%04X r1=%04X r2=%04X r3=%04X r4=%04X r5=%04X\n",
					dsp_runtime_peek_reg(p->runtime, 0), dsp_runtime_peek_reg(p->runtime, 1),
					dsp_runtime_peek_reg(p->runtime, 2), dsp_runtime_peek_reg(p->runtime, 3),
					dsp_runtime_peek_reg(p->runtime, 4), dsp_runtime_peek_reg(p->runtime, 5));
				{
					uint8_t modulo_enable = 0, stepi = 0, stepj = 0;
					uint16_t modi = 0, modj = 0;

					dsp_runtime_peek_modulo(p->runtime, &modulo_enable, &modi, &modj, &stepi, &stepj);
					DPRINTF("modulo: enable=%02X modi=%04X modj=%04X stepi=%02X stepj=%02X\n",
						modulo_enable, modi, modj, stepi, stepj);
					{ extern void teak_tcg_report_pcwatch(void); teak_tcg_report_pcwatch(); }
				DPRINTF("int groups: g1 f=%04X p=%04X  g2 f=%04X p=%04X  g3 f=%04X p=%04X  sched_level=%04X\n",
					dsp_runtime_get_irq_flags(p->runtime, 1), dsp_runtime_get_irq_pending_flags(p->runtime, 1),
					dsp_runtime_get_irq_flags(p->runtime, 2), dsp_runtime_get_irq_pending_flags(p->runtime, 2),
					dsp_runtime_get_irq_flags(p->runtime, 3), dsp_runtime_get_irq_pending_flags(p->runtime, 3),
					dsp_runtime_peek(p->runtime, 0x7D69));
				}
				DPRINTF("sem[7770..77D0]: %s\n", buf3);
				}
				{
					char buf[128];
					int n = 0;
					for (uint16_t a = 0xD000; a <= 0xD00F; a++)
					n += snprintf(buf + n, sizeof(buf) - n, "%04X ", dsp_runtime_peek(p->runtime, a));
				DPRINTF("mbox[D000..D00F]: %s\n", buf);
				}
			}
		}

	if (bql)
		bql_lock();
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

		case DSP_COM_STATUS: {
			uint32_t program_start_pc;
			bool reset_pending = qatomic_read(&p->reset_pending);

			if (reset_pending) {
				value = qatomic_read(&p->reset_comm_flags) | qatomic_read(&p->comm_status);
			} else {
				value = qatomic_read(&p->comm_status);
			}

			if (!reset_pending && dsp_runtime_take_program_start(p->runtime, &program_start_pc))
				DPRINTF("cold program start: pc=%05X flags=%04" PRIX64 "\n", program_start_pc, value);

			if (!reset_pending) {
				/*
				 * Two independent concerns are composed here rather than
				 * chosen between: (1) warming a freshly loaded program so its
				 * P-space is compiled, and (2) waiting for an ARM-requested
				 * comm handshake flag to be cleared by the DSP. A runtime
				 * audio command (e.g. VB_ON via comm flag 2) is issued right
				 * after a cmd-67 module load leaves the runtime in the warming
				 * state, so the handshake wait must NOT be gated behind
				 * "not warming" or those handshakes race and time out. The
				 * wait is uniform across every comm-flag bit: it triggers
				 * solely on dsp_comm_handshake_pending().
				 */
				if (dsp_runtime_is_program_warming(p->runtime))
					dsp_worker_synchronize_cold_program(p);

				if (dsp_comm_handshake_pending(p))
					dsp_wait_comm_clear(p);

				value = qatomic_read(&p->comm_status);
			}
			break;
		}

		case DSP_COM_SET:
		case DSP_COM_CLEAR:
		case DSP_SEM_SET:
		case DSP_SEM_CLEAR:
			break;

		case DSP_SEM_STATUS:
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
			dsp_runtime_set_clock(p->runtime, pmb887x_clc_is_enabled(&p->clc));
			dsp_worker_set_enabled(p, p->vm_running && pmb887x_clc_is_enabled(&p->clc));
			break;

		case DSP_COM_SET:
			if (dsp_comm_trace_enabled())
				DPRINTF("COM_SET %04X (status=%04X) pc=%08X\n",
					(uint16_t) (value & DSP_COM_SET_FLAGS),
					(uint16_t) qatomic_read(&p->comm_status),
					current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
			qemu_mutex_lock(&p->worker.mutex);
			if (qatomic_read(&p->reset_pending)) {
				qatomic_or(&p->reset_comm_flags, value & DSP_COM_SET_FLAGS);
			} else {
				dsp_runtime_set_comm(p->runtime, value & DSP_COM_SET_FLAGS);
				qatomic_or(&p->comm_status, value & DSP_COM_SET_FLAGS);
				qatomic_or(&p->comm_pending, value & DSP_COM_SET_FLAGS);
			}
			qemu_mutex_unlock(&p->worker.mutex);
			dsp_worker_kick(p);
			break;

		case DSP_COM_CLEAR:
			if (dsp_comm_trace_enabled())
				DPRINTF("COM_CLEAR %04X (status=%04X) pc=%08X\n",
					(uint16_t) (value & DSP_COM_CLEAR_FLAGS),
					(uint16_t) qatomic_read(&p->comm_status),
					current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
			qemu_mutex_lock(&p->worker.mutex);
			if (qatomic_read(&p->reset_pending)) {
				qatomic_and(&p->reset_comm_flags, (uint16_t) ~(value & DSP_COM_CLEAR_FLAGS));
			} else {
				dsp_runtime_clear_comm(p->runtime, value & DSP_COM_CLEAR_FLAGS);
				qatomic_and(&p->comm_status, (uint16_t) ~(value & DSP_COM_CLEAR_FLAGS));
				qatomic_and(&p->comm_pending, (uint16_t) ~(value & DSP_COM_CLEAR_FLAGS));
			}
			qemu_mutex_unlock(&p->worker.mutex);
			dsp_worker_kick(p);
			break;

		case DSP_SEM_SET:
			dsp_runtime_request_mcu_semaphores(p->runtime, value & DSP_SEM_SET_FLAGS);
			dsp_worker_kick(p);
			break;

		case DSP_SEM_CLEAR:
			dsp_runtime_release_mcu_semaphores(p->runtime, value & DSP_SEM_CLEAR_FLAGS);
			dsp_worker_kick(p);
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
	uint64_t value = pmb887x_clc_is_enabled(&p->clc) ? dsp_runtime_shared_read_bytes(p->runtime, haddr, size) : 0;

	IO_DUMP_READ(haddr + p->mmio.addr + DSP_RAM0, size, value);
	return value;
}

static void dsp_ram_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	dsp_state_t *p = opaque;

	IO_DUMP_WRITE(haddr + p->mmio.addr + DSP_RAM0, size, value);

	if (!pmb887x_clc_is_enabled(&p->clc))
		return;

	dsp_ramtap(DSP_RAMTAP_WRITE, haddr, value, size);
	dsp_runtime_shared_write_bytes(p->runtime, haddr, value, size);
	dsp_worker_kick(p);
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
	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	dsp_runtime_set_clock(p->runtime, false);
	dsp_reset_internal_state(p);
}

void pmb887x_dsp_set_config(DeviceState *dev, const pmb887x_dsp_config_t *config) {
	dsp_state_t *p = PMB887X_DSP(dev);
	p->config = config;
}

static const Property dsp_properties[] = {
	DEFINE_PROP_UINT32("revision", dsp_state_t, revision, 0),
	DEFINE_PROP_UINT32("rom_version", dsp_state_t, rom_version, 0),
	DEFINE_PROP_LINK("bus_ssc", dsp_state_t, ssc_bus, "SSI", SSIBus *),
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

	p->runtime = dsp_runtime_create(config, p->rom_version, rom->program_rom, rom->data_rom,
		p, dsp_worker_notify_activity, dsp_worker_notify_comm, dsp_ssc_transfer);

	p->worker.stop = false;
	p->worker.enabled = false;
	qemu_mutex_init(&p->worker.mutex);
	qemu_cond_init(&p->worker.cond);
	qemu_cond_init(&p->worker.idle_cond);
	qemu_event_init(&p->worker.event, false);
	p->worker.bh = qemu_bh_new(dsp_worker_bh, p);
	p->afe_timer = timer_new_ns(DSP_AFE_CLOCK, dsp_afe_timer_cb, p);
	qemu_thread_create(&p->worker.thread, "pmb887x-dsp", dsp_worker, p, QEMU_THREAD_JOINABLE);
	p->worker.created = true;

	p->vmstate = qdev_add_vm_change_state_handler(dev, dsp_vm_state_change, NULL, p);
	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	dsp_reset_internal_state(p);
	DPRINTF("core initialized: cpu=%s revision=%02X rom_version=%04X\n", config->name, p->revision, p->rom_version);
}

static void dsp_unrealize(DeviceState *dev) {
	dsp_state_t *p = PMB887X_DSP(dev);

	qemu_del_vm_change_state_handler(p->vmstate);
	p->vmstate = NULL;
	if (p->worker.created) {
		qemu_mutex_lock(&p->worker.mutex);
		p->worker.stop = true;
		qemu_cond_signal(&p->worker.cond);
		qemu_event_set(&p->worker.event);
		qemu_mutex_unlock(&p->worker.mutex);
		qemu_thread_join(&p->worker.thread);
		qemu_bh_delete(p->worker.bh);
		p->worker.bh = NULL;
		p->worker.created = false;
		qemu_cond_destroy(&p->worker.idle_cond);
		qemu_cond_destroy(&p->worker.cond);
		qemu_event_destroy(&p->worker.event);
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
