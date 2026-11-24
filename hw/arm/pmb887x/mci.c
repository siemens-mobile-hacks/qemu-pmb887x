#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/fifo32.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/sd/sd.h"

#include "hw/arm/pmb887x/gen/cpu_meta.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/mod.h"

#define TYPE_PMB887X_MCI	"pmb887x-mci"
#define PMB887X_MCI(obj)	OBJECT_CHECK(pmb887x_mci_t, (obj), TYPE_PMB887X_MCI)

#define MCI_FIFO_WORDS				16
#define MCI_FIFO_HALF_WORDS			8
#define MCI_COMMAND_BITS			48
#define MCI_SHORT_RESPONSE_BITS		48
#define MCI_LONG_RESPONSE_BITS		136
#define MCI_COMMAND_TIMEOUT_CYCLES	64
#define MCI_NCR_CYCLES				2
#define MCI_NAC_CYCLES				2
#define MCI_NWR_CYCLES				2
#define MCI_BLOCK_CRC_END_BITS		17
#define MCI_CRC_STATUS_CYCLES		8
#define MCI_RESPONSE_RESERVED_INDEX	0x3F
#define MCI_PCELL_ID				0xB105F00D
#define MCI_STATIC_FLAGS			MAKE_64BIT_MASK(0, 11)
#define MCI_FIFO_WINDOW				0x3000
#define MCI_FIFO_WINDOW_SIZE		0x4000
#define MCI_MMIO_SIZE				(MCI_FIFO_WINDOW + MCI_FIFO_WINDOW_SIZE)

typedef struct pmb887x_mci_t pmb887x_mci_t;

enum mci_cpsm_state {
	MCI_CPSM_IDLE,
	MCI_CPSM_PEND,
	MCI_CPSM_SEND,
	MCI_CPSM_WAIT,
	MCI_CPSM_RECEIVE,
};

enum mci_dpsm_state {
	MCI_DPSM_IDLE,
	MCI_DPSM_WAIT_R,
	MCI_DPSM_RECEIVE,
	MCI_DPSM_WAIT_S,
	MCI_DPSM_SEND,
	MCI_DPSM_BUSY,
};

enum mci_dma_request {
	MCI_DMA_SREQ,
	MCI_DMA_LSREQ,
	MCI_DMA_BREQ,
	MCI_DMA_LBREQ,
	MCI_DMA_REQUESTS,
	MCI_DMA_NONE = MCI_DMA_REQUESTS,
};

struct pmb887x_mci_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	DebugChannel dbg;
	Clock *mclk;
	SDBus card_bus;
	qemu_irq irq[2];
	qemu_irq dma_tx[MCI_DMA_REQUESTS];
	qemu_irq dma_rx[MCI_DMA_REQUESTS];
	uint32_t revision;
	uint32_t peripheral_id;
	bool quirk_sync_on_datactrl;

	uint32_t power;
	uint32_t clock;
	uint32_t argument;
	uint32_t command;
	uint32_t respcmd;
	uint32_t response[4];
	uint32_t datatimer;
	uint32_t datalength;
	uint32_t datactrl;
	uint32_t status;
	uint32_t mask[2];
	uint32_t select;
	uint32_t tcr;
	uint32_t itip;
	uint32_t itop;
	uint32_t card_clock_hz;

	enum mci_cpsm_state cpsm;
	QEMUTimer *cmd_timer;
	bool is_cmd_timed;
	int64_t cmd_start_ns;
	uint64_t cmd_cycles;
	uint8_t card_response[16];
	size_t card_response_size;

	enum mci_dpsm_state dpsm;
	QEMUTimer *data_timer;
	bool is_data_timed;
	int64_t data_start_ns;
	uint64_t data_cycles;
	bool is_data_start_pending;
	uint32_t data_count;
	uint32_t fifo_count;
	uint32_t tx_unwritten_words;
	uint32_t block_remaining;
	Fifo32 fifo;

	enum mci_dma_request dma_request;
	bool dmac_tx_clr;
	bool dmac_rx_clr;
};

static bool mci_is_receiving(pmb887x_mci_t *p) {
	return (p->datactrl & MCI_DATACTRL_DIRECTION) == MCI_DATACTRL_DIRECTION_READ;
}

static bool mci_is_stream(pmb887x_mci_t *p) {
	return (p->datactrl & MCI_DATACTRL_MODE) == MCI_DATACTRL_MODE_STREAM;
}

