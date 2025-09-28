/*
 * DMA Controller (PL080)
 * */
#define PMB887X_TRACE_ID		DMAC
#define PMB887X_TRACE_PREFIX	"pmb887x-dmac"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/dmac.h"
#include "hw/arm/pmb887x/trace.h"

#define DMAC_CHANNELS	8

static const uint32_t PCELL_ID = 0x0A141080;
static const uint32_t PERIPH_ID = 0xB105F00D;

typedef struct pmb887x_dmac_ch_t pmb887x_dmac_ch_t;

struct pmb887x_dmac_ch_t {
	uint8_t id;
	uint32_t src_addr;
	uint32_t dst_addr;
	uint32_t lli;
	uint32_t control;
	uint32_t config;
};

struct pmb887x_dmac_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	MemoryRegion *downstream;
	AddressSpace downstream_as;
	
	qemu_irq irq_err;
	qemu_irq irq_tc[DMAC_CHANNELS];
	
	pmb887x_srb_reg_t srb_tc;
	pmb887x_srb_reg_t srb_err;
	
	pmb887x_dmac_ch_t ch[DMAC_CHANNELS];

	bool is_busy;
	uint32_t config;
	uint32_t enabled_channels;
	uint32_t used_peripherals;
	
	uint32_t periph_request[16];
};

static uint32_t dmac_get_width(uint32_t s) {
	if (s > 2)
		hw_error("pmb887x-dmac: unknown width %d", s);
	return 1 << s;
}

static void dmac_channel_run(pmb887x_dmac_t *p, pmb887x_dmac_ch_t *ch) {
	if (!(ch->config & DMAC_CH_CONFIG_ENABLE) || !(p->config & DMAC_CONFIG_ENABLE))
		return;

	uint32_t src_width = dmac_get_width((ch->control & DMAC_CH_CONTROL_S_WIDTH) >> DMAC_CH_CONTROL_S_WIDTH_SHIFT);
	uint32_t dst_width = dmac_get_width((ch->control & DMAC_CH_CONTROL_D_WIDTH) >> DMAC_CH_CONTROL_D_WIDTH_SHIFT);
	uint8_t src_periph = (ch->config & DMAC_CH_CONFIG_SRC_PERIPH) >> DMAC_CH_CONFIG_SRC_PERIPH_SHIFT;
	uint8_t dst_periph = (ch->config & DMAC_CH_CONFIG_DST_PERIPH) >> DMAC_CH_CONFIG_DST_PERIPH_SHIFT;
	uint32_t tx_size = 0;
	bool is_periph_controlled = false;
	
	switch ((ch->config & DMAC_CH_CONFIG_FLOW_CTRL)) {
		case DMAC_CH_CONFIG_FLOW_CTRL_MEM2MEM:
			tx_size = (ch->control & DMAC_CH_CONTROL_TRANSFER_SIZE) >> DMAC_CH_CONTROL_TRANSFER_SIZE_SHIFT;
			break;

		case DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER:
			tx_size = (ch->control & DMAC_CH_CONTROL_TRANSFER_SIZE) >> DMAC_CH_CONTROL_TRANSFER_SIZE_SHIFT;
			if (!p->periph_request[dst_periph])
				return;
			p->periph_request[dst_periph] = 0;
			break;

		case DMAC_CH_CONFIG_FLOW_CTRL_PER2MEM:
			tx_size = (ch->control & DMAC_CH_CONTROL_TRANSFER_SIZE) >> DMAC_CH_CONTROL_TRANSFER_SIZE_SHIFT;
			if (!p->periph_request[src_periph])
				return;
			p->periph_request[src_periph] = 0;
			break;

		case DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER_PER:
			is_periph_controlled = true;
			if (!p->periph_request[dst_periph])
				return;
			tx_size = p->periph_request[dst_periph];
			p->periph_request[dst_periph] = 0;
			break;

		case DMAC_CH_CONFIG_FLOW_CTRL_PER2MEM_PER:
			is_periph_controlled = true;
			if (!p->periph_request[src_periph])
				return;
			tx_size = p->periph_request[src_periph];
			p->periph_request[src_periph] = 0;
			break;

		default:
			hw_error("pmb887x-dmac: unsupported flow type %08X", (ch->config & DMAC_CH_CONFIG_FLOW_CTRL));
	}

	if (!tx_size)
		return;

	p->is_busy = true;

	DPRINTF("CH%d: %08X [%dx%d] -> %08X [%dx%d]\n", ch->id, ch->src_addr, src_width, tx_size, ch->dst_addr, dst_width, tx_size);

	uint8_t buffer_size = 0;
	
	while (tx_size > 0) {
		uint8_t buffer[4];
		if (dst_width >= src_width) {
			address_space_read(&p->downstream_as, ch->src_addr, MEMTXATTRS_UNSPECIFIED, buffer + buffer_size, src_width);
			buffer_size += src_width;
			
			if ((ch->control & DMAC_CH_CONTROL_SI))
				ch->src_addr += src_width;
			
			if (buffer_size == dst_width) {
				address_space_write(&p->downstream_as, ch->dst_addr, MEMTXATTRS_UNSPECIFIED, buffer, dst_width);
				buffer_size = 0;
				
				if ((ch->control & DMAC_CH_CONTROL_DI))
					ch->dst_addr += dst_width;
			}
		} else {
			address_space_read(&p->downstream_as, ch->src_addr, MEMTXATTRS_UNSPECIFIED, buffer, src_width);
			
			if ((ch->control & DMAC_CH_CONTROL_SI))
				ch->src_addr += src_width;
			
			for (uint32_t j = 0; j < src_width; j += dst_width) {
				address_space_write(&p->downstream_as, ch->dst_addr, MEMTXATTRS_UNSPECIFIED, buffer + j, dst_width);
				
				if ((ch->control & DMAC_CH_CONTROL_DI))
					ch->dst_addr += dst_width;
			}
		}
		
		tx_size--;
		
		ch->control &= ~DMAC_CH_CONTROL_TRANSFER_SIZE;
		
		if (!is_periph_controlled)
			ch->control |= (tx_size << DMAC_CH_CONTROL_TRANSFER_SIZE_SHIFT);
	}
	
	pmb887x_srb_set_isr(&p->srb_tc, (1 << ch->id));
	ch->config &= ~DMAC_CH_CONFIG_ENABLE;

	p->is_busy = false;
}

