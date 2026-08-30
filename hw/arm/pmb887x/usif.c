/*
 * USIF (Universal Serial Interface)
 * */
#define PMB887X_TRACE_ID		USIF
#define PMB887X_TRACE_PREFIX	"pmb887x-usif"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_USIF	"pmb887x-usif"
#define PMB887X_USIF(obj)	OBJECT_CHECK(pmb887x_usif_t, (obj), TYPE_PMB887X_USIF)

#define USIF_TX_DMA_BURST	4
#define USIF_REG_BLOCK_SIZE	0x100

typedef struct pmb887x_usif_t pmb887x_usif_t;

struct pmb887x_usif_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	uint32_t revision;

	pmb887x_clc_reg_t clc;
	CharFrontend chr;

	uint32_t regs[USIF_REG_BLOCK_SIZE / 4];

	int tx_dma_remaining;
	int dmac_tx_clr;
	int dmac_rx_clr;

	qemu_irq dmac_tx_breq;
	qemu_irq dmac_tx_lbreq;
	qemu_irq dmac_rx_breq;
	qemu_irq dmac_rx_lbreq;
};

static void usif_update_tx_dma(pmb887x_usif_t *p) {
	bool active = p->tx_dma_remaining > 0;
	bool last = p->tx_dma_remaining <= USIF_TX_DMA_BURST;
	qemu_set_irq(p->dmac_tx_breq, active && !last);
	qemu_set_irq(p->dmac_tx_lbreq, active && last);
}

static uint64_t usif_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_usif_t *p = opaque;
	uint64_t value = 0;

	switch (haddr) {
		case USIF_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case USIF_TPS:
			// TX transfer count: upper bits kept, low 14 bits = bytes left to send.
			value = (p->regs[USIF_TPS / 4] & ~0x3FFFu) | (p->tx_dma_remaining & 0x3FFF);
			break;

		case USIF_FIFO_STAT:
			// TX FIFO always drained (fill == 0, not busy).
			value = 0;
			break;

		case USIF_RXD:
			// RX FIFO empty.
			value = 0;
			break;

		default:
			if (haddr < USIF_REG_BLOCK_SIZE) {
				value = p->regs[haddr / 4];
			} else {
				DPRINTF("unknown reg read: %02"PRIX64"\n", haddr);
				value = 0;
			}
			break;
	}

	IO_DUMP_READ(haddr + p->mmio.addr, size, value);

	return value;
}

static void usif_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_usif_t *p = opaque;

	IO_DUMP_WRITE(haddr + p->mmio.addr, size, value);

	switch (haddr) {
		case USIF_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case USIF_TPS:
			// Arm the TX transfer: low 14 bits are the byte count to send.
			p->regs[USIF_TPS / 4] = value;
			p->tx_dma_remaining = value & 0x3FFF;
			usif_update_tx_dma(p);
			break;

		case USIF_FIFO_STAT:
			// Read-only status register.
			break;

		case USIF_TXD: {
			// TX FIFO data (also the DMA TX endpoint). Consume and forward the
			// bytes to the char backend so the HCI stream can be observed.
			uint8_t bytes[4];
			for (unsigned i = 0; i < size && i < sizeof(bytes); i++)
				bytes[i] = (value >> (8 * i)) & 0xFF;
			qemu_chr_fe_write(&p->chr, bytes, MIN(size, sizeof(bytes)));

			if (p->tx_dma_remaining > 0) {
				p->tx_dma_remaining -= MIN(p->tx_dma_remaining, (int) size);
				usif_update_tx_dma(p);
			}
			break;
		}

		case USIF_RXD:
			// RX FIFO data: read-only.
			break;

		default:
			if (haddr < USIF_REG_BLOCK_SIZE) {
				p->regs[haddr / 4] = value;
			} else {
				DPRINTF("unknown reg write: %02"PRIX64" = %08"PRIX64"\n", haddr, value);
			}
			break;
	}
}

static const MemoryRegionOps io_ops = {
	.read			= usif_io_read,
	.write			= usif_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void usif_handle_dmac_tx_clr(void *opaque, int id, int level) {
	pmb887x_usif_t *p = opaque;
	p->dmac_tx_clr = level;
}

static void usif_handle_dmac_rx_clr(void *opaque, int id, int level) {
	pmb887x_usif_t *p = opaque;
	p->dmac_rx_clr = level;
}

static void usif_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_usif_t *p = PMB887X_USIF(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-usif", USIF_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);

	qdev_init_gpio_out_named(dev, &p->dmac_tx_breq, "DMAC_TX_BREQ", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_tx_lbreq, "DMAC_TX_LBREQ", 1);
	qdev_init_gpio_in_named(dev, usif_handle_dmac_tx_clr, "DMAC_TX_CLR", 1);

	qdev_init_gpio_out_named(dev, &p->dmac_rx_breq, "DMAC_RX_BREQ", 1);
	qdev_init_gpio_out_named(dev, &p->dmac_rx_lbreq, "DMAC_RX_LBREQ", 1);
	qdev_init_gpio_in_named(dev, usif_handle_dmac_rx_clr, "DMAC_RX_CLR", 1);
}

static void usif_realize(DeviceState *dev, Error **errp) {
	pmb887x_usif_t *p = PMB887X_USIF(dev);
	pmb887x_clc_init(&p->clc);
}

static void usif_reset(DeviceState *dev) {
	pmb887x_usif_t *p = PMB887X_USIF(dev);

	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);

	memset(p->regs, 0, sizeof(p->regs));
	p->tx_dma_remaining = 0;
	p->dmac_tx_clr = 0;
	p->dmac_rx_clr = 0;

	usif_update_tx_dma(p);
	qemu_set_irq(p->dmac_rx_breq, 0);
	qemu_set_irq(p->dmac_rx_lbreq, 0);
}

static const Property usif_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_usif_t, revision, 0),
	DEFINE_PROP_CHR("chardev", pmb887x_usif_t, chr),
};

static void usif_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, usif_properties);
	device_class_set_legacy_reset(dc, usif_reset);
	dc->realize = usif_realize;
}

static const TypeInfo usif_info = {
	.name          	= TYPE_PMB887X_USIF,
	.parent        	= TYPE_SYS_BUS_DEVICE,
	.instance_size 	= sizeof(struct pmb887x_usif_t),
	.instance_init 	= usif_init,
	.class_init    	= usif_class_init,
};

static void usif_register_types(void) {
	type_register_static(&usif_info);
}
type_init(usif_register_types)
