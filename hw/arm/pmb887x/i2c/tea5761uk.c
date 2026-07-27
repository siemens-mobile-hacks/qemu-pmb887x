/*
 * NXP TEA5761UK FM radio
 */
#define PMB887X_TRACE_ID		FM_RADIO
#define PMB887X_TRACE_PREFIX	"tea5761uk"
#define PMB887X_TRACE_IO		PMB887X_TRACE_IO_TEA5761UK

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_TEA5761UK	"tea5761uk"
#define PMB887X_TEA5761UK(obj)	OBJECT_CHECK(pmb887x_tea5761uk_t, (obj), TYPE_PMB887X_TEA5761UK)

#define TEA5761UK_READ_SIZE		16
#define TEA5761UK_WRITE_SIZE		7

#define TEA5761UK_INTREG_FRRFLAG		BIT(1)
#define TEA5761UK_FRQSET_MSB_MASK	0x3F
#define TEA5761UK_TNCTRL_PUPD		BIT(6)
#define TEA5761UK_TUNCHK_LD		BIT(3)

typedef struct pmb887x_tea5761uk_t pmb887x_tea5761uk_t;

struct pmb887x_tea5761uk_t {
	I2CSlave parent_obj;
	uint8_t regs[TEA5761UK_READ_SIZE];
	uint8_t read_index;
	uint8_t write_index;
	bool writing;
};

static const uint8_t default_regs[TEA5761UK_READ_SIZE] = {
	0x00, 0x00, 0x80, 0x00, 0x08, 0xD2, 0x00, 0x00,
	0x00, 0xF0, 0x00, 0x00, 0x40, 0x2B, 0x57, 0x61,
};

static const uint8_t write_registers[TEA5761UK_WRITE_SIZE] = {
	1, 2, 3, 4, 5, 10, 11,
};

static void tea5761uk_finish_write(pmb887x_tea5761uk_t *p) {
	if (p->write_index <= 1)
		return;

	if ((p->regs[4] & TEA5761UK_TNCTRL_PUPD) == 0) {
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
			if (p->writing)
				tea5761uk_finish_write(p);
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

	IO_DUMP_READ(index, 1, data);
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
		IO_DUMP_WRITE(write_registers[index], 1, data);
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
}

static void tea5761uk_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
	device_class_set_legacy_reset(dc, tea5761uk_reset);
	k->event = tea5761uk_event;
	k->recv = tea5761uk_recv;
	k->send = tea5761uk_send;
}

static const TypeInfo tea5761uk_info = {
	.name = TYPE_PMB887X_TEA5761UK,
	.parent = TYPE_I2C_SLAVE,
	.instance_size = sizeof(pmb887x_tea5761uk_t),
	.class_init = tea5761uk_class_init,
};

static void tea5761uk_register_types(void) {
	type_register_static(&tea5761uk_info);
}
type_init(tea5761uk_register_types)