static int64_t mci_cycles_to_ns(pmb887x_mci_t *p, uint64_t cycles) {
	return muldiv64(cycles, NANOSECONDS_PER_SECOND, p->card_clock_hz);
}

static uint64_t mci_data_cycles(pmb887x_mci_t *p, uint32_t bytes) {
	uint32_t bus_width = (p->clock & MCI_CLOCK_WIDEBUS) != 0 ? 4 : 1;
	return bytes * 8 / bus_width;
}

static uint32_t mci_get_status(pmb887x_mci_t *p) {
	uint32_t status = p->status;
	if (p->cpsm != MCI_CPSM_IDLE)
		status |= MCI_STATUS_CMDACTIVE;
	if (p->dpsm == MCI_DPSM_IDLE)
		return status;

	uint32_t used = fifo32_num_used(&p->fifo);
	if (mci_is_receiving(p)) {
		status |= MCI_STATUS_RXACTIVE;
		status |= used >= MCI_FIFO_HALF_WORDS ? MCI_STATUS_RXFIFOHALFFULL : 0;
		status |= used == MCI_FIFO_WORDS ? MCI_STATUS_RXFIFOFULL : 0;
		status |= used == 0 ? MCI_STATUS_RXFIFOEMPTY : MCI_STATUS_RXDATAAVLBL;
	} else {
		status |= MCI_STATUS_TXACTIVE;
		status |= MCI_FIFO_WORDS - used >= MCI_FIFO_HALF_WORDS ? MCI_STATUS_TXFIFOHALFEMPTY : 0;
		status |= used == MCI_FIFO_WORDS ? MCI_STATUS_TXFIFOFULL : 0;
		status |= used == 0 ? MCI_STATUS_TXFIFOEMPTY : MCI_STATUS_TXDATAAVLBL;
	}
	return status;
}

static enum mci_dma_request mci_get_rx_dma_request(pmb887x_mci_t *p) {
	uint32_t used = fifo32_num_used(&p->fifo);
	bool is_counter_zero = p->data_count == 0;
	if (used > MCI_FIFO_HALF_WORDS || (used == MCI_FIFO_HALF_WORDS && !is_counter_zero))
		return MCI_DMA_BREQ;
	if (!is_counter_zero || used == 0)
		return MCI_DMA_NONE;
	if (used == MCI_FIFO_HALF_WORDS)
		return MCI_DMA_LBREQ;
	return used > 1 ? MCI_DMA_SREQ : MCI_DMA_LSREQ;
}

static enum mci_dma_request mci_get_tx_dma_request(pmb887x_mci_t *p) {
	uint32_t free_words = fifo32_num_free(&p->fifo);
	if (p->tx_unwritten_words >= MCI_FIFO_HALF_WORDS) {
		if (free_words < MCI_FIFO_HALF_WORDS)
			return MCI_DMA_NONE;
		return p->tx_unwritten_words > MCI_FIFO_HALF_WORDS ? MCI_DMA_BREQ : MCI_DMA_LBREQ;
	}
	if (p->tx_unwritten_words == 0 || free_words == 0)
		return MCI_DMA_NONE;
	return p->tx_unwritten_words > 1 ? MCI_DMA_SREQ : MCI_DMA_LSREQ;
}

/* The phone routes the single PL180 request set to both DMAC request lines of the module in either direction. */
static void mci_set_dma_request(pmb887x_mci_t *p, enum mci_dma_request request) {
	enum mci_dma_request old_request = p->dma_request;
	if (request == old_request)
		return;

	p->dma_request = request;
	if (old_request != MCI_DMA_NONE) {
		qemu_irq_lower(p->dma_tx[old_request]);
		qemu_irq_lower(p->dma_rx[old_request]);
	}
	if (request != MCI_DMA_NONE && p->dma_request == request) {
		qemu_irq_raise(p->dma_tx[request]);
		qemu_irq_raise(p->dma_rx[request]);
	}
}

static void mci_update_dma(pmb887x_mci_t *p) {
	bool is_enabled = p->dpsm != MCI_DPSM_IDLE && (p->datactrl & MCI_DATACTRL_DMAENABLE) != 0;
	if (!is_enabled || p->dmac_tx_clr || p->dmac_rx_clr) {
		mci_set_dma_request(p, MCI_DMA_NONE);
		return;
	}
	if (p->dma_request != MCI_DMA_NONE)
		return;
	mci_set_dma_request(p, mci_is_receiving(p) ? mci_get_rx_dma_request(p) : mci_get_tx_dma_request(p));
}

