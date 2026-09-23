/*
 * Capture/Compare
 * */
#define PMB887X_TRACE_ID		CAPCOM
#define PMB887X_TRACE_PREFIX	"pmb887x-capcom"

#include "qemu/osdep.h"
#include <math.h>
#include "hw/core/sysbus.h"
#include "hw/core/hw-error.h"
#include "system/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/audio.h"
#include "hw/arm/pmb887x/pmic.h"
#include "hw/core/qdev-properties.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_CAPCOM	"pmb887x-capcom"
#define PMB887X_CAPCOM(obj)	OBJECT_CHECK(pmb887x_capcom_t, (obj), TYPE_PMB887X_CAPCOM)

/*
 * The firmware plays .srt ringtones as a square wave on a compare output: one
 * channel in compare mode 3, its timer reloaded to the note period and the
 * compare register to half of it. Nothing else in the machine carries that
 * signal, so the unit renders it to the host directly.
 */
#define CAPCOM_TONE_RATE	44100
#define CAPCOM_TONE_LEVEL	6000
/*
 * Only analog stages lie between the compare output and the speaker, so the
 * square wave reaches it without alias products. Each edge is rendered as a
 * band-limited step, the running integral of a Kaiser-windowed sinc, placed at
 * the exact time the edge falls between two samples. The rate is the audiodev
 * default, so the mixer passes the tone through without resampling it.
 */
#define CAPCOM_BLEP_TAPS	32
#define CAPCOM_BLEP_PHASES	256
#define CAPCOM_BLEP_CUTOFF	0.45
#define CAPCOM_BLEP_BETA	8.0
/* Keeps the voice open after the tone stops, until its last step has drained. */
#define CAPCOM_TONE_LINGER_NS	(50 * SCALE_MS)
/*
 * Timer input ahead of the T0I/T1I stage. The C16x CAPCOM documents a /8
 * prescaler on its module clock. The S-GOLD module clock is undocumented;
 * fsys/16 is that /8 from 26 MHz, and puts the .srt notes at their pitch.
 */
#define CAPCOM_TIMER_PRESCALER	16

typedef struct pmb887x_capcom_t pmb887x_capcom_t;
typedef struct pmb887x_capcom_cc_t pmb887x_capcom_cc_t;
typedef enum pmb887x_capcom_cc_mode_t pmb887x_capcom_cc_mode_t;

struct pmb887x_capcom_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	uint32_t revision;
	
	qemu_irq t_irq[2];
	qemu_irq cc_irq[8];
	
	pmb887x_src_reg_t t_src[2];
	pmb887x_src_reg_t cc_src[8];
	
	pmb887x_clc_reg_t clc;
	
	uint32_t pisel;
	uint32_t t01con;
	uint32_t ccm[2];
	uint32_t out;
	uint32_t ioc;
	uint32_t sem;
	uint32_t see;
	uint32_t drm;
	uint32_t whbssee;
	uint32_t whbcsee;
	uint32_t t0;
	uint32_t t0rel;
	uint32_t t1;
	uint32_t t1rel;
	uint32_t t01ocr;
	uint32_t whbsout;
	uint32_t whbcout;
	uint32_t cc[8];

	struct pmb887x_cgu_t *cgu;
	AudioBackend *audio;
	SWVoiceOut *tone_voice;
	QEMUTimer *tone_linger;
	bool tone_voice_on;
	/* The output the registers program, in samples per cycle; 0 is silence. */
	double tone_next_period;
	double tone_next_duty;
	/* The cycle being rendered. The timer takes a new reload at overflow. */
	double tone_period;
	double tone_duty;
	double tone_phase;
	bool tone_high;
	double tone_level;
	float tone_steps[CAPCOM_BLEP_TAPS];
	unsigned tone_step_pos;
	/* Rendered samples the voice has not taken yet. */
	int16_t tone_chunk[256];
	size_t tone_chunk_pos;
	size_t tone_chunk_len;
};

struct pmb887x_capcom_cc_t {
	uint32_t ccm_index;
	uint32_t acc_mask;
	uint32_t acc_shift;
	uint32_t mod_mask;
	uint32_t mod_shift;
};