static void dmac_update(pmb887x_dmac_t *p) {
	uint32_t err_mask = 0;
	uint32_t tc_mask = 0;
	
	p->enabled_channels = 0;
	p->used_peripherals = 0;
	
	for (int i = 0; i < DMAC_CHANNELS; i++) {
		pmb887x_dmac_ch_t *ch = &p->ch[i];
		
		uint32_t src_width = dmac_get_width((ch->control & DMAC_CH_CONTROL_S_WIDTH) >> DMAC_CH_CONTROL_S_WIDTH_SHIFT);
		uint32_t dst_width = dmac_get_width((ch->control & DMAC_CH_CONTROL_D_WIDTH) >> DMAC_CH_CONTROL_D_WIDTH_SHIFT);
		
		uint8_t mask = 1 << i;
		
		if ((ch->config & DMAC_CH_CONFIG_INT_MASK_ERR)) {
			err_mask |= mask;
		} else {
			err_mask &= ~mask;
		}
		
		if ((ch->config & DMAC_CH_CONFIG_INT_MASK_TC)) {
			tc_mask |= mask;
		} else {
			tc_mask &= ~mask;
		}
		
		if ((ch->config & DMAC_CH_CONFIG_ENABLE))
			p->enabled_channels |= mask;
		
		p->used_peripherals |= (1 << src_width) | (1 << dst_width);
		
		dmac_channel_run(p, ch);
	}
	
	pmb887x_srb_set_imsc(&p->srb_tc, tc_mask);
	pmb887x_srb_set_imsc(&p->srb_err, err_mask);
}

bool pmb887x_dmac_is_busy(pmb887x_dmac_t *p) {
	return p->is_busy;
}

void pmb887x_dmac_request(pmb887x_dmac_t *p, uint32_t per_id, uint32_t size) {
	// DPRINTF("pmb887x_dmac_request(%d, %d)\n", per_id, size);
	p->periph_request[per_id] = size;

	if (size) {
		if (p->used_peripherals & (1 << per_id)) {
			for (int i = 0; i < DMAC_CHANNELS; i++)
				dmac_channel_run(p, &p->ch[i]);
		}
	}
}

static uint32_t dmac_get_index_by_reg(uint32_t reg) {
	if (reg >= DMAC_CH_SRC_ADDR0 && reg <= DMAC_CH_SRC_ADDR7)
		return (reg - DMAC_CH_SRC_ADDR0) / 0x20;
	
	if (reg >= DMAC_CH_DST_ADDR0 && reg <= DMAC_CH_DST_ADDR7)
		return (reg - DMAC_CH_DST_ADDR0) / 0x20;
	
	if (reg >= DMAC_CH_LLI0 && reg <= DMAC_CH_LLI7)
		return (reg - DMAC_CH_LLI0) / 0x20;
	
	if (reg >= DMAC_CH_CONTROL0 && reg <= DMAC_CH_CONTROL7)
		return (reg - DMAC_CH_CONTROL0) / 0x20;
	
	if (reg >= DMAC_CH_CONFIG0 && reg <= DMAC_CH_CONFIG7)
		return (reg - DMAC_CH_CONFIG0) / 0x20;
	
	switch (reg) {
		case DMAC_PCELL_ID0:	return 0;
		case DMAC_PCELL_ID1:	return 1;
		case DMAC_PCELL_ID2:	return 2;
		case DMAC_PCELL_ID3:	return 3;
		case DMAC_PERIPH_ID0:	return 0;
		case DMAC_PERIPH_ID1:	return 1;
		case DMAC_PERIPH_ID2:	return 2;
		case DMAC_PERIPH_ID3:	return 3;
		default:
			hw_error("pmb887x-dmac: unknown reg %d", reg);
	}
}

