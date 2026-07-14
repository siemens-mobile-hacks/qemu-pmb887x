/*
 * DMA Controller (PL080)
 * */
#define PMB887X_TRACE_ID		DMAC
#define PMB887X_TRACE_PREFIX	"pmb887x-dmac"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "system/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/dmac.h"
#include "hw/arm/pmb887x/trace.h"

#define DMAC_MULTIPLEXOR	2
#define DMAC_CHANNELS		8
#define DMAC_REQUESTS		16
#define DMAC_FIFO			16

static const uint32_t PCELL_ID = 0xB105F00D;
static const uint32_t PERIPH_ID = 0x0A141080;

typedef struct pmb887x_dmac_ch_t pmb887x_dmac_ch_t;
typedef struct pmb887x_dmac_request_t pmb887x_dmac_request_t;

struct pmb887x_dmac_request_t {
	int level[DMAC_MULTIPLEXOR][DMAC_REQUESTS];
	uint16_t soft;
};

struct pmb887x_dmac_ch_t {
	uint8_t id;
	uint32_t src_addr;
	uint32_t dst_addr;
	uint32_t lli;
	uint32_t control;
	uint32_t config;
	bool is_source_complete;
	bool is_active;
};

struct pmb887x_dmac_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	QEMUTimer *timer;
	MemoryRegion *downstream;
	AddressSpace downstream_as;
	
	qemu_irq irq_err;
	qemu_irq irq_tc[DMAC_CHANNELS];
	
	pmb887x_srb_reg_t srb_tc;
	pmb887x_srb_reg_t srb_err;
	
	pmb887x_dmac_ch_t ch[DMAC_CHANNELS];

	bool dmac_pending;
	bool is_busy;
	uint32_t config;
	uint32_t sync;
	
	int sel[DMAC_REQUESTS];

	qemu_irq CLR[DMAC_MULTIPLEXOR][DMAC_REQUESTS];
	qemu_irq TC[DMAC_MULTIPLEXOR][DMAC_REQUESTS];

	pmb887x_dmac_request_t sreq;
	pmb887x_dmac_request_t breq;
	pmb887x_dmac_request_t lbreq;
	pmb887x_dmac_request_t lsreq;
};

static inline uint32_t dmac_get_width(uint32_t s) {
	if (s > 2)
		hw_error("pmb887x-dmac: unknown width %d", s);
	return 1 << s;
}

static inline uint32_t dmac_get_burst_size(uint32_t s) {
	if (s == 0)
		return 1;
	if (s > 7)
		hw_error("pmb887x-dmac: unknown burst size %d", s);
	return 1 << (s + 1);
}

static enum device_endian dmac_master_endian(pmb887x_dmac_t *p, bool is_ahb2) {
	return (p->config & (is_ahb2 ? DMAC_CONFIG_M2 : DMAC_CONFIG_M1)) ? DEVICE_BIG_ENDIAN : DEVICE_LITTLE_ENDIAN;
}

static void dmac_swap_byte_order(uint8_t *buffer, uint32_t width, uint32_t count) {
	for (uint32_t i = 0; i < count; i++) {
		uint8_t *transfer = buffer + i * width;
		switch (width) {
			case 2:
				bswap16s((uint16_t *) transfer);
				break;

			case 4:
				bswap32s((uint32_t *) transfer);
				break;

			default:
				g_assert_not_reached();
		}
	}
}

static void dmac_read(pmb887x_dmac_t *p, hwaddr addr, uint8_t *buffer, uint32_t width, uint32_t count, enum device_endian endian) {
	if (endian == DEVICE_BIG_ENDIAN && width < sizeof(uint32_t)) {
		for (uint32_t i = 0; i < count; i++) {
			hwaddr transfer_addr = (addr + i * width) ^ (sizeof(uint32_t) - width);
			address_space_read(&p->downstream_as, transfer_addr, MEMTXATTRS_UNSPECIFIED, buffer + i * width, width);
		}
	} else {
		address_space_read(&p->downstream_as, addr, MEMTXATTRS_UNSPECIFIED, buffer, width * count);
	}
}

static void dmac_write(pmb887x_dmac_t *p, hwaddr addr, const uint8_t *buffer, uint32_t width) {
	address_space_write(&p->downstream_as, addr, MEMTXATTRS_UNSPECIFIED, buffer, width);
}

static void dmac_schedule(pmb887x_dmac_t *p) {
	if (!p->dmac_pending) {
		p->dmac_pending = true;
		timer_mod(p->timer, 0);
	}
}

