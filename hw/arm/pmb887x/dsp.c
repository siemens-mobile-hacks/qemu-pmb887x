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
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/ssi/ssi.h"
#include "system/cpu-timers.h"
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
#define DSP_SYNC_PERIOD_NS	(100 * SCALE_US)
#define DSP_REGISTER_READ_WAIT_CYCLES	108
#define DSP_RAM_READ_WAIT_CYCLES	240
#define DSP_RAM_WRITE16_WAIT_CYCLES	276
#define DSP_RAM_WRITE32_WAIT_CYCLES	264
#define DSP_AFE_FREQUENCY	8000
#define DSP_SSC_BUS_NAME	"pmb887x-dsp-ssc"
#define TYPE_PMB887X_DSP	"pmb887x-dsp"
#define PMB887X_DSP(obj)	OBJECT_CHECK(dsp_state_t, (obj), TYPE_PMB887X_DSP)

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

	if (haddr != DSP_CLC && pmb887x_clc_get_hz(&p->clc) == 0) {
		value = UINT32_MAX;
		IO_DUMP_READ(haddr + p->mmio.addr, size, value);
		return value;
	}

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

	if (haddr != DSP_CLC && pmb887x_clc_get_hz(&p->clc) == 0)
		return;

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
	pmb887x_clc_init(&p->clc, DEVICE(obj));
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

	pmb887x_clc_set(&p->clc, 1U << MOD_CLC_RMC_SHIFT);

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

