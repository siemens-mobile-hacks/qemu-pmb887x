/*
 * TI/National LM4845 / LM4946 "Boomer" audio power amplifier (I2C control)
 * */
#define PMB887X_TRACE_ID		I2C
#define PMB887X_TRACE_PREFIX	"lm4946"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "hw/core/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_LM4946	"lm4946"
#define PMB887X_LM4946(obj)	OBJECT_CHECK(pmb887x_lm4946_t, (obj), TYPE_PMB887X_LM4946)

typedef struct pmb887x_lm4946_t pmb887x_lm4946_t;

struct pmb887x_lm4946_t {
	I2CSlave parent_obj;
	uint8_t mode;		// Mode Control (OCL, MC2..MC0)
	uint8_t n3d;		// Programmable 3D (N3D3..N3D0)
	uint8_t mono_vol;	// MVC4..MVC0
	uint8_t left_vol;	// LVC4..LVC0
	uint8_t right_vol;	// RVC4..RVC0
};

static int lm4946_event(I2CSlave *s, enum i2c_event event) {
	return 0;
}

static uint8_t lm4946_recv(I2CSlave *s) {
	// The amp is a write-only control interface; reads aren't defined.
	return 0;
}

static int lm4946_send(I2CSlave *s, uint8_t data) {
	pmb887x_lm4946_t *p = PMB887X_LM4946(s);

	if ((data & 0xE0) == 0x80) {
		p->mono_vol = data & 0x1F;
		DPRINTF("mono volume %02X\n", p->mono_vol);
	} else if ((data & 0xE0) == 0xC0) {
		p->left_vol = data & 0x1F;
		DPRINTF("left volume %02X\n", p->left_vol);
	} else if ((data & 0xE0) == 0xE0) {
		p->right_vol = data & 0x1F;
		DPRINTF("right volume %02X\n", p->right_vol);
	} else if ((data & 0xF0) == 0x40) {
		p->n3d = data & 0x0F;
		DPRINTF("3d %02X\n", p->n3d);
	} else if ((data & 0xF0) == 0x00) {
		p->mode = data & 0x0F;
		DPRINTF("mode %02X\n", p->mode);
	} else {
		DPRINTF("unknown command %02X\n", data);
	}

	return 0;
}

static void lm4946_realize(DeviceState *dev, Error **errp) {
}

static void lm4946_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	dc->realize = lm4946_realize;

	I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
	k->event = &lm4946_event;
	k->recv = &lm4946_recv;
	k->send = &lm4946_send;
}

static const TypeInfo lm4946_info = {
	.name          	= TYPE_PMB887X_LM4946,
	.parent        	= TYPE_I2C_SLAVE,
	.instance_size 	= sizeof(pmb887x_lm4946_t),
	.class_init    	= lm4946_class_init,
};

static void lm4946_register_types(void) {
	type_register_static(&lm4946_info);
}
type_init(lm4946_register_types)
