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

	int SREQ[DMAC_MULTIPLEXOR][DMAC_REQUESTS];
	int BREQ[DMAC_MULTIPLEXOR][DMAC_REQUESTS];
	int LBREQ[DMAC_MULTIPLEXOR][DMAC_REQUESTS];
	int LSREQ[DMAC_MULTIPLEXOR][DMAC_REQUESTS];
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

static void dmac_schedule(pmb887x_dmac_t *p) {
	if (!p->dmac_pending) {
		p->dmac_pending = true;
		timer_mod(p->timer, 0);
	}
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
		address_space_read(&p->downstream_as, lli_addr, MEMTXATTRS_UNSPECIFIED, lli, sizeof(lli));

		ch->src_addr = lli[0];
		ch->dst_addr = lli[1];
		ch->lli = lli[2];
		ch->control = lli[3];

		DPRINTF("CH%d LLI=%08X [%08X, %08X, %08X, %08X]\n", ch->id, lli_addr, lli[0], lli[1], lli[2], lli[3]);
	} else {
		DPRINTF("CH%d: transfer done\n", ch->id);
		ch->config &= ~DMAC_CH_CONFIG_ENABLE;
		pmb887x_srb_set_isr(&p->srb_tc, (1 << ch->id));
	}

	bool is_dst_fc = (
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER_PER ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_PER2PER_DST
	);

	bool is_src_fc = (
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_PER2MEM ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_PER2MEM_PER ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_PER2PER_SRC
	);

	if (is_dst_fc)
		qemu_set_irq(p->TC[dst_sel][dst_periph], 1);

	if (is_src_fc)
		qemu_set_irq(p->TC[src_sel][src_periph], 1);
}

static void dmac_transfer_memory(pmb887x_dmac_t *p, pmb887x_dmac_ch_t *ch, uint32_t burst_size) {
	uint8_t buffer[16 * 1024]; // 12bit TransferSize x DWORD
	uint32_t src_width = dmac_get_width((ch->control & DMAC_CH_CONTROL_S_WIDTH) >> DMAC_CH_CONTROL_S_WIDTH_SHIFT);
	uint32_t dst_width = dmac_get_width((ch->control & DMAC_CH_CONTROL_D_WIDTH) >> DMAC_CH_CONTROL_D_WIDTH_SHIFT);
	uint32_t flow_ctrl = (ch->config & DMAC_CH_CONFIG_FLOW_CTRL);
	uint32_t tx_size = (ch->control & DMAC_CH_CONTROL_TRANSFER_SIZE) >> DMAC_CH_CONTROL_TRANSFER_SIZE_SHIFT;

	bool dmac_is_fc = (
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2MEM ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_PER2MEM
	);

	bool simple_memcpy = (
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2MEM &&
		dst_width == src_width &&
		(ch->control & DMAC_CH_CONTROL_SI) != 0 &&
		(ch->control & DMAC_CH_CONTROL_DI) != 0
	);

	bool src_is_memory = (
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2MEM ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER ||
		flow_ctrl == DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER_PER
	);

	DPRINTF("CH%d: %08X [%dx%d] -> %08X [%dx%d]\n", ch->id, ch->src_addr, src_width, burst_size, ch->dst_addr, dst_width, burst_size);

	if (simple_memcpy) {
		address_space_read(&p->downstream_as, ch->src_addr, MEMTXATTRS_UNSPECIFIED, buffer, src_width * burst_size);
		address_space_write(&p->downstream_as, ch->dst_addr, MEMTXATTRS_UNSPECIFIED, buffer, dst_width * burst_size);
		ch->src_addr += src_width * burst_size;
		ch->dst_addr += dst_width * burst_size;
	} else if (dst_width == src_width) {
		while (burst_size > 0) {
			address_space_read(&p->downstream_as, ch->src_addr, MEMTXATTRS_UNSPECIFIED, buffer, src_width);
			address_space_write(&p->downstream_as, ch->dst_addr, MEMTXATTRS_UNSPECIFIED, buffer, dst_width);

			if ((ch->control & DMAC_CH_CONTROL_SI))
				ch->src_addr += src_width;

			if ((ch->control & DMAC_CH_CONTROL_DI))
				ch->dst_addr += dst_width;

			burst_size--;
		}
	} else if (dst_width > src_width) {
		uint32_t transfered = 0;
		uint32_t buffer_size = 0;
		while (transfered < burst_size) {
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
			transfered++;
		}
	} else {
		uint32_t transfered = 0;
		uint32_t src_burst_size = src_is_memory && (ch->control & DMAC_CH_CONTROL_SI) ? burst_size : 1;
		uint32_t src_burst_size_bytes = src_burst_size * src_width;
		while (transfered < burst_size) {
			address_space_read(&p->downstream_as, ch->src_addr, MEMTXATTRS_UNSPECIFIED, buffer, src_burst_size_bytes);

			if ((ch->control & DMAC_CH_CONTROL_SI))
				ch->src_addr += src_burst_size_bytes;

			for (uint32_t j = 0; j < src_burst_size_bytes; j += dst_width) {
				address_space_write(&p->downstream_as, ch->dst_addr, MEMTXATTRS_UNSPECIFIED, buffer + j, dst_width);

				if ((ch->control & DMAC_CH_CONTROL_DI))
					ch->dst_addr += dst_width;
			}
			transfered += src_burst_size;
		}
	}

	if (tx_size > 0) {
		if (!dmac_is_fc)
			hw_error("TransferSize must be zero when peripheral is flow controller!");

		tx_size -= burst_size;
		ch->control &= ~DMAC_CH_CONTROL_TRANSFER_SIZE;
		ch->control |= tx_size << DMAC_CH_CONTROL_TRANSFER_SIZE_SHIFT;

		if (tx_size == 0)
			dmac_transfer_finish(p, ch);
	}
}

