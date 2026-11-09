/*
 * TI/National LM4845 / LM4946 "Boomer" audio power amplifier (I2C control)
 * */
#include "qemu/osdep.h"
#include "hw/audio/audio-router.h"
#include "hw/i2c/i2c.h"
#include "system/debug-channel.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"
#include "hw/arm/pmb887x/gen/peripheral/LM4946.h"

#define TYPE_PMB887X_LM4946	"lm4946"
#define PMB887X_LM4946(obj)	OBJECT_CHECK(pmb887x_lm4946_t, (obj), TYPE_PMB887X_LM4946)

#define LM4946_SPEAKER_BUS_NAME		"pmb887x-lm4946-speaker"
#define LM4946_HEADPHONE_BUS_NAME	"pmb887x-lm4946-headphone"
#define LM4946_COMMAND_3BIT		0xE0
#define LM4946_COMMAND_4BIT		0xF0
#define LM4946_LINE_TO_SPEAKER_MB	602
#define LM4946_PHONE_TO_HEADPHONE_MB	(-602)

typedef enum lm4946_input_t lm4946_input_t;
typedef enum lm4946_output_t lm4946_output_t;
typedef struct lm4946_mode_t lm4946_mode_t;
typedef struct pmb887x_lm4946_t pmb887x_lm4946_t;

enum lm4946_input_t {
	LM4946_INPUT_PHONE,
	LM4946_INPUT_LINE,
	LM4946_INPUT_COUNT,
};

enum lm4946_output_t {
	LM4946_OUTPUT_SPEAKER,
	LM4946_OUTPUT_HEADPHONE,
	LM4946_OUTPUT_COUNT,
};

struct lm4946_mode_t {
	bool phone_to_speaker;
	bool line_to_speaker;
	bool phone_to_headphone;
	bool line_to_headphone;
};

static const lm4946_mode_t LM4946_MODES[] = {
	[LM4946_MODE_MC_ALL_SHUTDOWN] = { },
	[LM4946_MODE_MC_MONO_IN_TO_SPK] = { .phone_to_speaker = true },
	[LM4946_MODE_MC_MONO_IN_TO_HP] = { .phone_to_headphone = true },
	[LM4946_MODE_MC_LINE_IN_TO_SPK] = { .line_to_speaker = true },
	[LM4946_MODE_MC_LINE_IN_TO_HP] = { .line_to_headphone = true },
	[LM4946_MODE_MC_LINE_MONO_TO_SPK] = { .phone_to_speaker = true, .line_to_speaker = true },
	[LM4946_MODE_MC_LINE_MONO_TO_HP] = { .phone_to_headphone = true, .line_to_headphone = true },
	[LM4946_MODE_MC_LINE_IN_TO_SPK_HP] = { .line_to_speaker = true, .line_to_headphone = true },
};

static const int16_t LM4946_VOLUME_MB[] = {
	-5400, -4650, -4050, -3450, -3000, -2700, -2400, -2100,
	-1800, -1500, -1350, -1200, -1050, -900, -750, -600,
	-450, -300, -150, 0, 150, 300, 450, 600,
	750, 900, 1050, 1200, 1350, 1500, 1650, 1800,
};

struct pmb887x_lm4946_t {
	I2CSlave parent_obj;
	DebugChannel dbg;
	AudioRouterInput phone_input;
	AudioRouterInput line_input;
	AudioLineBus *output_buses[LM4946_OUTPUT_COUNT];
	AudioRouter router;
	uint8_t mode;
	uint8_t n3d;
	uint8_t mono_vol;
	uint8_t left_vol;
	uint8_t right_vol;
};

static void lm4946_routes_update(pmb887x_lm4946_t *p) {
	const lm4946_mode_t *mode = &LM4946_MODES[p->mode & LM4946_MODE_MC];
	int phone_mb = LM4946_VOLUME_MB[p->mono_vol];
	int line_mb = (LM4946_VOLUME_MB[p->left_vol] + LM4946_VOLUME_MB[p->right_vol]) / 2;

	audio_router_set_route(&p->router, LM4946_OUTPUT_SPEAKER, LM4946_INPUT_PHONE, mode->phone_to_speaker, phone_mb);
	audio_router_set_route(&p->router, LM4946_OUTPUT_SPEAKER, LM4946_INPUT_LINE, mode->line_to_speaker,
		line_mb + LM4946_LINE_TO_SPEAKER_MB);
	audio_router_set_route(&p->router, LM4946_OUTPUT_HEADPHONE, LM4946_INPUT_PHONE, mode->phone_to_headphone,
		phone_mb + LM4946_PHONE_TO_HEADPHONE_MB);
	audio_router_set_route(&p->router, LM4946_OUTPUT_HEADPHONE, LM4946_INPUT_LINE, mode->line_to_headphone, line_mb);
	audio_router_update(&p->router);
}