static void mci_update(pmb887x_mci_t *p) {
	uint32_t status = mci_get_status(p);
	qemu_set_irq(p->irq[0], (status & p->mask[0]) != 0);
	qemu_set_irq(p->irq[1], (status & p->mask[1]) != 0);
	mci_update_dma(p);
}

static void mci_cmd_arm(pmb887x_mci_t *p) {
	if (p->card_clock_hz == 0) {
		timer_del(p->cmd_timer);
		return;
	}
	timer_mod_ns(p->cmd_timer, p->cmd_start_ns + mci_cycles_to_ns(p, p->cmd_cycles));
}

static void mci_cmd_schedule(pmb887x_mci_t *p, enum mci_cpsm_state state, int64_t start_ns, uint64_t cycles) {
	p->cpsm = state;
	p->is_cmd_timed = true;
	p->cmd_start_ns = start_ns;
	p->cmd_cycles = cycles;
	mci_cmd_arm(p);
}

static void mci_data_arm(pmb887x_mci_t *p) {
	if (p->card_clock_hz == 0) {
		timer_del(p->data_timer);
		return;
	}
	timer_mod_ns(p->data_timer, p->data_start_ns + mci_cycles_to_ns(p, p->data_cycles));
}

static void mci_data_schedule(pmb887x_mci_t *p, enum mci_dpsm_state state, int64_t start_ns, uint64_t cycles) {
	p->dpsm = state;
	p->is_data_timed = true;
	p->data_start_ns = start_ns;
	p->data_cycles = cycles;
	mci_data_arm(p);
}

static void mci_data_cancel(pmb887x_mci_t *p) {
	timer_del(p->data_timer);
	p->is_data_timed = false;
}

static void mci_data_stop(pmb887x_mci_t *p) {
	mci_data_cancel(p);
	p->dpsm = MCI_DPSM_IDLE;
	p->is_data_start_pending = false;
	p->fifo_count = 0;
	p->block_remaining = 0;
	fifo32_reset(&p->fifo);
}

static uint32_t mci_block_bytes(pmb887x_mci_t *p) {
	if (mci_is_stream(p))
		return p->data_count;
	uint32_t block_size = 1U << ((p->datactrl & MCI_DATACTRL_BLOCKSIZE) >> MCI_DATACTRL_BLOCKSIZE_SHIFT);
	return MIN(block_size, p->data_count);
}

static void mci_cmd_start(pmb887x_mci_t *p, int64_t now_ns) {
	mci_cmd_schedule(p, MCI_CPSM_SEND, now_ns, MCI_COMMAND_BITS);
}

static void mci_receive_wait(pmb887x_mci_t *p, int64_t now_ns) {
	if (p->data_count == 0) {
		p->dpsm = MCI_DPSM_WAIT_R;
		mci_data_cancel(p);
		if (fifo32_is_empty(&p->fifo))
			mci_data_stop(p);
		return;
	}

	p->is_data_start_pending = p->cpsm == MCI_CPSM_IDLE && sdbus_data_ready(&p->card_bus);
	uint64_t cycles = p->is_data_start_pending ? MCI_NAC_CYCLES : p->datatimer;
	mci_data_schedule(p, MCI_DPSM_WAIT_R, now_ns, cycles);
}

static void mci_send_wait(pmb887x_mci_t *p, int64_t now_ns) {
	if (p->data_count == 0) {
		mci_data_stop(p);
		return;
	}

	p->dpsm = MCI_DPSM_WAIT_S;
	p->is_data_start_pending = !fifo32_is_empty(&p->fifo);
	if (p->is_data_start_pending)
		mci_data_schedule(p, MCI_DPSM_WAIT_S, now_ns, MCI_NWR_CYCLES);
	else
		mci_data_cancel(p);
}

static void mci_handle_card_response_end(pmb887x_mci_t *p, int64_t now_ns) {
	bool is_waiting_card = p->dpsm == MCI_DPSM_WAIT_R && p->data_count != 0 && !p->is_data_start_pending;
	if (is_waiting_card && sdbus_data_ready(&p->card_bus)) {
		p->is_data_start_pending = true;
		mci_data_schedule(p, MCI_DPSM_WAIT_R, now_ns, MCI_NAC_CYCLES);
	}
}