static bool dmac_is_request_pending(const pmb887x_dmac_request_t *request, uint8_t sel, uint8_t id) {
	return request->level[sel][id] || (request->soft & (1 << id));
}

static void dmac_ack_request(pmb887x_dmac_t *p, pmb887x_dmac_request_t *request, uint8_t sel, uint8_t id) {
	request->soft &= ~(1 << id);
	if (request->level[sel][id])
		qemu_set_irq(p->CLR[sel][id], 1);
}

static void dmac_clear_software_requests(pmb887x_dmac_t *p) {
	p->sreq.soft = 0;
	p->breq.soft = 0;
	p->lbreq.soft = 0;
	p->lsreq.soft = 0;
}

static void dmac_write_software_request(pmb887x_dmac_t *p, pmb887x_dmac_request_t *request, uint32_t value) {
	if ((p->config & DMAC_CONFIG_ENABLE))
		request->soft |= value;
}

static bool dmac_is_flow_controller(uint32_t flow_ctrl) {
	return (
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2MEM ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_PER2MEM ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_PER2PER
	);
}

static void dmac_update_final_addresses(pmb887x_dmac_ch_t *ch) {
	/* A stopped PL080 channel exposes the addresses of the last transfer. */
	uint32_t src_width = dmac_get_width((ch->control & DMAC_CH_CONTROL_S_WIDTH) >> DMAC_CH_CONTROL_S_WIDTH_SHIFT);
	uint32_t dst_width = dmac_get_width((ch->control & DMAC_CH_CONTROL_D_WIDTH) >> DMAC_CH_CONTROL_D_WIDTH_SHIFT);
	if ((ch->control & DMAC_CH_CONTROL_SI))
		ch->src_addr -= src_width;
	if ((ch->control & DMAC_CH_CONTROL_DI))
		ch->dst_addr -= dst_width;
}

static void dmac_transfer_finish(pmb887x_dmac_t *p, pmb887x_dmac_ch_t *ch) {
	uint32_t flow_ctrl = (ch->config & DMAC_CH_CONFIG_FLOW_CTRL);
	uint8_t src_periph = (ch->config & DMAC_CH_CONFIG_SRC_PERIPH) >> DMAC_CH_CONFIG_SRC_PERIPH_SHIFT;
	uint8_t dst_periph = (ch->config & DMAC_CH_CONFIG_DST_PERIPH) >> DMAC_CH_CONFIG_DST_PERIPH_SHIFT;
	uint8_t src_sel = p->sel[src_periph];
	uint8_t dst_sel = p->sel[dst_periph];

	uint32_t lli_addr = ch->lli & DMAC_CH_LLI_ITEM;
	if (lli_addr) {
		uint32_t lli[4];
		enum device_endian lli_endian = dmac_master_endian(p, (ch->lli & DMAC_CH_LLI_LM) != 0);
		dmac_read(p, lli_addr, (uint8_t *) lli, sizeof(uint32_t), ARRAY_SIZE(lli), lli_endian);
		if (lli_endian == DEVICE_BIG_ENDIAN)
			dmac_swap_byte_order((uint8_t *) lli, sizeof(uint32_t), ARRAY_SIZE(lli));

		if ((ch->control & DMAC_CH_CONTROL_I))
			pmb887x_srb_set_isr(&p->srb_tc, (1 << ch->id));

		ch->src_addr = lli[0];
		ch->dst_addr = lli[1];
		ch->lli = lli[2];
		ch->control = lli[3];

		DPRINTF("CH%d LLI=%08X [%08X, %08X, %08X, %08X]\n", ch->id, lli_addr, lli[0], lli[1], lli[2], lli[3]);
	} else {
		DPRINTF("CH%d: transfer done\n", ch->id);
		dmac_update_final_addresses(ch);
		ch->config &= ~DMAC_CH_CONFIG_ENABLE;
		ch->is_active = false;
		pmb887x_srb_set_isr(&p->srb_tc, (1 << ch->id));
	}

	bool is_dst_tc = (
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER_PER ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_PER2PER_DST
	);

	bool is_src_tc = (
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_PER2MEM ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_PER2MEM_PER ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_PER2PER_SRC
	);

	if (is_dst_tc)
		qemu_set_irq(p->TC[dst_sel][dst_periph], 1);

	if (is_src_tc)
		qemu_set_irq(p->TC[src_sel][src_periph], 1);
}