void pmb887x_dsp_set_iq_source(DeviceState *dev, pmb887x_rf_iq_source_t *source) {
	(void) dev;
	(void) source;
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

struct dsp_events_t {
	uint16_t interrupts;
	uint16_t output_events;
	uint16_t outputs;
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
	CPUState *cpu;
	QEMUTimer *sync_timer;
	VMChangeStateEntry *vmstate;
	uint64_t pending_cycles;
	uint64_t cycle_debt;
	uint64_t cycle_remainder;
	uint64_t afe_remainder;
	size_t pending_afe_samples;
	int64_t sync_time_ns;
	uint32_t clock_hz;
	uint16_t comm_status;
	uint16_t comm_pending;
	uint16_t reset_comm_flags;
	uint16_t reset_requests;
	Clock *gsm_clock;
	bool vm_running;
	bool reset_pending;
	bool reset_boot_flag_seen;
	bool sync_queued;
	bool syncing;
	qemu_irq mcu_interrupts[PMB887X_DSP_MCU_INT_COUNT];
	qemu_irq outputs[DSP_OUTPUT_COUNT];
	SSIBus *ssc_bus;
};

static void dsp_clock_update(void *opaque);

static bool dsp_is_clock_enabled(dsp_state_t *p) {
	return p->clock_hz != 0;
}

static void dsp_schedule_sync(dsp_state_t *p);

static uint32_t dsp_ssc_transfer(void *opaque, uint32_t value) {
	dsp_state_t *p = opaque;
	return ssi_transfer(p->ssc_bus, value);
}

static void dsp_take_events(dsp_state_t *p, dsp_events_t *events) {
	events->interrupts = dsp_runtime_take_mcu_irqs(p->runtime);
	events->output_events = dsp_runtime_take_output_events(p->runtime);
	events->outputs = dsp_runtime_get_outputs(p->runtime);
}

static void dsp_publish_events(dsp_state_t *p, const dsp_events_t *events) {
	uint16_t interrupts = (events->interrupts & MAKE_64BIT_MASK(0, PMB887X_DSP_MCU_INT_COUNT));

	for (size_t i = 0; i < ARRAY_SIZE(p->mcu_interrupts); i++)
		if ((interrupts & BIT(i)) != 0)
			qemu_irq_pulse(p->mcu_interrupts[i]);

	for (size_t i = 0; i < ARRAY_SIZE(p->outputs); i++)
		if ((events->output_events & BIT(i)) != 0)
			qemu_set_irq(p->outputs[i], (events->outputs & BIT(i)) != 0);
}

static int64_t dsp_get_time(void) {
	if (icount2_enabled())
		return icount2_get();
	return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void dsp_account_time(dsp_state_t *p) {
	int64_t now = dsp_get_time();
	uint64_t elapsed = MAX(now - p->sync_time_ns, 0);
	uint64_t scaled_cycles = elapsed * p->clock_hz + p->cycle_remainder;
	uint64_t scaled_samples = p->afe_remainder;
	uint64_t elapsed_cycles = scaled_cycles / NANOSECONDS_PER_SECOND;
	if (p->clock_hz != 0)
		scaled_samples += elapsed * DSP_AFE_FREQUENCY;

	p->sync_time_ns = now;
	if (p->cycle_debt >= elapsed_cycles) {
		p->cycle_debt -= elapsed_cycles;
	} else {
		p->pending_cycles += elapsed_cycles - p->cycle_debt;
		p->cycle_debt = 0;
	}
	p->cycle_remainder = scaled_cycles % NANOSECONDS_PER_SECOND;
	p->pending_afe_samples += scaled_samples / NANOSECONDS_PER_SECOND;
	p->afe_remainder = scaled_samples % NANOSECONDS_PER_SECOND;
}

static void dsp_finish_reset(dsp_state_t *p) {
	p->reset_pending = false;
	if (p->reset_comm_flags != 0)
		dsp_runtime_set_comm(p->runtime, p->reset_comm_flags);
	for (size_t i = 0; i < PMB887X_DSP_INT_COUNT; i++)
		if ((p->reset_requests & BIT(i)) != 0)
			dsp_runtime_set_request(p->runtime, i, true);
	p->reset_comm_flags = 0;
	p->reset_requests = 0;
}

static void dsp_sync(dsp_state_t *p) {
	dsp_events_t events = {};
	uint64_t cache_compiles = dsp_runtime_get_cache_compiles(p->runtime);

	dsp_account_time(p);
	if (!p->vm_running || p->clock_hz == 0)
		return;

	int64_t realtime_start = icount2_enabled() ? cpu_get_clock() : 0;
	size_t remaining_cycles = p->pending_cycles;

	p->pending_cycles = 0;
	p->syncing = true;
	while (remaining_cycles != 0 && dsp_runtime_is_running(p->runtime) && !dsp_runtime_is_idle(p->runtime)) {
		bool deferred;
		size_t executed = dsp_runtime_run(p->runtime, remaining_cycles, &deferred);

		if (executed > remaining_cycles) {
			p->cycle_debt += executed - remaining_cycles;
			remaining_cycles = 0;
		} else {
			remaining_cycles -= executed;
		}
		if (executed == 0 || deferred) {
			p->pending_cycles += remaining_cycles;
			remaining_cycles = 0;
			break;
		}
	}
	if (p->reset_pending && p->reset_boot_flag_seen && (dsp_runtime_get_comm(p->runtime) & BIT(0)) == 0)
		dsp_finish_reset(p);
	dsp_runtime_advance_idle(p->runtime, remaining_cycles, p->pending_afe_samples);
	p->pending_afe_samples = 0;
	p->syncing = false;

	p->comm_status = (dsp_runtime_get_comm(p->runtime) | p->reset_comm_flags);
	dsp_take_events(p, &events);
	dsp_publish_events(p, &events);

	cache_compiles = dsp_runtime_get_cache_compiles(p->runtime) - cache_compiles;
	if (dsp_runtime_is_program_warming(p->runtime) && cache_compiles == 0 && p->comm_status == 0)
		dsp_runtime_finish_program_warmup(p->runtime);

	if (icount2_enabled())
		icount2_exclude_realtime(cpu_get_clock() - realtime_start);
}

static void dsp_sync_on_cpu(CPUState *cpu, run_on_cpu_data data) {
	(void) cpu;

	dsp_state_t *p = data.host_ptr;

	p->sync_queued = false;
	dsp_sync(p);
}

static void dsp_schedule_sync(dsp_state_t *p) {
	if (!p->vm_running || p->clock_hz == 0 || p->syncing || p->sync_queued)
		return;

	p->sync_queued = true;
	async_run_on_cpu(p->cpu, dsp_sync_on_cpu, RUN_ON_CPU_HOST_PTR(p));
}

static void dsp_sync_access(dsp_state_t *p) {
	if (current_cpu == p->cpu) {
		dsp_sync(p);
	} else {
		dsp_schedule_sync(p);
	}
}

static void dsp_sync_timer(void *opaque) {
	dsp_state_t *p = opaque;

	dsp_sync_access(p);
	if (p->vm_running && p->clock_hz != 0)
		timer_mod(p->sync_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DSP_SYNC_PERIOD_NS);
}

static void dsp_update_timer(dsp_state_t *p) {
	if (p->vm_running && p->clock_hz != 0) {
		timer_mod(p->sync_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DSP_SYNC_PERIOD_NS);
	} else {
		timer_del(p->sync_timer);
	}
}

static void dsp_clock_update(void *opaque) {
	dsp_state_t *p = opaque;
	uint32_t divider = pmb887x_clc_get_rmc(&p->clc);

	dsp_account_time(p);
	p->clock_hz = pmb887x_clc_get_hz(&p->clc);
	dsp_runtime_set_clock(p->runtime, p->clock_hz != 0);
	if (divider != 0)
		dsp_runtime_set_clock_divider(p->runtime, divider);
	dsp_update_timer(p);
	dsp_schedule_sync(p);
}

static void dsp_vm_state_change(void *opaque, bool running, RunState state) {
	(void) state;

	dsp_state_t *p = opaque;

	dsp_account_time(p);
	p->vm_running = running;
	dsp_update_timer(p);
	dsp_schedule_sync(p);
}

static void dsp_notify_activity(void *opaque) {
	dsp_state_t *p = opaque;

	dsp_runtime_wake(p->runtime);
	dsp_schedule_sync(p);
}

static void dsp_notify_comm(void *opaque, uint16_t flags, bool set) {
	dsp_state_t *p = opaque;
	DPRINTF("comm: flags=%04X set=%u pc=%05X time=%" PRId64 "\n", flags, set,
		dsp_runtime_get_pc(p->runtime), dsp_get_time());

	if (set) {
		p->comm_status |= flags;
		if (p->reset_pending && (flags & BIT(0)) != 0)
			p->reset_boot_flag_seen = true;
	} else {
		p->comm_status &= (uint16_t) ~flags;
		p->comm_pending &= (uint16_t) ~flags;
	}
}

static void dsp_reset_internal_state(dsp_state_t *p) {
	dsp_runtime_reset(p->runtime);
	p->comm_status = 0;
	p->comm_pending = 0;
	p->reset_comm_flags = 0;
	p->reset_requests = 0;
	p->reset_boot_flag_seen = false;
	p->pending_cycles = 0;
	p->cycle_debt = 0;
	p->pending_afe_samples = 0;
	p->cycle_remainder = 0;
	p->afe_remainder = 0;
	p->sync_time_ns = dsp_get_time();
	p->reset_pending = true;
	for (size_t i = 0; i < ARRAY_SIZE(p->outputs); i++)
		qemu_irq_lower(p->outputs[i]);
	p->trace_boot_mode = true;
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

static void dsp_interrupt_input(void *opaque, int id, int level) {
	dsp_state_t *p = opaque;

	if (!level)
		return;

	if (pmb887x_trace_log_enabled(PMB887X_TRACE_DSP))
		dsp_trace_command(p, id);

	if (p->reset_pending) {
		p->reset_requests |= BIT(id);
	} else {
		dsp_runtime_set_request(p->runtime, id, true);
	}
	dsp_schedule_sync(p);
}

static void dsp_set_input(dsp_state_t *p, size_t index, int level) {
	dsp_runtime_set_input(p->runtime, index, level != 0);
	dsp_schedule_sync(p);
}

static void dsp_input0(void *opaque, int id, int level) {
	dsp_set_input(opaque, 0, level);
}

static void dsp_input1(void *opaque, int id, int level) {
	dsp_set_input(opaque, 1, level);
}

static void dsp_gsm_input(void *opaque, int signal, int level) {
	dsp_state_t *p = opaque;

	dsp_sync_access(p);
	dsp_runtime_set_gsm_signal(p->runtime, signal, level != 0);
	dsp_schedule_sync(p);
}

static uint64_t dsp_io_read(void *opaque, hwaddr haddr, unsigned size) {
	dsp_state_t *p = opaque;
	uint64_t value = 0;

	if (icount2_enabled())
		icount2_advance(DSP_REGISTER_READ_WAIT_CYCLES);
	dsp_sync_access(p);

	switch (haddr) {
		case DSP_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case DSP_ID:
			value = (0xF022C000 | p->revision);
			break;

		case DSP_COM_STATUS: {
			uint32_t program_start_pc;

			value = p->comm_status;
			if (dsp_runtime_take_program_start(p->runtime, &program_start_pc))
				DPRINTF("cold program start: pc=%05X flags=%04" PRIX64 "\n", program_start_pc, value);
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
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}

	IO_DUMP_READ(haddr + p->mmio.addr, size, value);
	return value;
}

static void dsp_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	dsp_state_t *p = opaque;

	dsp_sync_access(p);

	switch (haddr) {
		case DSP_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case DSP_COM_SET:
			if (p->reset_pending) {
				p->reset_comm_flags |= (value & DSP_COM_SET_FLAGS);
			} else {
				dsp_runtime_set_comm(p->runtime, (value & DSP_COM_SET_FLAGS));
			}
			p->comm_status |= (value & DSP_COM_SET_FLAGS);
			p->comm_pending |= (value & DSP_COM_SET_FLAGS);
			break;

		case DSP_COM_CLEAR:
			dsp_runtime_clear_comm(p->runtime, (value & DSP_COM_CLEAR_FLAGS));
			if (p->reset_pending)
				p->reset_comm_flags &= (uint16_t) ~(value & DSP_COM_CLEAR_FLAGS);
			p->comm_status &= (uint16_t) ~(value & DSP_COM_CLEAR_FLAGS);
			p->comm_pending &= (uint16_t) ~(value & DSP_COM_CLEAR_FLAGS);
			break;

		case DSP_SEM_SET:
			dsp_runtime_request_mcu_semaphores(p->runtime, (value & DSP_SEM_SET_FLAGS));
			break;

		case DSP_SEM_CLEAR:
			dsp_runtime_release_mcu_semaphores(p->runtime, (value & DSP_SEM_CLEAR_FLAGS));
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			break;
	}

	dsp_schedule_sync(p);
	IO_DUMP_WRITE(haddr + p->mmio.addr, size, value);
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
	uint64_t value;

	if (icount2_enabled())
		icount2_advance(DSP_RAM_READ_WAIT_CYCLES);
	dsp_sync_access(p);
	value = dsp_is_clock_enabled(p) ? dsp_runtime_shared_read_bytes(p->runtime, haddr, size) : 0;

	IO_DUMP_READ(haddr + p->mmio.addr + DSP_RAM0, size, value);
	return value;
}

static void dsp_ram_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	dsp_state_t *p = opaque;

	if (icount2_enabled())
		icount2_advance(size == 4 ? DSP_RAM_WRITE32_WAIT_CYCLES : DSP_RAM_WRITE16_WAIT_CYCLES);
	dsp_sync_access(p);

	if (dsp_is_clock_enabled(p)) {
		dsp_runtime_shared_write_bytes(p->runtime, haddr, value, size);
		dsp_schedule_sync(p);
	}

	IO_DUMP_WRITE(haddr + p->mmio.addr + DSP_RAM0, size, value);
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

	pmb887x_clc_init(&p->clc, DEVICE(obj));
	pmb887x_clc_set_callback(&p->clc, dsp_clock_update, p);
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
	p->clock_hz = 0;
	dsp_runtime_set_clock(p->runtime, false);
	dsp_update_timer(p);
	dsp_reset_internal_state(p);
}

void pmb887x_dsp_set_config(DeviceState *dev, const pmb887x_dsp_config_t *config) {
	dsp_state_t *p = PMB887X_DSP(dev);
	p->config = config;
}

void pmb887x_dsp_set_iq_source(DeviceState *dev, pmb887x_rf_iq_source_t *source) {
	dsp_state_t *p = PMB887X_DSP(dev);

	dsp_runtime_set_iq_source(p->runtime, source);
}

static const Property dsp_properties[] = {
	DEFINE_PROP_UINT32("revision", dsp_state_t, revision, 0),
	DEFINE_PROP_UINT32("rom_version", dsp_state_t, rom_version, 0),
	DEFINE_PROP_LINK("cpu", dsp_state_t, cpu, TYPE_CPU, CPUState *),
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
	if (p->cpu == NULL) {
		error_setg(errp, "DSP CPU link is not set");
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
		p, dsp_notify_activity, dsp_notify_comm, dsp_ssc_transfer);
	p->sync_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dsp_sync_timer, p);
	p->sync_time_ns = dsp_get_time();

	p->vmstate = qdev_add_vm_change_state_handler(dev, dsp_vm_state_change, NULL, p);
	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	dsp_reset_internal_state(p);
	DPRINTF("core initialized: cpu=%s revision=%02X rom_version=%04X\n", config->name, p->revision, p->rom_version);
}

static void dsp_unrealize(DeviceState *dev) {
	dsp_state_t *p = PMB887X_DSP(dev);

	qemu_del_vm_change_state_handler(p->vmstate);
	p->vmstate = NULL;
	timer_del(p->sync_timer);
	run_on_cpu(p->cpu, dsp_sync_on_cpu, RUN_ON_CPU_HOST_PTR(p));
	timer_free(p->sync_timer);
	p->sync_timer = NULL;

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