enum pmb887x_capcom_cc_mode_t {
	CAPCOM_CC_MODE_DISABLED = 0,
	CAPCOM_CC_MODE_RISING_EDGE,
	CAPCOM_CC_MODE_FALLING_EDGE,
	CAPCOM_CC_MODE_BOTH_EDGES,
	CAPCOM_CC_MODE_0,
	CAPCOM_CC_MODE_1,
	CAPCOM_CC_MODE_2,
	CAPCOM_CC_MODE_3,
};

const pmb887x_capcom_cc_t capcom_cc_list[] = {
	{ 0, CAPCOM_CCM0_ACC0, CAPCOM_CCM0_ACC0_SHIFT, CAPCOM_CCM0_MOD0, CAPCOM_CCM0_MOD0_SHIFT },
	{ 0, CAPCOM_CCM0_ACC1, CAPCOM_CCM0_ACC1_SHIFT, CAPCOM_CCM0_MOD1, CAPCOM_CCM0_MOD1_SHIFT },
	{ 0, CAPCOM_CCM0_ACC2, CAPCOM_CCM0_ACC2_SHIFT, CAPCOM_CCM0_MOD2, CAPCOM_CCM0_MOD2_SHIFT },
	{ 0, CAPCOM_CCM0_ACC3, CAPCOM_CCM0_ACC3_SHIFT, CAPCOM_CCM0_MOD3, CAPCOM_CCM0_MOD3_SHIFT },
	{ 1, CAPCOM_CCM1_ACC4, CAPCOM_CCM1_ACC4_SHIFT, CAPCOM_CCM1_MOD4, CAPCOM_CCM1_MOD4_SHIFT },
	{ 1, CAPCOM_CCM1_ACC5, CAPCOM_CCM1_ACC5_SHIFT, CAPCOM_CCM1_MOD5, CAPCOM_CCM1_MOD5_SHIFT },
	{ 1, CAPCOM_CCM1_ACC6, CAPCOM_CCM1_ACC6_SHIFT, CAPCOM_CCM1_MOD6, CAPCOM_CCM1_MOD6_SHIFT },
	{ 1, CAPCOM_CCM1_ACC7, CAPCOM_CCM1_ACC7_SHIFT, CAPCOM_CCM1_MOD7, CAPCOM_CCM1_MOD7_SHIFT },
};

static enum pmb887x_capcom_cc_mode_t capcom_get_mode(pmb887x_capcom_t *p, int id) {
	const pmb887x_capcom_cc_t *cc = &capcom_cc_list[id];
	uint32_t ccm = p->ccm[cc->ccm_index];
	return (ccm & cc->mod_mask) >> cc->mod_shift;
}

/* Ticks per second of a timer, or zero when it is stopped or counting events. */
static uint32_t capcom_timer_freq(pmb887x_capcom_t *p, bool timer1) {
	uint32_t input = timer1 ? CAPCOM_T01CON_T1I : CAPCOM_T01CON_T0I;
	uint32_t shift = timer1 ? CAPCOM_T01CON_T1I_SHIFT : CAPCOM_T01CON_T0I_SHIFT;
	uint32_t run = timer1 ? CAPCOM_T01CON_T1R : CAPCOM_T01CON_T0R;
	uint32_t counter = timer1 ? CAPCOM_T01CON_T1M : CAPCOM_T01CON_T0M;
	uint8_t rmc = pmb887x_clc_get_rmc(&p->clc);

	if (!p->cgu || rmc == 0 || (p->t01con & run) == 0 || (p->t01con & counter) != 0)
		return 0;
	return pmb887x_pll_get_fsys(p->cgu) / rmc /
		(CAPCOM_TIMER_PRESCALER << ((p->t01con & input) >> shift));
}

/*
 * The square wave the first compare channel in mode 3 drives on its output:
 * its timer runs free from the reload value to overflow, the output goes high
 * on the compare match and low again on the overflow. Returns its cycle in
 * output samples and the fraction of the cycle it spends high.
 */