static void mci_store_response(pmb887x_mci_t *p) {
	bool is_long = (p->command & MCI_COMMAND_LONGRSP) != 0;
	size_t expected_size = is_long ? 16 : 4;
	bool has_crc = sdbus_get_response_has_crc(&p->card_bus);

	p->response[0] = ldl_be_p(&p->card_response[0]);
	if (is_long) {
		p->response[1] = ldl_be_p(&p->card_response[4]);
		p->response[2] = ldl_be_p(&p->card_response[8]);
		p->response[3] = ldl_be_p(&p->card_response[12]) & ~1U;
	}

	if (is_long || !has_crc)
		p->respcmd = MCI_RESPONSE_RESERVED_INDEX;
	else
		p->respcmd = p->command & MCI_COMMAND_CMDINDEX;

	if (p->card_response_size == expected_size && has_crc)
		p->status |= MCI_STATUS_CMDRESPEND;
	else
		p->status |= MCI_STATUS_CMDCRCFAIL;
}

static void mci_cmd_send_end(pmb887x_mci_t *p, int64_t now_ns) {
	SDRequest request = {
		.cmd = p->command & MCI_COMMAND_CMDINDEX,
		.arg = p->argument,
	};
	p->card_response_size = sdbus_do_command(&p->card_bus, &request, p->card_response, sizeof(p->card_response));

	if ((p->command & MCI_COMMAND_RESPONSE) == 0) {
		p->cpsm = MCI_CPSM_IDLE;
		p->status |= MCI_STATUS_CMDSENT;
		mci_handle_card_response_end(p, now_ns);
		return;
	}

	if ((p->command & MCI_COMMAND_INTERRUPT) != 0) {
		qemu_log_mask(LOG_UNIMP, "%s: card interrupt mode is not supported\n", __func__);
		p->cpsm = MCI_CPSM_WAIT;
		return;
	}

	if (p->card_response_size == 0) {
		mci_cmd_schedule(p, MCI_CPSM_WAIT, now_ns, MCI_COMMAND_TIMEOUT_CYCLES);
		return;
	}

	bool is_long = (p->command & MCI_COMMAND_LONGRSP) != 0;
	uint64_t response_bits = is_long ? MCI_LONG_RESPONSE_BITS : MCI_SHORT_RESPONSE_BITS;
	mci_cmd_schedule(p, MCI_CPSM_RECEIVE, now_ns, MCI_NCR_CYCLES + response_bits);
}

static void mci_cmd_timer(void *opaque) {
	pmb887x_mci_t *p = opaque;
	int64_t event_ns = p->cmd_start_ns + mci_cycles_to_ns(p, p->cmd_cycles);
	p->is_cmd_timed = false;

	switch (p->cpsm) {
		case MCI_CPSM_SEND:
			mci_cmd_send_end(p, event_ns);
			break;
		case MCI_CPSM_WAIT:
			p->cpsm = MCI_CPSM_IDLE;
			p->status |= MCI_STATUS_CMDTIMEOUT;
			break;
		case MCI_CPSM_RECEIVE:
			p->cpsm = MCI_CPSM_IDLE;
			mci_store_response(p);
			mci_handle_card_response_end(p, event_ns);
			break;
		default:
			g_assert_not_reached();
	}
	mci_update(p);
}

static void mci_command_write(pmb887x_mci_t *p, uint32_t value) {
	p->command = value;
	timer_del(p->cmd_timer);
	p->is_cmd_timed = false;
	p->cpsm = MCI_CPSM_IDLE;
	if ((value & MCI_COMMAND_ENABLE) == 0)
		return;

	int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	if ((value & MCI_COMMAND_PENDING) != 0 && p->dpsm != MCI_DPSM_IDLE)
		p->cpsm = MCI_CPSM_PEND;
	else
		mci_cmd_start(p, now_ns);
}

static void mci_receive_start(pmb887x_mci_t *p, int64_t now_ns) {
	p->is_data_start_pending = false;
	p->block_remaining = mci_block_bytes(p);
	uint64_t cycles = 1 + mci_data_cycles(p, MIN(4, p->block_remaining));
	mci_data_schedule(p, MCI_DPSM_RECEIVE, now_ns, cycles);
}

