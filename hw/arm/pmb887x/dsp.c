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

#define STUB_DSP 1
#ifdef STUB_DSP
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

typedef struct dsp_state_t dsp_state_t;

struct dsp_state_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;

	uint32_t com_set;
	uint32_t com_status;
	uint32_t sem_set;
	uint32_t sem_status;
	uint8_t ram[DSP_RAM_SIZE];
	uint32_t ram0_value;
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
			p->com_status = 0;
			// if ((value & 1) != 0) dsp_exec_command_ch0(p); // noisy logs
			if ((value & 2) != 0) dsp_exec_command_ch1(p);
			if ((value & 4) != 0) dsp_exec_command_ch2(p);
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
	DEFINE_PROP_UINT32("ram0_value", dsp_state_t, ram0_value, 0x0801),
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

	dsp_ram_write(p, 0, p->ram0_value, 2);
	dsp_update_state(p);
}

static void dsp_unrealize(DeviceState *dev) {
	dsp_state_t *p = PMB887X_DSP(dev);

	if (p->refill_timer != NULL) {
		timer_free(p->refill_timer);
		p->refill_timer = NULL;
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

static void dsp_afe_timer_cb(void *opaque) {
	dsp_state_t *p = opaque;
	bool active = dsp_runtime_realtime_active(p->runtime);

#if 0	/* AFE timer debug */
	static uint32_t tn;
	if ((tn++ & 0x3FF) == 0)
		fprintf(stderr, "[afe-timer] n=%u active=%d\n", tn, active);
#endif

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

	if (!level)
		return;

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
		DPRINTF("comm handshake STALL: status=%04X pending=%04X dsp_pc=%05X int0_flags=%04X int0_pending=%04X "
			"ie=%d mask=%X lines=%X isr_active=%d running=%d idle=%d realtime=%d\n",
			qatomic_read(&p->comm_status), qatomic_read(&p->comm_pending),
			dsp_runtime_get_pc(p->runtime), dsp_runtime_get_irq_flags(p->runtime, 0),
			dsp_runtime_get_irq_pending_flags(p->runtime, 0), ie, mask, lines,
			dsp_runtime_is_maskable_interrupt_active(p->runtime), p->runtime_running,
			dsp_runtime_is_idle(p->runtime), dsp_runtime_realtime_active(p->runtime));

		/* If stuck in the mask-ROM timer-queue insert (0x2340-0x2346), dump the
		 * event-queue region: 0x7c54 sentinel, 0x7c55 head, 0x7c56.. node pool. */
		uint32_t pc = dsp_runtime_get_pc(p->runtime);
		if (pc >= 0x2340 && pc <= 0x2346) {
			char buf[256];
			int n = 0;
			for (uint16_t a = 0x7C54; a <= 0x7C63; a++)
				n += snprintf(buf + n, sizeof(buf) - n, "%04X ", dsp_runtime_peek(p->runtime, a));
			DPRINTF("evq[7C54..7C63]: %s\n", buf);
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
