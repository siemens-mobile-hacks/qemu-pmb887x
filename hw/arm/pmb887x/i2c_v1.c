/*
 * I2C
 * */
#include <stdint.h>
#define PMB887X_TRACE_ID		I2C
#define PMB887X_TRACE_PREFIX	"pmb887x-i2c"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"
#include "hw/i2c/i2c.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_I2C	"pmb887x-i2c-v1"
OBJECT_DECLARE_SIMPLE_TYPE(pmb887x_i2c_t, PMB887X_I2C);

#define I2C_TX_BYTE_TIME	100
#define FIFO_SIZE 4

enum {
	I2C_STATE_NONE,
	I2C_STATE_TX,
	I2C_STATE_TX_DONE,
	I2C_STATE_RX,
	I2C_STATE_RX_DONE,
	I2C_STATE_END,
};

enum {
	I2C_SINGLE_REQ_IRQ = 0,
	I2C_BURST_REQ_IRQ,
	I2C_ERROR_IRQ,
	I2C_PROTOCOL_IRQ,
};

struct pmb887x_i2c_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;

	I2CBus *bus;
	QEMUTimer *timer;

	pmb887x_clc_reg_t clc;

	int state;

	qemu_irq irq[3];
	pmb887x_src_reg_t end_src;
	pmb887x_src_reg_t proto_src;
	pmb887x_src_reg_t data_src;

	bool wait_for_next_tick;
	bool in_work;
	uint32_t trx_cnt;
	uint32_t wcycle;

	uint32_t rtb;
	uint32_t buscon;
	uint32_t syscon;
	uint32_t pisel;
};

static void i2c_work(pmb887x_i2c_t *p);

static bool i2c_is_running(pmb887x_i2c_t *p) {
	return pmb887x_clc_is_enabled(&p->clc) &&
		(p->syscon & I2Cv1_SYSCON_MOD) != I2Cv1_SYSCON_MOD_DISABLED;
}

static void i2c_timer_reset(void *opaque) {
	pmb887x_i2c_t *p = (pmb887x_i2c_t *) opaque;
	p->wait_for_next_tick = false;

	if (i2c_is_running(p))
		i2c_work(p);
}

static void i2c_timer_schedule(pmb887x_i2c_t *p) {
	if (!p->wait_for_next_tick) {
		timer_mod(p->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + I2C_TX_BYTE_TIME);
		p->wait_for_next_tick = true;
	}
}