static int lm4946_event(I2CSlave *_s, enum i2c_event _event) {
	return 0;
}

static uint8_t lm4946_recv(I2CSlave *_s) {
	return 0;
}

static int lm4946_send(I2CSlave *s, uint8_t data) {
	pmb887x_lm4946_t *p = PMB887X_LM4946(s);

	if ((data & LM4946_COMMAND_3BIT) == LM4946_MONO_VOL) {
		p->mono_vol = data & LM4946_MONO_VOL_VOL;
	} else if ((data & LM4946_COMMAND_3BIT) == LM4946_LEFT_VOL) {
		p->left_vol = data & LM4946_LEFT_VOL_VOL;
	} else if ((data & LM4946_COMMAND_3BIT) == LM4946_RIGHT_VOL) {
		p->right_vol = data & LM4946_RIGHT_VOL_VOL;
	} else if ((data & LM4946_COMMAND_4BIT) == LM4946_N3D) {
		p->n3d = data & ~LM4946_COMMAND_4BIT;
	} else if ((data & LM4946_COMMAND_4BIT) == LM4946_MODE) {
		p->mode = data & ~LM4946_COMMAND_4BIT;
	} else {
		DEBUG_UNIMP(&p->dbg, "unknown command %02X\n", data);
		return 0;
	}

	DEBUG_IO_WRITE(&p->dbg, data & LM4946_COMMAND_3BIT, 1, data & ~LM4946_COMMAND_3BIT);
	lm4946_routes_update(p);
	return 0;
}

static void lm4946_reset(DeviceState *dev) {
	pmb887x_lm4946_t *p = PMB887X_LM4946(dev);

	p->mode = LM4946_MODE_MC_ALL_SHUTDOWN;
	p->n3d = 0;
	p->mono_vol = 0;
	p->left_vol = 0;
	p->right_vol = 0;
	audio_router_reset(&p->router);
	lm4946_routes_update(p);
}

static void lm4946_realize(DeviceState *dev, Error **_errp) {
	pmb887x_lm4946_t *p = PMB887X_LM4946(dev);
	debug_channel_init_device_regs(&p->dbg, dev, &pmb887x_lm4946_regs);
}

static void lm4946_init(Object *obj) {
	pmb887x_lm4946_t *p = PMB887X_LM4946(obj);

	p->output_buses[LM4946_OUTPUT_SPEAKER] = audio_line_bus_init(DEVICE(obj), LM4946_SPEAKER_BUS_NAME);
	p->output_buses[LM4946_OUTPUT_HEADPHONE] = audio_line_bus_init(DEVICE(obj), LM4946_HEADPHONE_BUS_NAME);
	object_property_add_const_link(obj, "bus_speaker", OBJECT(p->output_buses[LM4946_OUTPUT_SPEAKER]));
	object_property_add_const_link(obj, "bus_headphone", OBJECT(p->output_buses[LM4946_OUTPUT_HEADPHONE]));
	audio_router_init(&p->router, p->output_buses, LM4946_OUTPUT_COUNT, LM4946_INPUT_COUNT, &p->dbg);
	audio_router_input_init(&p->phone_input, DEVICE(obj), "PHONE_IN", &p->router, LM4946_INPUT_PHONE);
	audio_router_input_init(&p->line_input, DEVICE(obj), "LINE_IN", &p->router, LM4946_INPUT_LINE);
}

static void lm4946_finalize(Object *obj) {
	audio_router_cleanup(&PMB887X_LM4946(obj)->router);
}

static void lm4946_class_init(ObjectClass *klass, const void *_data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

	dc->realize = lm4946_realize;
	device_class_set_legacy_reset(dc, lm4946_reset);
	k->event = lm4946_event;
	k->recv = lm4946_recv;
	k->send = lm4946_send;
	audio_router_sink_class_init(AUDIO_LINE_SINK_CLASS(klass));
}

static const TypeInfo lm4946_info = {
	.name = TYPE_PMB887X_LM4946,
	.parent = TYPE_I2C_SLAVE,
	.instance_size = sizeof(pmb887x_lm4946_t),
	.instance_init = lm4946_init,
	.instance_finalize = lm4946_finalize,
	.class_init = lm4946_class_init,
	.interfaces = (const InterfaceInfo[]) {
		{ TYPE_AUDIO_LINE_SINK },
		{ NULL },
	},
};

static void lm4946_register_types(void) {
	type_register_static(&lm4946_info);
}
type_init(lm4946_register_types)
