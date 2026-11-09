/*
 * Epson S1D13716
 * */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "qom/object.h"
#include "hw/core/irq.h"
#include "hw/core/hw-error.h"
#include "system/debug-channel.h"

typedef struct pmb887x_gimmick_t pmb887x_gimmick_t;

// 0x40 <REG_L> <REG_H> - select reg
// 0xC0 - read reg value
// 0x80 <VALUE> - write reg value

enum GimmickCommands {
	CMD_SELECT_REG		= 0x40,
	CMD_WRITE_REG		= 0x80,
	CMD_READ_REG		= 0xC0,
};

struct pmb887x_gimmick_t {
	SSIPeripheral dev;
	DebugChannel dbg;
	SSIBus *bus;

	uint8_t regs[0x10000];

	uint32_t wcycle;
	uint8_t cmd;
	uint16_t reg;

	bool cs_app;
	bool cs_lcd;
	bool is_command;

	qemu_irq int_out;
	qemu_irq nscs_out;
	qemu_irq srs_out;
};

#define TYPE_PMB887X_GIMMICK "s1d13716"
#define PMB887X_GIMMICK(obj)	OBJECT_CHECK(pmb887x_gimmick_t, (obj), TYPE_PMB887X_GIMMICK)

static uint32_t gimmick_transfer(SSIPeripheral *dev, uint32_t in) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(dev);

	if (!p->cs_app && p->cs_lcd)
		return ssi_transfer(p->bus, in);

	uint32_t response = 0;
	if (p->is_command) {
		p->cmd = in;
		p->wcycle = 0;
	} else {
		switch (p->cmd) {
			case CMD_SELECT_REG:
				if (p->wcycle == 0) {
					p->reg |= (in & 0xFF);
					p->wcycle++;
				} else if (p->wcycle == 1) {
					p->reg |= (in & 0xFF) << 8;
					p->wcycle = 0;
				}
				break;

			case CMD_WRITE_REG:
				p->regs[p->reg] = in;
				DEBUG_LOG(&p->dbg, "write reg %04X = %02X\n", p->reg, in);
				break;

			case CMD_READ_REG:
				response = p->regs[p->reg];
				DEBUG_LOG(&p->dbg, "read reg %04X = %02X\n", p->reg, response);
				break;
		}
	}

	return response;
}

static void gimmick_handle_rs(void *opaque, int n, int level) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(opaque);
	p->is_command = level == 0;
	qemu_set_irq(p->srs_out, level);
}

static void gimmick_handle_ncs_app(void *opaque, int n, int level) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(opaque);
	p->cs_app = level == 0;
}

static void gimmick_handle_ncs_lcd(void *opaque, int n, int level) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(opaque);
	p->cs_lcd = level == 0;
	qemu_set_irq(p->nscs_out, level);
}

static void gimmick_handle_nreset(void *opaque, int n, int level) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(opaque);
	if (level == 0) {
		DEBUG_LOG(&p->dbg, "reset!\n");
		p->wcycle = 0;
		p->cmd = 0;
	}
}

static void gimmick_realize(SSIPeripheral *d, Error **errp) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(d);
	debug_channel_init_device(&p->dbg, DEVICE(d));
	p->bus = ssi_create_bus(DEVICE(d), TYPE_PMB887X_GIMMICK);
	qdev_init_gpio_in_named(DEVICE(d), gimmick_handle_rs, "RS", 1);
	qdev_init_gpio_in_named(DEVICE(d), gimmick_handle_ncs_app, "NCS_APP", 1);
	qdev_init_gpio_in_named(DEVICE(d), gimmick_handle_ncs_lcd, "NCS_LCD", 1);
	qdev_init_gpio_in_named(DEVICE(d), gimmick_handle_nreset, "NRESET", 1);
	qdev_init_gpio_out_named(DEVICE(d), &p->int_out, "INT", 1);
	qdev_init_gpio_out_named(DEVICE(d), &p->nscs_out, "NSCS", 1);
	qdev_init_gpio_out_named(DEVICE(d), &p->srs_out, "SRS", 1);
}

static const Property gimmick_properties[] = {
	DEFINE_PROP_LINK("bus", pmb887x_gimmick_t, bus, TYPE_PMB887X_GIMMICK, SSIBus *)
};

static void gimmick_class_init(ObjectClass *klass, const void *data) {
	SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, gimmick_properties);
	k->realize = gimmick_realize;
	k->transfer = gimmick_transfer;
	k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo gimmick_info = {
	.name          = TYPE_PMB887X_GIMMICK,
	.parent        = TYPE_SSI_PERIPHERAL,
	.instance_size = sizeof(pmb887x_gimmick_t),
	.class_init    = gimmick_class_init,
};

static void gimmick_register_types(void) {
	type_register_static(&gimmick_info);
}

type_init(gimmick_register_types)