static void dmac_channel_run(pmb887x_dmac_t *p, pmb887x_dmac_ch_t *ch) {
	if (!(ch->config & DMAC_CH_CONFIG_ENABLE) || !(p->config & DMAC_CONFIG_ENABLE))
		return;

	uint32_t src_burst_size = dmac_get_burst_size((ch->control & DMAC_CH_CONTROL_SB_SIZE) >> DMAC_CH_CONTROL_SB_SIZE_SHIFT);
	uint32_t dst_burst_size = dmac_get_burst_size((ch->control & DMAC_CH_CONTROL_DB_SIZE) >> DMAC_CH_CONTROL_DB_SIZE_SHIFT);
	uint8_t src_periph = (ch->config & DMAC_CH_CONFIG_SRC_PERIPH) >> DMAC_CH_CONFIG_SRC_PERIPH_SHIFT;
	uint8_t dst_periph = (ch->config & DMAC_CH_CONFIG_DST_PERIPH) >> DMAC_CH_CONFIG_DST_PERIPH_SHIFT;
	uint8_t src_sel = p->sel[src_periph];
	uint8_t dst_sel = p->sel[dst_periph];
	uint32_t tx_size = (ch->control & DMAC_CH_CONTROL_TRANSFER_SIZE) >> DMAC_CH_CONTROL_TRANSFER_SIZE_SHIFT;

	switch ((ch->config & DMAC_CH_CONFIG_FLOW_CTRL)) {
		case DMAC_CH_CONFIG_FLOW_CTRL_MEM2MEM: {
			while ((ch->config & DMAC_CH_CONFIG_ENABLE) != 0)
				dmac_transfer_memory(p, ch, tx_size);
			break;
		}

		case DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER: {
			if (p->BREQ[dst_sel][dst_periph]) {
				dmac_transfer_memory(p, ch, MIN(tx_size, src_burst_size));
				qemu_set_irq(p->CLR[dst_sel][dst_periph], 1);
			}
			break;
		}

		case DMAC_CH_CONFIG_FLOW_CTRL_MEM2PER_PER: {
			if (p->BREQ[dst_sel][dst_periph] || p->LBREQ[dst_sel][dst_periph]) {
				dmac_transfer_memory(p, ch, dst_burst_size);
				if (p->LBREQ[dst_sel][dst_periph])
					dmac_transfer_finish(p, ch);
				qemu_set_irq(p->CLR[dst_sel][dst_periph], 1);
			} else if (p->SREQ[dst_sel][dst_periph] || p->LSREQ[dst_sel][dst_periph]) {
				dmac_transfer_memory(p, ch, 1);
				if (p->LSREQ[dst_sel][dst_periph])
					dmac_transfer_finish(p, ch);
				qemu_set_irq(p->CLR[dst_sel][dst_periph], 1);
			}
			break;
		}

		case DMAC_CH_CONFIG_FLOW_CTRL_PER2MEM:
			if (p->BREQ[src_sel][src_periph]) {
				dmac_transfer_memory(p, ch, MIN(tx_size, src_burst_size));
				qemu_set_irq(p->CLR[src_sel][src_periph], 1);
			} else if (p->SREQ[src_sel][src_periph]) {
				dmac_transfer_memory(p, ch, MIN(tx_size, 1));
				qemu_set_irq(p->CLR[src_sel][src_periph], 1);
			}
			break;

		case DMAC_CH_CONFIG_FLOW_CTRL_PER2MEM_PER:
			if (p->BREQ[src_sel][src_periph] || p->LBREQ[src_sel][src_periph]) {
				dmac_transfer_memory(p, ch, src_burst_size);
				if (p->LBREQ[src_sel][src_periph])
					dmac_transfer_finish(p, ch);
				qemu_set_irq(p->CLR[src_sel][src_periph], 1);
			} else if (p->SREQ[src_sel][src_periph] || p->LSREQ[src_sel][src_periph]) {
				dmac_transfer_memory(p, ch, 1);
				if (p->LSREQ[src_sel][src_periph])
					dmac_transfer_finish(p, ch);
				qemu_set_irq(p->CLR[src_sel][src_periph], 1);
			}
			break;

		default:
			hw_error("pmb887x-dmac: unsupported flow type %08X", (ch->config & DMAC_CH_CONFIG_FLOW_CTRL));
	}
}