static void dmac_transfer_memory(pmb887x_dmac_t *p, pmb887x_dmac_ch_t *ch, uint32_t burst_size) {
	uint8_t buffer[16 * 1024] QEMU_ALIGNED(4); // 12bit TransferSize x DWORD
	uint32_t src_width = dmac_get_width((ch->control & DMAC_CH_CONTROL_S_WIDTH) >> DMAC_CH_CONTROL_S_WIDTH_SHIFT);
	uint32_t dst_width = dmac_get_width((ch->control & DMAC_CH_CONTROL_D_WIDTH) >> DMAC_CH_CONTROL_D_WIDTH_SHIFT);
	uint32_t flow_ctrl = (ch->config & DMAC_CH_CONFIG_FLOW_CTRL);
	uint32_t tx_size = (ch->control & DMAC_CH_CONTROL_TRANSFER_SIZE) >> DMAC_CH_CONTROL_TRANSFER_SIZE_SHIFT;
	enum device_endian src_endian = dmac_master_endian(p, (ch->control & DMAC_CH_CONTROL_S_AHB2) != 0);
	enum device_endian dst_endian = dmac_master_endian(p, (ch->control & DMAC_CH_CONTROL_D_AHB2) != 0);

	bool is_simple_memcpy = (
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2MEM &&
		dst_width == src_width &&
		src_endian == dst_endian &&
		(src_endian == DEVICE_LITTLE_ENDIAN || src_width == sizeof(uint32_t)) &&
		(ch->control & DMAC_CH_CONTROL_SI) != 0 &&
		(ch->control & DMAC_CH_CONTROL_DI) != 0
	);

	bool is_src_memory = (
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2MEM ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER_PER
	);

	ch->src_addr &= ~(src_width - 1);
	ch->dst_addr &= ~(dst_width - 1);

	DPRINTF("CH%d: %08X [%dx%d] -> %08X [%dx%d] [%d]\n", ch->id, ch->src_addr, src_width, burst_size, ch->dst_addr, dst_width, burst_size, tx_size);
	ch->is_active = burst_size > 0;

	if (is_simple_memcpy) {
		address_space_read(&p->downstream_as, ch->src_addr, MEMTXATTRS_UNSPECIFIED, buffer, src_width * burst_size);
		address_space_write(&p->downstream_as, ch->dst_addr, MEMTXATTRS_UNSPECIFIED, buffer, dst_width * burst_size);
		ch->src_addr += src_width * burst_size;
		ch->dst_addr += dst_width * burst_size;
	} else if (dst_width == src_width) {
		uint32_t transferred = 0;
		while (transferred < burst_size) {
			dmac_read(p, ch->src_addr, buffer, src_width, 1, src_endian);
			if (src_endian != dst_endian && src_width > 1)
				dmac_swap_byte_order(buffer, dst_width, 1);
			dmac_write(p, ch->dst_addr, buffer, dst_width);

			if ((ch->control & DMAC_CH_CONTROL_SI))
				ch->src_addr += src_width;

			if ((ch->control & DMAC_CH_CONTROL_DI))
				ch->dst_addr += dst_width;

			transferred++;
		}
	} else if (dst_width > src_width) {
		uint32_t transferred = 0;
		uint32_t buffer_size = 0;
		while (transferred < burst_size) {
			dmac_read(p, ch->src_addr, buffer + buffer_size, src_width, 1, src_endian);
			if (src_endian == DEVICE_BIG_ENDIAN && src_width > 1)
				dmac_swap_byte_order(buffer + buffer_size, src_width, 1);
			buffer_size += src_width;

			if ((ch->control & DMAC_CH_CONTROL_SI))
				ch->src_addr += src_width;

			if (buffer_size == dst_width) {
				if (dst_endian == DEVICE_BIG_ENDIAN && dst_width > 1)
					dmac_swap_byte_order(buffer, dst_width, 1);
				dmac_write(p, ch->dst_addr, buffer, dst_width);
				buffer_size = 0;

				if ((ch->control & DMAC_CH_CONTROL_DI))
					ch->dst_addr += dst_width;
			}
			transferred++;
		}
	} else {
		uint32_t transferred = 0;
		uint32_t src_burst_size = is_src_memory && (ch->control & DMAC_CH_CONTROL_SI) ? burst_size : 1;
		uint32_t src_burst_size_bytes = src_burst_size * src_width;
		while (transferred < burst_size) {
			dmac_read(p, ch->src_addr, buffer, src_width, src_burst_size, src_endian);
			if (src_endian == DEVICE_BIG_ENDIAN)
				dmac_swap_byte_order(buffer, src_width, src_burst_size);

			if ((ch->control & DMAC_CH_CONTROL_SI))
				ch->src_addr += src_burst_size_bytes;

			for (uint32_t j = 0; j < src_burst_size_bytes; j += dst_width) {
				if (dst_endian == DEVICE_BIG_ENDIAN && dst_width > 1)
					dmac_swap_byte_order(buffer + j, dst_width, 1);
				dmac_write(p, ch->dst_addr, buffer + j, dst_width);

				if ((ch->control & DMAC_CH_CONTROL_DI))
					ch->dst_addr += dst_width;
			}
			transferred += src_burst_size;
		}
	}

	if (tx_size > 0) {
		if (!dmac_is_flow_controller(flow_ctrl))
			hw_error("TransferSize must be zero when peripheral is flow controller!");

		tx_size -= burst_size;
		ch->control &= ~DMAC_CH_CONTROL_TRANSFER_SIZE;
		ch->control |= tx_size << DMAC_CH_CONTROL_TRANSFER_SIZE_SHIFT;

		if (tx_size == 0)
			dmac_transfer_finish(p, ch);
	}
}

