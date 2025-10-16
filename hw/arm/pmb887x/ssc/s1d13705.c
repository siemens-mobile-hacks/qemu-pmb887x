/*
* Epson S1D13705
 * */
#define PMB887X_TRACE_ID		GIMMICK
#define PMB887X_TRACE_PREFIX	"s1d13705"

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "qom/object.h"
#include "hw/irq.h"
#include "hw/hw.h"
#include "hw/arm/pmb887x/trace.h"

// S1D13732
// S1D13716

typedef struct pmb887x_gimmick_t pmb887x_gimmick_t;

/*
	CMD: SA0=0, CS=0
	ARGx: SA0=1, CS=0

	TYPE	CMD		ARG0		ARG1
	WRITE	0x0000	<REG_ID>	<VALUE>		write reg value
	WRITE	0x4000	<REG_ID>	0x0000		select reg for read
	READ	0x8000							read current reg

	WRITE	0xD000							LCD data (bypass)
	WRITE	0x3xx0							xx - LCD command (bypass)
	WRITE	0x1xx0							xx - LCD data (bypass)
*/
enum GimmickCommands {
	CMD_NOOP				= 0x8000,
	CMD_READ_REG			= 0x4000,
	CMD_WRITE_REG			= 0x0000,
	CMD_LCD_SIGNLE_CMD		= 0x3000,
	CMD_LCD_SINGLE_DATA		= 0x1000,
	CMD_LCD_DATA_BYPASS		= 0xD000,
};

struct pmb887x_gimmick_t {
	SSIPeripheral dev;
	SSIBus *bus;
	bool is_command;

	uint16_t request;
	uint16_t trx_bits;
	uint16_t response;
	bool lcd_data_bypass;

	uint32_t wcycle;
	uint32_t cmd;
	uint32_t arg0;
	uint32_t arg1;
	qemu_irq fpline;
};

#define TYPE_PMB887X_GIMMICK "s1d13705"
#define PMB887X_GIMMICK(obj)	OBJECT_CHECK(pmb887x_gimmick_t, (obj), TYPE_PMB887X_GIMMICK)

#define GIMMICK_BUS_WIDTH 16

static uint32_t gimmick_read_reg(pmb887x_gimmick_t *p, uint16_t reg) {
	uint16_t value = 0;

	switch (reg) {
		case 0x0000: // ID
			value = 0x706B;
			break;

		case 0x0014:
			value = 0x04D1;
			break;

		case 0x0202:
			value = 0x0000;
			break;

		default:
			// Unknown reg
			break;
	}

	DPRINTF("read reg %04X: %04X\n", reg, value);
	return value;
}

static void gimmick_write_reg(pmb887x_gimmick_t *p, uint16_t reg, uint16_t value) {
	DPRINTF("write reg %04X: %04X\n", reg, value);
}

static uint32_t gimmick_transfer16(SSIPeripheral *dev, uint32_t data) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(dev);

	uint32_t value = 0;

	if (p->is_command) {
		p->lcd_data_bypass = false;

		if ((data & 0xF000) == CMD_LCD_SIGNLE_CMD) {
			qemu_set_irq(p->fpline, 1);
			value = ssi_transfer(p->bus, (data >> 4) & 0xFF);
			qemu_set_irq(p->fpline, 0);
			p->wcycle = 0;
		} else if ((data & 0xF000) == CMD_LCD_SINGLE_DATA) {
			value = ssi_transfer(p->bus, (data >> 4) & 0xFF);
			p->wcycle = 0;
		} else if (data == CMD_WRITE_REG) {
			p->cmd = data;
			p->wcycle = 1;
		} else if (data == CMD_READ_REG) {
			p->cmd = data;
			p->wcycle = 1;
		} else if (data == CMD_LCD_DATA_BYPASS) {
			p->lcd_data_bypass = true;
		} else if (data == CMD_NOOP) {
			// Just nothing
			p->wcycle = 0;
		}
	} else if (p->wcycle != 0) {
		if (p->cmd == CMD_WRITE_REG) {
			if (p->wcycle == 1) {
				p->arg0 = data;
				p->wcycle++;
			} else if (p->wcycle == 2) {
				gimmick_write_reg(p, p->arg0, data);
				p->wcycle = 0;
			} else {
				hw_error("Invalid CMD_WRITE_REG wcycle!");
			}
		} else if (p->cmd == CMD_READ_REG) {
			if (p->wcycle == 1) {
				p->arg0 = data;
				p->wcycle++;
			} else if (p->wcycle == 2) {
				value = gimmick_read_reg(p, p->arg0);
				p->wcycle = 0;
			}
		} else {
			EPRINTF("invalid wcycle for cmd %04X\n", p->cmd);
			exit(1);
		}
	} else {
		hw_error("Ignored data: %02X [cmd=%04X]", data, p->cmd);
	}

	return value;
}

static uint32_t gimmick_transfer(SSIPeripheral *dev, uint32_t in) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(dev);

	if (!p->is_command && p->lcd_data_bypass)
		return ssi_transfer(p->bus, in);

	uint32_t shift = (GIMMICK_BUS_WIDTH - 8) - p->trx_bits;
	uint16_t out = (p->response >> shift) & 0xFF;
	p->request |= (in & 0xFF) << shift;
	p->trx_bits += 8;
	if (p->trx_bits == GIMMICK_BUS_WIDTH) {
		p->response = gimmick_transfer16(dev, p->request);
		p->trx_bits = 0;
		p->request = 0;
	}
	return out;
}

static void gimmick_handle_sa0(void *opaque, int n, int level) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(opaque);

	bool new_is_command = level == 0;
	if (p->is_command != new_is_command) {
		p->is_command = new_is_command;
		p->trx_bits = 0;
		p->request = 0;

		if (p->is_command)
			p->lcd_data_bypass = false;
	}
}

static void gimmick_handle_reset(void *opaque, int n, int level) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(opaque);
	if (level == 0) {
		DPRINTF("reset!\n");
		p->wcycle = 0;
		p->trx_bits = 0;
		p->request = 0;
		p->response = 0;
		p->lcd_data_bypass = false;
	}
}

static void gimmick_realize(SSIPeripheral *d, Error **errp) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(d);
	p->bus = ssi_create_bus(DEVICE(d), TYPE_PMB887X_GIMMICK);
	qdev_init_gpio_in_named(DEVICE(d), gimmick_handle_sa0, "SA0_IN", 1);
	qdev_init_gpio_in_named(DEVICE(d), gimmick_handle_reset, "RESET_IN", 1);
	qdev_init_gpio_out_named(DEVICE(d), &p->fpline, "FPLINE_OUT", 1);
}

static const Property gimmick_properties[] = {
	DEFINE_PROP_LINK("bus", pmb887x_gimmick_t, bus, TYPE_PMB887X_GIMMICK, SSIBus *)
};

static void gimmick_class_init(ObjectClass *klass, void *data) {
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

static void acodec_register_types(void) {
	type_register_static(&gimmick_info);
}

type_init(acodec_register_types)