static void dmac_update(pmb887x_dmac_t *p) {
	uint32_t err_mask = 0;
	uint32_t tc_mask = 0;
	
	if ((p->config & DMAC_CONFIG_M1) != DMAC_CONFIG_M1_LE)
		hw_error("DMAC_CONFIG_M1: supported only LE mode.");

	if ((p->config & DMAC_CONFIG_M2) != DMAC_CONFIG_M2_LE)
		hw_error("DMAC_CONFIG_M2: supported only LE mode.");

	for (int i = 0; i < DMAC_CHANNELS; i++) {
		pmb887x_dmac_ch_t *ch = &p->ch[i];
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
	}
	
	pmb887x_srb_set_imsc(&p->srb_tc, tc_mask);
	pmb887x_srb_set_imsc(&p->srb_err, err_mask);

	dmac_schedule(p);
}

bool pmb887x_dmac_is_busy(pmb887x_dmac_t *p) {
	return p->is_busy;
}

void pmb887x_dmac_request(pmb887x_dmac_t *p, uint32_t per_id, uint32_t size) {
	// DPRINTF("pmb887x_dmac_request(%d, %d)\n", per_id, size);
}

static const char *dmac_signal_name(pmb887x_dmac_t *p, const int signal[]) {
	for (int i = 0; i < ARRAY_SIZE(p->sel); i++) {
		if (signal == p->SREQ[i]) {
			return "SREQ";
		} else if (signal == p->LSREQ[i]) {
			return "LSREQ";
		} else if (signal == p->BREQ[i]) {
			return "BREQ";
		} else if (signal == p->LBREQ[i]) {
			return "LBREQ";
		}
	}
	return "UNK";
}

