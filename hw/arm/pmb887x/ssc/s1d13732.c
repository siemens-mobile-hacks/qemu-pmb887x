/*
* Epson S1D13732
 * */
#define PMB887X_TRACE_ID		GIMMICK
#define PMB887X_TRACE_PREFIX	"s1d13732"

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/core/irq.h"
#include "hw/arm/pmb887x/trace.h"

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

enum S1D13732Registers {
	S1D13732_REG_PRODUCT_CODE = 0x0000,
	S1D13732_REG_MISC_CONFIG = 0x0014,
	S1D13732_REG_SOFTWARE_RESET = 0x0016,
	S1D13732_REG_SYSTEM_CLOCK = 0x0018,
	S1D13732_REG_MEMORY_ADDRESS_LOW = 0x0022,
	S1D13732_REG_MEMORY_ADDRESS_HIGH = 0x0024,
	S1D13732_REG_MEMORY_DATA = 0x0028,
	S1D13732_REG_SPI_HEADER = 0x0060,
	S1D13732_REG_DISPLAY_MODE = 0x0202,
	S1D13732_REG_INDIRECT_INTERRUPT = 0x0A20,
	S1D13732_REG_SD_CONFIG2 = 0x6004,
	S1D13732_REG_SD_INTERRUPT_FLAGS = 0x6008,
	S1D13732_REG_SD_FUNCTION = 0x6104,
	S1D13732_REG_SD_STATUS = 0x6106,
};

#define S1D13732_SRAM_SIZE 0x70000
#define S1D13732_SRAM_WORDS (S1D13732_SRAM_SIZE / 2)
#define S1D13732_MEMORY_WRITE_ERROR BIT(0)
#define S1D13732_MEMORY_READ_ERROR BIT(1)

struct pmb887x_gimmick_t {
	SSIPeripheral dev;
	SSIBus *bus;
	bool is_command;

	uint16_t request;
	uint16_t trx_bits;
	uint16_t response;
	bool lcd_data_bypass;
	bool reset_active;

	uint16_t regs[64 * 1024];
	uint16_t sram[S1D13732_SRAM_WORDS];
	uint32_t memory_address;

	uint32_t wcycle;
	uint32_t cmd;
	uint32_t arg0;
	uint32_t arg1;
	qemu_irq fpline;
	qemu_irq fpdat[8];
};

#define TYPE_PMB887X_GIMMICK "s1d13732"
#define PMB887X_GIMMICK(obj)	OBJECT_CHECK(pmb887x_gimmick_t, (obj), TYPE_PMB887X_GIMMICK)

#define GIMMICK_BUS_WIDTH 16

static void gimmick_update_fpdat(pmb887x_gimmick_t *p) {
	uint16_t value = p->regs[S1D13732_REG_SPI_HEADER];

	for (uint32_t i = 0; i < ARRAY_SIZE(p->fpdat); i++) {
		if (p->fpdat[i]) {
			qemu_set_irq(p->fpdat[i], (value >> i) & 1);
		}
	}
}

static void gimmick_reset_internal(pmb887x_gimmick_t *p, bool hardware_reset) {
	p->request = 0;
	p->trx_bits = 0;
	p->response = 0;
	p->lcd_data_bypass = false;
	p->memory_address = 0;
	p->wcycle = 0;
	p->cmd = 0;
	p->arg0 = 0;
	p->arg1 = 0;

	if (hardware_reset) {
		memset(p->regs, 0, sizeof(p->regs));
		p->regs[S1D13732_REG_PRODUCT_CODE] = 0x706B;
		p->regs[S1D13732_REG_MISC_CONFIG] = 0x04D1;
	} else {
		uint32_t first_reset_register = S1D13732_REG_SYSTEM_CLOCK + 1;
		memset(&p->regs[first_reset_register], 0, (ARRAY_SIZE(p->regs) - first_reset_register) * sizeof(p->regs[0]));
	}
	p->regs[S1D13732_REG_SPI_HEADER] = 0x0001;
	p->regs[S1D13732_REG_DISPLAY_MODE] = 0;
	/* Reset does not define display SRAM contents, so preserve the existing data. */

	if (p->fpline) {
		qemu_set_irq(p->fpline, 1);
	}
	gimmick_update_fpdat(p);
}