static void i2c_update_syscon(pmb887x_i2c_t *p, uint32_t value) {
	if ((p->syscon & I2Cv1_SYSCON_IRQD) && !(value & I2Cv1_SYSCON_IRQD))
		DPRINTF("reset IRQD\n");
	if (!(p->syscon & I2Cv1_SYSCON_IRQD) && (value & I2Cv1_SYSCON_IRQD))
		DPRINTF("set IRQD\n");

	if ((p->syscon & I2Cv1_SYSCON_IRQE) && !(value & I2Cv1_SYSCON_IRQE))
		DPRINTF("reset IRQE\n");
	if (!(p->syscon & I2Cv1_SYSCON_IRQE) && (value & I2Cv1_SYSCON_IRQE))
		DPRINTF("set IRQE\n");

	if ((p->syscon & I2Cv1_SYSCON_RSC) && !(value & I2Cv1_SYSCON_RSC))
		DPRINTF("reset ***RSC***\n");
	if (!(p->syscon & I2Cv1_SYSCON_RSC) && (value & I2Cv1_SYSCON_RSC))
		DPRINTF("set ***RSC***\n");

	if ((p->syscon & I2Cv1_SYSCON_BUM) && !(value & I2Cv1_SYSCON_BUM))
		DPRINTF("reset ***BUM***\n");
	if (!(p->syscon & I2Cv1_SYSCON_BUM) && (value & I2Cv1_SYSCON_BUM))
		DPRINTF("set ***BUM***\n");

	if ((p->syscon & I2Cv1_SYSCON_TRX) && !(value & I2Cv1_SYSCON_TRX))
		DPRINTF("reset ***TRX***\n");
	if (!(p->syscon & I2Cv1_SYSCON_TRX) && (value & I2Cv1_SYSCON_TRX))
		DPRINTF("set ***TRX***\n");

	if (!(p->syscon & I2Cv1_SYSCON_IRQD) && (value & I2Cv1_SYSCON_IRQD))
		pmb887x_src_update(&p->data_src, 0, MOD_SRC_SETR);

	if (!(p->syscon & I2Cv1_SYSCON_IRQP) && (value & I2Cv1_SYSCON_IRQP))
		pmb887x_src_update(&p->proto_src, 0, MOD_SRC_SETR);

	if (!(p->syscon & I2Cv1_SYSCON_IRQE) && (value & I2Cv1_SYSCON_IRQE))
		pmb887x_src_update(&p->end_src, 0, MOD_SRC_SETR);

	if ((value & I2Cv1_SYSCON_MOD) != I2Cv1_SYSCON_MOD_DISABLED && (value & I2Cv1_SYSCON_MOD) != I2Cv1_SYSCON_MOD_MASTER)
		hw_error("Only master/disabled modes is supported.\n");

	if ((value & (I2Cv1_SYSCON_WMEN | I2Cv1_SYSCON_RMEN)))
		hw_error("Read/write mirrors is not supported.\n");

	if ((value & I2Cv1_SYSCON_SLA))
		hw_error("Slave is not supported.\n");

	if ((value & I2Cv1_SYSCON_M10))
		hw_error("10-bit is not supported.\n");

	p->syscon = value;

	if ((p->syscon & I2Cv1_SYSCON_MOD) == I2Cv1_SYSCON_MOD_DISABLED) {
		p->state = I2C_STATE_NONE;
		p->trx_cnt = 0;
		p->wcycle = 0;
		p->syscon = p->syscon & ~(
			I2Cv1_SYSCON_BB |
			I2Cv1_SYSCON_STP |
			I2Cv1_SYSCON_IRQE |
			I2Cv1_SYSCON_IRQD |
			I2Cv1_SYSCON_IRQP
		);
	}

	if (i2c_is_running(p) && p->state == I2C_STATE_NONE) {
		if ((p->syscon & I2Cv1_SYSCON_BUM)) {
			if ((p->syscon & I2Cv1_SYSCON_TRX)) {
				p->state = I2C_STATE_TX;
				p->trx_cnt = 0;
				p->wcycle = 0;
			} else {
				p->state = I2C_STATE_RX;
				p->trx_cnt = 0;
				p->wcycle = 0;
			}
		}
	}

	if (!p->in_work)
		i2c_timer_schedule(p);
}

static uint32_t i2c_get_trx_size(pmb887x_i2c_t *p) {
	return ((p->syscon & I2Cv1_SYSCON_CI) >> I2Cv1_SYSCON_CI_SHIFT) + 1;
}

static bool i2c_tx(pmb887x_i2c_t *p) {
	uint8_t byte = (p->rtb >> (p->trx_cnt * 8)) & 0xFF;
	if (p->wcycle == 0) {
		if ((byte & 1)) {
			DPRINTF("[TX] i2c_start_transfer: %02X (read)\n", byte >> 1);
		} else {
			DPRINTF("[TX] i2c_start_transfer: %02X (write)\n", byte >> 1);
		}

		bool nack = i2c_start_transfer(p->bus, byte >> 1, (byte & 1) != 0);
		p->syscon |= I2Cv1_SYSCON_BB;
		p->syscon &= ~I2Cv1_SYSCON_RSC;
		p->syscon &= ~I2Cv1_SYSCON_IRQE;

		if (nack) {
			p->syscon |= I2Cv1_SYSCON_LRB;
			DPRINTF("[TX] NACK for address: %02X\n", byte >> 1);
		} else {
			p->syscon &= ~I2Cv1_SYSCON_LRB;
		}
	} else {
		DPRINTF("[TX] send %02X\n", byte);
		i2c_send(p->bus, byte);
	}
	p->wcycle++;
	p->trx_cnt++;
	return true;
}