static void mci_receive_discard_block(pmb887x_mci_t *p) {
	for (; p->block_remaining > 0; p->block_remaining--)
		sdbus_read_byte(&p->card_bus);
}

static void mci_receive_word(pmb887x_mci_t *p, int64_t now_ns) {
	uint32_t bytes = MIN(4, p->block_remaining);
	uint32_t word = 0;
	for (uint32_t i = 0; i < bytes; i++)
		word |= sdbus_read_byte(&p->card_bus) << (i * 8);
	p->block_remaining -= bytes;
	p->data_count -= bytes;

	if (fifo32_is_full(&p->fifo)) {
		p->status |= MCI_STATUS_RXOVERRUN;
		mci_receive_discard_block(p);
		mci_receive_wait(p, now_ns);
		return;
	}
	fifo32_push(&p->fifo, word);
	p->fifo_count--;

	if (p->block_remaining > 0) {
		mci_data_schedule(p, MCI_DPSM_RECEIVE, now_ns, mci_data_cycles(p, MIN(4, p->block_remaining)));
	} else if (mci_is_stream(p)) {
		p->status |= MCI_STATUS_DATAEND;
		mci_receive_wait(p, now_ns);
	} else {
		mci_data_schedule(p, MCI_DPSM_BUSY, now_ns, MCI_BLOCK_CRC_END_BITS);
	}
}

static void mci_receive_block_end(pmb887x_mci_t *p, int64_t now_ns) {
	p->status |= MCI_STATUS_DATABLOCKEND;
	if (p->data_count == 0)
		p->status |= MCI_STATUS_DATAEND;
	mci_receive_wait(p, now_ns);
}

static uint32_t mci_send_fifo_word(pmb887x_mci_t *p) {
	uint32_t word = fifo32_pop(&p->fifo);
	uint32_t bytes = MIN(4, p->block_remaining);
	for (uint32_t i = 0; i < bytes; i++)
		sdbus_write_byte(&p->card_bus, word >> (i * 8));
	p->fifo_count--;
	p->block_remaining -= bytes;
	p->data_count -= bytes;
	return bytes;
}

static void mci_send_word(pmb887x_mci_t *p, int64_t now_ns) {
	if (fifo32_is_empty(&p->fifo)) {
		p->status |= MCI_STATUS_TXUNDERRUN;
		mci_data_stop(p);
		return;
	}

	uint32_t bytes = mci_send_fifo_word(p);
	uint64_t cycles = mci_data_cycles(p, bytes);
	if (p->block_remaining > 0)
		mci_data_schedule(p, MCI_DPSM_SEND, now_ns, cycles);
	else if (mci_is_stream(p))
		mci_data_schedule(p, MCI_DPSM_BUSY, now_ns, cycles);
	else
		mci_data_schedule(p, MCI_DPSM_BUSY, now_ns, cycles + MCI_BLOCK_CRC_END_BITS + MCI_CRC_STATUS_CYCLES);
}

static void mci_send_block_end(pmb887x_mci_t *p, int64_t now_ns) {
	if (!mci_is_stream(p))
		p->status |= MCI_STATUS_DATABLOCKEND;
	if (p->data_count == 0)
		p->status |= MCI_STATUS_DATAEND;
	mci_send_wait(p, now_ns);
}

static void mci_data_event(pmb887x_mci_t *p, int64_t event_ns) {
	p->is_data_timed = false;
	switch (p->dpsm) {
		case MCI_DPSM_WAIT_R:
			if (p->is_data_start_pending) {
				mci_receive_start(p, event_ns);
			} else {
				p->status |= MCI_STATUS_DATATIMEOUT;
				mci_data_stop(p);
			}
			break;
		case MCI_DPSM_RECEIVE:
			mci_receive_word(p, event_ns);
			break;
		case MCI_DPSM_WAIT_S:
			p->is_data_start_pending = false;
			p->block_remaining = mci_block_bytes(p);
			mci_data_schedule(p, MCI_DPSM_SEND, event_ns, 1);
			break;
		case MCI_DPSM_SEND:
			mci_send_word(p, event_ns);
			break;
		case MCI_DPSM_BUSY:
			if (mci_is_receiving(p))
				mci_receive_block_end(p, event_ns);
			else
				mci_send_block_end(p, event_ns);
			break;
		default:
			g_assert_not_reached();
	}

	if (p->cpsm == MCI_CPSM_PEND && p->data_count == 0)
		mci_cmd_start(p, event_ns);
	mci_update(p);
}