static bool capcom_compare_output(pmb887x_capcom_t *p, double *period, double *duty) {
	for (size_t i = 0; i < ARRAY_SIZE(capcom_cc_list); i++) {
		const pmb887x_capcom_cc_t *cc = &capcom_cc_list[i];
		uint32_t ccm = p->ccm[cc->ccm_index];
		bool timer1 = (ccm & cc->acc_mask) != 0;
		uint32_t reload = timer1 ? p->t1rel : p->t0rel;
		uint32_t clock = capcom_timer_freq(p, timer1);
		uint32_t ticks = 0x10000 - reload;

		if (((ccm & cc->mod_mask) >> cc->mod_shift) != CAPCOM_CC_MODE_3)
			continue;
		if (clock == 0 || reload == 0 || p->cc[i] <= reload)
			continue;

		*period = (double) ticks * CAPCOM_TONE_RATE / clock;
		*duty = (double) (0x10000 - p->cc[i]) / ticks;
		return true;
	}
	return false;
}

static void capcom_update_state(pmb887x_capcom_t *p) {
	double period = 0, duty = 0;

	/* Tones below 20 Hz or above the output's Nyquist frequency are not heard. */
	if (!capcom_compare_output(p, &period, &duty) ||
			period > CAPCOM_TONE_RATE / 20.0 || period < 2.0)
		period = 0;
	p->tone_next_period = period;
	p->tone_next_duty = duty;

	if (p->tone_voice == NULL)
		return;
	if (period == 0) {
		if (p->tone_voice_on && !timer_pending(p->tone_linger))
			timer_mod(p->tone_linger, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT) + CAPCOM_TONE_LINGER_NS);
		return;
	}
	timer_del(p->tone_linger);
	if (!p->tone_voice_on) {
		p->tone_voice_on = true;
		audio_be_set_active_out(p->audio, p->tone_voice, true);
	}
}

static void capcom_tone_reset(pmb887x_capcom_t *p) {
	p->tone_period = 0;
	p->tone_phase = 0;
	p->tone_high = false;
	p->tone_level = 0;
	memset(p->tone_steps, 0, sizeof(p->tone_steps));
	p->tone_step_pos = 0;
	p->tone_chunk_pos = 0;
	p->tone_chunk_len = 0;
}

static void capcom_tone_linger(void *opaque) {
	pmb887x_capcom_t *p = opaque;

	if (p->tone_next_period != 0 && p->tone_voice_on)
		return;
	p->tone_voice_on = false;
	audio_be_set_active_out(p->audio, p->tone_voice, false);
	capcom_tone_reset(p);
}

static float capcom_blep[CAPCOM_BLEP_TAPS * CAPCOM_BLEP_PHASES + 1];

static double capcom_bessel_i0(double x) {
	double sum = 1, term = 1;

	for (int k = 1; k < 50; k++) {
		term *= (x / (2 * k)) * (x / (2 * k));
		sum += term;
	}
	return sum;
}

/* The step, centred in the table, from 0 at its start to 1 at its end. */
static void capcom_blep_init(void) {
	size_t n = ARRAY_SIZE(capcom_blep);
	double sum = 0;

	for (size_t i = 0; i < n; i++) {
		double x = (double) i / CAPCOM_BLEP_PHASES - CAPCOM_BLEP_TAPS / 2.0;
		double w = 2.0 * i / (n - 1) - 1.0;
		double arg = M_PI * 2 * CAPCOM_BLEP_CUTOFF * x;
		double sinc = x == 0 ? 1 : sin(arg) / arg;
		double window = capcom_bessel_i0(CAPCOM_BLEP_BETA * sqrt(1 - w * w)) /
			capcom_bessel_i0(CAPCOM_BLEP_BETA);

		sum += sinc * window;
		capcom_blep[i] = sum;
	}
	for (size_t i = 0; i < n; i++)
		capcom_blep[i] /= sum;
}

/*
 * Move the output by @height at @offset of a sample past the next one out.
 * The level takes the whole step now; the ring holds what the band-limited
 * edge still owes it over the next CAPCOM_BLEP_TAPS samples.
 */