static void i2c_work(pmb887x_i2c_t *p) {
	if (!i2c_is_running(p))
		return;

	p->in_work = true;

	if ((p->syscon & I2Cv1_SYSCON_RSC)) {
		p->wcycle = 0;
		p->syscon &= ~I2Cv1_SYSCON_RSC;
	}

	if (p->state == I2C_STATE_TX) {
		if (!(p->syscon & I2Cv1_SYSCON_IRQD)) {
			if (p->trx_cnt < i2c_get_trx_size(p))
				i2c_tx(p);

			if (p->trx_cnt >= i2c_get_trx_size(p)) {
				DPRINTF("[TX] all bytes transmitted\n");
				i2c_update_syscon(p, p->syscon | I2Cv1_SYSCON_IRQD);
				pmb887x_src_update(&p->data_src, 0, MOD_SRC_SETR);
				p->state = I2C_STATE_TX_DONE;
			}
			i2c_timer_schedule(p);
		}
	} else if (p->state == I2C_STATE_TX_DONE) {
		if (!(p->syscon & I2Cv1_SYSCON_BUM)) {
			DPRINTF("[TX] STOP\n");
			p->state = I2C_STATE_END;
		} else if (!(p->syscon & I2Cv1_SYSCON_TRX)) {
			DPRINTF("[TX] switch to RX\n");
			p->state = I2C_STATE_RX;
			p->trx_cnt = 0;
			p->wcycle = 0;
		} else if (!(p->syscon & I2Cv1_SYSCON_IRQD)) {
			DPRINTF("[TX] new transfer requested\n");
			p->state = I2C_STATE_TX;
			p->trx_cnt = 0;
		} else {
			i2c_update_syscon(p, p->syscon | I2Cv1_SYSCON_IRQD);
			pmb887x_src_update(&p->data_src, 0, MOD_SRC_SETR);
		}
		i2c_timer_schedule(p);
	} else if (p->state == I2C_STATE_RX) {
		if (!(p->syscon & I2Cv1_SYSCON_IRQD)) {
			if (p->trx_cnt < i2c_get_trx_size(p)) {
				uint8_t byte = i2c_recv(p->bus);
				if ((p->syscon & I2Cv1_SYSCON_ACKDIS)) {
					i2c_nack(p->bus);
				} else {
					i2c_ack(p->bus);
				}
				p->rtb &= ~(0xFF << (8 * p->trx_cnt));
				p->rtb |= byte << (8 * p->trx_cnt);
				p->trx_cnt++;
				DPRINTF("[RX] read %02X\n", byte);
			}
			DPRINTF("READ %d %d\n", p->trx_cnt, i2c_get_trx_size(p));

			if (p->trx_cnt >= i2c_get_trx_size(p)) {
				DPRINTF("[RX] all bytes read\n");
				i2c_update_syscon(p, p->syscon | I2Cv1_SYSCON_IRQD);
				pmb887x_src_update(&p->data_src, 0, MOD_SRC_SETR);
				if (!(p->syscon & I2Cv1_SYSCON_STP)) {
					p->state = I2C_STATE_RX_DONE;
				} else {
					DPRINTF("[RX] STOP\n");
					p->state = I2C_STATE_END;
				}
			}
			i2c_timer_schedule(p);
		}
	} else if (p->state == I2C_STATE_RX_DONE) {
		if ((p->syscon & I2Cv1_SYSCON_TRX)) {
			DPRINTF("[RX] switch to TX\n");
			p->state = I2C_STATE_TX;
			p->trx_cnt = 0;
			p->wcycle = 0;
		} else if (!(p->syscon & I2Cv1_SYSCON_IRQD)) {
			DPRINTF("[RX] new transfer requested\n");
			p->state = I2C_STATE_RX;
			p->trx_cnt = 0;
		} else {
			i2c_update_syscon(p, p->syscon | I2Cv1_SYSCON_IRQD);
			pmb887x_src_update(&p->data_src, 0, MOD_SRC_SETR);
		}
		i2c_timer_schedule(p);
	} else if (p->state == I2C_STATE_END) {
		DPRINTF("[TRX] i2c_end_transfer\n");
		if (!(p->state & I2Cv1_SYSCON_RSC)) {
			i2c_end_transfer(p->bus);
			i2c_update_syscon(p, (p->syscon & ~(I2Cv1_SYSCON_BB | I2Cv1_SYSCON_BUM | I2Cv1_SYSCON_STP | I2Cv1_SYSCON_ACKDIS)) | I2Cv1_SYSCON_IRQE);
		} else {
			i2c_update_syscon(p, (p->syscon & ~(I2Cv1_SYSCON_BB | I2Cv1_SYSCON_STP | I2Cv1_SYSCON_ACKDIS)) | I2Cv1_SYSCON_IRQE);
		}
		pmb887x_src_update(&p->end_src, 0, MOD_SRC_SETR);
		p->state = I2C_STATE_NONE;
		p->wcycle = 0;
	}

	p->in_work = false;
}