static void gimmick_update_memory_address_registers(pmb887x_gimmick_t *p) {
	uint16_t direction = p->regs[S1D13732_REG_MEMORY_ADDRESS_LOW] & 1;
	p->regs[S1D13732_REG_MEMORY_ADDRESS_LOW] = (uint16_t) p->memory_address | direction;
	p->regs[S1D13732_REG_MEMORY_ADDRESS_HIGH] =
		(p->regs[S1D13732_REG_MEMORY_ADDRESS_HIGH] & ~7U) | ((p->memory_address >> 16) & 7U);
}

static void gimmick_advance_memory_address(pmb887x_gimmick_t *p) {
	p->memory_address += 2;
	gimmick_update_memory_address_registers(p);
}

static uint16_t gimmick_read_memory(pmb887x_gimmick_t *p) {
	if ((p->regs[S1D13732_REG_MEMORY_ADDRESS_LOW] & 1) == 0) {
		p->regs[S1D13732_REG_INDIRECT_INTERRUPT] |= S1D13732_MEMORY_READ_ERROR;
		return 0;
	}
	if (p->memory_address > S1D13732_SRAM_SIZE - 2) {
		p->regs[S1D13732_REG_INDIRECT_INTERRUPT] |= S1D13732_MEMORY_READ_ERROR;
		return 0;
	}

	uint16_t value = p->sram[p->memory_address / 2];
	gimmick_advance_memory_address(p);
	return value;
}

static void gimmick_write_memory(pmb887x_gimmick_t *p, uint16_t value) {
	if ((p->regs[S1D13732_REG_MEMORY_ADDRESS_LOW] & 1) != 0) {
		p->regs[S1D13732_REG_INDIRECT_INTERRUPT] |= S1D13732_MEMORY_WRITE_ERROR;
		return;
	}
	if (p->memory_address > S1D13732_SRAM_SIZE - 2) {
		p->regs[S1D13732_REG_INDIRECT_INTERRUPT] |= S1D13732_MEMORY_WRITE_ERROR;
		return;
	}

	p->sram[p->memory_address / 2] = value;
	gimmick_advance_memory_address(p);
}

static uint32_t gimmick_read_reg(pmb887x_gimmick_t *p, uint16_t reg) {
	uint16_t value = reg == S1D13732_REG_MEMORY_DATA ? gimmick_read_memory(p) : p->regs[reg];

	/* Temporary: report no SD card until the controller has a storage backend. */
	if (reg == S1D13732_REG_SD_CONFIG2) {
		value |= BIT(0);
	} else if (reg == S1D13732_REG_SD_INTERRUPT_FLAGS) {
		value |= BIT(9);
	} else if (reg == S1D13732_REG_SD_STATUS) {
		value |= BIT(6);
	}

	DPRINTF("read reg %04X: %04X\n", reg, value);
	return value;
}

static void gimmick_write_reg(pmb887x_gimmick_t *p, uint16_t reg, uint16_t value) {
	DPRINTF("write reg %04X: %04X\n", reg, value);

	if (reg == S1D13732_REG_PRODUCT_CODE) {
		return;
	}
	if (reg == S1D13732_REG_SOFTWARE_RESET) {
		gimmick_reset_internal(p, false);
		return;
	}
	if (reg == S1D13732_REG_MEMORY_DATA) {
		gimmick_write_memory(p, value);
		return;
	}
	if (reg == S1D13732_REG_INDIRECT_INTERRUPT) {
		p->regs[reg] &= ~value;
		return;
	}
	if (reg == S1D13732_REG_SD_FUNCTION) {
		/* Temporary: complete SD controller operations immediately until its backend is implemented. */
		p->regs[reg] = 0;
		return;
	}

	p->regs[reg] = value;
	if (reg == S1D13732_REG_MEMORY_ADDRESS_LOW) {
		p->memory_address = ((uint32_t) (p->regs[S1D13732_REG_MEMORY_ADDRESS_HIGH] & 7) << 16) | (value & ~1U);
	} else if (reg == S1D13732_REG_SPI_HEADER) {
		/* In panel SPI mode the header value is presented on FPDAT[7:0]. */
		gimmick_update_fpdat(p);
	}
}

