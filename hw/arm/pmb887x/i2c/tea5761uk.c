/*
 * NXP TEA5761UK FM radio
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/timer.h"
#include "hw/audio/audio-line.h"
#include "hw/i2c/i2c.h"
#include "system/debug-channel.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"
#include "hw/arm/pmb887x/gen/peripheral/TEA5761UK.h"

#define TYPE_PMB887X_TEA5761UK	"tea5761uk"
#define PMB887X_TEA5761UK(obj)	OBJECT_CHECK(pmb887x_tea5761uk_t, (obj), TYPE_PMB887X_TEA5761UK)

#define TEA5761UK_READ_SIZE		16
#define TEA5761UK_WRITE_SIZE		7

#define TEA5761UK_INTREG_FRRFLAG		BIT(1)
#define TEA5761UK_FRQSET_MSB_MASK	0x3F
#define TEA5761UK_TUNCHK_LD		BIT(3)
#define TEA5761UK_AUDIO_BUS_NAME	"pmb887x-tea5761uk-audio"
#define TEA5761UK_AUDIO_RATE_HZ		48000
#define TEA5761UK_AUDIO_CHANNELS	2
#define TEA5761UK_AUDIO_PERIOD_NS	(10 * SCALE_MS)
#define TEA5761UK_AUDIO_PERIOD_FRAMES	(TEA5761UK_AUDIO_RATE_HZ / 100)
#define TEA5761UK_NOISE_RMS		(INT16_MAX / 10)

typedef struct pmb887x_tea5761uk_t pmb887x_tea5761uk_t;

struct pmb887x_tea5761uk_t {
	I2CSlave parent_obj;
	DebugChannel dbg;
	uint8_t regs[TEA5761UK_READ_SIZE];
	uint8_t read_index;
	uint8_t write_index;
	bool writing;
	AudioLineBus *audio_output;
	QEMUTimer *audio_timer;
	int64_t audio_deadline_ns;
	uint32_t noise_state;
	bool audio_active;
};

static const uint8_t default_regs[TEA5761UK_READ_SIZE] = {
	0x00, 0x00, 0x80, 0x00, 0x08, 0xD2, 0x00, 0x00,
	0x00, 0xF0, 0x00, 0x00, 0x40, 0x2B, 0x57, 0x61,
};

static const uint8_t write_registers[TEA5761UK_WRITE_SIZE] = {
	1, 2, 3, 4, 5, 10, 11,
};

static int16_t tea5761uk_noise_sample(pmb887x_tea5761uk_t *p) {
	int uniform_peak = lround(TEA5761UK_NOISE_RMS * sqrt(3.0));

	p->noise_state ^= p->noise_state << 13;
	p->noise_state ^= p->noise_state >> 17;
	p->noise_state ^= p->noise_state << 5;
	return (int32_t) (p->noise_state % (2 * uniform_peak + 1)) - uniform_peak;
}

static void tea5761uk_audio_timer(void *opaque) {
	pmb887x_tea5761uk_t *p = opaque;
	int16_t frames[TEA5761UK_AUDIO_PERIOD_FRAMES * TEA5761UK_AUDIO_CHANNELS];

	for (size_t i = 0; i < ARRAY_SIZE(frames); i++)
		frames[i] = tea5761uk_noise_sample(p);
	audio_line_write(p->audio_output, 0, frames, TEA5761UK_AUDIO_PERIOD_FRAMES);

	p->audio_deadline_ns += TEA5761UK_AUDIO_PERIOD_NS;
	timer_mod(p->audio_timer, p->audio_deadline_ns);
}

static bool tea5761uk_audio_is_active(const pmb887x_tea5761uk_t *p) {
	if ((p->regs[TEA5761UK_TNCTRL_CONTROL] & TEA5761UK_TNCTRL_CONTROL_PUPD) == 0)
		return false;
	if ((p->regs[TEA5761UK_TNCTRL_CONTROL] & TEA5761UK_TNCTRL_CONTROL_AFM) != 0)
		return false;
	return (p->regs[TEA5761UK_TNCTRL_AUDIO] & TEA5761UK_TNCTRL_AUDIO_MU) == 0;
}

static void tea5761uk_audio_update(pmb887x_tea5761uk_t *p) {
	static const AudioLineFormat FORMAT = { TEA5761UK_AUDIO_RATE_HZ, TEA5761UK_AUDIO_CHANNELS };
	bool active = tea5761uk_audio_is_active(p);

	if (active == p->audio_active)
		return;

	p->audio_active = active;
	if (active) {
		audio_line_start(p->audio_output, 0, &FORMAT);
		p->audio_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TEA5761UK_AUDIO_PERIOD_NS;
		timer_mod(p->audio_timer, p->audio_deadline_ns);
	} else {
		timer_del(p->audio_timer);
		audio_line_stop(p->audio_output, 0);
	}
}

static void tea5761uk_finish_write(pmb887x_tea5761uk_t *p) {
	if (p->write_index <= 1)
		return;

	if ((p->regs[TEA5761UK_TNCTRL_CONTROL] & TEA5761UK_TNCTRL_CONTROL_PUPD) == 0) {
		p->regs[0] &= ~TEA5761UK_INTREG_FRRFLAG;
		p->regs[9] &= ~TEA5761UK_TUNCHK_LD;
		return;
	}

	p->regs[6] = p->regs[2] & TEA5761UK_FRQSET_MSB_MASK;
	p->regs[7] = p->regs[3];
	p->regs[9] |= TEA5761UK_TUNCHK_LD;
	p->regs[0] |= TEA5761UK_INTREG_FRRFLAG;
}

static int tea5761uk_event(I2CSlave *s, enum i2c_event event) {
	pmb887x_tea5761uk_t *p = PMB887X_TEA5761UK(s);

	switch (event) {
		case I2C_START_SEND:
		case I2C_START_SEND_ASYNC:
			p->write_index = 0;
			p->writing = true;
			break;

		case I2C_START_RECV:
			p->read_index = 0;
			p->writing = false;
			break;

		case I2C_FINISH:
			if (p->writing) {
				tea5761uk_finish_write(p);
				tea5761uk_audio_update(p);
			}
			p->writing = false;
			break;

		case I2C_NACK:
			p->writing = false;
			break;
	}

	return 0;
}

static uint8_t tea5761uk_recv(I2CSlave *s) {
	pmb887x_tea5761uk_t *p = PMB887X_TEA5761UK(s);
	uint8_t index = p->read_index;
	uint8_t data = index < ARRAY_SIZE(p->regs) ? p->regs[index] : 0;

	DEBUG_IO_READ(&p->dbg, index, 1, data);
	p->read_index++;
	if (index == 0) {
		p->regs[0] = 0;
	} else if (index == 1) {
		p->regs[1] = 0;
	}

	return data;
}

static int tea5761uk_send(I2CSlave *s, uint8_t data) {
	pmb887x_tea5761uk_t *p = PMB887X_TEA5761UK(s);
	uint8_t index = p->write_index;

	if (index < ARRAY_SIZE(write_registers)) {
		DEBUG_IO_WRITE(&p->dbg, write_registers[index], 1, data);
		p->regs[write_registers[index]] = data;
	}
	p->write_index++;

	return 0;
}

static void tea5761uk_reset(DeviceState *dev) {
	pmb887x_tea5761uk_t *p = PMB887X_TEA5761UK(dev);

	memcpy(p->regs, default_regs, sizeof(default_regs));
	p->read_index = 0;
	p->write_index = 0;
	p->writing = false;
	p->noise_state = 1;
	tea5761uk_audio_update(p);
}

static void tea5761uk_realize(DeviceState *dev, Error **errp) {
	pmb887x_tea5761uk_t *p = PMB887X_TEA5761UK(dev);
	debug_channel_init_device_regs(&p->dbg, dev, &pmb887x_tea5761uk_regs);
	p->audio_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tea5761uk_audio_timer, p);
}

static void tea5761uk_unrealize(DeviceState *dev) {
	timer_free(PMB887X_TEA5761UK(dev)->audio_timer);
}

static void tea5761uk_init(Object *obj) {
	pmb887x_tea5761uk_t *p = PMB887X_TEA5761UK(obj);

	p->audio_output = audio_line_bus_init(DEVICE(obj), TEA5761UK_AUDIO_BUS_NAME);
	object_property_add_const_link(obj, "bus_out", OBJECT(p->audio_output));
}

static void tea5761uk_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
	device_class_set_legacy_reset(dc, tea5761uk_reset);
	dc->realize = tea5761uk_realize;
	dc->unrealize = tea5761uk_unrealize;
	k->event = tea5761uk_event;
	k->recv = tea5761uk_recv;
	k->send = tea5761uk_send;
}

static const TypeInfo tea5761uk_info = {
	.name = TYPE_PMB887X_TEA5761UK,
	.parent = TYPE_I2C_SLAVE,
	.instance_size = sizeof(pmb887x_tea5761uk_t),
	.instance_init = tea5761uk_init,
	.class_init = tea5761uk_class_init,
};

static void tea5761uk_register_types(void) {
	type_register_static(&tea5761uk_info);
}
type_init(tea5761uk_register_types)
