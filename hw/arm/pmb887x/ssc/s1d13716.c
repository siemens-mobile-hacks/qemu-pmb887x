/*
 * Epson S1D13716
 * */
#define PMB887X_TRACE_ID		GIMMICK
#define PMB887X_TRACE_PREFIX	"s1d13716"

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "qom/object.h"
#include "hw/irq.h"
#include "hw/hw.h"
#include "hw/arm/pmb887x/trace.h"

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
				DPRINTF("write reg %04X = %02X\n", p->reg, in);
				break;

			case CMD_READ_REG:
				response = p->regs[p->reg];
				DPRINTF("read reg %04X = %02X\n", p->reg, response);
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
		DPRINTF("reset!\n");
		p->wcycle = 0;
		p->cmd = 0;
	}
}

static void gimmick_realize(SSIPeripheral *d, Error **errp) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(d);
	p->bus = ssi_create_bus(DEVICE(d), TYPE_PMB887X_GIMMICK);
	qdev_init_gpio_in_named(DEVICE(d), gimmick_handle_rs, "RS_IN", 1);
	qdev_init_gpio_in_named(DEVICE(d), gimmick_handle_ncs_app, "NCS_APP_IN", 1);
	qdev_init_gpio_in_named(DEVICE(d), gimmick_handle_ncs_lcd, "NCS_LCD_IN", 1);
	qdev_init_gpio_in_named(DEVICE(d), gimmick_handle_nreset, "NRESET_IN", 1);
	qdev_init_gpio_out_named(DEVICE(d), &p->int_out, "INT_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(d), &p->nscs_out, "NSCS_OUT", 1);
	qdev_init_gpio_out_named(DEVICE(d), &p->srs_out, "SRS_OUT", 1);
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