static uint64_t i2c_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_i2c_t *p = opaque;

	uint64_t value = 0;

	switch (haddr) {
		case I2Cv2_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case I2Cv2_ID:
			value = 0x00004604;
			break;

		case I2Cv1_PISEL:
			value = p->pisel;
			break;

		case I2Cv1_SYSCON:
			value = p->syscon;
			value &= ~I2Cv1_SYSCON_CO;
			value |= p->trx_cnt <<I2Cv1_SYSCON_CO_SHIFT;
			break;

		case I2Cv1_BUSCON:
			value = p->buscon;
			break;

		case I2Cv1_RTB: {
			DPRINTF("R I2Cv1_RTB\n");

			value = p->rtb;
			if (!(p->syscon & I2Cv1_SYSCON_INT) && (p->syscon & I2Cv1_SYSCON_IRQD))
				i2c_update_syscon(p, p->syscon & ~I2Cv1_SYSCON_IRQD);
			p->trx_cnt = 0;
			break;
		}

		case I2Cv1_WHBSYSCON:
			value = 0;
			break;

		case I2Cv1_END_SRC:
			value = pmb887x_src_get(&p->end_src);
			break;

		case I2Cv1_PROTO_SRC:
			value = pmb887x_src_get(&p->proto_src);
			break;

		case I2Cv1_DATA_SRC:
			value = pmb887x_src_get(&p->data_src);
			break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}

	IO_DUMP(haddr + p->mmio.addr, size, value, false);

	return value;
}