static void dmac_handle_signal(pmb887x_dmac_t *p, int signal[], int request, int level) {
	if (signal[request] == level)
		return;

	DPRINTF("%s request=%d, level=%d\n", dmac_signal_name(p, signal), request, level);
	signal[request] = level;

	if (level == 0) {
		uint32_t sel = p->sel[request];
		uint32_t signals = 0;
		signals |= p->SREQ[sel][request];
		signals |= p->LSREQ[sel][request];
		signals |= p->BREQ[sel][request];
		signals |= p->LBREQ[sel][request];
		if (!signals) {
			qemu_set_irq(p->CLR[sel][request], 0);
			qemu_set_irq(p->TC[sel][request], 0);
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
		case DMAC_CH_CONFIG7:
			value = p->ch[(haddr - DMAC_CH_CONFIG0) / 0x20].config;
			break;

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
			for (int i = 0; i < DMAC_REQUESTS; i++)
				value |= p->BREQ[p->sel[i]][i] << i;
			break;

		case DMAC_SOFT_SREQ:
			for (int i = 0; i < DMAC_REQUESTS; i++)
				value |= p->SREQ[p->sel[i]][i] << i;
			break;

		case DMAC_SOFT_LBREQ:
			for (int i = 0; i < DMAC_REQUESTS; i++)
				value |= p->LBREQ[p->sel[i]][i] << i;
			break;

		case DMAC_SOFT_LSREQ:
			for (int i = 0; i < DMAC_REQUESTS; i++)
				value |= p->LBREQ[p->sel[i]][i] << i;
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
		case DMAC_CH_CONFIG7:
			p->ch[(haddr - DMAC_CH_CONFIG0) / 0x20].config = value;
			break;
		
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
			for (int i = 0; i < DMAC_REQUESTS; i++) {
				if ((value & (1 << i)))
					dmac_handle_signal(p, p->BREQ[p->sel[i]], i, 1);
			}
			break;

		case DMAC_SOFT_SREQ:
			for (int i = 0; i < DMAC_REQUESTS; i++) {
				if ((value & (1 << i)))
					dmac_handle_signal(p, p->SREQ[p->sel[i]], i, 1);
			}
			break;

		case DMAC_SOFT_LBREQ:
			for (int i = 0; i < DMAC_REQUESTS; i++) {
				if ((value & (1 << i)))
					dmac_handle_signal(p, p->LBREQ[p->sel[i]], i, 1);
			}
			break;

		case DMAC_SOFT_LSREQ:
			for (int i = 0; i < DMAC_REQUESTS; i++) {
				if ((value & (1 << i)))
					dmac_handle_signal(p, p->LSREQ[p->sel[i]], i, 1);
			}
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
	dmac_handle_signal(p, p->SREQ[0], request, level);
}

static void dmac_handle_signal_sel0_breq(void *opaque, int request, int level) {
	pmb887x_dmac_t *p = opaque;
	dmac_handle_signal(p, p->BREQ[0], request, level);
}

static void dmac_handle_signal_sel0_lsreq(void *opaque, int request, int level) {
	pmb887x_dmac_t *p = opaque;
	dmac_handle_signal(p, p->LSREQ[0], request, level);
}

static void dmac_handle_signal_sel0_lbreq(void *opaque, int request, int level) {
	pmb887x_dmac_t *p = opaque;
	dmac_handle_signal(p, p->LBREQ[0], request, level);
}

static void dmac_handle_signal_sel1_sreq(void *opaque, int request, int level) {
	pmb887x_dmac_t *p = opaque;
	dmac_handle_signal(p, p->SREQ[1], request, level);
}

static void dmac_handle_signal_sel1_breq(void *opaque, int request, int level) {
	pmb887x_dmac_t *p = opaque;
	dmac_handle_signal(p, p->BREQ[1], request, level);
}

static void dmac_handle_signal_sel1_lsreq(void *opaque, int request, int level) {
	pmb887x_dmac_t *p = opaque;
	dmac_handle_signal(p, p->LSREQ[1], request, level);
}

static void dmac_handle_signal_sel1_lbreq(void *opaque, int request, int level) {
	pmb887x_dmac_t *p = opaque;
	dmac_handle_signal(p, p->LBREQ[1], request, level);
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

static const Property dmac_properties[] = {
	DEFINE_PROP_LINK("downstream", pmb887x_dmac_t, downstream, TYPE_MEMORY_REGION, MemoryRegion *),
};

static void dmac_class_init(ObjectClass *klass, const void *data) {
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