static void mci_data_timer(void *opaque) {
	pmb887x_mci_t *p = opaque;
	int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

	do {
		mci_data_event(p, p->data_start_ns + mci_cycles_to_ns(p, p->data_cycles));
	} while (timer_pending(p->data_timer) && timer_expire_time_ns(p->data_timer) <= now_ns);
}

static bool mci_is_send_written(pmb887x_mci_t *p) {
	bool is_sending = p->dpsm == MCI_DPSM_WAIT_S || p->dpsm == MCI_DPSM_SEND || p->dpsm == MCI_DPSM_BUSY;
	return is_sending && !mci_is_receiving(p) && p->tx_unwritten_words == 0;
}

static void mci_send_fifo_to_card(pmb887x_mci_t *p) {
	while (p->data_count > 0 && !fifo32_is_empty(&p->fifo)) {
		if (p->block_remaining == 0)
			p->block_remaining = mci_block_bytes(p);
		mci_send_fifo_word(p);
	}
}

static void mci_datactrl_write(pmb887x_mci_t *p, uint32_t value) {
	/*
	 * Quirk: the phone's firmware clears DATACTRL as soon as its transmit DMA channel completes, without waiting for
	 * DataEnd. The phone aborts the transfer at that point too, but its slower CPU lets the FIFO drain to the card
	 * first. With the quirk, the words already in the FIFO reach the card before the abort.
	 */
	if (p->quirk_sync_on_datactrl && mci_is_send_written(p))
		mci_send_fifo_to_card(p);

	p->datactrl = value;
	mci_data_stop(p);
	mci_set_dma_request(p, MCI_DMA_NONE);
	if ((value & MCI_DATACTRL_ENABLE) == 0)
		return;

	int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	p->data_count = p->datalength;
	p->fifo_count = DIV_ROUND_UP(p->datalength, 4);
	p->tx_unwritten_words = p->fifo_count;
	if (mci_is_receiving(p))
		mci_receive_wait(p, now_ns);
	else
		mci_send_wait(p, now_ns);
}

static uint32_t mci_fifo_read(pmb887x_mci_t *p) {
	if (p->dpsm == MCI_DPSM_IDLE || !mci_is_receiving(p) || fifo32_is_empty(&p->fifo))
		return 0;

	uint32_t word = fifo32_pop(&p->fifo);
	if (p->dpsm == MCI_DPSM_WAIT_R && p->data_count == 0 && fifo32_is_empty(&p->fifo))
		mci_data_stop(p);
	return word;
}