static void capcom_tone_edge(pmb887x_capcom_t *p, double offset, double height) {
	for (int k = 0; k < CAPCOM_BLEP_TAPS; k++) {
		double x = (k - offset) * CAPCOM_BLEP_PHASES;
		double step = 0;

		if (x >= 0) {
			size_t i = (size_t) x;
			double frac = x - i;
			step = capcom_blep[i] + frac * (capcom_blep[i + 1] - capcom_blep[i]);
		}
		p->tone_steps[(p->tone_step_pos + k) % CAPCOM_BLEP_TAPS] += height * (step - 1);
	}
	p->tone_level += height;
}

/* The output levels, with the mean the coupling to the amplifier blocks taken out. */
static double capcom_tone_pin_level(pmb887x_capcom_t *p, bool high) {
	return high ? 2 * (1 - p->tone_duty) : -2 * p->tone_duty;
}

static void capcom_tone_move(pmb887x_capcom_t *p, double offset, double level) {
	if (level != p->tone_level)
		capcom_tone_edge(p, offset, level - p->tone_level);
}

/* Render the edges that fall within the next sample period. */
static void capcom_tone_advance(pmb887x_capcom_t *p) {
	double t = 0;

	if (p->tone_period == 0 && p->tone_next_period != 0) {
		p->tone_period = p->tone_next_period;
		p->tone_duty = p->tone_next_duty;
		p->tone_phase = 0;
		p->tone_high = false;
		capcom_tone_move(p, 0, capcom_tone_pin_level(p, false));
	} else if (p->tone_period != 0 && p->tone_next_period == 0) {
		p->tone_period = 0;
		capcom_tone_move(p, 0, 0);
	}
	if (p->tone_period == 0)
		return;

	for (;;) {
		double edge = p->tone_high ? p->tone_period : p->tone_period * (1 - p->tone_duty);
		double until = edge - p->tone_phase;

		if (t + until >= 1.0) {
			p->tone_phase += 1.0 - t;
			return;
		}
		t += until;
		if (p->tone_high) {
			p->tone_period = p->tone_next_period;
			p->tone_duty = p->tone_next_duty;
			p->tone_phase = 0;
		} else {
			p->tone_phase = edge;
		}
		p->tone_high = !p->tone_high;
		capcom_tone_move(p, t, capcom_tone_pin_level(p, p->tone_high));
	}
}

static void capcom_tone_render(pmb887x_capcom_t *p, size_t count) {
	/*
	 * The compare output drives the codec's amplifier path 0, which the
	 * firmware walks up from silence to fade a ringtone in.
	 */
	double scale = (double) CAPCOM_TONE_LEVEL *
		pmb887x_pmic_output_gain(PMB887X_PMIC_PATH_TONE) / PMB887X_PMIC_GAIN_UNITY;

	for (size_t i = 0; i < count; i++) {
		float *owed = &p->tone_steps[p->tone_step_pos];

		capcom_tone_advance(p);
		p->tone_chunk[i] = (int16_t) lrint((p->tone_level + *owed) * scale);
		*owed = 0;
		p->tone_step_pos = (p->tone_step_pos + 1) % CAPCOM_BLEP_TAPS;
	}
	p->tone_chunk_pos = 0;
	p->tone_chunk_len = count;
}

static void capcom_tone_callback(void *opaque, int free_bytes) {
	pmb887x_capcom_t *p = opaque;

	while (free_bytes >= (int) sizeof(p->tone_chunk[0])) {
		size_t written;

		if (p->tone_chunk_pos == p->tone_chunk_len)
			capcom_tone_render(p, MIN((size_t) free_bytes / sizeof(p->tone_chunk[0]), ARRAY_SIZE(p->tone_chunk)));

		written = audio_be_write(p->audio, p->tone_voice, &p->tone_chunk[p->tone_chunk_pos],
			(p->tone_chunk_len - p->tone_chunk_pos) * sizeof(p->tone_chunk[0]));
		if (written == 0)
			break;
		p->tone_chunk_pos += written / sizeof(p->tone_chunk[0]);
		free_bytes -= written;
	}
}