static uint32_t gimmick_transfer16(SSIPeripheral *dev, uint32_t data) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(dev);
	uint32_t value = 0;

	DPRINTF("%04X // cmd=%d\n", data, p->is_command);

	if (p->is_command) {
		p->lcd_data_bypass = false;

		if ((data & 0xF000) == CMD_LCD_SIGNLE_CMD) {
			qemu_set_irq(p->fpline, 0);
			value = ssi_transfer(p->bus, (data >> 4) & 0xFF);
			qemu_set_irq(p->fpline, 1);
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
				if (p->arg0 != S1D13732_REG_MEMORY_DATA) {
					p->wcycle = 0;
				}
			} else {
				EPRINTF("invalid CMD_WRITE_REG wcycle: %u\n", p->wcycle);
				p->wcycle = 0;
			}
		} else if (p->cmd == CMD_READ_REG) {
			if (p->wcycle == 1) {
				p->arg0 = data;
				p->wcycle++;
			} else {
				value = gimmick_read_reg(p, p->arg0);
				p->wcycle++;
				if (p->arg0 != S1D13732_REG_MEMORY_DATA) {
					p->arg0 += 2;
				}
			}
		} else {
			EPRINTF("invalid wcycle for cmd %04X\n", p->cmd);
			p->wcycle = 0;
		}
	} else {
		EPRINTF("ignored data: %04X [cmd=%04X]\n", data, p->cmd);
	}

	return value;
}

static uint32_t gimmick_transfer(SSIPeripheral *dev, uint32_t in) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(dev);

	if (p->reset_active) {
		return 0;
	}
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

static uint32_t gimmick_transfer_raw(SSIPeripheral *dev, uint32_t in) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(dev);
	bool selected = !dev->cs;
	if (!selected && !p->lcd_data_bypass)
		return 0;

	return gimmick_transfer(dev, in);
}

static void gimmick_handle_cs(void *opaque, int n, int level) {
	SSIPeripheral *dev = SSI_PERIPHERAL(opaque);
	dev->cs = level != 0;
}

static void gimmick_handle_sa0(void *opaque, int n, int level) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(opaque);
	bool new_is_command = level == 0;

	if (p->is_command != new_is_command) {
		p->is_command = new_is_command;
		p->trx_bits = 0;
		p->request = 0;

		if (p->is_command) {
			p->wcycle = 0;
			p->cmd = 0;
			p->lcd_data_bypass = false;
		}
	}
}

static void gimmick_reset(DeviceState *dev) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(dev);
	gimmick_reset_internal(p, true);
}

static void gimmick_handle_reset(void *opaque, int n, int level) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(opaque);

	if (level == 0) {
		DPRINTF("reset!\n");
		gimmick_reset(DEVICE(p));
		p->reset_active = true;
	} else {
		p->reset_active = false;
	}
}

static void gimmick_realize(SSIPeripheral *d, Error **errp) {
	pmb887x_gimmick_t *p = PMB887X_GIMMICK(d);
	static const char * const fpdat_names[] = {
		"FPDAT0_OUT", "FPDAT1_OUT", "FPDAT2_OUT", "FPDAT3_OUT",
		"FPDAT4_OUT", "FPDAT5_OUT", "FPDAT6_OUT", "FPDAT7_OUT",
	};
	p->bus = ssi_create_bus(DEVICE(d), TYPE_PMB887X_GIMMICK);
	qdev_init_gpio_in_named(DEVICE(d), gimmick_handle_cs, SSI_GPIO_CS, 1);
	qdev_init_gpio_in_named(DEVICE(d), gimmick_handle_sa0, "SA0_IN", 1);
	qdev_init_gpio_in_named(DEVICE(d), gimmick_handle_reset, "RESET_IN", 1);
	qdev_init_gpio_out_named(DEVICE(d), &p->fpline, "FPLINE_OUT", 1);
	for (uint32_t i = 0; i < ARRAY_SIZE(p->fpdat); i++) {
		qdev_init_gpio_out_named(DEVICE(d), &p->fpdat[i], fpdat_names[i], 1);
	}
}

static const Property gimmick_properties[] = {
	DEFINE_PROP_LINK("bus", pmb887x_gimmick_t, bus, TYPE_PMB887X_GIMMICK, SSIBus *)
};

static void gimmick_class_init(ObjectClass *klass, const void *data) {
	SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, gimmick_properties);
	device_class_set_legacy_reset(dc, gimmick_reset);
	k->realize = gimmick_realize;
	k->transfer = gimmick_transfer;
	k->transfer_raw = gimmick_transfer_raw;
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