static uint64_t dmac_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_dmac_t *p = opaque;
	
	uint64_t value = 0;

	switch (haddr) {
		case DMAC_CONFIG:
			value = p->config;
			break;

		case DMAC_INT_STATUS:
			value = pmb887x_srb_get_mis(&p->srb_tc) | pmb887x_srb_get_mis(&p->srb_err);
			break;

		case DMAC_TC_STATUS:
			value = pmb887x_srb_get_mis(&p->srb_tc);
			break;

		case DMAC_ERR_STATUS:
			value = pmb887x_srb_get_mis(&p->srb_err);
			break;

		case DMAC_RAW_TC_STATUS:
			value = pmb887x_srb_get_ris(&p->srb_tc);
			break;

		case DMAC_RAW_ERR_STATUS:
			value = pmb887x_srb_get_ris(&p->srb_err);
			break;

		case DMAC_TC_CLEAR:
		case DMAC_ERR_CLEAR:
			value = 0;
			break;

		case DMAC_EN_CHAN:
			value = p->enabled_channels;
			break;

		case DMAC_PCELL_ID0:
		case DMAC_PCELL_ID1:
		case DMAC_PCELL_ID2:
		case DMAC_PCELL_ID3:
			value = (PCELL_ID >> dmac_get_index_by_reg(haddr) * 8) & 0xFF;
			break;

		case DMAC_PERIPH_ID0:
		case DMAC_PERIPH_ID1:
		case DMAC_PERIPH_ID2:
		case DMAC_PERIPH_ID3:
			value = (PERIPH_ID >> dmac_get_index_by_reg(haddr) * 8) & 0xFF;
			break;

		case DMAC_CH_SRC_ADDR0:
		case DMAC_CH_SRC_ADDR1:
		case DMAC_CH_SRC_ADDR2:
		case DMAC_CH_SRC_ADDR3:
		case DMAC_CH_SRC_ADDR4:
		case DMAC_CH_SRC_ADDR5:
		case DMAC_CH_SRC_ADDR6:
		case DMAC_CH_SRC_ADDR7:
			value = p->ch[dmac_get_index_by_reg(haddr)].src_addr;
			break;

		case DMAC_CH_DST_ADDR0:
		case DMAC_CH_DST_ADDR1:
		case DMAC_CH_DST_ADDR2:
		case DMAC_CH_DST_ADDR3:
		case DMAC_CH_DST_ADDR4:
		case DMAC_CH_DST_ADDR5:
		case DMAC_CH_DST_ADDR6:
		case DMAC_CH_DST_ADDR7:
			value = p->ch[dmac_get_index_by_reg(haddr)].dst_addr;
			break;

		case DMAC_CH_CONFIG0:
		case DMAC_CH_CONFIG1:
		case DMAC_CH_CONFIG2:
		case DMAC_CH_CONFIG3:
		case DMAC_CH_CONFIG4:
		case DMAC_CH_CONFIG5:
		case DMAC_CH_CONFIG6:
		case DMAC_CH_CONFIG7:
			value = p->ch[dmac_get_index_by_reg(haddr)].config;
			break;

		case DMAC_CH_CONTROL0:
		case DMAC_CH_CONTROL1:
		case DMAC_CH_CONTROL2:
		case DMAC_CH_CONTROL3:
		case DMAC_CH_CONTROL4:
		case DMAC_CH_CONTROL5:
		case DMAC_CH_CONTROL6:
		case DMAC_CH_CONTROL7:
			value = p->ch[dmac_get_index_by_reg(haddr)].control;
			break;

		case DMAC_CH_LLI0:
		case DMAC_CH_LLI1:
		case DMAC_CH_LLI2:
		case DMAC_CH_LLI3:
		case DMAC_CH_LLI4:
		case DMAC_CH_LLI5:
		case DMAC_CH_LLI6:
		case DMAC_CH_LLI7:
			value = p->ch[dmac_get_index_by_reg(haddr)].lli;
			break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}

	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void dmac_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_dmac_t *p = opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case DMAC_CONFIG:
			p->config = value;
			break;
		
		case DMAC_TC_CLEAR:
			pmb887x_srb_set_icr(&p->srb_tc, value);
			break;
		
		case DMAC_ERR_CLEAR:
			pmb887x_srb_set_icr(&p->srb_err, value);
			break;
		
		case DMAC_CH_SRC_ADDR0:
		case DMAC_CH_SRC_ADDR1:
		case DMAC_CH_SRC_ADDR2:
		case DMAC_CH_SRC_ADDR3:
		case DMAC_CH_SRC_ADDR4:
		case DMAC_CH_SRC_ADDR5:
		case DMAC_CH_SRC_ADDR6:
		case DMAC_CH_SRC_ADDR7:
			p->ch[dmac_get_index_by_reg(haddr)].src_addr = value;
			break;
		
		case DMAC_CH_DST_ADDR0:
		case DMAC_CH_DST_ADDR1:
		case DMAC_CH_DST_ADDR2:
		case DMAC_CH_DST_ADDR3:
		case DMAC_CH_DST_ADDR4:
		case DMAC_CH_DST_ADDR5:
		case DMAC_CH_DST_ADDR6:
		case DMAC_CH_DST_ADDR7:
			p->ch[dmac_get_index_by_reg(haddr)].dst_addr = value;
			break;
		
		case DMAC_CH_CONFIG0:
		case DMAC_CH_CONFIG1:
		case DMAC_CH_CONFIG2:
		case DMAC_CH_CONFIG3:
		case DMAC_CH_CONFIG4:
		case DMAC_CH_CONFIG5:
		case DMAC_CH_CONFIG6:
		case DMAC_CH_CONFIG7:
			p->ch[dmac_get_index_by_reg(haddr)].config = value;
			break;
		
		case DMAC_CH_CONTROL0:
		case DMAC_CH_CONTROL1:
		case DMAC_CH_CONTROL2:
		case DMAC_CH_CONTROL3:
		case DMAC_CH_CONTROL4:
		case DMAC_CH_CONTROL5:
		case DMAC_CH_CONTROL6:
		case DMAC_CH_CONTROL7:
			p->ch[dmac_get_index_by_reg(haddr)].control = value;
			break;
		
		case DMAC_CH_LLI0:
		case DMAC_CH_LLI1:
		case DMAC_CH_LLI2:
		case DMAC_CH_LLI3:
		case DMAC_CH_LLI4:
		case DMAC_CH_LLI5:
		case DMAC_CH_LLI6:
		case DMAC_CH_LLI7:
			p->ch[dmac_get_index_by_reg(haddr)].lli = value;
			break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	dmac_update(p);
}