static int capcom_get_index_from_reg(uint32_t reg) {
	switch (reg) {
		case CAPCOM_CC7_SRC:	return 7;
		case CAPCOM_CC6_SRC:	return 6;
		case CAPCOM_CC5_SRC:	return 5;
		case CAPCOM_CC4_SRC:	return 4;
		case CAPCOM_CC3_SRC:	return 3;
		case CAPCOM_CC2_SRC:	return 2;
		case CAPCOM_CC1_SRC:	return 1;
		case CAPCOM_CC0_SRC:	return 0;
		case CAPCOM_T1_SRC:		return 1;
		case CAPCOM_T0_SRC:		return 0;
		default:				abort();
	};
}

static uint64_t capcom_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_capcom_t *p = opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case CAPCOM_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;
		
		case CAPCOM_ID:
			value = 0x00005000 | p->revision;
			break;
		
		case CAPCOM_PISEL:
			value = p->pisel;
			break;
		
		case CAPCOM_T01CON:
			value = p->t01con;
			break;
		
		case CAPCOM_CCM0:
			value = p->ccm[0];
			break;
		
		case CAPCOM_CCM1:
			value = p->ccm[1];
			break;
		
		case CAPCOM_OUT:
			value = p->out;
			break;
		
		case CAPCOM_IOC:
			value = p->ioc;
			break;
		
		case CAPCOM_SEM:
			value = p->sem;
			break;
		
		case CAPCOM_SEE:
			value = p->see;
			break;
		
		case CAPCOM_DRM:
			value = p->drm;
			break;
		
		case CAPCOM_WHBSSEE:
			value = p->whbssee;
			break;
		
		case CAPCOM_WHBCSEE:
			value = p->whbcsee;
			break;
		
		case CAPCOM_T0:
			value = p->t0;
			break;
		
		case CAPCOM_T0REL:
			value = p->t0rel;
			break;
		
		case CAPCOM_T1:
			value = p->t1;
			break;
		
		case CAPCOM_T1REL:
			value = p->t1rel;
			break;
		
		case CAPCOM_T01OCR:
			value = p->t01ocr;
			break;
		
		case CAPCOM_WHBSOUT:
			value = p->whbsout;
			break;
		
		case CAPCOM_WHBCOUT:
			value = p->whbcout;
			break;
		
		case CAPCOM_CC7_SRC:
		case CAPCOM_CC6_SRC:
		case CAPCOM_CC5_SRC:
		case CAPCOM_CC4_SRC:
		case CAPCOM_CC3_SRC:
		case CAPCOM_CC2_SRC:
		case CAPCOM_CC1_SRC:
		case CAPCOM_CC0_SRC:
			value = pmb887x_src_get(&p->cc_src[capcom_get_index_from_reg(haddr)]);
			break;
		
		case CAPCOM_T1_SRC:
		case CAPCOM_T0_SRC:
			value = pmb887x_src_get(&p->t_src[capcom_get_index_from_reg(haddr)]);
			break;
		
		case CAPCOM_CC0:
		case CAPCOM_CC1:
		case CAPCOM_CC2:
		case CAPCOM_CC3:
		case CAPCOM_CC4:
		case CAPCOM_CC5:
		case CAPCOM_CC6:
		case CAPCOM_CC7:
			value = p->cc[(haddr - CAPCOM_CC0) / 4];
			break;

		default:
			IO_DUMP_READ(haddr + p->mmio.addr, size, 0xFFFFFFFF);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}
	
	IO_DUMP_READ(haddr + p->mmio.addr, size, value);
	
	return value;
}