static void i2c_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_i2c_t *p = opaque;

	IO_DUMP(haddr + p->mmio.addr, size, value, true);

	switch (haddr) {
		case I2Cv2_CLC:
			pmb887x_clc_set(&p->clc, value);
			i2c_update_syscon(p, p->syscon);
			break;

		case I2Cv1_PISEL:
			p->pisel = value;
			break;

		case I2Cv1_SYSCON:
			i2c_update_syscon(p, value);
			break;

		case I2Cv1_BUSCON:
			p->buscon = value;
			break;

		case I2Cv1_RTB: {
			p->rtb = value;

			DPRINTF("W I2Cv1_RTB\n");

			uint32_t syscon = p->syscon;
			syscon |= I2Cv1_SYSCON_TRX;

			if (!(p->syscon & I2Cv1_SYSCON_INT) && (p->syscon & I2Cv1_SYSCON_IRQD))
				syscon &= ~I2Cv1_SYSCON_IRQD;

			i2c_update_syscon(p, syscon);
			p->trx_cnt = 0;
			break;
		}

		case I2Cv1_WHBSYSCON: {
			uint32_t new_syscon = p->syscon;

			if ((value & I2Cv1_WHBSYSCON_CLRAL))
				new_syscon &= ~I2Cv1_SYSCON_AL;
			if ((value & I2Cv1_WHBSYSCON_SETAL))
				new_syscon |= I2Cv1_SYSCON_AL;

			if ((value & I2Cv1_WHBSYSCON_CLRIRQD))
				new_syscon &= ~I2Cv1_SYSCON_IRQD;
			if ((value & I2Cv1_WHBSYSCON_SETIRQD))
				new_syscon |= I2Cv1_SYSCON_IRQD;

			if ((value & I2Cv1_WHBSYSCON_CLRIRQP))
				new_syscon &= ~I2Cv1_SYSCON_IRQP;
			if ((value & I2Cv1_WHBSYSCON_SETIRQP))
				new_syscon |= I2Cv1_SYSCON_IRQP;

			if ((value & I2Cv1_WHBSYSCON_CLRIRQE))
				new_syscon &= ~I2Cv1_SYSCON_IRQE;
			if ((value & I2Cv1_WHBSYSCON_SETIRQE))
				new_syscon |= I2Cv1_SYSCON_IRQE;

			if ((value & I2Cv1_WHBSYSCON_CLRRMEN))
				new_syscon &= ~I2Cv1_SYSCON_RMEN;
			if ((value & I2Cv1_WHBSYSCON_SETRMEN))
				new_syscon |= I2Cv1_SYSCON_RMEN;

			if ((value & I2Cv1_WHBSYSCON_CLRWMEN))
				new_syscon &= ~I2Cv1_SYSCON_WMEN;
			if ((value & I2Cv1_WHBSYSCON_SETWMEN))
				new_syscon |= I2Cv1_SYSCON_WMEN;

			if ((value & I2Cv1_WHBSYSCON_CLRRSC))
				new_syscon &= ~I2Cv1_SYSCON_RSC;
			if ((value & I2Cv1_WHBSYSCON_SETRSC))
				new_syscon |= I2Cv1_SYSCON_RSC;

			if ((value & I2Cv1_WHBSYSCON_CLRBUM))
				new_syscon &= ~I2Cv1_SYSCON_BUM;
			if ((value & I2Cv1_WHBSYSCON_SETBUM))
				new_syscon |= I2Cv1_SYSCON_BUM;

			if ((value & I2Cv1_WHBSYSCON_CLRSTP))
				new_syscon &= ~I2Cv1_SYSCON_STP;
			if ((value & I2Cv1_WHBSYSCON_SETSTP))
				new_syscon |= I2Cv1_SYSCON_STP;

			if ((value & I2Cv1_WHBSYSCON_CLRACKDIS))
				new_syscon &= ~I2Cv1_SYSCON_ACKDIS;
			if ((value & I2Cv1_WHBSYSCON_SETACKDIS))
				new_syscon |= I2Cv1_SYSCON_ACKDIS;

			if ((value & I2Cv1_WHBSYSCON_CLRTRX))
				new_syscon &= ~I2Cv1_SYSCON_TRX;
			if ((value & I2Cv1_WHBSYSCON_SETTRX))
				new_syscon |= I2Cv1_SYSCON_TRX;

			i2c_update_syscon(p, new_syscon);
			break;
		}

		case I2Cv1_END_SRC:
			pmb887x_src_set(&p->end_src, value);
			break;

		case I2Cv1_PROTO_SRC:
			pmb887x_src_set(&p->proto_src, value);
			break;

		case I2Cv1_DATA_SRC:
			pmb887x_src_set(&p->data_src, value);
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
}

static const MemoryRegionOps io_ops = {
	.read			= i2c_io_read,
	.write			= i2c_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 4,
		.max_access_size	= 4
	}
};

static void i2c_init(Object *obj) {
	pmb887x_i2c_t *p = PMB887X_I2C(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-i2c-v1", I2Cv2_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	p->bus = i2c_init_bus(DEVICE(obj), NULL);

	for (int i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void i2c_realize(DeviceState *dev, Error **errp) {
	pmb887x_i2c_t *p = PMB887X_I2C(dev);

	pmb887x_clc_init(&p->clc);

	pmb887x_src_init(&p->data_src, p->irq[0]);
	pmb887x_src_init(&p->proto_src, p->irq[1]);
	pmb887x_src_init(&p->end_src, p->irq[2]);

	p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, i2c_timer_reset, p);
}

static const Property i2c_properties[] = {
	DEFINE_PROP_LINK("bus", pmb887x_i2c_t, bus, TYPE_I2C_BUS, I2CBus *)
};

static void i2c_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, i2c_properties);
	dc->realize = i2c_realize;
}

static const TypeInfo i2c_info = {
	.name          	= TYPE_PMB887X_I2C,
	.parent        	= TYPE_SYS_BUS_DEVICE,
	.instance_size 	= sizeof(pmb887x_i2c_t),
	.instance_init 	= i2c_init,
	.class_init    	= i2c_class_init,
};

static void i2c_register_types(void) {
	type_register_static(&i2c_info);
}
type_init(i2c_register_types)