static void mci_fifo_write(pmb887x_mci_t *p, uint32_t value) {
	if (p->dpsm == MCI_DPSM_IDLE || mci_is_receiving(p) || fifo32_is_full(&p->fifo))
		return;

	fifo32_push(&p->fifo, value);
	if (p->tx_unwritten_words > 0)
		p->tx_unwritten_words--;
	if (p->dpsm == MCI_DPSM_WAIT_S && !p->is_data_start_pending)
		mci_send_wait(p, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static uint32_t mci_get_card_clock_hz(pmb887x_mci_t *p) {
	bool is_powered = (p->power & MCI_POWER_CTRL) == MCI_POWER_CTRL_POWER_ON;
	if (!is_powered || (p->clock & MCI_CLOCK_ENABLE) == 0)
		return 0;

	uint32_t mclk_hz = clock_get_hz(p->mclk);
	if ((p->clock & MCI_CLOCK_BYPASS) != 0)
		return mclk_hz;
	uint32_t clkdiv = (p->clock & MCI_CLOCK_CLKDIV) >> MCI_CLOCK_CLKDIV_SHIFT;
	return mclk_hz / (2 * (clkdiv + 1));
}

static void mci_update_card_clock(pmb887x_mci_t *p) {
	uint32_t card_clock_hz = mci_get_card_clock_hz(p);
	if (card_clock_hz == p->card_clock_hz)
		return;

	int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	if (p->card_clock_hz == 0) {
		p->cmd_start_ns = now_ns;
		p->data_start_ns = now_ns;
	}
	p->card_clock_hz = card_clock_hz;

	if (p->is_cmd_timed)
		mci_cmd_arm(p);
	if (p->is_data_timed)
		mci_data_arm(p);
}

static void mci_mclk_update(void *opaque, ClockEvent _event) {
	pmb887x_mci_t *p = opaque;
	mci_update_card_clock(p);
}

static void mci_handle_dmac_tx_clr(void *opaque, int _id, int level) {
	pmb887x_mci_t *p = opaque;
	p->dmac_tx_clr = level != 0;
	mci_update_dma(p);
}

static void mci_handle_dmac_rx_clr(void *opaque, int _id, int level) {
	pmb887x_mci_t *p = opaque;
	p->dmac_rx_clr = level != 0;
	mci_update_dma(p);
}

static uint64_t mci_io_read(void *opaque, hwaddr haddr, unsigned _size) {
	pmb887x_mci_t *p = opaque;
	uint64_t value = 0;

	switch (haddr) {
		case MCI_POWER:
			value = p->power;
			break;
		case MCI_CLOCK:
			value = p->clock;
			break;
		case MCI_ARGUMENT:
			value = p->argument;
			break;
		case MCI_COMMAND:
			value = p->command;
			break;
		case MCI_RESPCMD:
			value = p->respcmd;
			break;
		case MCI_RESPONSE0:
		case MCI_RESPONSE1:
		case MCI_RESPONSE2:
		case MCI_RESPONSE3:
			value = p->response[(haddr - MCI_RESPONSE0) / 4];
			break;
		case MCI_DATATIMER:
			value = p->datatimer;
			break;
		case MCI_DATALENGTH:
			value = p->datalength;
			break;
		case MCI_DATACTRL:
			value = p->datactrl;
			break;
		case MCI_DATACNT:
			value = p->data_count;
			break;
		case MCI_STATUS:
			value = mci_get_status(p);
			break;
		case MCI_MASK0:
		case MCI_MASK1:
			value = p->mask[(haddr - MCI_MASK0) / 4];
			break;
		case MCI_SELECT:
			value = p->select;
			break;
		case MCI_FIFOCNT:
			value = p->fifo_count;
			break;
		case MCI_FIFO0 ... MCI_FIFO15:
		case MCI_FIFO_WINDOW ... MCI_MMIO_SIZE - 4:
			value = mci_fifo_read(p);
			mci_update(p);
			break;
		case MCI_TCR:
			value = p->tcr;
			break;
		case MCI_ITIP:
			value = p->itip;
			break;
		case MCI_ITOP:
			value = p->itop;
			break;
		case MCI_PERIPH_ID0 ... MCI_PERIPH_ID3:
			value = extract32(p->peripheral_id, (haddr - MCI_PERIPH_ID0) * 2, 8);
			break;
		case MCI_PCELL_ID0 ... MCI_PCELL_ID3:
			value = extract32(MCI_PCELL_ID, (haddr - MCI_PCELL_ID0) * 2, 8);
			break;
	}

	return value;
}

static void mci_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned _size) {
	pmb887x_mci_t *p = opaque;

	switch (haddr) {
		case MCI_POWER:
			p->power = value;
			mci_update_card_clock(p);
			break;
		case MCI_CLOCK:
			p->clock = value;
			mci_update_card_clock(p);
			break;
		case MCI_ARGUMENT:
			p->argument = value;
			break;
		case MCI_COMMAND:
			mci_command_write(p, value);
			break;
		case MCI_DATATIMER:
			p->datatimer = value;
			break;
		case MCI_DATALENGTH:
			p->datalength = value & MCI_DATALENGTH_DATALENGTH;
			break;
		case MCI_DATACTRL:
			mci_datactrl_write(p, value);
			break;
		case MCI_CLEAR:
			p->status &= ~(value & MCI_STATIC_FLAGS);
			break;
		case MCI_MASK0:
		case MCI_MASK1:
			p->mask[(haddr - MCI_MASK0) / 4] = value;
			break;
		case MCI_SELECT:
			p->select = value;
			break;
		case MCI_FIFO0 ... MCI_FIFO15:
		case MCI_FIFO_WINDOW ... MCI_MMIO_SIZE - 4:
			mci_fifo_write(p, value);
			break;
		case MCI_TCR:
			p->tcr = value;
			break;
		case MCI_ITIP:
			p->itip = value;
			break;
		case MCI_ITOP:
			p->itop = value;
			break;
		default:
			return;
	}
	mci_update(p);
}

static const MemoryRegionOps mci_io_ops = {
	.read = mci_io_read,
	.write = mci_io_write,
	.endianness = DEVICE_NATIVE_ENDIAN,
	.valid = {
		.min_access_size = 4,
		.max_access_size = 4,
	},
};

static void mci_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_mci_t *p = PMB887X_MCI(obj);

	memory_region_init_io(&p->mmio, obj, &mci_io_ops, p, "pmb887x-mci", MCI_MMIO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	for (size_t i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);

	p->mclk = qdev_init_clock_in(dev, "MCLK", mci_mclk_update, p, ClockUpdate);
	qbus_init(&p->card_bus, sizeof(p->card_bus), TYPE_SD_BUS, dev, "CARD");

	static const char *const DMA_REQUEST_NAMES[MCI_DMA_REQUESTS] = {
		[MCI_DMA_SREQ] = "SREQ",
		[MCI_DMA_LSREQ] = "LSREQ",
		[MCI_DMA_BREQ] = "BREQ",
		[MCI_DMA_LBREQ] = "LBREQ",
	};
	for (size_t i = 0; i < MCI_DMA_REQUESTS; i++) {
		g_autofree char *tx_name = g_strdup_printf("DMAC_TX_%s", DMA_REQUEST_NAMES[i]);
		g_autofree char *rx_name = g_strdup_printf("DMAC_RX_%s", DMA_REQUEST_NAMES[i]);
		qdev_init_gpio_out_named(dev, &p->dma_tx[i], tx_name, 1);
		qdev_init_gpio_out_named(dev, &p->dma_rx[i], rx_name, 1);
	}
	qdev_init_gpio_in_named(dev, mci_handle_dmac_tx_clr, "DMAC_TX_CLR", 1);
	qdev_init_gpio_in_named(dev, mci_handle_dmac_rx_clr, "DMAC_RX_CLR", 1);
}

static void mci_realize(DeviceState *dev, Error **_errp) {
	pmb887x_mci_t *p = PMB887X_MCI(dev);

	pmb887x_mod_debug_init(&p->dbg, dev, &p->mmio, &pmb887x_mci_regs);
	fifo32_create(&p->fifo, MCI_FIFO_WORDS);
	p->dma_request = MCI_DMA_NONE;
	p->cmd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mci_cmd_timer, p);
	p->data_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mci_data_timer, p);
}