static void capcom_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_capcom_t *p = opaque;
	
	IO_DUMP_WRITE(haddr + p->mmio.addr, size, value);
	
	switch (haddr) {
		case CAPCOM_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;
		
		case CAPCOM_PISEL:
			p->pisel = value;
			break;
		
		case CAPCOM_T01CON:
			p->t01con = value;
			break;
		
		case CAPCOM_CCM0:
			p->ccm[0] = value;
			break;
		
		case CAPCOM_CCM1:
			p->ccm[1] = value;
			break;
		
		case CAPCOM_OUT:
			p->out = value;
			break;
		
		case CAPCOM_IOC:
			p->ioc = value;
			break;
		
		case CAPCOM_SEM:
			p->sem = value;
			break;
		
		case CAPCOM_SEE:
			p->see = value;
			break;
		
		case CAPCOM_DRM:
			p->drm = value;
			break;
		
		case CAPCOM_WHBSSEE:
			p->whbssee = value;
			break;
		
		case CAPCOM_WHBCSEE:
			p->whbcsee = value;
			break;
		
		case CAPCOM_T0:
			p->t0 = value;
			break;
		
		case CAPCOM_T0REL:
			p->t0rel = value;
			break;
		
		case CAPCOM_T1:
			p->t1 = value;
			break;
		
		case CAPCOM_T1REL:
			p->t1rel = value;
			break;
		
		case CAPCOM_T01OCR:
			p->t01ocr = value;
			break;
		
		case CAPCOM_WHBSOUT:
			p->whbsout = value;
			break;
		
		case CAPCOM_WHBCOUT:
			p->whbcout = value;
			break;
		
		case CAPCOM_CC7_SRC:
		case CAPCOM_CC6_SRC:
		case CAPCOM_CC5_SRC:
		case CAPCOM_CC4_SRC:
		case CAPCOM_CC3_SRC:
		case CAPCOM_CC2_SRC:
		case CAPCOM_CC1_SRC:
		case CAPCOM_CC0_SRC:
			pmb887x_src_set(&p->cc_src[capcom_get_index_from_reg(haddr)], value);
			break;
		
		case CAPCOM_T1_SRC:
		case CAPCOM_T0_SRC:
			pmb887x_src_set(&p->t_src[capcom_get_index_from_reg(haddr)], value);
			break;

		case CAPCOM_CC0:
		case CAPCOM_CC1:
		case CAPCOM_CC2:
		case CAPCOM_CC3:
		case CAPCOM_CC4:
		case CAPCOM_CC5:
		case CAPCOM_CC6:
		case CAPCOM_CC7:
			p->cc[(haddr - CAPCOM_CC0) / 4] = value;
			break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}
	
	capcom_update_state(p);
}

static void capcom_handle_input_change(pmb887x_capcom_t *p, int id, int level) {
	DPRINTF("CC%d MODE=%d\n", id, capcom_get_mode(p, id));

	if (capcom_get_mode(p, id) == CAPCOM_CC_MODE_RISING_EDGE && level == 1)
		pmb887x_src_update(&p->cc_src[id], 0, MOD_SRC_SETR);

	if (capcom_get_mode(p, id) == CAPCOM_CC_MODE_FALLING_EDGE && level == 0)
		pmb887x_src_update(&p->cc_src[id], 0, MOD_SRC_SETR);

	if (capcom_get_mode(p, id) == CAPCOM_CC_MODE_BOTH_EDGES)
		pmb887x_src_update(&p->cc_src[id], 0, MOD_SRC_SETR);
}

static void capcom_input_cc0_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 0, level);
}

static void capcom_input_cc1_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 1, level);
}

static void capcom_input_cc2_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 2, level);
}

static void capcom_input_cc3_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 3, level);
}

static void capcom_input_cc4_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 4, level);
}

static void capcom_input_cc5_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 5, level);
}

static void capcom_input_cc6_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 6, level);
}

static void capcom_input_cc7_handler(void *opaque, int id, int level) {
	capcom_handle_input_change(opaque, 7, level);
}