static bool dmac_service_request(pmb887x_dmac_t *p, pmb887x_dmac_ch_t *ch, pmb887x_dmac_request_t *request, pmb887x_dmac_request_t *last_request, uint8_t sel, uint8_t id, uint32_t count) {
	bool is_last = dmac_is_request_pending(last_request, sel, id);
	if (!is_last && !dmac_is_request_pending(request, sel, id))
		return false;

	dmac_transfer_memory(p, ch, count);
	if (is_last)
		dmac_transfer_finish(p, ch);
	dmac_ack_request(p, is_last ? last_request : request, sel, id);
	return true;
}

static void dmac_channel_run(pmb887x_dmac_t *p, pmb887x_dmac_ch_t *ch) {
	if (!(ch->config & DMAC_CH_CONFIG_ENABLE) || !(p->config & DMAC_CONFIG_ENABLE))
		return;
	if ((ch->config & DMAC_CH_CONFIG_HALT) && !ch->is_active)
		return;

	uint32_t src_burst_size = dmac_get_burst_size((ch->control & DMAC_CH_CONTROL_SB_SIZE) >> DMAC_CH_CONTROL_SB_SIZE_SHIFT);
	uint32_t dst_burst_size = dmac_get_burst_size((ch->control & DMAC_CH_CONTROL_DB_SIZE) >> DMAC_CH_CONTROL_DB_SIZE_SHIFT);
	uint8_t src_periph = (ch->config & DMAC_CH_CONFIG_SRC_PERIPH) >> DMAC_CH_CONFIG_SRC_PERIPH_SHIFT;
	uint8_t dst_periph = (ch->config & DMAC_CH_CONFIG_DST_PERIPH) >> DMAC_CH_CONFIG_DST_PERIPH_SHIFT;
	uint8_t src_sel = p->sel[src_periph];
	uint8_t dst_sel = p->sel[dst_periph];
	uint32_t flow_ctrl = (ch->config & DMAC_CH_CONFIG_FLOW_CTRL);
	uint32_t tx_size = (ch->control & DMAC_CH_CONTROL_TRANSFER_SIZE) >> DMAC_CH_CONTROL_TRANSFER_SIZE_SHIFT;
	if (dmac_is_flow_controller(flow_ctrl) && !tx_size)
		return;

	switch (flow_ctrl) {
		case DMAC_CH_CONFIG_FLOW_CTRL_MEM2MEM: {
			while ((ch->config & DMAC_CH_CONFIG_ENABLE) != 0) {
				tx_size = (ch->control & DMAC_CH_CONTROL_TRANSFER_SIZE) >> DMAC_CH_CONTROL_TRANSFER_SIZE_SHIFT;
				if (!tx_size)
					break;
				dmac_transfer_memory(p, ch, tx_size);
			}
			break;
		}

		case DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER: {
			if (dmac_is_request_pending(&p->breq, dst_sel, dst_periph)) {
				dmac_transfer_memory(p, ch, MIN(tx_size, src_burst_size));
				dmac_ack_request(p, &p->breq, dst_sel, dst_periph);
			}
			break;
		}

		case DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER_PER: {
			if (!dmac_service_request(p, ch, &p->breq, &p->lbreq, dst_sel, dst_periph, dst_burst_size))
				dmac_service_request(p, ch, &p->sreq, &p->lsreq, dst_sel, dst_periph, 1);
			break;
		}

		case DMAC_CH_CONFIG_FLOW_CTRL_PER2MEM:
			if (dmac_is_request_pending(&p->breq, src_sel, src_periph)) {
				dmac_transfer_memory(p, ch, MIN(tx_size, src_burst_size));
				dmac_ack_request(p, &p->breq, src_sel, src_periph);
			} else if (dmac_is_request_pending(&p->sreq, src_sel, src_periph)) {
				dmac_transfer_memory(p, ch, MIN(tx_size, 1));
				dmac_ack_request(p, &p->sreq, src_sel, src_periph);
			}
			break;

		case DMAC_CH_CONFIG_FLOW_CTRL_PER2PER:
			if (dmac_is_request_pending(&p->breq, src_sel, src_periph) &&
				dmac_is_request_pending(&p->breq, dst_sel, dst_periph)) {
				dmac_transfer_memory(p, ch, MIN(tx_size, MIN(src_burst_size, dst_burst_size)));
				dmac_ack_request(p, &p->breq, src_sel, src_periph);
				dmac_ack_request(p, &p->breq, dst_sel, dst_periph);
			}
			break;

		case DMAC_CH_CONFIG_FLOW_CTRL_PER2PER_DST:
			if (dmac_is_request_pending(&p->breq, src_sel, src_periph) &&
				(dmac_is_request_pending(&p->breq, dst_sel, dst_periph) ||
				 dmac_is_request_pending(&p->lbreq, dst_sel, dst_periph))) {
				bool is_last = dmac_is_request_pending(&p->lbreq, dst_sel, dst_periph);
				dmac_transfer_memory(p, ch, MIN(src_burst_size, dst_burst_size));
				dmac_ack_request(p, &p->breq, src_sel, src_periph);
				dmac_ack_request(p, is_last ? &p->lbreq : &p->breq, dst_sel, dst_periph);
				if (is_last)
					dmac_transfer_finish(p, ch);
			}
			break;

		case DMAC_CH_CONFIG_FLOW_CTRL_PER2PER_SRC:
			if (dmac_is_request_pending(&p->lbreq, src_sel, src_periph)) {
				ch->is_source_complete = true;
				dmac_ack_request(p, &p->lbreq, src_sel, src_periph);
			}
			if (ch->is_source_complete && dmac_is_request_pending(&p->breq, dst_sel, dst_periph)) {
				dmac_transfer_memory(p, ch, MIN(src_burst_size, dst_burst_size));
				dmac_ack_request(p, &p->breq, dst_sel, dst_periph);
				dmac_transfer_finish(p, ch);
				ch->is_source_complete = false;
			}
			break;

		case DMAC_CH_CONFIG_FLOW_CTRL_PER2MEM_PER:
			if (!dmac_service_request(p, ch, &p->breq, &p->lbreq, src_sel, src_periph, src_burst_size))
				dmac_service_request(p, ch, &p->sreq, &p->lsreq, src_sel, src_periph, 1);
			break;

		default:
			hw_error("pmb887x-dmac: unsupported flow type %08X", (ch->config & DMAC_CH_CONFIG_FLOW_CTRL));
	}
}