static void mci_reset(DeviceState *dev) {
	pmb887x_mci_t *p = PMB887X_MCI(dev);

	timer_del(p->cmd_timer);
	p->is_cmd_timed = false;
	mci_data_stop(p);
	p->cpsm = MCI_CPSM_IDLE;
	p->power = 0;
	p->clock = 0;
	p->argument = 0;
	p->command = 0;
	p->respcmd = 0;
	memset(p->response, 0, sizeof(p->response));
	p->datatimer = 0;
	p->datalength = 0;
	p->datactrl = 0;
	p->status = 0;
	memset(p->mask, 0, sizeof(p->mask));
	p->select = 0;
	p->tcr = 0;
	p->itip = 0;
	p->itop = 0;
	p->card_clock_hz = 0;
	p->data_count = 0;
	p->tx_unwritten_words = 0;
	p->dmac_tx_clr = false;
	p->dmac_rx_clr = false;
	mci_update(p);
}

static const Property mci_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_mci_t, revision, 0),
	DEFINE_PROP_UINT32("peripheral-id", pmb887x_mci_t, peripheral_id, 0),
	DEFINE_PROP_BOOL("quirk_sync_on_datactrl", pmb887x_mci_t, quirk_sync_on_datactrl, false),
};

static void mci_class_init(ObjectClass *klass, const void *_data) {
	DeviceClass *dc = DEVICE_CLASS(klass);

	device_class_set_props(dc, mci_properties);
	device_class_set_legacy_reset(dc, mci_reset);
	dc->realize = mci_realize;
}

static const TypeInfo mci_info = {
	.name = TYPE_PMB887X_MCI,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(pmb887x_mci_t),
	.instance_init = mci_init,
	.class_init = mci_class_init,
};

static void mci_register_types(void) {
	type_register_static(&mci_info);
}
type_init(mci_register_types)