static const MemoryRegionOps io_ops = {
	.read			= capcom_io_read,
	.write			= capcom_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void capcom_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_capcom_t *p = PMB887X_CAPCOM(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-capcom", CAPCOM_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (int i = 0; i < ARRAY_SIZE(p->t_src); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->t_irq[i]);
	
	for (int i = 0; i < ARRAY_SIZE(p->cc_src); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->cc_irq[i]);

	qdev_init_gpio_in_named(dev, capcom_input_cc0_handler, "CC0_IN", 1);
	qdev_init_gpio_in_named(dev, capcom_input_cc1_handler, "CC1_IN", 1);
	qdev_init_gpio_in_named(dev, capcom_input_cc2_handler, "CC2_IN", 1);
	qdev_init_gpio_in_named(dev, capcom_input_cc3_handler, "CC3_IN", 1);
	qdev_init_gpio_in_named(dev, capcom_input_cc4_handler, "CC4_IN", 1);
	qdev_init_gpio_in_named(dev, capcom_input_cc5_handler, "CC5_IN", 1);
	qdev_init_gpio_in_named(dev, capcom_input_cc6_handler, "CC6_IN", 1);
	qdev_init_gpio_in_named(dev, capcom_input_cc7_handler, "CC7_IN", 1);
}

static void capcom_reset(DeviceState *dev) {
	pmb887x_capcom_t *p = PMB887X_CAPCOM(dev);

	pmb887x_clc_init(&p->clc);

	for (size_t i = 0; i < ARRAY_SIZE(p->t_src); i++)
		pmb887x_src_reset(&p->t_src[i]);
	for (size_t i = 0; i < ARRAY_SIZE(p->cc_src); i++)
		pmb887x_src_reset(&p->cc_src[i]);

	p->pisel = 0;
	p->t01con = 0;
	memset(p->ccm, 0, sizeof(p->ccm));
	p->out = 0;
	p->ioc = 0;
	p->sem = 0;
	p->see = 0;
	p->drm = 0;
	p->whbssee = 0;
	p->whbcsee = 0;
	p->t0 = 0;
	p->t0rel = 0;
	p->t1 = 0;
	p->t1rel = 0;
	p->t01ocr = 0;
	p->whbsout = 0;
	p->whbcout = 0;
	memset(p->cc, 0, sizeof(p->cc));

	capcom_update_state(p);
}

static void capcom_realize(DeviceState *dev, Error **errp) {
	pmb887x_capcom_t *p = PMB887X_CAPCOM(dev);
	
	pmb887x_clc_init(&p->clc);
	
	int irqn = 0;
	
	for (int i = 0; i < ARRAY_SIZE(p->t_src); i++) {
		if (!p->t_irq[i])
			hw_error("pmb887x-scu: irq %d (T%d) not set", irqn, i);
		pmb887x_src_init(&p->t_src[i], p->t_irq[i]);
		irqn++;
	}
	
	for (int i = 0; i < ARRAY_SIZE(p->cc_src); i++) {
		if (!p->cc_irq[i])
			hw_error("pmb887x-scu: irq %d (CC%d) not set", irqn, i);
		pmb887x_src_init(&p->cc_src[i], p->cc_irq[i]);
		irqn++;
	}
	
	if (audio_be_check(&p->audio, NULL)) {
		struct audsettings as = {
			.freq = CAPCOM_TONE_RATE,
			.nchannels = 1,
			.fmt = AUDIO_FORMAT_S16,
			.big_endian = false,
		};
		capcom_blep_init();
		p->tone_linger = timer_new_ns(QEMU_CLOCK_VIRTUAL_RT, capcom_tone_linger, p);
		p->tone_voice = audio_be_open_out(p->audio, NULL, "pmb887x-capcom-tone",
			p, capcom_tone_callback, &as);
	}

	capcom_update_state(p);
}

static const Property capcom_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_capcom_t, revision, 0),
	DEFINE_PROP_LINK("cgu", pmb887x_capcom_t, cgu, "pmb887x-cgu", struct pmb887x_cgu_t *),
};

static void capcom_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, capcom_properties);
	device_class_set_legacy_reset(dc, capcom_reset);
	dc->realize = capcom_realize;
}

static const TypeInfo capcom_info = {
    .name          	= TYPE_PMB887X_CAPCOM,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(struct pmb887x_capcom_t),
    .instance_init 	= capcom_init,
    .class_init    	= capcom_class_init,
};

static void capcom_register_types(void) {
	type_register_static(&capcom_info);
}
type_init(capcom_register_types)