static void dmac_update(pmb887x_dmac_t *p) {
	uint32_t err_mask = 0;
	uint32_t tc_mask = 0;
	
	for (int i = 0; i < DMAC_CHANNELS; i++) {
		pmb887x_dmac_ch_t *ch = &p->ch[i];
		uint8_t mask = 1 << i;
		
		if ((ch->config & DMAC_CH_CONFIG_INT_MASK_ERR))
			err_mask |= mask;
		
		if ((ch->config & DMAC_CH_CONFIG_INT_MASK_TC))
			tc_mask |= mask;
	}
	
	pmb887x_srb_set_imsc(&p->srb_tc, tc_mask);
	pmb887x_srb_set_imsc(&p->srb_err, err_mask);

	dmac_schedule(p);
}

static const char *dmac_request_name(pmb887x_dmac_t *p, const pmb887x_dmac_request_t *request) {
	if (request == &p->sreq) {
		return "SREQ";
	} else if (request == &p->lsreq) {
		return "LSREQ";
	} else if (request == &p->breq) {
		return "BREQ";
	} else if (request == &p->lbreq) {
		return "LBREQ";
	}
	return "UNK";
}

static void dmac_handle_signal(pmb887x_dmac_t *p, pmb887x_dmac_request_t *request, int sel, int id, int level) {
	if (request->level[sel][id] == level)
		return;

	DPRINTF("%s request=%d, level=%d\n", dmac_request_name(p, request), id, level);
	request->level[sel][id] = level;

	if (level == 0) {
		bool is_pending = (
			p->sreq.level[sel][id] || p->lsreq.level[sel][id] ||
			p->breq.level[sel][id] || p->lbreq.level[sel][id]
		);
		if (!is_pending) {
			qemu_set_irq(p->CLR[sel][id], 0);
			qemu_set_irq(p->TC[sel][id], 0);
		}
	}

	if (level != 0)
		dmac_schedule(p);
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

		case DMAC_EN_CHAN: {
			for (int i = 0; i < DMAC_CHANNELS; i++) {
				pmb887x_dmac_ch_t *ch = &p->ch[i];
				if ((ch->config & DMAC_CH_CONFIG_ENABLE))
					value |= 1 << i;
			}
			break;
		}

		case DMAC_PCELL_ID0:
		case DMAC_PCELL_ID1:
		case DMAC_PCELL_ID2:
		case DMAC_PCELL_ID3:
			value = (PCELL_ID >> ((haddr - DMAC_PCELL_ID0) * 2)) & 0xFF;
			break;

		case DMAC_PERIPH_ID0:
		case DMAC_PERIPH_ID1:
		case DMAC_PERIPH_ID2:
		case DMAC_PERIPH_ID3:
			value = (PERIPH_ID >> ((haddr - DMAC_PERIPH_ID0) * 2)) & 0xFF;
			break;

		case DMAC_CH_SRC_ADDR0:
		case DMAC_CH_SRC_ADDR1:
		case DMAC_CH_SRC_ADDR2:
		case DMAC_CH_SRC_ADDR3:
		case DMAC_CH_SRC_ADDR4:
		case DMAC_CH_SRC_ADDR5:
		case DMAC_CH_SRC_ADDR6:
		case DMAC_CH_SRC_ADDR7:
			value = p->ch[(haddr - DMAC_CH_SRC_ADDR0) / 0x20].src_addr;
			break;

		case DMAC_CH_DST_ADDR0:
		case DMAC_CH_DST_ADDR1:
		case DMAC_CH_DST_ADDR2:
		case DMAC_CH_DST_ADDR3:
		case DMAC_CH_DST_ADDR4:
		case DMAC_CH_DST_ADDR5:
		case DMAC_CH_DST_ADDR6:
		case DMAC_CH_DST_ADDR7:
			value = p->ch[(haddr - DMAC_CH_DST_ADDR0) / 0x20].dst_addr;
			break;

		case DMAC_CH_CONFIG0:
		case DMAC_CH_CONFIG1:
		case DMAC_CH_CONFIG2:
		case DMAC_CH_CONFIG3:
		case DMAC_CH_CONFIG4:
		case DMAC_CH_CONFIG5:
		case DMAC_CH_CONFIG6:
		case DMAC_CH_CONFIG7: {
			pmb887x_dmac_ch_t *ch = &p->ch[(haddr - DMAC_CH_CONFIG0) / 0x20];
			value = ch->config | (ch->is_active ? DMAC_CH_CONFIG_ACTIVE : 0);
			break;
		}

		case DMAC_CH_CONTROL0:
		case DMAC_CH_CONTROL1:
		case DMAC_CH_CONTROL2:
		case DMAC_CH_CONTROL3:
		case DMAC_CH_CONTROL4:
		case DMAC_CH_CONTROL5:
		case DMAC_CH_CONTROL6:
		case DMAC_CH_CONTROL7:
			value = p->ch[(haddr - DMAC_CH_CONTROL0) / 0x20].control;
			break;

		case DMAC_CH_LLI0:
		case DMAC_CH_LLI1:
		case DMAC_CH_LLI2:
		case DMAC_CH_LLI3:
		case DMAC_CH_LLI4:
		case DMAC_CH_LLI5:
		case DMAC_CH_LLI6:
		case DMAC_CH_LLI7:
			value = p->ch[(haddr - DMAC_CH_LLI0) / 0x20].lli;
			break;

		case DMAC_SOFT_BREQ:
			value = p->breq.soft;
			break;

		case DMAC_SOFT_SREQ:
			value = p->sreq.soft;
			break;

		case DMAC_SOFT_LBREQ:
			value = p->lbreq.soft;
			break;

		case DMAC_SOFT_LSREQ:
			value = p->lsreq.soft;
			break;

		case DMAC_SYNC:
			value = p->sync;
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
			p->config = value & (DMAC_CONFIG_ENABLE | DMAC_CONFIG_M1 | DMAC_CONFIG_M2);
			if (!(p->config & DMAC_CONFIG_ENABLE))
				dmac_clear_software_requests(p);
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
			p->ch[(haddr - DMAC_CH_SRC_ADDR0) / 0x20].src_addr = value;
			break;
		
		case DMAC_CH_DST_ADDR0:
		case DMAC_CH_DST_ADDR1:
		case DMAC_CH_DST_ADDR2:
		case DMAC_CH_DST_ADDR3:
		case DMAC_CH_DST_ADDR4:
		case DMAC_CH_DST_ADDR5:
		case DMAC_CH_DST_ADDR6:
		case DMAC_CH_DST_ADDR7:
			p->ch[(haddr - DMAC_CH_DST_ADDR0) / 0x20].dst_addr = value;
			break;
		
		case DMAC_CH_CONFIG0:
		case DMAC_CH_CONFIG1:
		case DMAC_CH_CONFIG2:
		case DMAC_CH_CONFIG3:
		case DMAC_CH_CONFIG4:
		case DMAC_CH_CONFIG5:
		case DMAC_CH_CONFIG6:
		case DMAC_CH_CONFIG7: {
			pmb887x_dmac_ch_t *ch = &p->ch[(haddr - DMAC_CH_CONFIG0) / 0x20];
			if (!(ch->config & DMAC_CH_CONFIG_ENABLE) && (value & DMAC_CH_CONFIG_ENABLE))
				ch->is_source_complete = false;
			ch->config = value & ~DMAC_CH_CONFIG_ACTIVE;
			if (!(ch->config & DMAC_CH_CONFIG_ENABLE))
				ch->is_active = false;
			break;
		}
		
		case DMAC_CH_CONTROL0:
		case DMAC_CH_CONTROL1:
		case DMAC_CH_CONTROL2:
		case DMAC_CH_CONTROL3:
		case DMAC_CH_CONTROL4:
		case DMAC_CH_CONTROL5:
		case DMAC_CH_CONTROL6:
		case DMAC_CH_CONTROL7:
			p->ch[(haddr - DMAC_CH_CONTROL0) / 0x20].control = value;
			break;
		
		case DMAC_CH_LLI0:
		case DMAC_CH_LLI1:
		case DMAC_CH_LLI2:
		case DMAC_CH_LLI3:
		case DMAC_CH_LLI4:
		case DMAC_CH_LLI5:
		case DMAC_CH_LLI6:
		case DMAC_CH_LLI7:
			p->ch[(haddr - DMAC_CH_LLI0) / 0x20].lli = value;
			break;

		case DMAC_SOFT_BREQ:
			dmac_write_software_request(p, &p->breq, value);
			break;

		case DMAC_SOFT_SREQ:
			dmac_write_software_request(p, &p->sreq, value);
			break;

		case DMAC_SOFT_LBREQ:
			dmac_write_software_request(p, &p->lbreq, value);
			break;

		case DMAC_SOFT_LSREQ:
			dmac_write_software_request(p, &p->lsreq, value);
			break;

		case DMAC_SYNC:
			p->sync = value;
			break;

		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	dmac_update(p);
}