static const MemoryRegionOps io_ops = {
	.read			= dmac_io_read,
	.write			= dmac_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void dmac_init(Object *obj) {
	pmb887x_dmac_t *p = PMB887X_DMAC(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-dmac", DMAC_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq_err);
	
	for (int i = 0; i < DMAC_CHANNELS; i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq_tc[i]);
}

static int dmac_tc_irq_router(void *opaque, int event_id) {
	if (event_id < DMAC_CHANNELS)
		return event_id;
	
	hw_error("Unknown event id: %d\n", event_id);
}

static int dmac_err_irq_router(void *opaque, int event_id) {
	return 0;
}

static void dmac_realize(DeviceState *dev, Error **errp) {
	pmb887x_dmac_t *p = PMB887X_DMAC(dev);
	
	if (!p->downstream) {
		error_setg(errp, "DMAC 'downstream' link not set");
		return;
	}
	
	address_space_init(&p->downstream_as, p->downstream, "pl080-downstream");
	
	for (int i = 0; i < DMAC_CHANNELS; i++)
		p->ch[i].id = i;
	
	pmb887x_srb_init(&p->srb_err, &p->irq_err, 1);
	pmb887x_srb_set_irq_router(&p->srb_err, p, dmac_err_irq_router);
	
	pmb887x_srb_init(&p->srb_tc, p->irq_tc, ARRAY_SIZE(p->irq_tc));
	pmb887x_srb_set_irq_router(&p->srb_tc, p, dmac_tc_irq_router);
}

static const Property dmac_properties[] = {
	DEFINE_PROP_LINK("downstream", pmb887x_dmac_t, downstream, TYPE_MEMORY_REGION, MemoryRegion *),
};

static void dmac_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, dmac_properties);
	dc->realize = dmac_realize;
}

static const TypeInfo dmac_info = {
    .name          	= TYPE_PMB887X_DMAC,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_dmac_t),
    .instance_init 	= dmac_init,
    .class_init    	= dmac_class_init,
};

static void dmac_register_types(void) {
	type_register_static(&dmac_info);
}
type_init(dmac_register_types)