void pmb887x_dmac_set_sel(pmb887x_dmac_t *p, uint32_t value) {
	for (int i = 0; i < DMAC_REQUESTS; i++)
		p->sel[i] = (value & (1 << i)) ? 1 : 0;
	dmac_schedule(p);
}

uint32_t pmb887x_dmac_get_sel(pmb887x_dmac_t *p) {
	uint32_t value = 0;
	for (int i = 0; i < DMAC_REQUESTS; i++)
		value |= p->sel[i] << i;
	return value;
}

static void dmac_timer_reset(void *opaque) {
	pmb887x_dmac_t *p = opaque;
	while (p->dmac_pending) {
		p->dmac_pending = false;
		for (int i = 0; i < DMAC_CHANNELS; i++)
			dmac_channel_run(p, &p->ch[i]);
		if (pmb887x_srb_get_ris(&p->srb_tc) || pmb887x_srb_get_ris(&p->srb_err))
			break;
	}
}

static void dmac_handle_signal_sel0_sreq(void *opaque, int request, int level) {
	pmb887x_dmac_t *p = opaque;
	dmac_handle_signal(p, &p->sreq, 0, request, level);
}

static void dmac_handle_signal_sel0_breq(void *opaque, int request, int level) {
	pmb887x_dmac_t *p = opaque;
	dmac_handle_signal(p, &p->breq, 0, request, level);
}

static void dmac_handle_signal_sel0_lsreq(void *opaque, int request, int level) {
	pmb887x_dmac_t *p = opaque;
	dmac_handle_signal(p, &p->lsreq, 0, request, level);
}

static void dmac_handle_signal_sel0_lbreq(void *opaque, int request, int level) {
	pmb887x_dmac_t *p = opaque;
	dmac_handle_signal(p, &p->lbreq, 0, request, level);
}

static void dmac_handle_signal_sel1_sreq(void *opaque, int request, int level) {
	pmb887x_dmac_t *p = opaque;
	dmac_handle_signal(p, &p->sreq, 1, request, level);
}

static void dmac_handle_signal_sel1_breq(void *opaque, int request, int level) {
	pmb887x_dmac_t *p = opaque;
	dmac_handle_signal(p, &p->breq, 1, request, level);
}

static void dmac_handle_signal_sel1_lsreq(void *opaque, int request, int level) {
	pmb887x_dmac_t *p = opaque;
	dmac_handle_signal(p, &p->lsreq, 1, request, level);
}

static void dmac_handle_signal_sel1_lbreq(void *opaque, int request, int level) {
	pmb887x_dmac_t *p = opaque;
	dmac_handle_signal(p, &p->lbreq, 1, request, level);
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
	DeviceState *dev = DEVICE(obj);
	pmb887x_dmac_t *p = PMB887X_DMAC(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-dmac", DMAC_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq_err);
	
	for (int i = 0; i < DMAC_CHANNELS; i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq_tc[i]);

	qdev_init_gpio_out_named(dev, p->TC[0], "SEL0_TC", DMAC_REQUESTS);
	qdev_init_gpio_out_named(dev, p->TC[1], "SEL1_TC", DMAC_REQUESTS);

	qdev_init_gpio_out_named(dev, p->CLR[0], "SEL0_CLR", DMAC_REQUESTS);
	qdev_init_gpio_out_named(dev, p->CLR[1], "SEL1_CLR", DMAC_REQUESTS);

	qdev_init_gpio_in_named(dev, dmac_handle_signal_sel0_sreq, "SEL0_SREQ", DMAC_REQUESTS);
	qdev_init_gpio_in_named(dev, dmac_handle_signal_sel0_breq, "SEL0_BREQ", DMAC_REQUESTS);
	qdev_init_gpio_in_named(dev, dmac_handle_signal_sel0_lsreq, "SEL0_LSREQ", DMAC_REQUESTS);
	qdev_init_gpio_in_named(dev, dmac_handle_signal_sel0_lbreq, "SEL0_LBREQ", DMAC_REQUESTS);

	qdev_init_gpio_in_named(dev, dmac_handle_signal_sel1_sreq, "SEL1_SREQ", DMAC_REQUESTS);
	qdev_init_gpio_in_named(dev, dmac_handle_signal_sel1_breq, "SEL1_BREQ", DMAC_REQUESTS);
	qdev_init_gpio_in_named(dev, dmac_handle_signal_sel1_lsreq, "SEL1_LSREQ", DMAC_REQUESTS);
	qdev_init_gpio_in_named(dev, dmac_handle_signal_sel1_lbreq, "SEL1_LBREQ", DMAC_REQUESTS);
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

	p->timer = timer_new_ns(QEMU_CLOCK_REALTIME, dmac_timer_reset, p);
}

static void dmac_reset(DeviceState *dev) {
	pmb887x_dmac_t *p = PMB887X_DMAC(dev);

	timer_del(p->timer);

	pmb887x_srb_reset(&p->srb_err);
	pmb887x_srb_reset(&p->srb_tc);

	for (int i = 0; i < DMAC_CHANNELS; i++)
		p->ch[i] = (pmb887x_dmac_ch_t) { .id = i };

	p->dmac_pending = false;
	p->is_busy = false;
	p->config = 0;
	p->sync = 0;
	memset(p->sel, 0, sizeof(p->sel));
	memset(&p->sreq, 0, sizeof(p->sreq));
	memset(&p->breq, 0, sizeof(p->breq));
	memset(&p->lbreq, 0, sizeof(p->lbreq));
	memset(&p->lsreq, 0, sizeof(p->lsreq));

	for (size_t sel = 0; sel < DMAC_MULTIPLEXOR; sel++) {
		for (size_t request = 0; request < DMAC_REQUESTS; request++) {
			qemu_set_irq(p->CLR[sel][request], 0);
			qemu_set_irq(p->TC[sel][request], 0);
		}
	}
}

static const Property dmac_properties[] = {
	DEFINE_PROP_LINK("downstream", pmb887x_dmac_t, downstream, TYPE_MEMORY_REGION, MemoryRegion *),
};

static void dmac_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, dmac_properties);
	device_class_set_legacy_reset(dc, dmac_reset);
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
